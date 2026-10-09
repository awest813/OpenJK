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
// Smoke test for the web build, in headless Chromium (Playwright):
//
//   node tools/web/smoke-test.mjs build-web
//
// The retail game data can't be used here, so the test adds a tiny made-up
// .pk3 with just enough in it for the engines to finish initialising. For
// each client it goes through the launcher like a player would (add the
// .pk3, press Play), checks that the engine gets to its main loop without
// errors, and then reloads to check that the game data and the config file
// written by the engine are still there. The MP client also connects to a
// fake UDP server through the relay in tools/web/server.js.

import { spawn } from 'child_process';
import fs from 'fs';
import dgram from 'dgram';
import path from 'path';
import { fileURLToPath } from 'url';
import { createRequire } from 'module';
import zlib from 'zlib';

const require = createRequire(import.meta.url);
let chromium;
try {
	({ chromium } = require('playwright'));
} catch (error) {
	// fall back to a globally installed Playwright
	const globalRoot = require('child_process').execSync('npm root -g').toString().trim();
	({ chromium } = createRequire(path.join(globalRoot, 'noop.js'))('playwright'));
}

const here = path.dirname(fileURLToPath(import.meta.url));
const buildDir = path.resolve(process.argv[2] || 'build-web');
const httpPort = 18080;
const udpPort = 29170;

//============================================================================
// A stored (uncompressed) zip file, which is all a .pk3 is

function makeZip(files) {
	const local = [];
	const central = [];
	let offset = 0;
	for (const [name, text] of Object.entries(files)) {
		const nameBytes = Buffer.from(name);
		const data = Buffer.from(text);
		const crc = zlib.crc32(data);
		const header = Buffer.alloc(30);
		header.writeUInt32LE(0x04034b50, 0);
		header.writeUInt16LE(20, 4);
		header.writeUInt32LE(crc, 14);
		header.writeUInt32LE(data.length, 18);
		header.writeUInt32LE(data.length, 22);
		header.writeUInt16LE(nameBytes.length, 26);
		local.push(header, nameBytes, data);

		const entry = Buffer.alloc(46);
		entry.writeUInt32LE(0x02014b50, 0);
		entry.writeUInt16LE(20, 4);
		entry.writeUInt16LE(20, 6);
		entry.writeUInt32LE(crc, 16);
		entry.writeUInt32LE(data.length, 20);
		entry.writeUInt32LE(data.length, 24);
		entry.writeUInt16LE(nameBytes.length, 28);
		entry.writeUInt32LE(offset, 42);
		central.push(entry, nameBytes);
		offset += header.length + nameBytes.length + data.length;
	}
	const centralSize = central.reduce((sum, b) => sum + b.length, 0);
	const end = Buffer.alloc(22);
	end.writeUInt32LE(0x06054b50, 0);
	end.writeUInt16LE(Object.keys(files).length, 8);
	end.writeUInt16LE(Object.keys(files).length, 10);
	end.writeUInt32LE(centralSize, 12);
	end.writeUInt32LE(offset, 16);
	return Buffer.concat([...local, ...central, end]);
}

const testData = makeZip({
	'default.cfg': 'echo smoke: default.cfg\n',
	'mpdefault.cfg': 'echo smoke: mpdefault.cfg\n',
	'shaders/smoke.shader': 'textures/smoke\n{\n}\n',
	'ext_data/Siege/Classes/smoke.scl': 'ClassInfo\n{\n\tname "Smoke"\n\tweapons WP_NONE\n\tuishader "textures/smoke"\n}\n',
	'ext_data/Siege/Teams/smoke.team': 'name "SmokeTeam"\nClasses\n{\n\tclass1 "Smoke"\n}\n',
	'ui/jampmenus.txt': '{\n}\n',
	'ui/menus.txt': '{\n}\n',
});

//============================================================================

function startServer() {
	const server = spawn(process.execPath, [path.join(here, 'server.js'),
		'--root', buildDir, '--port', String(httpPort), '--allow-private'], { stdio: ['ignore', 'pipe', 'inherit'] });
	return new Promise((resolve, reject) => {
		server.stdout.on('data', (data) => {
			if (data.toString().includes('relay on')) {
				resolve(server);
			}
		});
		server.on('exit', (code) => reject(new Error(`server.js exited with ${code}`)));
	});
}

// Answers a client's connection attempt the way a server would start to.
function startFakeGameServer() {
	const socket = dgram.createSocket('udp4');
	const received = [];
	socket.on('message', (data, rinfo) => {
		received.push(data);
		const text = data.subarray(4).toString('latin1');
		if (text.startsWith('getchallenge')) {
			socket.send(Buffer.concat([Buffer.from([255, 255, 255, 255]), Buffer.from('challengeResponse 424242')]), rinfo.port, rinfo.address);
		} else if (text.startsWith('connect')) {
			socket.send(Buffer.concat([Buffer.from([255, 255, 255, 255]), Buffer.from('print\nsmoke: hello through the relay\n')]), rinfo.port, rinfo.address);
		}
	});
	socket.bind(udpPort, '127.0.0.1');
	return { socket, received };
}

async function runClient(browser, page, { args = '', expect = [], addData = true }) {
	const lines = [];
	const errors = [];
	page.on('console', (message) => lines.push(message.text()));
	page.on('pageerror', (error) => errors.push(`page error: ${error.message}`));
	page.on('dialog', (dialog) => {
		errors.push(`dialog: ${dialog.message()}`);
		dialog.dismiss().catch(() => {});
	});

	await page.goto(`http://localhost:${httpPort}/${page.__file}`);
	if (addData) {
		await page.setInputFiles('#add-files', { name: 'zz-smoke.pk3', mimeType: 'application/octet-stream', buffer: testData });
	}
	await page.waitForSelector('#play:not([disabled])', { timeout: 120000 });
	await page.evaluate(() => { document.getElementById('advanced').open = true; });
	await page.fill('#args', args);
	await page.click('#play');

	const wanted = ['--- Common Initialization Complete ---', ...expect];
	const deadline = Date.now() + 90000;
	while (Date.now() < deadline && !errors.length) {
		if (wanted.every((text) => lines.some((line) => line.includes(text)))) {
			break;
		}
		await page.waitForTimeout(250);
	}
	// keep running for a bit to catch errors in the main loop
	await page.waitForTimeout(3000);

	const missing = wanted.filter((text) => !lines.some((line) => line.includes(text)));
	return { lines, errors, missing };
}

function report(name, result) {
	const ok = !result.errors.length && !result.missing.length;
	console.log(`${ok ? 'PASS' : 'FAIL'} ${name}`);
	if (!ok) {
		for (const error of result.errors) console.log(`  ${error}`);
		for (const text of result.missing) console.log(`  never saw: ${text}`);
		console.log('  last output:');
		for (const line of result.lines.filter((l) => !l.startsWith('WebGL:')).slice(-25)) console.log(`    ${line}`);
	}
	return ok;
}

async function main() {
	const server = await startServer();
	const gameServer = startFakeGameServer();
	const browser = await chromium.launch({
		args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-gpu-blocklist', '--autoplay-policy=no-user-gesture-required'],
	});
	let ok = true;
	try {
		for (const client of [
			{ name: 'SP client', file: 'openjk_sp.wasm32.html', config: 'openjk_sp.cfg' },
			// the build without JSPI, for browsers that don't have it
			{ name: 'SP client without JSPI', file: 'openjk_sp.wasm32.html?jspi=0', config: 'openjk_sp.cfg' },
			{ name: 'MP client', file: 'openjk.wasm32.html', config: 'openjk.cfg',
				args: `+connect 127.0.0.1:${udpPort}`, expect: ['smoke: hello through the relay'] },
			{ name: 'MP client with rend2', file: 'openjk_rend2.wasm32.html', config: 'openjk.cfg',
				expect: ['----- finished R_Init -----'], optional: true },
		]) {
			if (client.optional && !fs.existsSync(path.join(buildDir, client.file.split('?')[0]))) {
				console.log(`SKIP ${client.name} (not built)`);
				continue;
			}
			// a fresh profile, then a reload in the same profile
			const context = await browser.newContext();
			let page = await context.newPage();
			page.__file = client.file;
			ok = report(client.name, await runClient(browser, page, client)) && ok;
			await page.close();

			page = await context.newPage();
			page.__file = client.file;
			ok = report(`${client.name}, reloaded (game data and ${client.config} kept)`, await runClient(browser, page, {
				addData: false, expect: [`execing ${client.config}`],
			})) && ok;
			await context.close();
		}
	} finally {
		await browser.close();
		gameServer.socket.close();
		server.kill();
	}
	process.exit(ok ? 0 : 1);
}

main().catch((error) => {
	console.error(error);
	process.exit(1);
});
