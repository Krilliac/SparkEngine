/**
 * @file gl.h
 * @brief Minimal OpenGL type definitions — fallback for when libgl-dev is not installed.
 *
 * This header provides the bare-minimum GL typedefs required by SDL2's OpenGL
 * detection (CheckOpenGL in sdlchecks.cmake) and by the SparkEngine OpenGL RHI
 * backend. It is NOT a complete <GL/gl.h> — it only defines types and constants
 * that the engine actually uses. GLAD handles all GL function loading at runtime.
 *
 * When libgl-dev (or equivalent) is installed, the system header takes priority
 * via the standard include path. This file is only added as a fallback by CMake
 * when the system headers are absent.
 */
#ifndef __gl_h_
#define __gl_h_

#include <stddef.h>

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#endif /* __gl_h_ */
