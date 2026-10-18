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

// Flare occlusion for WebGL.
//
// The desktop test reads one depth pixel and keeps the flare when the
// surface there is less than 24 units closer than the light. WebGL's
// readPixels rejects GL_DEPTH_COMPONENT, and blitting the default
// framebuffer's depth into a texture is rejected too. Drawing a one-pixel
// point at the slack depth answers the same question: the point passes
// GL_LEQUAL only when the buffer is at least that far away. The colour
// that proves it passed is then put back, and every piece of GL state
// this touched is restored. glGetError is left clear: both renderers
// treat a leftover error as fatal.

#include <GLES3/gl3.h>
#include <emscripten/html5.h>

#include <math.h>
#include <stdio.h>

#include "webgl_flare.h"

#ifndef GL_BACK
#define GL_BACK 0x0405
#endif

static const char *vertexSource =
	"#version 300 es\n"
	"uniform vec4 uClip;\n"
	"void main() {\n"
	"  gl_Position = uClip;\n"
	"  gl_PointSize = 1.0;\n"
	"}\n";

static const char *fragmentSource =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform vec4 uColor;\n"
	"out vec4 oColor;\n"
	"void main() { oColor = uColor; }\n";

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE flareContext = 0;
static GLuint flareProgram = 0;
static GLuint flareVao = 0;
static GLint flareClip = -1;
static GLint flareColor = -1;
static bool flareFailed = false;

static void DrainError( void )
{
	while ( glGetError() != GL_NO_ERROR )
		;
}

static GLuint Compile( GLenum type, const char *source )
{
	GLuint shader = glCreateShader( type );
	glShaderSource( shader, 1, &source, NULL );
	glCompileShader( shader );
	GLint ok = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &ok );
	if ( !ok )
	{
		char log[1024];
		glGetShaderInfoLog( shader, sizeof( log ), NULL, log );
		printf( "webgl flare: shader compile error:\n%s\n", log );
		glDeleteShader( shader );
		return 0;
	}
	return shader;
}

static bool EnsureProgram( void )
{
	const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_get_current_context();
	if ( !context )
		return false;
	if ( flareContext != context )
	{
		// The previous context's objects are already gone (vid_restart).
		flareContext = context;
		flareProgram = 0;
		flareVao = 0;
		flareFailed = false;
	}
	if ( flareProgram )
		return true;
	if ( flareFailed )
		return false;

	GLint previous = 0;
	glGetIntegerv( GL_CURRENT_PROGRAM, &previous );

	GLuint vertex = Compile( GL_VERTEX_SHADER, vertexSource );
	GLuint fragment = Compile( GL_FRAGMENT_SHADER, fragmentSource );
	flareProgram = glCreateProgram();
	if ( vertex )
		glAttachShader( flareProgram, vertex );
	if ( fragment )
		glAttachShader( flareProgram, fragment );
	glLinkProgram( flareProgram );
	if ( vertex )
		glDeleteShader( vertex );
	if ( fragment )
		glDeleteShader( fragment );

	GLint linked = GL_FALSE;
	glGetProgramiv( flareProgram, GL_LINK_STATUS, &linked );
	if ( !linked || !vertex || !fragment )
	{
		char log[1024];
		glGetProgramInfoLog( flareProgram, sizeof( log ), NULL, log );
		printf( "webgl flare: program link error:\n%s\n", log );
		glDeleteProgram( flareProgram );
		flareProgram = 0;
		flareFailed = true;
		glUseProgram( previous );
		DrainError();
		return false;
	}

	flareClip = glGetUniformLocation( flareProgram, "uClip" );
	flareColor = glGetUniformLocation( flareProgram, "uColor" );
	flareVao = 0;
	glGenVertexArrays( 1, &flareVao );
	glUseProgram( previous );
	DrainError();
	return flareVao != 0;
}

// 1 = visible, 0 = hidden, -1 = test `*windowZ` (0 near, 1 far).
static int Classify( float eyeZ, const float *m, float *windowZ )
{
	// Ortho: the desktop formula's eye z does not depend on the depth value.
	if ( fabsf( m[11] ) < 1.0e-6f )
	{
		if ( fabsf( m[10] ) < 1.0e-8f )
			return 1;
		const float screenZ = m[14] / -m[10];
		return ( -eyeZ + screenZ ) < 24.0f ? 1 : 0;
	}

	// Slack plane, 24 units in front of the light. The desktop test keeps
	// the flare when (-eyeZ - -screenZ) < 24, i.e. the surface is farther
	// than this plane.
	const float S = eyeZ + 24.0f;
	if ( !( S == S ) || fabsf( S ) < 1.0e-4f )
		return 1;

	// clip w <= 0 is behind the camera, so nothing in the buffer can be
	// 24 units closer than the light.
	const float clipW = m[11] * S + m[15];
	if ( !( clipW == clipW ) || clipW <= 0.0f )
		return 1;

	// Inverse of the desktop readback:
	//   screenZ = m14 / ((2*depth - 1) * m11 - m10)
	const float ndc = ( m[14] / S + m[10] ) / m[11];
	if ( !( ndc == ndc ) )
		return 1;
	const float depth = 0.5f * ( ndc + 1.0f );
	if ( !( depth == depth ) || depth <= 0.0f )
		return 1;
	if ( depth >= 1.0f )
		return 0;

	*windowZ = depth;
	return -1;
}

struct FlareState
{
	GLint readFbo;
	GLint drawFbo;
	GLint program;
	GLint vao;
	GLint packBuffer;
	GLint packAlign;
	GLint viewport[4];
	GLint scissorBox[4];
	GLint depthFunc;
	GLint drawBuffers[4];
	GLboolean depthMask;
	GLboolean colorMask[4];
	GLboolean scissor;
	GLboolean depth;
	GLboolean blend;
	GLboolean cull;
	GLboolean stencil;
	GLboolean dither;
	GLboolean rasterDiscard;
	GLboolean sampleAlpha;
	GLboolean sampleCoverage;
	GLboolean polygonOffset;
	bool asFloat;
};

static void SaveState( FlareState *s )
{
	glGetIntegerv( GL_READ_FRAMEBUFFER_BINDING, &s->readFbo );
	glGetIntegerv( GL_DRAW_FRAMEBUFFER_BINDING, &s->drawFbo );
	glGetIntegerv( GL_CURRENT_PROGRAM, &s->program );
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &s->vao );
	glGetIntegerv( GL_PIXEL_PACK_BUFFER_BINDING, &s->packBuffer );
	glGetIntegerv( GL_PACK_ALIGNMENT, &s->packAlign );
	glGetIntegerv( GL_VIEWPORT, s->viewport );
	glGetIntegerv( GL_SCISSOR_BOX, s->scissorBox );
	glGetIntegerv( GL_DEPTH_FUNC, &s->depthFunc );
	for ( int i = 0; i < 4; i++ )
		glGetIntegerv( GL_DRAW_BUFFER0 + i, &s->drawBuffers[i] );
	glGetBooleanv( GL_DEPTH_WRITEMASK, &s->depthMask );
	glGetBooleanv( GL_COLOR_WRITEMASK, s->colorMask );
	s->scissor = glIsEnabled( GL_SCISSOR_TEST );
	s->depth = glIsEnabled( GL_DEPTH_TEST );
	s->blend = glIsEnabled( GL_BLEND );
	s->cull = glIsEnabled( GL_CULL_FACE );
	s->stencil = glIsEnabled( GL_STENCIL_TEST );
	s->dither = glIsEnabled( GL_DITHER );
	s->rasterDiscard = glIsEnabled( GL_RASTERIZER_DISCARD );
	s->sampleAlpha = glIsEnabled( GL_SAMPLE_ALPHA_TO_COVERAGE );
	s->sampleCoverage = glIsEnabled( GL_SAMPLE_COVERAGE );
	s->polygonOffset = glIsEnabled( GL_POLYGON_OFFSET_FILL );

	s->asFloat = false;
	if ( s->drawFbo )
	{
		GLint component = 0;
		glGetFramebufferAttachmentParameteriv( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component );
		if ( glGetError() == GL_NO_ERROR )
			s->asFloat = component == GL_FLOAT;
	}
}

static void RestoreState( const FlareState *s )
{
	GLenum buffers[4];
	int count = 1;
	for ( int i = 0; i < 4; i++ )
	{
		buffers[i] = (GLenum)s->drawBuffers[i];
		if ( i > 0 && s->drawBuffers[i] != GL_NONE )
			count = i + 1;
	}
	if ( !s->drawFbo )
		count = 1;
	glDrawBuffers( count, buffers );

	glUseProgram( s->program );
	glBindVertexArray( s->vao );
	glBindBuffer( GL_PIXEL_PACK_BUFFER, s->packBuffer );
	glPixelStorei( GL_PACK_ALIGNMENT, s->packAlign );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, s->readFbo );
	glBindFramebuffer( GL_DRAW_FRAMEBUFFER, s->drawFbo );

	if ( s->scissor )
		glEnable( GL_SCISSOR_TEST );
	else
		glDisable( GL_SCISSOR_TEST );
	glScissor( s->scissorBox[0], s->scissorBox[1], s->scissorBox[2], s->scissorBox[3] );

	if ( s->depth )
		glEnable( GL_DEPTH_TEST );
	else
		glDisable( GL_DEPTH_TEST );
	glDepthFunc( s->depthFunc );
	glDepthMask( s->depthMask );

	if ( s->blend )
		glEnable( GL_BLEND );
	else
		glDisable( GL_BLEND );
	if ( s->cull )
		glEnable( GL_CULL_FACE );
	else
		glDisable( GL_CULL_FACE );
	if ( s->stencil )
		glEnable( GL_STENCIL_TEST );
	else
		glDisable( GL_STENCIL_TEST );
	if ( s->dither )
		glEnable( GL_DITHER );
	else
		glDisable( GL_DITHER );
	if ( s->rasterDiscard )
		glEnable( GL_RASTERIZER_DISCARD );
	else
		glDisable( GL_RASTERIZER_DISCARD );
	if ( s->sampleAlpha )
		glEnable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	else
		glDisable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	if ( s->sampleCoverage )
		glEnable( GL_SAMPLE_COVERAGE );
	else
		glDisable( GL_SAMPLE_COVERAGE );
	if ( s->polygonOffset )
		glEnable( GL_POLYGON_OFFSET_FILL );
	else
		glDisable( GL_POLYGON_OFFSET_FILL );

	glColorMask( s->colorMask[0], s->colorMask[1], s->colorMask[2], s->colorMask[3] );
}

static void PrepareDraw( const FlareState *s, int x, int y )
{
	const GLenum buffer = s->drawFbo ? GL_COLOR_ATTACHMENT0 : GL_BACK;
	glDrawBuffers( 1, &buffer );
	glBindFramebuffer( GL_READ_FRAMEBUFFER, s->drawFbo );
	glBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );

	glEnable( GL_SCISSOR_TEST );
	glScissor( x, y, 1, 1 );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glDepthMask( GL_FALSE );
	glDisable( GL_BLEND );
	glDisable( GL_CULL_FACE );
	glDisable( GL_STENCIL_TEST );
	glDisable( GL_DITHER );
	glDisable( GL_RASTERIZER_DISCARD );
	glDisable( GL_SAMPLE_ALPHA_TO_COVERAGE );
	glDisable( GL_SAMPLE_COVERAGE );
	glDisable( GL_POLYGON_OFFSET_FILL );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );

	glUseProgram( flareProgram );
	glBindVertexArray( flareVao );
}

static bool ReadPixel( int x, int y, bool asFloat, unsigned char *bytes, float *floats )
{
	if ( asFloat )
		glReadPixels( x, y, 1, 1, GL_RGBA, GL_FLOAT, floats );
	else
		glReadPixels( x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bytes );
	return glGetError() == GL_NO_ERROR;
}

static void WritePixel( float ndcX, float ndcY, float ndcZ, float r, float g, float b, float a )
{
	glUniform4f( flareClip, ndcX, ndcY, ndcZ, 1.0f );
	glUniform4f( flareColor, r, g, b, a );
	glDrawArrays( GL_POINTS, 0, 1 );
}

extern "C" int WebGL_FlareVisible( int x, int y, float eyeZ, const float *projection )
{
	float windowZ = 1.0f;
	const int classified = Classify( eyeZ, projection, &windowZ );
	if ( classified >= 0 )
		return classified;
	if ( !EnsureProgram() )
		return 0;

	FlareState saved;
	SaveState( &saved );

	const float vw = (float)saved.viewport[2];
	const float vh = (float)saved.viewport[3];
	const bool inside = vw >= 1.0f && vh >= 1.0f
		&& x >= saved.viewport[0] && y >= saved.viewport[1]
		&& x < saved.viewport[0] + saved.viewport[2]
		&& y < saved.viewport[1] + saved.viewport[3];
	if ( !inside )
	{
		RestoreState( &saved );
		DrainError();
		return 0;
	}
	if ( !saved.drawFbo )
	{
		int fbWidth = 0;
		int fbHeight = 0;
		emscripten_webgl_get_drawing_buffer_size( flareContext, &fbWidth, &fbHeight );
		if ( x < 0 || y < 0 || x >= fbWidth || y >= fbHeight )
		{
			RestoreState( &saved );
			DrainError();
			return 0;
		}
	}

	PrepareDraw( &saved, x, y );

	unsigned char beforeBytes[4] = { 0, 0, 0, 0 };
	float beforeFloat[4] = { 0, 0, 0, 0 };
	if ( !ReadPixel( x, y, saved.asFloat, beforeBytes, beforeFloat ) )
	{
		RestoreState( &saved );
		DrainError();
		return 0;
	}

	const float ndcX = ( ( x + 0.5f - (float)saved.viewport[0] ) / vw ) * 2.0f - 1.0f;
	const float ndcY = ( ( y + 0.5f - (float)saved.viewport[1] ) / vh ) * 2.0f - 1.0f;
	const float ndcZ = windowZ * 2.0f - 1.0f;

	// A colour the pixel cannot already be, so "unchanged" means the depth
	// test rejected the point rather than the test colour matching by luck.
	float testR, testG, testB, testA;
	if ( saved.asFloat )
	{
		testR = beforeFloat[0] > 0.5f ? 0.0f : 1.0f;
		testG = 0.0f;
		testB = 1.0f;
		testA = 1.0f;
	}
	else
	{
		testR = ( beforeBytes[0] ^ 0xFF ) / 255.0f;
		testG = 0.0f;
		testB = 1.0f;
		testA = 1.0f;
	}
	WritePixel( ndcX, ndcY, ndcZ, testR, testG, testB, testA );

	unsigned char afterBytes[4] = { 0, 0, 0, 0 };
	float afterFloat[4] = { 0, 0, 0, 0 };
	const bool readBack = ReadPixel( x, y, saved.asFloat, afterBytes, afterFloat );
	bool passed = false;
	if ( readBack )
	{
		if ( saved.asFloat )
			passed = afterFloat[0] != beforeFloat[0];
		else
			passed = afterBytes[0] != beforeBytes[0];
	}

	glDisable( GL_DEPTH_TEST );
	if ( saved.asFloat )
		WritePixel( ndcX, ndcY, 0.0f, beforeFloat[0], beforeFloat[1], beforeFloat[2], beforeFloat[3] );
	else
		WritePixel( ndcX, ndcY, 0.0f,
			beforeBytes[0] / 255.0f, beforeBytes[1] / 255.0f,
			beforeBytes[2] / 255.0f, beforeBytes[3] / 255.0f );

	RestoreState( &saved );
	DrainError();
	return passed ? 1 : 0;
}
