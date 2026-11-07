/* Cabecera mínima de EGL SOLO para compilar las pruebas sin paquetes -dev. */
#pragma once
#include <stdint.h>
typedef void* EGLDisplay; typedef void* EGLConfig; typedef void* EGLContext; typedef void* EGLSurface;
typedef int32_t EGLint; typedef unsigned int EGLenum; typedef unsigned int EGLBoolean;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
extern "C" {
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char*);
EGLDisplay eglGetDisplay(void*); EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*); EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext); EGLBoolean eglTerminate(EGLDisplay);
}
