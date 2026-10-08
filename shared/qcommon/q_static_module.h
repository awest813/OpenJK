/*
===========================================================================
Copyright (C) 2013 - 2026, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// Force-included (-include) into every source file of a renderer or game
// module when modules are statically linked into the engine instead of being
// loaded as shared libraries (OPENJK_STATIC_MODULES, used by web builds).
//
// Shared code (q_math, q_shared, ...) is linked once, by the engine. A few
// globals are defined by both the engine and a module with different meanings
// though; those are renamed here to <OPENJK_MODULE_PREFIX><name> so they don't
// collide at link time. Keep shared/sys/sys_static_modules.cpp in sync when
// renaming a module entry point.

#pragma once

#ifndef OPENJK_MODULE_PREFIX
#error OPENJK_MODULE_PREFIX must be defined by the build system
#endif

#define OJK_MODULE_SYMBOL_CAT2( a, b ) a##b
#define OJK_MODULE_SYMBOL_CAT( a, b ) OJK_MODULE_SYMBOL_CAT2( a, b )
#define OJK_MODULE_SYMBOL( name ) OJK_MODULE_SYMBOL_CAT( OPENJK_MODULE_PREFIX, name )

// module-side wrappers around the import table
#define Com_Printf				OJK_MODULE_SYMBOL( Com_Printf )
#define Com_DPrintf				OJK_MODULE_SYMBOL( Com_DPrintf )
#define Com_Error				OJK_MODULE_SYMBOL( Com_Error )

// module-local copies of engine cvars
#define com_buildScript			OJK_MODULE_SYMBOL( com_buildScript )
#define se_language				OJK_MODULE_SYMBOL( se_language )
#define sv_mapname				OJK_MODULE_SYMBOL( sv_mapname )
#define sv_mapChecksum			OJK_MODULE_SYMBOL( sv_mapChecksum )

// renderer: window_t window (engine: mp3 decoder table)
#define window					OJK_MODULE_SYMBOL( window )

// SP game: cgame code that the engine's built-in UI duplicates
#define vmMain					OJK_MODULE_SYMBOL( vmMain )
#define animTable				OJK_MODULE_SYMBOL( animTable )
#define SaberParms				OJK_MODULE_SYMBOL( SaberParms )
#define TranslateSaberColor		OJK_MODULE_SYMBOL( TranslateSaberColor )
#define trap_CIN_PlayCinematic	OJK_MODULE_SYMBOL( trap_CIN_PlayCinematic )
#define trap_CIN_StopCinematic	OJK_MODULE_SYMBOL( trap_CIN_StopCinematic )
