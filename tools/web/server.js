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
// Serves the web build of OpenJK and relays its network traffic.
//
// Browsers can't send UDP, so the web client (codemp/qcommon/net_web.cpp)
// sends every datagram over one WebSocket to /relay, and this server sends it
// on as UDP from a socket of its own, passing replies back. See net_web.cpp
// for the message format.
//
//   node tools/web/server.js --root build-web [--port 8080]
//
// Only Node.js is needed (no npm packages). Run it behind a TLS-terminating
// proxy for https/wss. Relaying is restricted (see --help) so that the
// server can't be used to send UDP to arbitrary hosts.

'use strict';

const crypto = require('crypto');
const dgram = require('dgram');
const dns = require('dns');
const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');

const USAGE = `usage: node server.js [options]

  --root DIR          serve the web build from DIR (default: current directory)
  --port N            listen on port N (default: 8080)
  --host ADDR         listen on ADDR (default: all interfaces)
  --no-static         only relay, don't serve files
  --allow HOST[:PORT] only relay to these servers (repeatable; host names are
                      resolved once at startup). Without it, any public
                      address on ports --min-port..--max-port is allowed.
  --allow-private     also allow private, loopback and link-local addresses
                      (for LAN games and local testing)
  --min-port N        lowest UDP port relayed to (default: 1024)
  --max-port N        highest UDP port relayed to (default: 65535)
  --max-clients N     at most N relay connections at once (default: 256)
  --rate N            at most N packets per second per client (default: 500)
  --verbose           log every relay connection
`;

function parseArgs(argv) {
	const options = {
		root: process.cwd(), port: 8080, host: undefined, serveStatic: true,
		allow: [], allowPrivate: false, minPort: 1024, maxPort: 65535,
		maxClients: 256, rate: 500, verbose: false,
	};
	for (let i = 0; i < argv.length; i++) {
		const arg = argv[i];
		const value = () => {
			if (i + 1 >= argv.length) {
				console.error(`${arg} needs a value\n\n${USAGE}`);
				process.exit(1);
			}
			return argv[++i];
		};
		switch (arg) {
			case '--root': options.root = path.resolve(value()); break;
			case '--port': options.port = parseInt(value(), 10); break;
			case '--host': options.host = value(); break;
			case '--no-static': options.serveStatic = false; break;
			case '--allow': options.allow.push(value()); break;
			case '--allow-private': options.allowPrivate = true; break;
			case '--min-port': options.minPort = parseInt(value(), 10); break;
			case '--max-port': options.maxPort = parseInt(value(), 10); break;
			case '--max-clients': options.maxClients = parseInt(value(), 10); break;
			case '--rate': options.rate = parseInt(value(), 10); break;
			case '--verbose': options.verbose = true; break;
			case '-h': case '--help': console.log(USAGE); process.exit(0);
			default: console.error(`unknown option ${arg}\n\n${USAGE}`); process.exit(1);
		}
	}
	return options;
}

const options = parseArgs(process.argv.slice(2));

//============================================================================
// Relay policy

function isPrivateAddress(ip) {
	const b = ip.split('.').map(Number);
	return b[0] === 0 || b[0] === 10 || b[0] === 127 || b[0] >= 224 ||
		(b[0] === 100 && (b[1] & 0xc0) === 64) ||   // carrier-grade NAT
		(b[0] === 169 && b[1] === 254) ||
		(b[0] === 172 && (b[1] & 0xf0) === 16) ||
		(b[0] === 192 && b[1] === 168) ||
		(b[0] === 198 && (b[1] & 0xfe) === 18);       // placeholder range used by the client
}

// "ip:port" (port optional) entries from --allow, filled in at startup
const allowList = [];

async function resolveAllowList() {
	for (const entry of options.allow) {
		const match = /^(.*?)(?::(\d+))?$/.exec(entry);
		const host = match[1];
		const port = match[2] ? parseInt(match[2], 10) : null;
		const { address } = await dns.promises.lookup(host, { family: 4 });
		allowList.push({ ip: address, port });
		console.log(`relaying to ${host} (${address})${port ? ':' + port : ''}`);
	}
}

function isAllowed(ip, port) {
	if (allowList.length) {
		return allowList.some((entry) => entry.ip === ip && (entry.port === null || entry.port === port));
	}
	if (port < options.minPort || port > options.maxPort) {
		return false;
	}
	return options.allowPrivate || !isPrivateAddress(ip);
}

//============================================================================
// Relay connection: one WebSocket and one UDP socket per client

const MSG_DATAGRAM = 0;
const MSG_NAME = 1;
const MSG_NOTICE = 2;
const MAX_DATAGRAM = 65507;

let relayClients = 0;

class RelayClient {
	constructor(socket, remote) {
		this.ws = new WebSocketConnection(socket);
		this.remote = remote;
		this.names = new Map();     // placeholder ip -> { host, ip (when resolved), waiting: [] }
		this.realToPlaceholder = new Map();
		this.contacted = new Set(); // "ip:port" we sent to; only they may answer
		this.tokens = options.rate;
		this.lastRefill = Date.now();
		this.noticed = new Set();

		this.udp = dgram.createSocket('udp4');
		this.udp.on('message', (data, rinfo) => this.onDatagram(data, rinfo));
		this.udp.on('error', (error) => this.notice(`UDP error: ${error.message}`));
		this.udp.bind(0);

		this.ws.onMessage = (data) => this.onMessage(data);
		this.ws.onClose = () => this.close();

		relayClients++;
		if (options.verbose) {
			console.log(`relay: ${remote} connected (${relayClients} connections)`);
		}
	}

	close() {
		if (this.closed) {
			return;
		}
		this.closed = true;
		relayClients--;
		this.udp.close();
		if (options.verbose) {
			console.log(`relay: ${this.remote} disconnected`);
		}
	}

	notice(text) {
		// send each distinct notice once, so a refused address doesn't spam
		if (this.noticed.has(text) || this.noticed.size > 64) {
			return;
		}
		this.noticed.add(text);
		this.ws.send(Buffer.concat([Buffer.from([MSG_NOTICE]), Buffer.from(text)]));
	}

	takeToken() {
		const now = Date.now();
		this.tokens = Math.min(options.rate, this.tokens + (now - this.lastRefill) * options.rate / 1000);
		this.lastRefill = now;
		if (this.tokens < 1) {
			return false;
		}
		this.tokens--;
		return true;
	}

	onMessage(data) {
		if (data.length < 1) {
			return;
		}
		if (data[0] === MSG_DATAGRAM && data.length >= 7) {
			if (!this.takeToken()) {
				return;
			}
			const ip = `${data[1]}.${data[2]}.${data[3]}.${data[4]}`;
			const port = data.readUInt16BE(5);
			this.sendDatagram(ip, port, data.subarray(7));
		} else if (data[0] === MSG_NAME && data.length > 5 && data.length < 5 + 254) {
			const placeholder = `${data[1]}.${data[2]}.${data[3]}.${data[4]}`;
			const host = data.subarray(5).toString();
			if (this.names.has(placeholder) || this.names.size >= 1024) {
				return;
			}
			const name = { host, ip: null, waiting: [] };
			this.names.set(placeholder, name);
			dns.lookup(host, { family: 4 }, (error, address) => {
				if (this.closed) {
					return;
				}
				if (error) {
					this.notice(`can't resolve ${host}`);
					name.waiting = null;
					return;
				}
				name.ip = address;
				if (!this.realToPlaceholder.has(address)) {
					this.realToPlaceholder.set(address, placeholder);
				}
				for (const [port, payload] of name.waiting) {
					this.sendDatagram(address, port, payload);
				}
				name.waiting = null;
			});
		}
	}

	sendDatagram(ip, port, payload) {
		const name = this.names.get(ip);
		if (name) {
			if (!name.ip) {
				if (name.waiting && name.waiting.length < 64) {
					name.waiting.push([port, Buffer.from(payload)]);
				}
				return;
			}
			ip = name.ip;
		}
		if (!net.isIPv4(ip) || payload.length > MAX_DATAGRAM) {
			return;
		}
		if (!isAllowed(ip, port)) {
			this.notice(`not allowed to relay to ${ip}:${port}`);
			return;
		}
		this.contacted.add(`${ip}:${port}`);
		this.udp.send(payload, port, ip);
	}

	onDatagram(data, rinfo) {
		if (rinfo.family !== 'IPv4' || !this.contacted.has(`${rinfo.address}:${rinfo.port}`)) {
			return;
		}
		const ip = (this.realToPlaceholder.get(rinfo.address) || rinfo.address).split('.').map(Number);
		const header = Buffer.from([MSG_DATAGRAM, ip[0], ip[1], ip[2], ip[3], rinfo.port >> 8, rinfo.port & 0xff]);
		this.ws.send(Buffer.concat([header, data]));
	}
}

//============================================================================
// Minimal WebSocket server side (RFC 6455): binary messages, ping, close

class WebSocketConnection {
	constructor(socket) {
		this.socket = socket;
		this.buffer = Buffer.alloc(0);
		this.fragments = [];
		this.onMessage = () => {};
		this.onClose = () => {};
		socket.setNoDelay(true);
		socket.on('data', (chunk) => this.onData(chunk));
		socket.on('close', () => this.onClose());
		socket.on('error', () => socket.destroy());
	}

	onData(chunk) {
		this.buffer = this.buffer.length ? Buffer.concat([this.buffer, chunk]) : chunk;
		while (this.buffer.length >= 2) {
			const first = this.buffer[0];
			const second = this.buffer[1];
			let length = second & 0x7f;
			let offset = 2;
			if (length === 126) {
				if (this.buffer.length < 4) return;
				length = this.buffer.readUInt16BE(2);
				offset = 4;
			} else if (length === 127) {
				if (this.buffer.length < 10) return;
				const high = this.buffer.readUInt32BE(2);
				if (high !== 0) {
					this.socket.destroy();
					return;
				}
				length = this.buffer.readUInt32BE(6);
				offset = 10;
			}
			if (length > 1 << 20) {
				this.socket.destroy();
				return;
			}
			const masked = second & 0x80;
			if (!masked) {
				// clients must mask their frames
				this.socket.destroy();
				return;
			}
			if (this.buffer.length < offset + 4 + length) return;
			const mask = this.buffer.subarray(offset, offset + 4);
			const payload = Buffer.from(this.buffer.subarray(offset + 4, offset + 4 + length));
			for (let i = 0; i < payload.length; i++) {
				payload[i] ^= mask[i & 3];
			}
			this.buffer = this.buffer.subarray(offset + 4 + length);
			this.onFrame(first & 0x80, first & 0x0f, payload);
		}
	}

	onFrame(fin, opcode, payload) {
		switch (opcode) {
			case 0x0: // continuation
			case 0x1: // text
			case 0x2: // binary
				this.fragments.push(payload);
				if (fin) {
					const message = Buffer.concat(this.fragments);
					this.fragments = [];
					this.onMessage(message);
				}
				break;
			case 0x8: // close
				this.sendFrame(0x8, payload.subarray(0, 2));
				this.socket.end();
				break;
			case 0x9: // ping
				this.sendFrame(0xa, payload);
				break;
			default: // pong and others
				break;
		}
	}

	sendFrame(opcode, payload) {
		if (this.socket.destroyed || !this.socket.writable) {
			return;
		}
		let header;
		if (payload.length < 126) {
			header = Buffer.from([0x80 | opcode, payload.length]);
		} else if (payload.length < 65536) {
			header = Buffer.alloc(4);
			header[0] = 0x80 | opcode;
			header[1] = 126;
			header.writeUInt16BE(payload.length, 2);
		} else {
			header = Buffer.alloc(10);
			header[0] = 0x80 | opcode;
			header[1] = 127;
			header.writeUInt32BE(0, 2);
			header.writeUInt32BE(payload.length, 6);
		}
		this.socket.write(Buffer.concat([header, payload]));
	}

	send(data) {
		this.sendFrame(0x2, data);
	}
}

function acceptWebSocket(request, socket) {
	const key = request.headers['sec-websocket-key'];
	if (!key || (request.headers.upgrade || '').toLowerCase() !== 'websocket') {
		socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
		return false;
	}
	const accept = crypto.createHash('sha1')
		.update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
		.digest('base64');
	socket.write('HTTP/1.1 101 Switching Protocols\r\n' +
		'Upgrade: websocket\r\n' +
		'Connection: Upgrade\r\n' +
		`Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
	return true;
}

//============================================================================
// Static files

const MIME_TYPES = {
	'.html': 'text/html; charset=utf-8',
	'.js': 'text/javascript; charset=utf-8',
	'.mjs': 'text/javascript; charset=utf-8',
	'.wasm': 'application/wasm',
	'.css': 'text/css; charset=utf-8',
	'.json': 'application/json',
	'.png': 'image/png',
	'.svg': 'image/svg+xml',
	'.ico': 'image/x-icon',
	'.data': 'application/octet-stream',
	'.pk3': 'application/octet-stream',
};

function serveStatic(request, response) {
	if (!options.serveStatic || (request.method !== 'GET' && request.method !== 'HEAD')) {
		response.writeHead(404);
		response.end();
		return;
	}
	let urlPath;
	try {
		urlPath = decodeURIComponent(new URL(request.url, 'http://localhost').pathname);
	} catch (error) {
		response.writeHead(400);
		response.end();
		return;
	}
	if (urlPath.endsWith('/')) {
		urlPath += 'index.html';
	}
	const file = path.join(options.root, path.normalize(urlPath));
	if (!file.startsWith(options.root + path.sep) && file !== options.root) {
		response.writeHead(403);
		response.end();
		return;
	}
	fs.stat(file, (error, stat) => {
		if (error || !stat.isFile()) {
			response.writeHead(404, { 'Content-Type': 'text/plain' });
			response.end('not found');
			return;
		}
		response.writeHead(200, {
			'Content-Type': MIME_TYPES[path.extname(file)] || 'application/octet-stream',
			'Content-Length': stat.size,
			'Cache-Control': 'no-cache',
		});
		if (request.method === 'HEAD') {
			response.end();
			return;
		}
		fs.createReadStream(file).pipe(response);
	});
}

//============================================================================

async function main() {
	await resolveAllowList();

	const server = http.createServer(serveStatic);
	server.on('upgrade', (request, socket) => {
		const urlPath = new URL(request.url, 'http://localhost').pathname;
		if (urlPath !== '/relay') {
			socket.end('HTTP/1.1 404 Not Found\r\n\r\n');
			return;
		}
		if (relayClients >= options.maxClients) {
			socket.end('HTTP/1.1 503 Service Unavailable\r\n\r\n');
			return;
		}
		if (acceptWebSocket(request, socket)) {
			new RelayClient(socket, `${request.socket.remoteAddress}:${request.socket.remotePort}`);
		}
	});
	server.listen(options.port, options.host, () => {
		const where = `${options.host || 'localhost'}:${options.port}`;
		if (options.serveStatic) {
			console.log(`serving ${options.root} on http://${where}/`);
		}
		console.log(`relay on ws://${where}/relay`);
	});
}

main().catch((error) => {
	console.error(error.message);
	process.exit(1);
});
