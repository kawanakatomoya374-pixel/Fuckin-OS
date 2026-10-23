#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "matrix.h"
#include "util.h"
#include "r3d.h"

int rand_int(int n) {
    int result;
    while (n <= (result = rand() / (RAND_MAX / n)));
    return result;
}

double rand_double() {
    return (double)rand() / (double)RAND_MAX;
}

void update_fps(FPS *fps) {
    fps->frames++;
    double now = glfwGetTime();
    double elapsed = now - fps->since;
    if (elapsed >= 1) {
        fps->fps = round(fps->frames / elapsed);
        fps->frames = 0;
        fps->since = now;
    }
}

/* "GL buffers": handles into a table of malloc'd float arrays (r3d draws straight from them) */
typedef struct { float *data; } CBuf;
static CBuf *s_bufs; static unsigned s_nbufs, s_capbufs; static unsigned *s_free; static unsigned s_nfree, s_capfree;

GLuint gen_buffer(GLsizei size, GLfloat *data) {
    float *copy = (float *)malloc((size_t)size > 0 ? (size_t)size : 4);
    if (!copy) return 0;
    if (data) memcpy(copy, data, (size_t)size);
    unsigned idx;
    if (s_nfree) idx = s_free[--s_nfree];
    else {
        if (s_nbufs + 1 >= s_capbufs) { s_capbufs = s_capbufs ? s_capbufs * 2 : 4096; s_bufs = (CBuf *)realloc(s_bufs, s_capbufs * sizeof(CBuf)); }
        idx = ++s_nbufs;                                         /* handle 0 means "no buffer" */
    }
    s_bufs[idx].data = copy;
    return idx;
}
void del_buffer(GLuint buffer) {
    r3d_flush();                                                 /* queued block draws may still point at this buffer */
    if (!buffer || buffer > s_nbufs || !s_bufs[buffer].data) return;
    free(s_bufs[buffer].data); s_bufs[buffer].data = NULL;
    if (s_nfree == s_capfree) { s_capfree = s_capfree ? s_capfree * 2 : 1024; s_free = (unsigned *)realloc(s_free, s_capfree * sizeof(unsigned)); }
    s_free[s_nfree++] = buffer;
}
const float *buffer_data(GLuint buffer) { return buffer && buffer <= s_nbufs ? s_bufs[buffer].data : NULL; }

GLfloat *malloc_faces(int components, int faces) {
    return malloc(sizeof(GLfloat) * 6 * components * faces);
}

GLuint gen_faces(int components, int faces, GLfloat *data) {
    GLuint buffer = gen_buffer(
        sizeof(GLfloat) * 6 * components * faces, data);
    free(data);
    return buffer;
}

char *tokenize(char *str, const char *delim, char **key) {
    char *result;
    if (str == NULL) {
        str = *key;
    }
    str += strspn(str, delim);
    if (*str == '\0') {
        return NULL;
    }
    result = str;
    str += strcspn(str, delim);
    if (*str) {
        *str++ = '\0';
    }
    *key = str;
    return result;
}

int char_width(char input) {
    static const int lookup[128] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        4, 2, 4, 7, 6, 9, 7, 2, 3, 3, 4, 6, 3, 5, 2, 7,
        6, 3, 6, 6, 6, 6, 6, 6, 6, 6, 2, 3, 5, 6, 5, 7,
        8, 6, 6, 6, 6, 6, 6, 6, 6, 4, 6, 6, 5, 8, 8, 6,
        6, 7, 6, 6, 6, 6, 8,10, 8, 6, 6, 3, 6, 3, 6, 6,
        4, 7, 6, 6, 6, 6, 5, 6, 6, 2, 5, 5, 2, 9, 6, 6,
        6, 6, 6, 6, 5, 6, 6, 6, 6, 6, 6, 4, 2, 5, 7, 0
    };
    return lookup[input];
}

int string_width(const char *input) {
    int result = 0;
    int length = strlen(input);
    for (int i = 0; i < length; i++) {
        result += char_width(input[i]);
    }
    return result;
}

int wrap(const char *input, int max_width, char *output, int max_length) {
    *output = '\0';
    char *text = malloc(sizeof(char) * (strlen(input) + 1));
    strcpy(text, input);
    int space_width = char_width(' ');
    int line_number = 0;
    char *key1, *key2;
    char *line = tokenize(text, "\r\n", &key1);
    while (line) {
        int line_width = 0;
        char *token = tokenize(line, " ", &key2);
        while (token) {
            int token_width = string_width(token);
            if (line_width) {
                if (line_width + token_width > max_width) {
                    line_width = 0;
                    line_number++;
                    strncat(output, "\n", max_length - strlen(output) - 1);
                }
                else {
                    strncat(output, " ", max_length - strlen(output) - 1);
                }
            }
            strncat(output, token, max_length - strlen(output) - 1);
            line_width += token_width + space_width;
            token = tokenize(NULL, " ", &key2);
        }
        line_number++;
        strncat(output, "\n", max_length - strlen(output) - 1);
        line = tokenize(NULL, "\r\n", &key1);
    }
    free(text);
    return line_number;
}
