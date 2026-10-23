/* gl_cos.h - what util.h / cube.h / matrix.h need from "OpenGL": the types. Drawing goes through r3d.h (see craft.c). */
#ifndef CRAFT_GL_COS_H
#define CRAFT_GL_COS_H
#include <stdint.h>
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned int GLenum;
typedef int GLint;
double glfwGetTime(void);
#endif
