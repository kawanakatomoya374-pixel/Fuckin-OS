/* GL/gl.h - the subset of OpenGL 1.x (fixed function) that mc-classic-c uses, implemented by mcgl.c (a CPU rasteriser). */
#ifndef COS_MC_GL_H
#define COS_MC_GL_H
#include <stddef.h>
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef unsigned char GLboolean, GLubyte;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef void GLvoid;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_POINTS 0
#define GL_LINES 1
#define GL_LINE_LOOP 2
#define GL_LINE_STRIP 3
#define GL_TRIANGLES 4
#define GL_TRIANGLE_STRIP 5
#define GL_TRIANGLE_FAN 6
#define GL_QUADS 7
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207
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
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901
#define GL_CULL_FACE 0x0B44
#define GL_LIGHTING 0x0B50
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_COLOR_MATERIAL 0x0B57
#define GL_FOG 0x0B60
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_DEPTH_TEST 0x0B71
#define GL_NORMALIZE 0x0BA1
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_VIEWPORT 0x0BA2
#define GL_ALPHA_TEST 0x0BC0
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_TEXTURE_2D 0x0DE1
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_COMPILE 0x1300
#define GL_COMPILE_AND_EXECUTE 0x1301
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_POSITION 0x1203
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RENDER 0x1C00
#define GL_SELECT 0x1C02
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_CLAMP 0x2900
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_LIGHT0 0x4000
#define GL_RESCALE_NORMAL 0x803A
#define GL_VERTEX_ARRAY 0x8074
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_NORMAL_ARRAY 0x8075
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_NICEST 0x1102
#define GL_FASTEST 0x1101
#define GL_DONT_CARE 0x1100
#define GL_LINE_SMOOTH 0x0B20
#define GL_POLYGON_OFFSET_FILL 0x8037

void glEnable(GLenum), glDisable(GLenum);
void glClear(GLbitfield); void glClearColor(GLclampf, GLclampf, GLclampf, GLclampf); void glClearDepth(GLclampd);
void glViewport(GLint, GLint, GLsizei, GLsizei);
void glMatrixMode(GLenum), glLoadIdentity(void), glPushMatrix(void), glPopMatrix(void);
void glTranslatef(GLfloat, GLfloat, GLfloat), glTranslated(GLdouble, GLdouble, GLdouble);
void glRotatef(GLfloat, GLfloat, GLfloat, GLfloat), glRotated(GLdouble, GLdouble, GLdouble, GLdouble);
void glScalef(GLfloat, GLfloat, GLfloat), glScaled(GLdouble, GLdouble, GLdouble);
void glOrtho(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glFrustum(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
void glLoadMatrixf(const GLfloat *), glMultMatrixf(const GLfloat *);
void glBegin(GLenum), glEnd(void);
void glVertex2f(GLfloat, GLfloat), glVertex3f(GLfloat, GLfloat, GLfloat);
void glTexCoord2f(GLfloat, GLfloat);
void glColor3f(GLfloat, GLfloat, GLfloat), glColor4f(GLfloat, GLfloat, GLfloat, GLfloat);
void glColor3ub(GLubyte, GLubyte, GLubyte), glColor4ub(GLubyte, GLubyte, GLubyte, GLubyte);
void glNormal3f(GLfloat, GLfloat, GLfloat);
void glBindTexture(GLenum, GLuint), glGenTextures(GLsizei, GLuint *), glDeleteTextures(GLsizei, const GLuint *);
void glTexParameteri(GLenum, GLenum, GLint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
void glPixelStorei(GLenum, GLint);
void glBlendFunc(GLenum, GLenum), glAlphaFunc(GLenum, GLclampf), glDepthFunc(GLenum), glDepthMask(GLboolean);
void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean);
void glCullFace(GLenum), glFrontFace(GLenum), glShadeModel(GLenum), glHint(GLenum, GLenum), glLineWidth(GLfloat);
void glFogf(GLenum, GLfloat), glFogi(GLenum, GLint), glFogfv(GLenum, const GLfloat *);
void glLightfv(GLenum, GLenum, const GLfloat *), glLightModelfv(GLenum, const GLfloat *), glColorMaterial(GLenum, GLenum);
void glEnableClientState(GLenum), glDisableClientState(GLenum);
void glVertexPointer(GLint, GLenum, GLsizei, const void *), glTexCoordPointer(GLint, GLenum, GLsizei, const void *);
void glColorPointer(GLint, GLenum, GLsizei, const void *), glNormalPointer(GLenum, GLsizei, const void *);
void glDrawArrays(GLenum, GLint, GLsizei);
GLuint glGenLists(GLsizei); void glNewList(GLuint, GLenum), glEndList(void), glCallList(GLuint), glDeleteLists(GLuint, GLsizei);
void glCallLists(GLsizei, GLenum, const void *);
void glSelectBuffer(GLsizei, GLuint *); GLint glRenderMode(GLenum);
void glInitNames(void), glPushName(GLuint), glPopName(void), glLoadName(GLuint);
void glGetFloatv(GLenum, GLfloat *), glGetIntegerv(GLenum, GLint *);
GLenum glGetError(void);
#endif
