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

// The OpenGL 1.x subset that rd-vanilla uses, implemented on WebGL 2 (see
// qgl_fixed.cpp). qgl.h maps the renderer's qgl* calls to these on web.
// Include after a header that defines the GL types.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// for refimport_t::GL_ExtensionSupported and GL_GetProcAddress
int GLF_ExtensionSupported( const char *extension );
void *GLF_GetProcAddress( const char *name );

void glfBegin( GLenum mode );
void glfEnd( void );
void glfVertex2f( GLfloat x, GLfloat y );
void glfVertex3f( GLfloat x, GLfloat y, GLfloat z );
void glfVertex3fv( const GLfloat *v );
void glfTexCoord2f( GLfloat s, GLfloat t );
void glfTexCoord2fv( const GLfloat *v );
void glfMultiTexCoord2f( GLenum target, GLfloat s, GLfloat t );
void glfColor3f( GLfloat r, GLfloat g, GLfloat b );
void glfColor4f( GLfloat r, GLfloat g, GLfloat b, GLfloat a );
void glfColor4ub( GLubyte r, GLubyte g, GLubyte b, GLubyte a );
void glfColor4ubv( const GLubyte *v );

void glfMatrixMode( GLenum mode );
void glfLoadIdentity( void );
void glfLoadMatrixf( const GLfloat *m );
void glfPushMatrix( void );
void glfPopMatrix( void );
void glfOrtho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar );
void glfTranslatef( GLfloat x, GLfloat y, GLfloat z );
void glfRotatef( GLfloat angle, GLfloat x, GLfloat y, GLfloat z );

void glfEnable( GLenum cap );
void glfDisable( GLenum cap );
GLboolean glfIsEnabled( GLenum cap );
void glfActiveTexture( GLenum texture );
void glfClientActiveTexture( GLenum texture );
void glfEnableClientState( GLenum array );
void glfDisableClientState( GLenum array );
void glfVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer );
void glfColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer );
void glfTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer );
void glfDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices );
void glfDrawArrays( GLenum mode, GLint first, GLsizei count );
void glfArrayElement( GLint i );

void glfTexEnvf( GLenum target, GLenum pname, GLfloat param );
void glfTexEnvi( GLenum target, GLenum pname, GLint param );
void glfAlphaFunc( GLenum func, GLclampf ref );
void glfFogf( GLenum pname, GLfloat param );
void glfFogi( GLenum pname, GLint param );
void glfFogfv( GLenum pname, const GLfloat *params );
void glfClipPlane( GLenum plane, const GLdouble *equation );

void glfTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
	GLint border, GLenum format, GLenum type, const GLvoid *pixels );
void glfTexParameterf( GLenum target, GLenum pname, GLfloat param );
void glfTexParameteri( GLenum target, GLenum pname, GLint param );
void glfTexParameterfv( GLenum target, GLenum pname, const GLfloat *params );
void glfCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
	GLsizei width, GLsizei height, GLint border );
void glfCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
	GLsizei width, GLsizei height );
void glfReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels );
void glfGetIntegerv( GLenum pname, GLint *params );
void glfGetFloatv( GLenum pname, GLfloat *params );
void glfClearDepth( GLclampd depth );
void glfDepthRange( GLclampd zNear, GLclampd zFar );
void glfDrawBuffer( GLenum mode );
void glfPolygonMode( GLenum face, GLenum mode );
void glfShadeModel( GLenum mode );
void glfPushAttrib( GLbitfield mask );
void glfPopAttrib( void );
GLuint glfGenLists( GLsizei range );
void glfNewList( GLuint list, GLenum mode );
void glfEndList( void );
void glfCallList( GLuint list );
void glfDeleteLists( GLuint list, GLsizei range );

#ifdef __cplusplus
}
#endif
