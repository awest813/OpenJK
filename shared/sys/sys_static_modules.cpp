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

// Statically linked replacement for dlopen()/dlsym(), used when the renderer
// and game modules are linked into the engine (OPENJK_STATIC_MODULES).
//
// The usual library search in sys_main.cpp is kept: a "library" is found when
// the file name of the path being tried matches one of the modules below.

#include "qcommon/q_shared.h"
#include "sys_loadlib.h"

#include <stdint.h>

#ifdef OPENJK_STATIC_MODULES

struct staticFunction_t {
	const char	*name;
	void		*address;
};

struct staticModule_t {
	const char				*fileName;
	const staticFunction_t	*functions;
};

// Module entry points, prefixed per module by tools/wasm/isolate_module.py.
// Only their ABI matters here (pointers and integers in, pointer out), so the
// real argument types aren't pulled in.
#define DECLARE_LEGACY_ENTRY_POINTS( prefix ) \
	void prefix##dllEntry( intptr_t (*syscallptr)( intptr_t arg, ... ) ); \
	intptr_t prefix##vmMain( int command, intptr_t arg0, intptr_t arg1, intptr_t arg2, intptr_t arg3, \
		intptr_t arg4, intptr_t arg5, intptr_t arg6, intptr_t arg7, intptr_t arg8, intptr_t arg9, \
		intptr_t arg10, intptr_t arg11 );

#define LEGACY_ENTRY_POINTS( prefix ) \
	{ "dllEntry", (void *)prefix##dllEntry }, \
	{ "vmMain", (void *)prefix##vmMain },

extern "C" {
	void *renderer_GetRefAPI( int apiVersion, void *rimp );
}

static const staticFunction_t rendererFunctions[] = {
	{ "GetRefAPI", (void *)renderer_GetRefAPI },
	{ NULL, NULL }
};

#if defined(_JK2EXE)
// SP: the game module also contains cgame, whose entry points are the legacy
// ones (with 8 arguments to vmMain).
extern "C" {
	void *game_GetGameAPI( void *import );
	void game_dllEntry( intptr_t (*syscallptr)( intptr_t arg, ... ) );
	intptr_t game_vmMain( int command, intptr_t arg0, intptr_t arg1, intptr_t arg2, intptr_t arg3,
		intptr_t arg4, intptr_t arg5, intptr_t arg6, intptr_t arg7 );
}

static const staticFunction_t gameFunctions[] = {
	{ "GetGameAPI", (void *)game_GetGameAPI },
	{ "dllEntry", (void *)game_dllEntry },
	{ "vmMain", (void *)game_vmMain },
	{ NULL, NULL }
};

static const staticModule_t staticModules[] = {
#ifdef JK2_MODE
	{ "rdjosp-vanilla_" ARCH_STRING DLL_EXT, rendererFunctions },
	{ "jospgame" ARCH_STRING DLL_EXT, gameFunctions },
#else
	{ "rdsp-vanilla_" ARCH_STRING DLL_EXT, rendererFunctions },
	{ "jagame" ARCH_STRING DLL_EXT, gameFunctions },
#endif
	{ NULL, NULL }
};
#else
// MP: game, cgame and ui are separate modules
extern "C" {
	void *game_GetModuleAPI( int apiVersion, void *import );
	void *cgame_GetModuleAPI( int apiVersion, void *import );
	void *ui_GetModuleAPI( int apiVersion, void *import );
	DECLARE_LEGACY_ENTRY_POINTS( game_ )
	DECLARE_LEGACY_ENTRY_POINTS( cgame_ )
	DECLARE_LEGACY_ENTRY_POINTS( ui_ )
}

static const staticFunction_t gameFunctions[] = {
	{ "GetModuleAPI", (void *)game_GetModuleAPI },
	LEGACY_ENTRY_POINTS( game_ )
	{ NULL, NULL }
};

static const staticFunction_t cgameFunctions[] = {
	{ "GetModuleAPI", (void *)cgame_GetModuleAPI },
	LEGACY_ENTRY_POINTS( cgame_ )
	{ NULL, NULL }
};

static const staticFunction_t uiFunctions[] = {
	{ "GetModuleAPI", (void *)ui_GetModuleAPI },
	LEGACY_ENTRY_POINTS( ui_ )
	{ NULL, NULL }
};

static const staticModule_t staticModules[] = {
#ifdef DEFAULT_RENDER_LIBRARY
	{ DEFAULT_RENDER_LIBRARY "_" ARCH_STRING DLL_EXT, rendererFunctions },
#else
	{ "rd-vanilla_" ARCH_STRING DLL_EXT, rendererFunctions },
#endif
	{ "jampgame" ARCH_STRING DLL_EXT, gameFunctions },
	{ "cgame" ARCH_STRING DLL_EXT, cgameFunctions },
	{ "ui" ARCH_STRING DLL_EXT, uiFunctions },
	{ NULL, NULL }
};
#endif

static const char *staticLibraryError = "";

void *Sys_StaticLoadLibrary( const char *path )
{
	const char *fileName = strrchr( path, PATH_SEP );
	fileName = fileName ? fileName + 1 : path;

	for ( const staticModule_t *module = staticModules; module->fileName; module++ )
	{
		if ( !Q_stricmp( fileName, module->fileName ) )
			return (void *)module;
	}

	staticLibraryError = "not a statically linked module";
	return NULL;
}

void *Sys_StaticLoadFunction( void *handle, const char *name )
{
	const staticModule_t *module = (const staticModule_t *)handle;

	for ( const staticFunction_t *function = module->functions; function->name; function++ )
	{
		if ( !strcmp( name, function->name ) )
			return function->address;
	}

	staticLibraryError = "function not exported by statically linked module";
	return NULL;
}

const char *Sys_StaticLibraryError( void )
{
	return staticLibraryError;
}

#endif // OPENJK_STATIC_MODULES
