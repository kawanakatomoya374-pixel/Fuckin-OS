#ifndef COS_MC_GLEW_H
#define COS_MC_GLEW_H
#include <GL/gl.h>
#define GLEW_OK 0
GLenum glewInit(void);
const GLubyte *glewGetErrorString(GLenum);
#endif
