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

// WebGL cannot read the depth buffer, which is how the desktop renderers
// decide whether a flare is behind a wall. See webgl_flare.cpp.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 1 if a light at eye-space z `eyeZ` is in front of the depth buffer at
// window pixel (x, y). `projection` is the column-major matrix the desktop
// test reconstructs eye z from. The 24-unit slack of that test is kept.
int WebGL_FlareVisible( int x, int y, float eyeZ, const float *projection );

#ifdef __cplusplus
}
#endif
