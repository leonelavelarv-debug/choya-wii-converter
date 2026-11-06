/* Cabecera mínima SOLO para revisar la sintaxis en máquinas sin GLES. No se usa en la compilación real. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei;
typedef unsigned char GLboolean; typedef float GLfloat; typedef char GLchar; typedef unsigned char GLubyte;
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#define GL_RED 0x1903
#define GL_GREEN 0x1904
#define GL_NO_ERROR 0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TEXTURE0 0x84C0
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_TRIANGLE_STRIP 0x0005
extern "C" {
void glGenTextures(GLsizei, GLuint*); void glBindTexture(GLenum, GLuint); void glPixelStorei(GLenum, GLint);
void glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void glTexParameteri(GLenum, GLenum, GLint); GLenum glGetError(void); void glDeleteTextures(GLsizei, const GLuint*);
GLuint glCreateShader(GLenum); void glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*);
void glCompileShader(GLuint); void glGetShaderiv(GLuint, GLenum, GLint*);
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar*); GLuint glCreateProgram(void);
void glAttachShader(GLuint, GLuint); void glBindAttribLocation(GLuint, GLuint, const GLchar*);
void glLinkProgram(GLuint); void glUseProgram(GLuint); GLint glGetUniformLocation(GLuint, const GLchar*);
void glUniform1i(GLint, GLint); const GLubyte* glGetString(GLenum); void glViewport(GLint, GLint, GLsizei, GLsizei);
void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat); void glClear(unsigned int); void glActiveTexture(GLenum);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
void glEnableVertexAttribArray(GLuint); void glDrawArrays(GLenum, GLint, GLsizei);
}
