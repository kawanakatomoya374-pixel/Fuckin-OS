/* stbi_impl.c - stb_image compiled once, for JPEG/PNG/BMP/GIF only (thumbnails and preview). */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ASSERT(x) ((void)0)
#include "../../third_party/stb_image.h"
