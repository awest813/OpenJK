#!/usr/bin/env node
//============================================================================
// Copyright (C) 2026, OpenJK contributors
//
// This file is part of the OpenJK source code.
//
// OpenJK is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, see <http://www.gnu.org/licenses/>.
//============================================================================
//
// Checks tools/web/server.js without a browser: static files (including a
// path that must not escape the root), cache validators, the relay's origin
// check, and one UDP round trip.
//
//   node tools/web/server.test.mjs

import { spawn } from 'child_process';
import crypto from 'crypto';
import dgram from 'dgram';
import fs from 'fs';
import http from 'http';
import os from 'os';
import path from 'path';
import { fileURLToPath } from 'url';

const here = path.dirname(fileURLToPath(import.meta.url));
const secret = `secret-${crypto.randomBytes(8).toString('hex')}`;
let failed = 0;

function check(name, ok, detail) {
	console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ` (${detail})` : ''}`);
	if (!ok) failed++;
}

function startServer(args) {
	const child = spawn(process.execPath, [path.join(here, 'server.js'), ...args], {
		stdio: ['ignore', 'pipe', 'pipe'],
	});
	let output = '';
	return new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error(`server did not start:\n${output}`)), 5000);
		const onData = (chunk) => {
			output += chunk.toString();
			const match = /relay on ws:\/\/localhost:(\d+)\/relay/.exec(output);
			if (match) {
				clearTimeout(timer);
				resolve({ child, port: Number(match[1]), output });
			}
		};
		child.stdout.on('data', onData);
		child.stderr.on('data', onData);
		child.on('exit', (code) => {
			clearTimeout(timer);
			reject(new Error(`server.js exited with ${code}: ${output}`));
		});
	});
}

function request(port, method, urlPath, headers) {
	return new Promise((resolve, reject) => {
		const req = http.request({ port, host: '127.0.0.1', method, path: urlPath, headers }, (res) => {
			const chunks = [];
			res.on('data', (chunk) => chunks.push(chunk));
			res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks).toString() }));
		});
		req.on('error', reject);
		req.end();
	});
}

function rawUpgrade(port, headers) {
	return new Promise((resolve, reject) => {
		const req = http.request({
			port,
			host: '127.0.0.1',
			path: '/relay',
			headers: {
				Connection: 'Upgrade',
				Upgrade: 'websocket',
				'Sec-WebSocket-Key': crypto.randomBytes(16).toString('base64'),
				'Sec-WebSocket-Version': '13',
				Host: `127.0.0.1:${port}`,
				...headers,
			},
		});
		req.on('upgrade', (res, socket) => {
			socket.end();
			resolve(res.statusCode);
		});
		req.on('response', (res) => {
			res.resume();
			resolve(res.statusCode);
		});
		req.on('error', reject);
		req.end();
	});
}

function onceEvent(target, type) {
	return new Promise((resolve) => {
		target.addEventListener(type, function handler(event) {
			target.removeEventListener(type, handler);
			resolve(event);
		});
	});
}

async function main() {
	const bad = spawn(process.execPath, [path.join(here, 'server.js'), '--port', 'nope'], { stdio: 'ignore' });
	const badCode = await new Promise((resolve) => bad.on('exit', resolve));
	check('rejects a non-numeric port', badCode === 1, `exit ${badCode}`);

	const root = fs.mkdtempSync(path.join(os.tmpdir(), 'openjk-web-'));
	const outside = path.join(path.dirname(root), 'secret.txt');
	fs.writeFileSync(path.join(root, 'hello.txt'), 'hello');
	fs.writeFileSync(outside, secret);

	const { child, port } = await startServer(['--root', root, '--port', '0', '--allow-private']);
	try {
		const hello = await request(port, 'GET', '/hello.txt');
		check('serves a file', hello.status === 200 && hello.body === 'hello', `${hello.status} ${hello.body}`);
		check('sends an etag', typeof hello.headers.etag === 'string');

		const cached = await request(port, 'GET', '/hello.txt', { 'If-None-Match': hello.headers.etag });
		check('revalidates with 304', cached.status === 304 && cached.body === '', String(cached.status));

		const escaped = await request(port, 'GET', '/%2e%2e/secret.txt');
		check('does not serve files outside the root', escaped.status === 404 && !escaped.body.includes(secret),
			`${escaped.status}`);

		const cross = await rawUpgrade(port, { Origin: 'http://evil.example' });
		check('refuses a cross-origin relay', cross === 403, String(cross));

		const same = await rawUpgrade(port, { Origin: `http://127.0.0.1:${port}` });
		check('accepts a same-origin relay', same === 101, String(same));

		const udp = dgram.createSocket('udp4');
		await new Promise((resolve) => udp.bind(0, '127.0.0.1', resolve));
		const udpPort = udp.address().port;
		const ws = new WebSocket(`ws://127.0.0.1:${port}/relay`);
		ws.binaryType = 'arraybuffer';
		await onceEvent(ws, 'open');
		const payload = Buffer.from('ping');
		const message = Buffer.alloc(7 + payload.length);
		message[0] = 0;
		message.set([127, 0, 0, 1], 1);
		message.writeUInt16BE(udpPort, 5);
		message.set(payload, 7);
		const received = new Promise((resolve) => udp.once('message', (data, rinfo) => resolve({ data, rinfo })));
		ws.send(message);
		const { data, rinfo } = await Promise.race([
			received,
			new Promise((_, reject) => setTimeout(() => reject(new Error('no datagram')), 2000)),
		]);
		check('relays a datagram to UDP', data.toString() === 'ping', data.toString());
		udp.send(Buffer.from('pong'), rinfo.port, rinfo.address);
		const reply = await Promise.race([
			onceEvent(ws, 'message'),
			new Promise((_, reject) => setTimeout(() => reject(new Error('no reply')), 2000)),
		]);
		const bytes = Buffer.from(reply.data);
		check('passes the UDP reply back', bytes[0] === 0 && bytes.subarray(7).toString() === 'pong',
			bytes.subarray(7).toString());
		ws.close();
		udp.close();
	} finally {
		child.kill();
		fs.rmSync(root, { recursive: true, force: true });
		fs.rmSync(outside, { force: true });
	}

	if (failed) {
		process.exit(1);
	}
}

main().catch((error) => {
	console.error(error);
	process.exit(1);
});
