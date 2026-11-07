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
#define GL_TRIANGLES 0x0004
#define GL_TRUE 1
#define GL_UNSIGNED_INT 0x1405
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DEPTH_TEST 0x0B71
#define GL_CW 0x0900
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_CULL_FACE 0x0B44
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
typedef ptrdiff_t GLsizeiptr;
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_LEQUAL 0x0203
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
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
void glGenVertexArrays(GLsizei, GLuint*); void glBindVertexArray(GLuint); void glDeleteVertexArrays(GLsizei, const GLuint*);
void glGenBuffers(GLsizei, GLuint*); void glBindBuffer(GLenum, GLuint); void glDeleteBuffers(GLsizei, const GLuint*);
void glBufferData(GLenum, GLsizeiptr, const void*, GLenum); void glDrawElements(GLenum, GLsizei, GLenum, const void*);
void glEnable(GLenum); void glDisable(GLenum); void glCullFace(GLenum); void glFrontFace(GLenum);
void glBlendFunc(GLenum, GLenum); void glDepthMask(GLboolean); void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*);
void glUniform3f(GLint, GLfloat, GLfloat, GLfloat);
void glBlendEquation(GLenum); void glDepthFunc(GLenum); void glDeleteProgram(GLuint); void glDeleteShader(GLuint);
void glGenFramebuffers(GLsizei, GLuint*); void glBindFramebuffer(GLenum, GLuint); void glGenRenderbuffers(GLsizei, GLuint*);
void glBindRenderbuffer(GLenum, GLuint); void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
}
