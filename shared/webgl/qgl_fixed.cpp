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

// rd-vanilla's OpenGL 1.x, implemented on WebGL 2.
//
// The vanilla renderers are written against the fixed-function pipeline:
// matrix stacks, immediate mode (glBegin/glEnd), client-side vertex arrays,
// texture environments, alpha test, fog and a clip plane. WebGL 2 has none
// of these, so this file provides exactly the subset the renderers use:
//
// - one shader program does all of it, driven by uniforms (no per-state
//   variants, so nothing is compiled while playing), and uniforms are only
//   uploaded when they change;
// - client-side arrays and immediate mode vertices are streamed into
//   buffer objects for each draw;
// - quads and polygons become triangles.
//
// Everything that WebGL 2 does have (blending, depth, stencil, textures,
// ...) goes straight to WebGL.

#include <GLES3/gl3.h>
#include <emscripten/html5.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef double GLdouble;
typedef double GLclampd;

#include "qgl_fixed.h"

// OpenGL 1.x enums that the OpenGL ES headers don't have
#define GL_POINTS_				0x0000
#define GL_QUADS				0x0007
#define GL_QUAD_STRIP			0x0008
#define GL_POLYGON				0x0009
#define GL_MODELVIEW			0x1700
#define GL_PROJECTION			0x1701
#define GL_TEXTURE_MATRIX_MODE	0x1702 // GL_TEXTURE as a matrix mode
#define GL_ALPHA_TEST			0x0BC0
#define GL_FOG					0x0B60
#define GL_CLIP_PLANE0			0x3000
#define GL_TEXTURE_3D_			0x806F
#define GL_TEXTURE_RECTANGLE	0x84F5
#define GL_VERTEX_PROGRAM_ARB	0x8620
#define GL_FRAGMENT_PROGRAM_ARB	0x8804
#define GL_REGISTER_COMBINERS_NV	0x8522
#define GL_POLYGON_OFFSET_LINE	0x2A02
#define GL_LIGHTING				0x0B50
#define GL_VERTEX_ARRAY			0x8074
#define GL_NORMAL_ARRAY			0x8075
#define GL_COLOR_ARRAY			0x8076
#define GL_TEXTURE_COORD_ARRAY	0x8078
#define GL_TEXTURE_ENV			0x2300
#define GL_TEXTURE_ENV_MODE		0x2200
#define GL_MODULATE				0x2100
#define GL_DECAL				0x2101
#define GL_ADD					0x0104
#define GL_FOG_MODE				0x0B65
#define GL_FOG_DENSITY			0x0B62
#define GL_FOG_START			0x0B63
#define GL_FOG_END				0x0B64
#define GL_FOG_COLOR			0x0B66
#define GL_EXP					0x0800
#define GL_EXP2					0x0801
#define GL_CLAMP				0x2900
#define GL_MAX_TEXTURE_UNITS	0x84E2
#define GL_TEXTURE_MAX_ANISOTROPY	0x84FE
#define GL_DOUBLE_				0x140A

#define NUM_TEXTURE_UNITS	2
#define MAX_STACK_DEPTH		32

// attribute locations
enum {
	ATTR_POSITION,
	ATTR_COLOR,
	ATTR_TEXCOORD0,
	ATTR_TEXCOORD1,
	NUM_ATTRS
};

// fragment shader texture environment codes
enum {
	ENV_OFF,
	ENV_MODULATE,
	ENV_REPLACE,
	ENV_DECAL,
	ENV_ADD
};

// fragment shader alpha test codes
enum {
	ALPHA_ALWAYS,
	ALPHA_NEVER,
	ALPHA_LESS,
	ALPHA_EQUAL,
	ALPHA_LEQUAL,
	ALPHA_GREATER,
	ALPHA_NOTEQUAL,
	ALPHA_GEQUAL
};

// fragment shader fog codes
enum {
	FOG_OFF,
	FOG_LINEAR,
	FOG_EXP,
	FOG_EXP2
};

struct matrix_t {
	float m[16]; // column major, like OpenGL
};

struct clientArray_t {
	bool enabled;
	GLint size;
	GLenum type;
	GLsizei stride;
	const GLvoid *pointer;
};

struct immVertex_t {
	float position[4];
	float color[4];
	float texCoord[NUM_TEXTURE_UNITS][2];
};

struct streamBuffer_t {
	GLuint buffer;
	GLenum target;
	GLsizeiptr size;
	GLsizeiptr offset;
};

// uniform values, as uploaded
struct uniforms_t {
	matrix_t modelView;
	matrix_t projection;
	int texEnv[NUM_TEXTURE_UNITS];
	int alphaFunc;
	float alphaRef;
	int fogMode;
	float fogColor[4];
	float fogParams[3];
	int clip;
	float clipPlane[4];
};

static struct {
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context; // what the GL objects belong to

	GLuint program;
	GLuint vao;
	streamBuffer_t vertexStream;
	streamBuffer_t indexStream;
	struct {
		GLint modelView, projection, texEnv[NUM_TEXTURE_UNITS], alphaFunc, alphaRef, fogMode,
			fogColor, fogParams, clip, clipPlane;
	} location;
	uniforms_t uploaded;
	bool uploadedValid;

	GLenum matrixMode;
	matrix_t modelView[MAX_STACK_DEPTH];
	matrix_t projection[MAX_STACK_DEPTH];
	matrix_t textureMatrix; // accepted but unused; the renderers don't set it
	int modelViewDepth;
	int projectionDepth;

	float color[4];
	float texCoord[NUM_TEXTURE_UNITS][2];

	int activeTexture;
	int clientActiveTexture;
	bool texture2D[NUM_TEXTURE_UNITS];
	GLenum texEnv[NUM_TEXTURE_UNITS];

	bool alphaTest;
	GLenum alphaFunc;
	float alphaRef;

	bool fog;
	GLenum fogMode;
	float fogColor[4];
	float fogDensity;
	float fogStart;
	float fogEnd;

	bool clipPlane;
	float clipPlaneEye[4];

	clientArray_t vertexArray;
	clientArray_t colorArray;
	clientArray_t texCoordArray[NUM_TEXTURE_UNITS];

	bool inBegin;
	GLenum beginMode;
	immVertex_t *immVertices;
	int numImmVertices;
	int maxImmVertices;

	uint32_t *scratchIndices;
	int maxScratchIndices;
	uint8_t *scratchPixels;
	size_t scratchPixelsSize;
} glf = {
	0, // context
};

static bool glfStateInitialised;

//=============================================================================
// Shaders

static const char *vertexShaderSource =
	"#version 300 es\n"
	"layout(location = 0) in vec4 a_position;\n"
	"layout(location = 1) in vec4 a_color;\n"
	"layout(location = 2) in vec2 a_texCoord0;\n"
	"layout(location = 3) in vec2 a_texCoord1;\n"
	"uniform mat4 u_modelView;\n"
	"uniform mat4 u_projection;\n"
	"uniform vec4 u_clipPlane;\n"
	"out vec4 v_color;\n"
	"out vec2 v_texCoord0;\n"
	"out vec2 v_texCoord1;\n"
	"out float v_eyeDepth;\n"
	"out float v_clipDistance;\n"
	"void main() {\n"
	"	vec4 eye = u_modelView * a_position;\n"
	"	gl_Position = u_projection * eye;\n"
	"	gl_PointSize = 1.0;\n"
	"	v_color = a_color;\n"
	"	v_texCoord0 = a_texCoord0;\n"
	"	v_texCoord1 = a_texCoord1;\n"
	"	v_eyeDepth = abs(eye.z);\n"
	"	v_clipDistance = dot(eye, u_clipPlane);\n"
	"}\n";

static const char *fragmentShaderSource =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D u_texture0;\n"
	"uniform sampler2D u_texture1;\n"
	"uniform int u_texEnv0;\n"
	"uniform int u_texEnv1;\n"
	"uniform int u_alphaFunc;\n"
	"uniform float u_alphaRef;\n"
	"uniform int u_fogMode;\n"
	"uniform vec4 u_fogColor;\n"
	"uniform vec3 u_fogParams; // density, start, end\n"
	"uniform bool u_clip;\n"
	"in vec4 v_color;\n"
	"in vec2 v_texCoord0;\n"
	"in vec2 v_texCoord1;\n"
	"in float v_eyeDepth;\n"
	"in float v_clipDistance;\n"
	"out vec4 out_Color;\n"
	"vec4 applyTexEnv(int env, vec4 c, vec4 t) {\n"
	"	if (env == 1) return c * t;\n"                                        // GL_MODULATE
	"	if (env == 2) return t;\n"                                            // GL_REPLACE
	"	if (env == 3) return vec4(mix(c.rgb, t.rgb, t.a), c.a);\n"            // GL_DECAL
	"	return vec4(c.rgb + t.rgb, c.a * t.a);\n"                             // GL_ADD
	"}\n"
	"bool alphaTest(float a) {\n"
	"	if (u_alphaFunc == 1) return false;\n"
	"	if (u_alphaFunc == 2) return a < u_alphaRef;\n"
	"	if (u_alphaFunc == 3) return a == u_alphaRef;\n"
	"	if (u_alphaFunc == 4) return a <= u_alphaRef;\n"
	"	if (u_alphaFunc == 5) return a > u_alphaRef;\n"
	"	if (u_alphaFunc == 6) return a != u_alphaRef;\n"
	"	if (u_alphaFunc == 7) return a >= u_alphaRef;\n"
	"	return true;\n"
	"}\n"
	"void main() {\n"
	"	if (u_clip && v_clipDistance < 0.0) discard;\n"
	"	vec4 c = v_color;\n"
	"	if (u_texEnv0 != 0) c = applyTexEnv(u_texEnv0, c, texture(u_texture0, v_texCoord0));\n"
	"	if (u_texEnv1 != 0) c = applyTexEnv(u_texEnv1, c, texture(u_texture1, v_texCoord1));\n"
	"	if (!alphaTest(c.a)) discard;\n"
	"	if (u_fogMode != 0) {\n"
	"		float f;\n"
	"		if (u_fogMode == 1) f = (u_fogParams.z - v_eyeDepth) / (u_fogParams.z - u_fogParams.y);\n"
	"		else if (u_fogMode == 2) f = exp(-u_fogParams.x * v_eyeDepth);\n"
	"		else { float d = u_fogParams.x * v_eyeDepth; f = exp(-d * d); }\n"
	"		c.rgb = mix(u_fogColor.rgb, c.rgb, clamp(f, 0.0, 1.0));\n"
	"	}\n"
	"	out_Color = c;\n"
	"}\n";

static GLuint CompileShader( GLenum type, const char *source )
{
	GLuint shader = glCreateShader( type );
	glShaderSource( shader, 1, &source, NULL );
	glCompileShader( shader );

	GLint compiled = GL_FALSE;
	glGetShaderiv( shader, GL_COMPILE_STATUS, &compiled );
	if ( !compiled )
	{
		char log[2048];
		glGetShaderInfoLog( shader, sizeof( log ), NULL, log );
		printf( "qgl_fixed: shader compile error:\n%s\n", log );
	}
	return shader;
}

//=============================================================================
// Matrices

static void Matrix_Identity( matrix_t *out )
{
	memset( out, 0, sizeof( *out ) );
	out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
}

// out = a * b
static void Matrix_Multiply( const matrix_t *a, const matrix_t *b, matrix_t *out )
{
	matrix_t result;
	for ( int column = 0; column < 4; column++ )
	{
		for ( int row = 0; row < 4; row++ )
		{
			float sum = 0.0f;
			for ( int k = 0; k < 4; k++ )
				sum += a->m[k * 4 + row] * b->m[column * 4 + k];
			result.m[column * 4 + row] = sum;
		}
	}
	*out = result;
}

static bool Matrix_Invert( const matrix_t *in, matrix_t *out )
{
	const float *m = in->m;
	float inv[16];

	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

	float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	if ( det == 0.0f )
		return false;

	det = 1.0f / det;
	for ( int i = 0; i < 16; i++ )
		out->m[i] = inv[i] * det;
	return true;
}

static matrix_t *CurrentMatrix( void )
{
	switch ( glf.matrixMode )
	{
	case GL_PROJECTION:
		return &glf.projection[glf.projectionDepth];
	case GL_TEXTURE_MATRIX_MODE:
		return &glf.textureMatrix;
	default:
		return &glf.modelView[glf.modelViewDepth];
	}
}

static void MultiplyCurrent( const matrix_t *m )
{
	matrix_t *current = CurrentMatrix();
	Matrix_Multiply( current, m, current );
}

//=============================================================================
// Set up

static void InitState( void )
{
	glf.matrixMode = GL_MODELVIEW;
	glf.modelViewDepth = 0;
	glf.projectionDepth = 0;
	Matrix_Identity( &glf.modelView[0] );
	Matrix_Identity( &glf.projection[0] );
	Matrix_Identity( &glf.textureMatrix );

	for ( int i = 0; i < 4; i++ )
		glf.color[i] = 1.0f;
	for ( int unit = 0; unit < NUM_TEXTURE_UNITS; unit++ )
	{
		glf.texEnv[unit] = GL_MODULATE;
		glf.texture2D[unit] = false;
	}

	glf.alphaFunc = GL_ALWAYS;
	glf.fogMode = GL_EXP;
	glf.fogDensity = 1.0f;
	glf.fogStart = 0.0f;
	glf.fogEnd = 1.0f;

	glfStateInitialised = true;
}

// Creates the GL objects for the current context (again, after vid_restart).
static void EnsureContext( void )
{
	const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_get_current_context();
	if ( glf.context == context && glf.program )
		return;

	if ( !glfStateInitialised )
		InitState();

	glf.context = context;

	GLuint vertexShader = CompileShader( GL_VERTEX_SHADER, vertexShaderSource );
	GLuint fragmentShader = CompileShader( GL_FRAGMENT_SHADER, fragmentShaderSource );
	glf.program = glCreateProgram();
	glAttachShader( glf.program, vertexShader );
	glAttachShader( glf.program, fragmentShader );
	glLinkProgram( glf.program );
	glDeleteShader( vertexShader );
	glDeleteShader( fragmentShader );

	GLint linked = GL_FALSE;
	glGetProgramiv( glf.program, GL_LINK_STATUS, &linked );
	if ( !linked )
	{
		char log[2048];
		glGetProgramInfoLog( glf.program, sizeof( log ), NULL, log );
		printf( "qgl_fixed: program link error:\n%s\n", log );
	}

	glUseProgram( glf.program );
	glUniform1i( glGetUniformLocation( glf.program, "u_texture0" ), 0 );
	glUniform1i( glGetUniformLocation( glf.program, "u_texture1" ), 1 );
	glf.location.modelView = glGetUniformLocation( glf.program, "u_modelView" );
	glf.location.projection = glGetUniformLocation( glf.program, "u_projection" );
	glf.location.texEnv[0] = glGetUniformLocation( glf.program, "u_texEnv0" );
	glf.location.texEnv[1] = glGetUniformLocation( glf.program, "u_texEnv1" );
	glf.location.alphaFunc = glGetUniformLocation( glf.program, "u_alphaFunc" );
	glf.location.alphaRef = glGetUniformLocation( glf.program, "u_alphaRef" );
	glf.location.fogMode = glGetUniformLocation( glf.program, "u_fogMode" );
	glf.location.fogColor = glGetUniformLocation( glf.program, "u_fogColor" );
	glf.location.fogParams = glGetUniformLocation( glf.program, "u_fogParams" );
	glf.location.clip = glGetUniformLocation( glf.program, "u_clip" );
	glf.location.clipPlane = glGetUniformLocation( glf.program, "u_clipPlane" );
	glf.uploadedValid = false;

	glGenVertexArrays( 1, &glf.vao );
	glBindVertexArray( glf.vao );

	glf.vertexStream.target = GL_ARRAY_BUFFER;
	glf.vertexStream.size = 8 * 1024 * 1024;
	glf.indexStream.target = GL_ELEMENT_ARRAY_BUFFER;
	glf.indexStream.size = 2 * 1024 * 1024;
	streamBuffer_t *streams[] = { &glf.vertexStream, &glf.indexStream };
	for ( streamBuffer_t *stream : streams )
	{
		glGenBuffers( 1, &stream->buffer );
		glBindBuffer( stream->target, stream->buffer );
		glBufferData( stream->target, stream->size, NULL, GL_STREAM_DRAW );
		stream->offset = 0;
	}

	// the renderer's texture unit
	glActiveTexture( GL_TEXTURE0 + glf.activeTexture );
}

// Copies data into the stream buffer and returns its offset there.
static GLintptr StreamUpload( streamBuffer_t *stream, const void *data, GLsizeiptr length )
{
	const GLsizeiptr aligned = ( length + 15 ) & ~15;

	if ( aligned > stream->size )
	{
		// grow
		while ( stream->size < aligned )
			stream->size *= 2;
		glBufferData( stream->target, stream->size, NULL, GL_STREAM_DRAW );
		stream->offset = 0;
	}
	else if ( stream->offset + aligned > stream->size )
	{
		// start over in a fresh buffer instead of waiting for the old one
		glBufferData( stream->target, stream->size, NULL, GL_STREAM_DRAW );
		stream->offset = 0;
	}

	const GLintptr offset = stream->offset;
	glBufferSubData( stream->target, offset, length, data );
	stream->offset += aligned;
	return offset;
}

static int TypeSize( GLenum type )
{
	switch ( type )
	{
	case GL_BYTE:
	case GL_UNSIGNED_BYTE:
		return 1;
	case GL_SHORT:
	case GL_UNSIGNED_SHORT:
		return 2;
	case GL_DOUBLE_:
		return 8;
	default:
		return 4;
	}
}

static int TexEnvCode( int unit )
{
	if ( !glf.texture2D[unit] )
		return ENV_OFF;

	switch ( glf.texEnv[unit] )
	{
	case GL_REPLACE:
		return ENV_REPLACE;
	case GL_DECAL:
		return ENV_DECAL;
	case GL_ADD:
		return ENV_ADD;
	default:
		return ENV_MODULATE;
	}
}

static int AlphaFuncCode( void )
{
	if ( !glf.alphaTest )
		return ALPHA_ALWAYS;

	switch ( glf.alphaFunc )
	{
	case GL_NEVER: return ALPHA_NEVER;
	case GL_LESS: return ALPHA_LESS;
	case GL_EQUAL: return ALPHA_EQUAL;
	case GL_LEQUAL: return ALPHA_LEQUAL;
	case GL_GREATER: return ALPHA_GREATER;
	case GL_NOTEQUAL: return ALPHA_NOTEQUAL;
	case GL_GEQUAL: return ALPHA_GEQUAL;
	default: return ALPHA_ALWAYS;
	}
}

static int FogModeCode( void )
{
	if ( !glf.fog )
		return FOG_OFF;

	switch ( glf.fogMode )
	{
	case GL_LINEAR: return FOG_LINEAR;
	case GL_EXP2: return FOG_EXP2;
	default: return FOG_EXP;
	}
}

// Brings the program's uniforms up to date, uploading only what changed.
static void UpdateUniforms( void )
{
	uniforms_t now;
	memset( &now, 0, sizeof( now ) );
	now.modelView = glf.modelView[glf.modelViewDepth];
	now.projection = glf.projection[glf.projectionDepth];
	for ( int unit = 0; unit < NUM_TEXTURE_UNITS; unit++ )
		now.texEnv[unit] = TexEnvCode( unit );
	now.alphaFunc = AlphaFuncCode();
	now.alphaRef = glf.alphaRef;
	now.fogMode = FogModeCode();
	memcpy( now.fogColor, glf.fogColor, sizeof( now.fogColor ) );
	now.fogParams[0] = glf.fogDensity;
	now.fogParams[1] = glf.fogStart;
	now.fogParams[2] = glf.fogEnd;
	now.clip = glf.clipPlane;
	memcpy( now.clipPlane, glf.clipPlaneEye, sizeof( now.clipPlane ) );

	uniforms_t& was = glf.uploaded;
	const bool all = !glf.uploadedValid;

#define CHANGED( field ) ( all || memcmp( &now.field, &was.field, sizeof( now.field ) ) != 0 )
	if ( CHANGED( modelView ) )
		glUniformMatrix4fv( glf.location.modelView, 1, GL_FALSE, now.modelView.m );
	if ( CHANGED( projection ) )
		glUniformMatrix4fv( glf.location.projection, 1, GL_FALSE, now.projection.m );
	for ( int unit = 0; unit < NUM_TEXTURE_UNITS; unit++ )
	{
		if ( CHANGED( texEnv[unit] ) )
			glUniform1i( glf.location.texEnv[unit], now.texEnv[unit] );
	}
	if ( CHANGED( alphaFunc ) )
		glUniform1i( glf.location.alphaFunc, now.alphaFunc );
	if ( CHANGED( alphaRef ) )
		glUniform1f( glf.location.alphaRef, now.alphaRef );
	if ( CHANGED( fogMode ) )
		glUniform1i( glf.location.fogMode, now.fogMode );
	if ( now.fogMode != FOG_OFF )
	{
		if ( CHANGED( fogColor ) )
			glUniform4fv( glf.location.fogColor, 1, now.fogColor );
		if ( CHANGED( fogParams ) )
			glUniform3fv( glf.location.fogParams, 1, now.fogParams );
	}
	else
	{
		// not uploaded; compare against what is
		memcpy( now.fogColor, was.fogColor, sizeof( now.fogColor ) );
		memcpy( now.fogParams, was.fogParams, sizeof( now.fogParams ) );
	}
	if ( CHANGED( clip ) )
		glUniform1i( glf.location.clip, now.clip );
	if ( now.clip && CHANGED( clipPlane ) )
		glUniform4fv( glf.location.clipPlane, 1, now.clipPlane );
	else if ( !now.clip )
		memcpy( now.clipPlane, was.clipPlane, sizeof( now.clipPlane ) );
#undef CHANGED

	glf.uploaded = now;
	glf.uploadedValid = true;
}

// Prepares for a draw: program, uniforms, and the stream buffers bound.
static void BeginDraw( void )
{
	EnsureContext();
	glUseProgram( glf.program );
	glBindVertexArray( glf.vao );
	glBindBuffer( GL_ARRAY_BUFFER, glf.vertexStream.buffer );
	glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, glf.indexStream.buffer );
	UpdateUniforms();
}

// Uploads the enabled client arrays for vertices [0, numVertices) and points
// the attributes at them; disabled arrays become constant attributes.
static void SetupClientArrays( int numVertices )
{
	const clientArray_t *arrays[NUM_ATTRS] = {
		&glf.vertexArray, &glf.colorArray, &glf.texCoordArray[0], &glf.texCoordArray[1]
	};

	for ( int attr = 0; attr < NUM_ATTRS; attr++ )
	{
		const clientArray_t *array = arrays[attr];
		if ( !array->enabled || !array->pointer || numVertices <= 0 )
		{
			glDisableVertexAttribArray( attr );
			switch ( attr )
			{
			case ATTR_COLOR:
				glVertexAttrib4fv( attr, glf.color );
				break;
			case ATTR_TEXCOORD0:
			case ATTR_TEXCOORD1:
			{
				const float *tc = glf.texCoord[attr - ATTR_TEXCOORD0];
				glVertexAttrib4f( attr, tc[0], tc[1], 0.0f, 1.0f );
				break;
			}
			default:
				glVertexAttrib4f( attr, 0.0f, 0.0f, 0.0f, 1.0f );
				break;
			}
			continue;
		}

		const int elementSize = array->size * TypeSize( array->type );
		const int stride = array->stride ? array->stride : elementSize;
		const GLsizeiptr length = (GLsizeiptr)( numVertices - 1 ) * stride + elementSize;
		const GLintptr offset = StreamUpload( &glf.vertexStream, array->pointer, length );

		glEnableVertexAttribArray( attr );
		glVertexAttribPointer( attr, array->size, array->type,
			attr == ATTR_COLOR && array->type == GL_UNSIGNED_BYTE ? GL_TRUE : GL_FALSE,
			stride, (const void *)offset );
	}
}

static uint32_t *ScratchIndices( int count )
{
	if ( count > glf.maxScratchIndices )
	{
		glf.maxScratchIndices = count * 2;
		glf.scratchIndices = (uint32_t *)realloc( glf.scratchIndices, glf.maxScratchIndices * sizeof( uint32_t ) );
	}
	return glf.scratchIndices;
}

// Indices that draw quads (or a quad strip) as triangles
static int QuadIndices( GLenum mode, int first, int count, uint32_t *indices )
{
	int numIndices = 0;
	if ( mode == GL_QUADS )
	{
		for ( int quad = 0; quad + 3 < count; quad += 4 )
		{
			const uint32_t v = first + quad;
			const uint32_t triangles[6] = { v, v + 1, v + 2, v, v + 2, v + 3 };
			memcpy( indices + numIndices, triangles, sizeof( triangles ) );
			numIndices += 6;
		}
	}
	else // GL_QUAD_STRIP
	{
		for ( int i = 0; i + 3 < count; i += 2 )
		{
			const uint32_t v = first + i;
			const uint32_t triangles[6] = { v, v + 1, v + 3, v, v + 3, v + 2 };
			memcpy( indices + numIndices, triangles, sizeof( triangles ) );
			numIndices += 6;
		}
	}
	return numIndices;
}

//=============================================================================
// Extensions

int GLF_ExtensionSupported( const char *extension )
{
	static const char *implemented[] = {
		"GL_ARB_multitexture",
		"GL_EXT_texture_env_add",
		"GL_EXT_texture_edge_clamp",
		"GL_SGIS_texture_edge_clamp",
	};

	for ( const char *name : implemented )
	{
		if ( !strcmp( extension, name ) )
			return 1;
	}

	if ( !strcmp( extension, "GL_EXT_texture_filter_anisotropic" ) )
	{
		return emscripten_webgl_enable_extension( emscripten_webgl_get_current_context(),
			"EXT_texture_filter_anisotropic" ) ? 1 : 0;
	}

	return 0;
}

void *GLF_GetProcAddress( const char *name )
{
	static const struct {
		const char *name;
		void *function;
	} functions[] = {
		{ "glActiveTextureARB", (void *)glfActiveTexture },
		{ "glClientActiveTextureARB", (void *)glfClientActiveTexture },
		{ "glMultiTexCoord2fARB", (void *)glfMultiTexCoord2f },
		{ "glStencilOpSeparate", (void *)glStencilOpSeparate },
	};

	for ( const auto& function : functions )
	{
		if ( !strcmp( name, function.name ) )
			return function.function;
	}

	return NULL;
}

//=============================================================================
// Immediate mode

static void EmitVertex( float x, float y, float z )
{
	if ( !glf.inBegin )
		return;

	if ( glf.numImmVertices == glf.maxImmVertices )
	{
		glf.maxImmVertices = glf.maxImmVertices ? glf.maxImmVertices * 2 : 1024;
		glf.immVertices = (immVertex_t *)realloc( glf.immVertices, glf.maxImmVertices * sizeof( immVertex_t ) );
	}

	immVertex_t *v = &glf.immVertices[glf.numImmVertices++];
	v->position[0] = x;
	v->position[1] = y;
	v->position[2] = z;
	v->position[3] = 1.0f;
	memcpy( v->color, glf.color, sizeof( v->color ) );
	memcpy( v->texCoord, glf.texCoord, sizeof( v->texCoord ) );
}

void glfBegin( GLenum mode )
{
	if ( !glfStateInitialised )
		InitState();
	glf.inBegin = true;
	glf.beginMode = mode;
	glf.numImmVertices = 0;
}

void glfEnd( void )
{
	if ( !glf.inBegin )
		return;
	glf.inBegin = false;

	const int count = glf.numImmVertices;
	if ( !count )
		return;

	BeginDraw();

	const GLintptr offset = StreamUpload( &glf.vertexStream, glf.immVertices, count * sizeof( immVertex_t ) );
	const GLsizei stride = sizeof( immVertex_t );
	glEnableVertexAttribArray( ATTR_POSITION );
	glVertexAttribPointer( ATTR_POSITION, 4, GL_FLOAT, GL_FALSE, stride, (const void *)( offset + offsetof( immVertex_t, position ) ) );
	glEnableVertexAttribArray( ATTR_COLOR );
	glVertexAttribPointer( ATTR_COLOR, 4, GL_FLOAT, GL_FALSE, stride, (const void *)( offset + offsetof( immVertex_t, color ) ) );
	glEnableVertexAttribArray( ATTR_TEXCOORD0 );
	glVertexAttribPointer( ATTR_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( offset + offsetof( immVertex_t, texCoord[0] ) ) );
	glEnableVertexAttribArray( ATTR_TEXCOORD1 );
	glVertexAttribPointer( ATTR_TEXCOORD1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)( offset + offsetof( immVertex_t, texCoord[1] ) ) );

	switch ( glf.beginMode )
	{
	case GL_QUADS:
	case GL_QUAD_STRIP:
	{
		uint32_t *indices = ScratchIndices( count * 3 );
		const int numIndices = QuadIndices( glf.beginMode, 0, count, indices );
		if ( numIndices )
		{
			const GLintptr indexOffset = StreamUpload( &glf.indexStream, indices, numIndices * sizeof( uint32_t ) );
			glDrawElements( GL_TRIANGLES, numIndices, GL_UNSIGNED_INT, (const void *)indexOffset );
		}
		break;
	}
	case GL_POLYGON:
		glDrawArrays( GL_TRIANGLE_FAN, 0, count );
		break;
	default:
		glDrawArrays( glf.beginMode, 0, count );
		break;
	}
}

void glfVertex2f( GLfloat x, GLfloat y ) { EmitVertex( x, y, 0.0f ); }
void glfVertex3f( GLfloat x, GLfloat y, GLfloat z ) { EmitVertex( x, y, z ); }
void glfVertex3fv( const GLfloat *v ) { EmitVertex( v[0], v[1], v[2] ); }

void glfTexCoord2f( GLfloat s, GLfloat t )
{
	glf.texCoord[0][0] = s;
	glf.texCoord[0][1] = t;
}

void glfTexCoord2fv( const GLfloat *v ) { glfTexCoord2f( v[0], v[1] ); }

void glfMultiTexCoord2f( GLenum target, GLfloat s, GLfloat t )
{
	const int unit = target - GL_TEXTURE0;
	if ( unit < 0 || unit >= NUM_TEXTURE_UNITS )
		return;
	glf.texCoord[unit][0] = s;
	glf.texCoord[unit][1] = t;
}

void glfColor4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a )
{
	glf.color[0] = r;
	glf.color[1] = g;
	glf.color[2] = b;
	glf.color[3] = a;
}

void glfColor3f( GLfloat r, GLfloat g, GLfloat b ) { glfColor4f( r, g, b, 1.0f ); }

void glfColor4ub( GLubyte r, GLubyte g, GLubyte b, GLubyte a )
{
	glfColor4f( r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f );
}

void glfColor4ubv( const GLubyte *v ) { glfColor4ub( v[0], v[1], v[2], v[3] ); }

//=============================================================================
// Matrices

void glfMatrixMode( GLenum mode )
{
	if ( !glfStateInitialised )
		InitState();
	glf.matrixMode = mode;
}

void glfLoadIdentity( void )
{
	if ( !glfStateInitialised )
		InitState();
	Matrix_Identity( CurrentMatrix() );
}

void glfLoadMatrixf( const GLfloat *m )
{
	if ( !glfStateInitialised )
		InitState();
	memcpy( CurrentMatrix()->m, m, sizeof( float ) * 16 );
}

void glfPushMatrix( void )
{
	if ( glf.matrixMode == GL_PROJECTION )
	{
		if ( glf.projectionDepth + 1 < MAX_STACK_DEPTH )
		{
			glf.projection[glf.projectionDepth + 1] = glf.projection[glf.projectionDepth];
			glf.projectionDepth++;
		}
	}
	else if ( glf.matrixMode == GL_MODELVIEW )
	{
		if ( glf.modelViewDepth + 1 < MAX_STACK_DEPTH )
		{
			glf.modelView[glf.modelViewDepth + 1] = glf.modelView[glf.modelViewDepth];
			glf.modelViewDepth++;
		}
	}
}

void glfPopMatrix( void )
{
	if ( glf.matrixMode == GL_PROJECTION )
	{
		if ( glf.projectionDepth > 0 )
			glf.projectionDepth--;
	}
	else if ( glf.matrixMode == GL_MODELVIEW )
	{
		if ( glf.modelViewDepth > 0 )
			glf.modelViewDepth--;
	}
}

void glfOrtho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar )
{
	matrix_t m;
	Matrix_Identity( &m );
	m.m[0] = (float)( 2.0 / ( right - left ) );
	m.m[5] = (float)( 2.0 / ( top - bottom ) );
	m.m[10] = (float)( -2.0 / ( zFar - zNear ) );
	m.m[12] = (float)( -( right + left ) / ( right - left ) );
	m.m[13] = (float)( -( top + bottom ) / ( top - bottom ) );
	m.m[14] = (float)( -( zFar + zNear ) / ( zFar - zNear ) );
	MultiplyCurrent( &m );
}

void glfTranslatef( GLfloat x, GLfloat y, GLfloat z )
{
	matrix_t m;
	Matrix_Identity( &m );
	m.m[12] = x;
	m.m[13] = y;
	m.m[14] = z;
	MultiplyCurrent( &m );
}

void glfRotatef( GLfloat angle, GLfloat x, GLfloat y, GLfloat z )
{
	const float length = sqrtf( x * x + y * y + z * z );
	if ( length == 0.0f )
		return;
	x /= length;
	y /= length;
	z /= length;

	const float radians = angle * (float)M_PI / 180.0f;
	const float c = cosf( radians );
	const float s = sinf( radians );
	const float t = 1.0f - c;

	matrix_t m;
	Matrix_Identity( &m );
	m.m[0] = x * x * t + c;
	m.m[1] = y * x * t + z * s;
	m.m[2] = x * z * t - y * s;
	m.m[4] = x * y * t - z * s;
	m.m[5] = y * y * t + c;
	m.m[6] = y * z * t + x * s;
	m.m[8] = x * z * t + y * s;
	m.m[9] = y * z * t - x * s;
	m.m[10] = z * z * t + c;
	MultiplyCurrent( &m );
}

//=============================================================================
// State

// Fixed-function state that is ours, not WebGL's. Returns false for
// capabilities that go to WebGL.
static bool SetCapability( GLenum cap, bool enable )
{
	switch ( cap )
	{
	case GL_TEXTURE_2D:
		glf.texture2D[glf.activeTexture] = enable;
		return true;
	case GL_ALPHA_TEST:
		glf.alphaTest = enable;
		return true;
	case GL_FOG:
		glf.fog = enable;
		return true;
	case GL_CLIP_PLANE0:
		glf.clipPlane = enable;
		return true;
	case GL_TEXTURE_3D_:
	case GL_TEXTURE_RECTANGLE:
	case GL_VERTEX_PROGRAM_ARB:
	case GL_FRAGMENT_PROGRAM_ARB:
	case GL_REGISTER_COMBINERS_NV:
	case GL_POLYGON_OFFSET_LINE:
	case GL_LIGHTING:
		// not available on WebGL, and the renderer doesn't rely on them there
		return true;
	default:
		return false;
	}
}

void glfEnable( GLenum cap )
{
	if ( !glfStateInitialised )
		InitState();
	if ( !SetCapability( cap, true ) )
		glEnable( cap );
}

void glfDisable( GLenum cap )
{
	if ( !glfStateInitialised )
		InitState();
	if ( !SetCapability( cap, false ) )
		glDisable( cap );
}

GLboolean glfIsEnabled( GLenum cap )
{
	switch ( cap )
	{
	case GL_TEXTURE_2D:
		return glf.texture2D[glf.activeTexture];
	case GL_ALPHA_TEST:
		return glf.alphaTest;
	case GL_FOG:
		return glf.fog;
	case GL_CLIP_PLANE0:
		return glf.clipPlane;
	default:
		return glIsEnabled( cap );
	}
}

void glfActiveTexture( GLenum texture )
{
	const int unit = texture - GL_TEXTURE0;
	if ( unit < 0 || unit >= NUM_TEXTURE_UNITS )
		return;
	glf.activeTexture = unit;
	glActiveTexture( texture );
}

void glfClientActiveTexture( GLenum texture )
{
	const int unit = texture - GL_TEXTURE0;
	if ( unit < 0 || unit >= NUM_TEXTURE_UNITS )
		return;
	glf.clientActiveTexture = unit;
}

static clientArray_t *ClientArray( GLenum array )
{
	switch ( array )
	{
	case GL_VERTEX_ARRAY:
		return &glf.vertexArray;
	case GL_COLOR_ARRAY:
		return &glf.colorArray;
	case GL_TEXTURE_COORD_ARRAY:
		return &glf.texCoordArray[glf.clientActiveTexture];
	default:
		return NULL;
	}
}

void glfEnableClientState( GLenum array )
{
	clientArray_t *a = ClientArray( array );
	if ( a )
		a->enabled = true;
}

void glfDisableClientState( GLenum array )
{
	clientArray_t *a = ClientArray( array );
	if ( a )
		a->enabled = false;
}

static void SetClientArray( clientArray_t *array, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	array->size = size;
	array->type = type;
	array->stride = stride;
	array->pointer = pointer;
}

void glfVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	SetClientArray( &glf.vertexArray, size, type, stride, pointer );
}

void glfColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	SetClientArray( &glf.colorArray, size, type, stride, pointer );
}

void glfTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	SetClientArray( &glf.texCoordArray[glf.clientActiveTexture], size, type, stride, pointer );
}

void glfDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices )
{
	if ( count <= 0 )
		return;

	uint32_t maxIndex = 0;
	if ( type == GL_UNSIGNED_SHORT )
	{
		const uint16_t *i16 = (const uint16_t *)indices;
		for ( GLsizei i = 0; i < count; i++ )
			maxIndex = i16[i] > maxIndex ? i16[i] : maxIndex;
	}
	else if ( type == GL_UNSIGNED_BYTE )
	{
		const uint8_t *i8 = (const uint8_t *)indices;
		for ( GLsizei i = 0; i < count; i++ )
			maxIndex = i8[i] > maxIndex ? i8[i] : maxIndex;
	}
	else
	{
		const uint32_t *i32 = (const uint32_t *)indices;
		for ( GLsizei i = 0; i < count; i++ )
			maxIndex = i32[i] > maxIndex ? i32[i] : maxIndex;
	}

	BeginDraw();
	SetupClientArrays( maxIndex + 1 );
	const GLintptr offset = StreamUpload( &glf.indexStream, indices, count * TypeSize( type ) );
	glDrawElements( mode, count, type, (const void *)offset );
}

void glfDrawArrays( GLenum mode, GLint first, GLsizei count )
{
	if ( count <= 0 )
		return;

	BeginDraw();
	SetupClientArrays( first + count );

	switch ( mode )
	{
	case GL_QUADS:
	case GL_QUAD_STRIP:
	{
		uint32_t *indices = ScratchIndices( count * 3 );
		const int numIndices = QuadIndices( mode, first, count, indices );
		if ( numIndices )
		{
			const GLintptr offset = StreamUpload( &glf.indexStream, indices, numIndices * sizeof( uint32_t ) );
			glDrawElements( GL_TRIANGLES, numIndices, GL_UNSIGNED_INT, (const void *)offset );
		}
		break;
	}
	case GL_POLYGON:
		glDrawArrays( GL_TRIANGLE_FAN, first, count );
		break;
	default:
		glDrawArrays( mode, first, count );
		break;
	}
}

void glfArrayElement( GLint i )
{
	// only used with r_primitives 1, which isn't used on web
}

void glfTexEnvf( GLenum target, GLenum pname, GLfloat param ) { glfTexEnvi( target, pname, (GLint)param ); }

void glfTexEnvi( GLenum target, GLenum pname, GLint param )
{
	if ( target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE )
		glf.texEnv[glf.activeTexture] = param;
}

void glfAlphaFunc( GLenum func, GLclampf ref )
{
	glf.alphaFunc = func;
	glf.alphaRef = ref;
}

void glfFogf( GLenum pname, GLfloat param )
{
	switch ( pname )
	{
	case GL_FOG_MODE: glf.fogMode = (GLenum)param; break;
	case GL_FOG_DENSITY: glf.fogDensity = param; break;
	case GL_FOG_START: glf.fogStart = param; break;
	case GL_FOG_END: glf.fogEnd = param; break;
	default: break;
	}
}

void glfFogi( GLenum pname, GLint param )
{
	if ( pname == GL_FOG_MODE )
		glf.fogMode = param;
	else
		glfFogf( pname, (GLfloat)param );
}

void glfFogfv( GLenum pname, const GLfloat *params )
{
	if ( pname == GL_FOG_COLOR )
		memcpy( glf.fogColor, params, sizeof( glf.fogColor ) );
	else
		glfFogf( pname, params[0] );
}

void glfClipPlane( GLenum plane, const GLdouble *equation )
{
	if ( plane != GL_CLIP_PLANE0 )
		return;

	// the plane is stored in eye space: equation * inverse(modelview)
	matrix_t inverse;
	if ( !Matrix_Invert( &glf.modelView[glf.modelViewDepth], &inverse ) )
		Matrix_Identity( &inverse );

	for ( int column = 0; column < 4; column++ )
	{
		double sum = 0.0;
		for ( int row = 0; row < 4; row++ )
			sum += equation[row] * inverse.m[column * 4 + row];
		glf.clipPlaneEye[column] = (float)sum;
	}
}

//=============================================================================
// Textures and the rest

static bool IsWebGLTextureTarget( GLenum target )
{
	return target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP
		|| ( target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z );
}

void glfTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
	GLint border, GLenum format, GLenum type, const GLvoid *pixels )
{
	if ( !IsWebGLTextureTarget( target ) )
		return;

	// the renderers upload RGBA bytes with sized or compressed internal
	// formats; let WebGL store what it's given
	glTexImage2D( target, level, format, width, height, border, format, type, pixels );
}

static GLint TexParameterValue( GLenum pname, GLint param )
{
	if ( ( pname == GL_TEXTURE_WRAP_S || pname == GL_TEXTURE_WRAP_T || pname == GL_TEXTURE_WRAP_R ) && param == GL_CLAMP )
		return GL_CLAMP_TO_EDGE;
	return param;
}

void glfTexParameterf( GLenum target, GLenum pname, GLfloat param )
{
	if ( !IsWebGLTextureTarget( target ) )
		return;
	if ( pname == GL_TEXTURE_MAX_ANISOTROPY )
		glTexParameterf( target, pname, param );
	else
		glTexParameteri( target, pname, TexParameterValue( pname, (GLint)param ) );
}

void glfTexParameteri( GLenum target, GLenum pname, GLint param )
{
	if ( !IsWebGLTextureTarget( target ) )
		return;
	glTexParameteri( target, pname, TexParameterValue( pname, param ) );
}

void glfTexParameterfv( GLenum target, GLenum pname, const GLfloat *params )
{
	// only border colours, which WebGL doesn't have
}

void glfCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
	GLsizei width, GLsizei height, GLint border )
{
	if ( !IsWebGLTextureTarget( target ) )
		return;
	// the internal format has to match the framebuffer's
	glCopyTexImage2D( target, level, GL_RGBA, x, y, width, height, 0 );
}

void glfCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
	GLsizei width, GLsizei height )
{
	if ( !IsWebGLTextureTarget( target ) )
		return;
	glCopyTexSubImage2D( target, level, xoffset, yoffset, x, y, width, height );
}

void glfReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels )
{
	if ( format == GL_DEPTH_COMPONENT )
	{
		// WebGL can't read the depth buffer: report everything as far away
		const size_t count = (size_t)width * height;
		if ( type == GL_FLOAT )
		{
			for ( size_t i = 0; i < count; i++ )
				( (float *)pixels )[i] = 1.0f;
		}
		else
		{
			memset( pixels, 0xff, count * TypeSize( type ) );
		}
		return;
	}

	if ( format == GL_RGBA || type != GL_UNSIGNED_BYTE )
	{
		glReadPixels( x, y, width, height, format, type, pixels );
		return;
	}

	// WebGL only promises RGBA bytes
	const size_t size = (size_t)width * height * 4;
	if ( size > glf.scratchPixelsSize )
	{
		glf.scratchPixels = (uint8_t *)realloc( glf.scratchPixels, size );
		glf.scratchPixelsSize = size;
	}
	glReadPixels( x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, glf.scratchPixels );

	GLint alignment = 4;
	glGetIntegerv( GL_PACK_ALIGNMENT, &alignment );
	const int components = format == GL_RGB ? 3 : 1;
	const size_t rowLength = ( (size_t)width * components + alignment - 1 ) / alignment * alignment;
	for ( GLsizei row = 0; row < height; row++ )
	{
		const uint8_t *in = glf.scratchPixels + (size_t)row * width * 4;
		uint8_t *out = (uint8_t *)pixels + row * rowLength;
		for ( GLsizei column = 0; column < width; column++ )
		{
			for ( int c = 0; c < components; c++ )
				out[column * components + c] = in[column * 4 + c];
		}
	}
}

void glfGetIntegerv( GLenum pname, GLint *params )
{
	if ( pname == GL_MAX_TEXTURE_UNITS )
		*params = NUM_TEXTURE_UNITS;
	else
		glGetIntegerv( pname, params );
}

void glfGetFloatv( GLenum pname, GLfloat *params ) { glGetFloatv( pname, params ); }
void glfClearDepth( GLclampd depth ) { glClearDepthf( (GLfloat)depth ); }
void glfDepthRange( GLclampd zNear, GLclampd zFar ) { glDepthRangef( (GLfloat)zNear, (GLfloat)zFar ); }

void glfDrawBuffer( GLenum mode )
{
	// the default framebuffer only has a back buffer
	GLenum buffer = mode == GL_NONE ? GL_NONE : GL_BACK;
	glDrawBuffers( 1, &buffer );
}

// no wireframes, flat shading, attribute stacks or display lists on WebGL;
// the renderers only use them for debugging or with features that are off
void glfPolygonMode( GLenum face, GLenum mode ) {}
void glfShadeModel( GLenum mode ) {}
void glfPushAttrib( GLbitfield mask ) {}
void glfPopAttrib( void ) {}
GLuint glfGenLists( GLsizei range ) { return 0; }
void glfNewList( GLuint list, GLenum mode ) {}
void glfEndList( void ) {}
void glfCallList( GLuint list ) {}
void glfDeleteLists( GLuint list, GLsizei range ) {}
