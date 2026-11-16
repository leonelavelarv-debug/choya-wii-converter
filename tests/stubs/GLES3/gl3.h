/* Cabecera mínima de GLES 3.0 para máquinas sin paquetes -dev (libgles-dev). Los prototipos y los valores son los de
 * la cabecera de Khronos, así que alcanza para compilar el runtime contra la libGLESv2.so.2 de Mesa (pruebas en la PC)
 * y para revisar la sintaxis. La compilación para la consola usa la cabecera real del SDK. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint; typedef int GLsizei;
typedef unsigned char GLboolean; typedef float GLfloat; typedef char GLchar; typedef unsigned char GLubyte;
typedef unsigned int GLbitfield; typedef ptrdiff_t GLsizeiptr; typedef ptrdiff_t GLintptr;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_POINTS 0x0000
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_TEXTURE_2D 0x0DE1
#define GL_CULL_FACE 0x0B44
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_SCISSOR_TEST 0x0C11
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_VIEWPORT 0x0BA2
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_NUM_COMPRESSED_TEXTURE_FORMATS 0x86A2
#define GL_COMPRESSED_TEXTURE_FORMATS 0x86A3
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RED 0x1903
#define GL_GREEN 0x1904
#define GL_BLUE 0x1905
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_LUMINANCE_ALPHA 0x190A
#define GL_UNSIGNED_SHORT_4_4_4_4 0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1 0x8034
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE0 0x84C0
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_MIRRORED_REPEAT 0x8370
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_RGBA8 0x8058
#define GL_RGB8 0x8051
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_INVALID_INDEX 0xFFFFFFFFu
#define GL_COMPRESSED_R11_EAC 0x9270
#define GL_COMPRESSED_SIGNED_R11_EAC 0x9271
#define GL_COMPRESSED_RG11_EAC 0x9272
#define GL_COMPRESSED_SIGNED_RG11_EAC 0x9273
#define GL_COMPRESSED_RGB8_ETC2 0x9274
#define GL_COMPRESSED_SRGB8_ETC2 0x9275
#define GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9276
#define GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2 0x9277
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#define GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC 0x9279
#ifdef __cplusplus
extern "C" {
#endif
void glActiveTexture(GLenum); void glAttachShader(GLuint, GLuint); void glBindAttribLocation(GLuint, GLuint, const GLchar*);
void glBindBuffer(GLenum, GLuint); void glBindBufferBase(GLenum, GLuint, GLuint); void glBindFramebuffer(GLenum, GLuint);
void glBindRenderbuffer(GLenum, GLuint); void glBindTexture(GLenum, GLuint); void glBindVertexArray(GLuint);
void glBlendEquation(GLenum); void glBlendFunc(GLenum, GLenum);
void glBufferData(GLenum, GLsizeiptr, const void*, GLenum); void glBufferSubData(GLenum, GLintptr, GLsizeiptr, const void*);
GLenum glCheckFramebufferStatus(GLenum); void glClear(GLbitfield); void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean); void glCompileShader(GLuint);
void glCompressedTexImage2D(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
GLuint glCreateProgram(void); GLuint glCreateShader(GLenum); void glCullFace(GLenum);
void glDeleteBuffers(GLsizei, const GLuint*); void glDeleteFramebuffers(GLsizei, const GLuint*); void glDeleteProgram(GLuint);
void glDeleteRenderbuffers(GLsizei, const GLuint*); void glDeleteShader(GLuint); void glDeleteTextures(GLsizei, const GLuint*);
void glDeleteVertexArrays(GLsizei, const GLuint*); void glDepthFunc(GLenum); void glDepthMask(GLboolean); void glDisable(GLenum);
void glDrawArrays(GLenum, GLint, GLsizei); void glDrawElements(GLenum, GLsizei, GLenum, const void*); void glEnable(GLenum);
void glEnableVertexAttribArray(GLuint); void glFramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
void glFramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint); void glFrontFace(GLenum);
void glGenBuffers(GLsizei, GLuint*); void glGenFramebuffers(GLsizei, GLuint*); void glGenRenderbuffers(GLsizei, GLuint*);
void glGenTextures(GLsizei, GLuint*); void glGenVertexArrays(GLsizei, GLuint*); GLenum glGetError(void);
void glGetIntegerv(GLenum, GLint*); void glGetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar*);
void glGetProgramiv(GLuint, GLenum, GLint*); void glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar*);
void glGetShaderiv(GLuint, GLenum, GLint*); const GLubyte* glGetString(GLenum);
GLuint glGetUniformBlockIndex(GLuint, const GLchar*); GLint glGetUniformLocation(GLuint, const GLchar*);
void glLinkProgram(GLuint); void glPixelStorei(GLenum, GLint); void glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
void glRenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
void glShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void glTexParameteri(GLenum, GLenum, GLint); void glTexStorage2D(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
void glUniform1i(GLint, GLint); void glUniform1f(GLint, GLfloat); void glUniform3f(GLint, GLfloat, GLfloat, GLfloat);
void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat); void glUniform1iv(GLint, GLsizei, const GLint*); void glUniform4i(GLint, GLint, GLint, GLint, GLint);
void glUniform3fv(GLint, GLsizei, const GLfloat*); void glUniform4fv(GLint, GLsizei, const GLfloat*);
void glUniformBlockBinding(GLuint, GLuint, GLuint); void glUniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*);
void glUseProgram(GLuint); void glVertexAttribIPointer(GLuint, GLint, GLenum, GLsizei, const void*);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*); void glViewport(GLint, GLint, GLsizei, GLsizei);
#ifdef __cplusplus
}
#endif
