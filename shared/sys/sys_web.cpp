/*
===========================================================================
Copyright (C) 2026, OpenJK contributors

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

// Web: letting the browser show frames.
//
// A browser only shows what was drawn once the engine returns to it, so a
// map load, which runs inside a single frame, would show nothing until it's
// done. Builds linked with JSPI (JavaScript Promise Integration) can instead
// wait for the browser's next frame in the middle of anything: the main loop
// does so after every frame, and WIN_Present does so when a loading screen
// draws more frames inside one main loop frame.

#include "qcommon/q_shared.h"
#include "sys_public.h"

#ifdef __EMSCRIPTEN__

extern "C" {
	int ojk_can_wait_for_frame( void );
	void ojk_wait_for_frame( void );
}

// show loading screen updates at most this often, so that waiting for the
// browser doesn't slow loading down much
#define LOADING_FRAME_MSEC 100

static int lastWaitTime;
static int framesSinceWait;

qboolean Sys_WebCanWaitForFrame( void )
{
	return ojk_can_wait_for_frame() ? qtrue : qfalse;
}

void Sys_WebWaitForFrame( void )
{
	if ( !ojk_can_wait_for_frame() )
		return;

	ojk_wait_for_frame();
	lastWaitTime = Sys_Milliseconds();
	framesSinceWait = 0;
}

void Sys_WebFramePresented( void )
{
	// the first frame is the main loop's own; later ones are loading screens
	if ( ++framesSinceWait > 1 && Sys_Milliseconds() - lastWaitTime >= LOADING_FRAME_MSEC )
		Sys_WebWaitForFrame();
}

#endif // __EMSCRIPTEN__
