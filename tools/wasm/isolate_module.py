#!/usr/bin/env python3
#============================================================================
# Copyright (C) 2026, OpenJK contributors
#
# This file is part of the OpenJK source code.
#
# OpenJK is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <http://www.gnu.org/licenses/>.
#============================================================================
"""Give a statically linked module the symbol isolation of a shared library.

Web builds link the renderer and game modules into the engine instead of
loading them as shared libraries. Modules define plenty of symbols that the
engine or other modules also define (shared code compiled with different
defines, trap_* functions, ...), which a shared library keeps to itself.

Input is a relocatable wasm object holding the whole module (made with
`emcc -r`). In its symbol table, this script:

- makes every defined symbol with hidden visibility local, which is what
  -fvisibility=hidden means for a shared library;
- prefixes the remaining defined (exported) symbols, e.g. GetModuleAPI
  becomes cgame_GetModuleAPI, so modules exporting the same entry point
  don't collide;
- prefixes COMDAT group names, so the linker doesn't merge one module's
  inline functions into another's.

llvm-objcopy can't edit wasm symbol tables, hence this script.
"""

import argparse
import sys

WASM_MAGIC = b'\0asm'

WASM_SYMBOL_TABLE = 8
WASM_COMDAT_INFO = 7

SYMTAB_FUNCTION = 0
SYMTAB_DATA = 1
SYMTAB_GLOBAL = 2
SYMTAB_SECTION = 3
SYMTAB_TAG = 4
SYMTAB_TABLE = 5

FLAG_BINDING_WEAK = 0x1
FLAG_BINDING_LOCAL = 0x2
FLAG_VISIBILITY_HIDDEN = 0x4
FLAG_UNDEFINED = 0x10
FLAG_EXPLICIT_NAME = 0x40


class Reader:
	def __init__(self, data, pos=0):
		self.data = data
		self.pos = pos

	def u8(self):
		value = self.data[self.pos]
		self.pos += 1
		return value

	def leb(self):
		result = 0
		shift = 0
		while True:
			byte = self.u8()
			result |= (byte & 0x7f) << shift
			shift += 7
			if not byte & 0x80:
				return result

	def bytes(self, count):
		value = self.data[self.pos:self.pos + count]
		self.pos += count
		return value

	def name(self):
		return self.bytes(self.leb())


def leb(value):
	out = bytearray()
	while True:
		byte = value & 0x7f
		value >>= 7
		if value:
			out.append(byte | 0x80)
		else:
			out.append(byte)
			return bytes(out)


def name(value):
	return leb(len(value)) + value


def rewrite_symbol_table(payload, prefix, stats):
	r = Reader(payload)
	count = r.leb()
	out = bytearray(leb(count))
	for _ in range(count):
		kind = r.u8()
		flags = r.leb()
		defined = not flags & FLAG_UNDEFINED
		local = flags & FLAG_BINDING_LOCAL

		new_flags = flags
		rename = False
		if defined and not local and kind != SYMTAB_SECTION:
			if flags & FLAG_VISIBILITY_HIDDEN:
				new_flags = (flags | FLAG_BINDING_LOCAL) & ~FLAG_BINDING_WEAK
				stats['localized'] += 1
			else:
				rename = True

		entry = bytearray()
		entry.append(kind)
		entry += leb(new_flags)
		if kind in (SYMTAB_FUNCTION, SYMTAB_GLOBAL, SYMTAB_TAG, SYMTAB_TABLE):
			entry += leb(r.leb())
			if defined or flags & FLAG_EXPLICIT_NAME:
				symbol_name = r.name()
				if rename:
					symbol_name = prefix + symbol_name
					stats['renamed'].append(symbol_name)
				entry += name(symbol_name)
		elif kind == SYMTAB_DATA:
			symbol_name = r.name()
			if rename:
				symbol_name = prefix + symbol_name
				stats['renamed'].append(symbol_name)
			entry += name(symbol_name)
			if defined:
				entry += leb(r.leb())  # segment index
				entry += leb(r.leb())  # offset
				entry += leb(r.leb())  # size
		elif kind == SYMTAB_SECTION:
			entry += leb(r.leb())
		else:
			sys.exit('unknown symbol kind %d' % kind)
		out += entry

	if r.pos != len(payload):
		sys.exit('trailing data in symbol table')
	return bytes(out)


def rewrite_comdat_info(payload, prefix, stats):
	r = Reader(payload)
	count = r.leb()
	out = bytearray(leb(count))
	for _ in range(count):
		out += name(prefix + r.name())
		out += leb(r.leb())  # flags
		entries = r.leb()
		out += leb(entries)
		for _ in range(entries):
			out += leb(r.leb())  # kind
			out += leb(r.leb())  # index
		stats['comdats'] += 1
	return bytes(out)


def rewrite_linking(payload, prefix, stats):
	r = Reader(payload)
	version = r.leb()
	if version != 2:
		sys.exit('unsupported linking section version %d' % version)
	out = bytearray(leb(version))
	while r.pos < len(payload):
		subsection = r.u8()
		body = r.bytes(r.leb())
		if subsection == WASM_SYMBOL_TABLE:
			body = rewrite_symbol_table(body, prefix, stats)
		elif subsection == WASM_COMDAT_INFO:
			body = rewrite_comdat_info(body, prefix, stats)
		out.append(subsection)
		out += leb(len(body))
		out += body
	return bytes(out)


def main():
	parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
	parser.add_argument('--prefix', required=True, help='prefix for exported symbols')
	parser.add_argument('--verbose', action='store_true')
	parser.add_argument('input')
	parser.add_argument('output')
	args = parser.parse_args()

	prefix = args.prefix.encode()
	data = open(args.input, 'rb').read()
	if data[:4] != WASM_MAGIC:
		sys.exit('%s is not a wasm object' % args.input)

	stats = {'localized': 0, 'renamed': [], 'comdats': 0}
	found_linking = False
	r = Reader(data, 8)
	out = bytearray(data[:8])
	while r.pos < len(data):
		section_id = r.u8()
		payload = r.bytes(r.leb())
		if section_id == 0:
			pr = Reader(payload)
			section_name = pr.name()
			if section_name == b'linking':
				found_linking = True
				payload = name(section_name) + rewrite_linking(payload[pr.pos:], prefix, stats)
		out.append(section_id)
		out += leb(len(payload))
		out += payload

	if not found_linking:
		sys.exit('%s has no linking section; is it a relocatable object?' % args.input)

	open(args.output, 'wb').write(out)

	print('%s: %d symbols made local, %d exported symbols prefixed with "%s", %d comdats renamed'
		% (args.output, stats['localized'], len(stats['renamed']), args.prefix, stats['comdats']))
	if args.verbose:
		for symbol in stats['renamed']:
			print('  ' + symbol.decode())


if __name__ == '__main__':
	main()
