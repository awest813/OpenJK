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

// Pixel tests for shared/webgl/qgl_fixed.cpp, the OpenGL 1.x layer the web
// build's vanilla renderers draw with. Each test draws into its own 16x16
// cell of a 128x64 canvas and checks the colour in the middle of the cell
// against what fixed-function OpenGL produces. Run by tools/web/smoke-test.mjs:
//
//   em++ -O2 tools/web/qgl-fixed-test.cpp shared/webgl/qgl_fixed.cpp -Ishared \
//     -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -o qgl-fixed-test.html

#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <math.h>

typedef double GLdouble;
typedef double GLclampd;
#include "webgl/qgl_fixed.h"

#define GL_QUADS 0x0007
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_ALPHA_TEST 0x0BC0
#define GL_FOG 0x0B60
#define GL_CLIP_PLANE0 0x3000
#define GL_COLOR_ARRAY 0x8076
#define GL_VERTEX_ARRAY 0x8074
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_ADD 0x0104
#define GL_FOG_MODE 0x0B65
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_COLOR 0x0B66

static const int width = 128, height = 64, cell = 16;
static int failures, tests;

static void Quad( float x, float y, float size, float z = 0.0f )
{
	glfBegin( GL_QUADS );
	glfTexCoord2f( 0, 0 ); glfVertex3f( x, y, z );
	glfTexCoord2f( 1, 0 ); glfVertex3f( x + size, y, z );
	glfTexCoord2f( 1, 1 ); glfVertex3f( x + size, y + size, z );
	glfTexCoord2f( 0, 1 ); glfVertex3f( x, y + size, z );
	glfEnd();
}

static GLuint SolidTexture( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
	unsigned char pixels[4 * 4];
	for ( int i = 0; i < 4; i++ )
	{
		pixels[i * 4 + 0] = r;
		pixels[i * 4 + 1] = g;
		pixels[i * 4 + 2] = b;
		pixels[i * 4 + 3] = a;
	}
	GLuint texture;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	// a sized internal format, like the renderers use
	glfTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glfTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glfTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	return texture;
}

// Checks the colour in the middle of a cell (column, row from the top).
static void Expect( const char *name, int column, int row, int r, int g, int b )
{
	unsigned char rgb[3 * 4];
	const int x = column * cell + cell / 2;
	const int y = height - ( row * cell + cell / 2 ) - 1; // GL's origin is at the bottom
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glfReadPixels( x, y, 1, 1, 0x1907 /* GL_RGB */, GL_UNSIGNED_BYTE, rgb );

	tests++;
	const bool ok = abs( rgb[0] - r ) <= 3 && abs( rgb[1] - g ) <= 3 && abs( rgb[2] - b ) <= 3;
	if ( !ok )
		failures++;
	printf( "%s %s: got %d %d %d, expected %d %d %d\n", ok ? "PASS" : "FAIL", name, rgb[0], rgb[1], rgb[2], r, g, b );
}

int main()
{
	EmscriptenWebGLContextAttributes attributes;
	emscripten_webgl_init_context_attributes( &attributes );
	attributes.majorVersion = 2;
	attributes.preserveDrawingBuffer = true;
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_create_context( "#canvas", &attributes );
	emscripten_webgl_make_context_current( context );

	glViewport( 0, 0, width, height );
	glClearColor( 0, 0, 0, 1 );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	// 2D projection in pixels, y down, like the renderers' 2D mode
	glfMatrixMode( GL_PROJECTION );
	glfLoadIdentity();
	glfOrtho( 0, width, height, 0, -100, 100 );
	glfMatrixMode( GL_MODELVIEW );
	glfLoadIdentity();

	// 0: immediate mode quad, constant colour
	glfColor4f( 1, 0, 0, 1 );
	Quad( 0, 0, cell );

	// 1: texture modulated by the colour
	GLuint green = SolidTexture( 0, 255, 0, 255 );
	glfEnable( GL_TEXTURE_2D );
	glfTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
	glfColor4f( 0.5f, 0.5f, 0.5f, 1 );
	Quad( 16, 0, cell );

	// 2: GL_REPLACE ignores the colour
	glfTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE );
	glfColor4f( 1, 0, 0, 1 );
	Quad( 32, 0, cell );

	// 3: GL_DECAL with a half transparent texture
	GLuint halfBlue = SolidTexture( 0, 0, 255, 128 );
	glBindTexture( GL_TEXTURE_2D, halfBlue );
	glfTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL );
	glfColor4f( 1, 0, 0, 1 );
	Quad( 48, 0, cell );
	glfDisable( GL_TEXTURE_2D );

	// 4: client arrays with byte colours and glDrawElements
	{
		const float xyz[4][4] = { { 64, 0, 0, 0 }, { 80, 0, 0, 0 }, { 80, 16, 0, 0 }, { 64, 16, 0, 0 } };
		const unsigned char colors[4][4] = { { 0, 0, 255, 255 }, { 0, 0, 255, 255 }, { 0, 0, 255, 255 }, { 0, 0, 255, 255 } };
		const unsigned int indices[6] = { 0, 1, 2, 0, 2, 3 };
		glfEnableClientState( GL_VERTEX_ARRAY );
		glfVertexPointer( 3, GL_FLOAT, 16, xyz ); // vec4 stride, like tess.xyz
		glfEnableClientState( GL_COLOR_ARRAY );
		glfColorPointer( 4, GL_UNSIGNED_BYTE, 0, colors );
		glfDrawElements( GL_TRIANGLES, 6, GL_UNSIGNED_INT, indices );
		glfDisableClientState( GL_COLOR_ARRAY );
	}

	// 5: alpha test discards alpha 0.25 against GL_GREATER 0.5
	glfEnable( GL_ALPHA_TEST );
	glfAlphaFunc( GL_GREATER, 0.5f );
	glfColor4f( 1, 1, 1, 0.25f );
	Quad( 80, 0, cell );
	glfDisable( GL_ALPHA_TEST );

	// 6: linear fog halfway between start and end (eye depth 50)
	glfEnable( GL_FOG );
	glfFogf( GL_FOG_MODE, GL_LINEAR );
	glfFogf( GL_FOG_START, 0 );
	glfFogf( GL_FOG_END, 100 );
	const float fogColor[4] = { 0, 0, 1, 1 };
	glfFogfv( GL_FOG_COLOR, fogColor );
	glfColor4f( 1, 0, 0, 1 );
	Quad( 96, 0, cell, -50 );
	glfDisable( GL_FOG );

	// 7: the matrix stack: translate within push/pop
	glfPushMatrix();
	glfTranslatef( 112, 0, 0 );
	glfColor4f( 1, 1, 0, 1 );
	Quad( 0, 0, cell );
	glfPopMatrix();

	// row 1, 0: clip plane keeps x < 8 of the cell (plane -x + 8 >= 0)
	{
		const double plane[4] = { -1, 0, 0, 8 };
		glfClipPlane( GL_CLIP_PLANE0, plane );
		glfEnable( GL_CLIP_PLANE0 );
		glfColor4f( 0, 1, 1, 1 );
		Quad( 0, 16, cell );
		glfDisable( GL_CLIP_PLANE0 );
	}

	// row 1, 1: multitexture, unit 1 added to unit 0
	{
		GLuint red = SolidTexture( 255, 0, 0, 255 );
		GLuint dimGreen = SolidTexture( 0, 128, 0, 255 );
		glfActiveTexture( GL_TEXTURE0 );
		glBindTexture( GL_TEXTURE_2D, red );
		glfEnable( GL_TEXTURE_2D );
		glfTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE );
		glfActiveTexture( GL_TEXTURE1 );
		glBindTexture( GL_TEXTURE_2D, dimGreen );
		glfEnable( GL_TEXTURE_2D );
		glfTexEnvf( GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD );
		glfColor4f( 1, 1, 1, 1 );
		glfBegin( GL_QUADS );
		glfMultiTexCoord2f( GL_TEXTURE0, 0, 0 ); glfMultiTexCoord2f( GL_TEXTURE1, 0, 0 ); glfVertex2f( 16, 16 );
		glfMultiTexCoord2f( GL_TEXTURE0, 1, 0 ); glfMultiTexCoord2f( GL_TEXTURE1, 1, 0 ); glfVertex2f( 32, 16 );
		glfMultiTexCoord2f( GL_TEXTURE0, 1, 1 ); glfMultiTexCoord2f( GL_TEXTURE1, 1, 1 ); glfVertex2f( 32, 32 );
		glfMultiTexCoord2f( GL_TEXTURE0, 0, 1 ); glfMultiTexCoord2f( GL_TEXTURE1, 0, 1 ); glfVertex2f( 16, 32 );
		glfEnd();
		glfDisable( GL_TEXTURE_2D );
		glfActiveTexture( GL_TEXTURE0 );
		glfDisable( GL_TEXTURE_2D );
	}

	// row 1, 2: blending is WebGL's own, mixed with the fixed-function colour
	glfColor4f( 1, 1, 1, 1 );
	Quad( 32, 16, cell );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	glfColor4f( 0, 0, 0, 0.5f );
	Quad( 32, 16, cell );
	glDisable( GL_BLEND );

	Expect( "constant colour", 0, 0, 255, 0, 0 );
	Expect( "GL_MODULATE", 1, 0, 0, 128, 0 );
	Expect( "GL_REPLACE", 2, 0, 0, 255, 0 );
	Expect( "GL_DECAL", 3, 0, 127, 0, 128 );
	Expect( "client arrays", 4, 0, 0, 0, 255 );
	Expect( "alpha test", 5, 0, 0, 0, 0 );
	Expect( "linear fog", 6, 0, 128, 0, 128 );
	Expect( "matrix stack", 7, 0, 255, 255, 0 );
	{
		// left half drawn, right half clipped
		tests++;
		unsigned char left[3], right[3];
		glfReadPixels( 4, height - 24 - 1, 1, 1, 0x1907, GL_UNSIGNED_BYTE, left );
		glfReadPixels( 12, height - 24 - 1, 1, 1, 0x1907, GL_UNSIGNED_BYTE, right );
		const bool ok = left[1] == 255 && left[2] == 255 && right[1] == 0 && right[2] == 0;
		failures += !ok;
		printf( "%s clip plane: left %d %d %d, right %d %d %d\n", ok ? "PASS" : "FAIL",
			left[0], left[1], left[2], right[0], right[1], right[2] );
	}
	Expect( "multitexture GL_ADD", 1, 1, 255, 128, 0 );
	Expect( "blending", 2, 1, 128, 128, 128 );

	printf( "qgl_fixed: %d of %d tests passed\n", tests - failures, tests );
	return 0;
}
