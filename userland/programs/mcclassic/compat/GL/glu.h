#ifndef COS_MC_GLU_H
#define COS_MC_GLU_H
#include <GL/gl.h>
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);
void gluPickMatrix(GLdouble x, GLdouble y, GLdouble w, GLdouble h, const GLint viewport[4]);
int gluBuild2DMipmaps(GLenum target, GLint comps, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data);
const GLubyte *gluErrorString(GLenum);
#endif
