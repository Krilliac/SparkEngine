/**
 * stb_image_write - v1.16 - public domain image writer
 * https://github.com/nothings/stb
 *
 * STUB header providing the stb_image_write API.
 * Replace with the real header from:
 *   curl -L https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h -o ThirdParty/Utils/stb/stb_image_write.h
 *
 * License: Public Domain / MIT
 */

#ifndef INCLUDE_STB_IMAGE_WRITE_H
#define INCLUDE_STB_IMAGE_WRITE_H

#include <cstdlib>

#ifndef STBIWDEF
#ifdef STB_IMAGE_WRITE_STATIC
#define STBIWDEF static
#else
#define STBIWDEF extern
#endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    STBIWDEF int stbi_write_png(const char* filename, int w, int h, int comp, const void* data, int stride_in_bytes);
    STBIWDEF int stbi_write_bmp(const char* filename, int w, int h, int comp, const void* data);
    STBIWDEF int stbi_write_tga(const char* filename, int w, int h, int comp, const void* data);
    STBIWDEF int stbi_write_jpg(const char* filename, int w, int h, int comp, const void* data, int quality);
    STBIWDEF int stbi_write_hdr(const char* filename, int w, int h, int comp, const float* data);

#ifdef __cplusplus
}
#endif

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION

STBIWDEF int stbi_write_png(const char* filename, int w, int h, int comp, const void* data, int stride_in_bytes)
{
    (void)filename;
    (void)w;
    (void)h;
    (void)comp;
    (void)data;
    (void)stride_in_bytes;
    return 0; // Not implemented in stub — replace with real stb_image_write.h
}

STBIWDEF int stbi_write_bmp(const char* filename, int w, int h, int comp, const void* data)
{
    (void)filename;
    (void)w;
    (void)h;
    (void)comp;
    (void)data;
    return 0;
}

STBIWDEF int stbi_write_tga(const char* filename, int w, int h, int comp, const void* data)
{
    (void)filename;
    (void)w;
    (void)h;
    (void)comp;
    (void)data;
    return 0;
}

STBIWDEF int stbi_write_jpg(const char* filename, int w, int h, int comp, const void* data, int quality)
{
    (void)filename;
    (void)w;
    (void)h;
    (void)comp;
    (void)data;
    (void)quality;
    return 0;
}

STBIWDEF int stbi_write_hdr(const char* filename, int w, int h, int comp, const float* data)
{
    (void)filename;
    (void)w;
    (void)h;
    (void)comp;
    (void)data;
    return 0;
}

#endif // STB_IMAGE_WRITE_IMPLEMENTATION

#endif // INCLUDE_STB_IMAGE_WRITE_H
