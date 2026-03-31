/**
 * stb_image - v2.30 - public domain image loader
 * https://github.com/nothings/stb
 *
 * This is a STUB header that provides the stb_image API declarations.
 * The full library is a single public-domain header by Sean Barrett.
 *
 * To get the real implementation:
 *   curl -L https://raw.githubusercontent.com/nothings/stb/master/stb_image.h -o ThirdParty/Utils/stb/stb_image.h
 *
 * Supported formats: JPEG, PNG, BMP, PSD, TGA, GIF, HDR, PIC, PNM
 * License: Public Domain / MIT
 */

#ifndef STBI_INCLUDE_STB_IMAGE_H
#define STBI_INCLUDE_STB_IMAGE_H

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>

#ifndef STBIDEF
#ifdef STB_IMAGE_STATIC
#define STBIDEF static
#else
#define STBIDEF extern
#endif
#endif

enum
{
    STBI_default = 0,
    STBI_grey = 1,
    STBI_grey_alpha = 2,
    STBI_rgb = 3,
    STBI_rgb_alpha = 4
};

typedef unsigned char stbi_uc;
typedef unsigned short stbi_us;

#ifdef __cplusplus
extern "C"
{
#endif

    // Primary API — loads image from file path
    STBIDEF stbi_uc* stbi_load(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels);

    // Load from memory
    STBIDEF stbi_uc* stbi_load_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file,
                                            int desired_channels);

    // Load from FILE*
    STBIDEF stbi_uc* stbi_load_from_file(FILE* f, int* x, int* y, int* channels_in_file, int desired_channels);

    // HDR image loading
    STBIDEF float* stbi_loadf(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels);
    STBIDEF float* stbi_loadf_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file,
                                           int desired_channels);

    // 16-bit image loading
    STBIDEF stbi_us* stbi_load_16(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels);
    STBIDEF stbi_us* stbi_load_16_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file,
                                               int desired_channels);

    // Query image dimensions without loading
    STBIDEF int stbi_info(const char* filename, int* x, int* y, int* comp);
    STBIDEF int stbi_info_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* comp);

    // Check if HDR format
    STBIDEF int stbi_is_hdr(const char* filename);
    STBIDEF int stbi_is_hdr_from_memory(const stbi_uc* buffer, int len);

    // Free loaded image data
    STBIDEF void stbi_image_free(void* retval_from_stbi_load);

    // Error reporting
    STBIDEF const char* stbi_failure_reason(void);

    // Flip vertically on load (useful for OpenGL)
    STBIDEF void stbi_set_flip_vertically_on_load(int flag_true_if_should_flip);

#ifdef __cplusplus
}
#endif

// ============================================================================
// Implementation
// ============================================================================
#ifdef STB_IMAGE_IMPLEMENTATION

// Minimal fallback implementation that handles basic image loading.
// Replace this file with the real stb_image.h for production use.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static int stbi__flip_vertically = 0;
static const char* stbi__failure_reason_str = "";

STBIDEF void stbi_set_flip_vertically_on_load(int flag_true_if_should_flip)
{
    stbi__flip_vertically = flag_true_if_should_flip;
}

STBIDEF const char* stbi_failure_reason(void)
{
    return stbi__failure_reason_str;
}

STBIDEF void stbi_image_free(void* retval_from_stbi_load)
{
    free(retval_from_stbi_load);
}

// Helper: flip image rows vertically
static void stbi__vertical_flip(void* image, int w, int h, int bytes_per_pixel)
{
    int stride = w * bytes_per_pixel;
    unsigned char* row_buffer = (unsigned char*)malloc(stride);
    if (!row_buffer)
        return;
    unsigned char* bytes = (unsigned char*)image;
    for (int row = 0; row < h / 2; ++row)
    {
        unsigned char* row0 = bytes + row * stride;
        unsigned char* row1 = bytes + (h - 1 - row) * stride;
        memcpy(row_buffer, row0, stride);
        memcpy(row0, row1, stride);
        memcpy(row1, row_buffer, stride);
    }
    free(row_buffer);
}

// Minimal PNG decoder (reads uncompressed / basic zlib-compressed PNGs)
// For a real project, replace this entire file with the actual stb_image.h

// Minimal BMP loader
static stbi_uc* stbi__load_bmp(FILE* f, int* x, int* y, int* comp, int req_comp)
{
    unsigned char header[54];
    if (fread(header, 1, 54, f) != 54)
    {
        stbi__failure_reason_str = "Not a BMP file";
        return NULL;
    }

    if (header[0] != 'B' || header[1] != 'M')
    {
        stbi__failure_reason_str = "Not a BMP file";
        return NULL;
    }

    int width = *(int*)&header[18];
    int height = *(int*)&header[22];
    short bpp = *(short*)&header[28];
    int data_offset = *(int*)&header[10];

    if (width <= 0 || height <= 0 || (bpp != 24 && bpp != 32))
    {
        stbi__failure_reason_str = "Unsupported BMP format";
        return NULL;
    }

    *x = width;
    int abs_height = height > 0 ? height : -height;
    *y = abs_height;
    int src_channels = bpp / 8;
    *comp = src_channels;

    int out_channels = req_comp > 0 ? req_comp : src_channels;
    stbi_uc* output = (stbi_uc*)malloc(width * abs_height * out_channels);
    if (!output)
    {
        stbi__failure_reason_str = "Out of memory";
        return NULL;
    }

    fseek(f, data_offset, SEEK_SET);
    int row_size = ((width * src_channels + 3) / 4) * 4; // BMP rows are 4-byte aligned
    unsigned char* row_buf = (unsigned char*)malloc(row_size);

    for (int row = 0; row < abs_height; ++row)
    {
        int dest_row = (height > 0) ? (abs_height - 1 - row) : row;
        if (fread(row_buf, 1, row_size, f) != (size_t)row_size)
        {
            free(row_buf);
            free(output);
            stbi__failure_reason_str = "Truncated BMP";
            return NULL;
        }
        for (int col = 0; col < width; ++col)
        {
            unsigned char b = row_buf[col * src_channels + 0];
            unsigned char g = row_buf[col * src_channels + 1];
            unsigned char r = row_buf[col * src_channels + 2];
            unsigned char a = (src_channels == 4) ? row_buf[col * src_channels + 3] : 255;

            stbi_uc* dst = output + (dest_row * width + col) * out_channels;
            if (out_channels >= 1)
                dst[0] = r;
            if (out_channels >= 2)
                dst[1] = g;
            if (out_channels >= 3)
                dst[2] = b;
            if (out_channels >= 4)
                dst[3] = a;
        }
    }
    free(row_buf);

    if (stbi__flip_vertically)
        stbi__vertical_flip(output, width, abs_height, out_channels);

    return output;
}

// TGA loader (uncompressed)
static stbi_uc* stbi__load_tga(FILE* f, int* x, int* y, int* comp, int req_comp)
{
    unsigned char header[18];
    if (fread(header, 1, 18, f) != 18)
    {
        stbi__failure_reason_str = "Not a TGA file";
        return NULL;
    }

    int width = header[12] | (header[13] << 8);
    int height = header[14] | (header[15] << 8);
    int bpp = header[16];
    int image_type = header[2];

    if (width <= 0 || height <= 0 || (bpp != 24 && bpp != 32) || (image_type != 2 && image_type != 10))
    {
        stbi__failure_reason_str = "Unsupported TGA format";
        return NULL;
    }

    *x = width;
    *y = height;
    int src_channels = bpp / 8;
    *comp = src_channels;

    int out_channels = req_comp > 0 ? req_comp : src_channels;
    size_t pixel_count = (size_t)width * height;
    stbi_uc* output = (stbi_uc*)malloc(pixel_count * out_channels);
    if (!output)
    {
        stbi__failure_reason_str = "Out of memory";
        return NULL;
    }

    // Skip image ID field
    if (header[0] > 0)
        fseek(f, header[0], SEEK_CUR);

    // Uncompressed true-color
    if (image_type == 2)
    {
        for (size_t i = 0; i < pixel_count; ++i)
        {
            unsigned char pixel[4];
            if (fread(pixel, 1, src_channels, f) != (size_t)src_channels)
            {
                free(output);
                stbi__failure_reason_str = "Truncated TGA";
                return NULL;
            }
            stbi_uc* dst = output + i * out_channels;
            // TGA stores as BGR(A)
            if (out_channels >= 1)
                dst[0] = pixel[2]; // R
            if (out_channels >= 2)
                dst[1] = pixel[1]; // G
            if (out_channels >= 3)
                dst[2] = pixel[0]; // B
            if (out_channels >= 4)
                dst[3] = (src_channels == 4) ? pixel[3] : 255;
        }
    }
    else
    {
        // RLE compressed — basic support
        size_t pixels_read = 0;
        while (pixels_read < pixel_count)
        {
            unsigned char packet;
            if (fread(&packet, 1, 1, f) != 1)
                break;

            int count = (packet & 0x7F) + 1;
            if (packet & 0x80)
            {
                // Run-length packet
                unsigned char pixel[4];
                if (fread(pixel, 1, src_channels, f) != (size_t)src_channels)
                    break;
                for (int j = 0; j < count && pixels_read < pixel_count; ++j, ++pixels_read)
                {
                    stbi_uc* dst = output + pixels_read * out_channels;
                    if (out_channels >= 1)
                        dst[0] = pixel[2];
                    if (out_channels >= 2)
                        dst[1] = pixel[1];
                    if (out_channels >= 3)
                        dst[2] = pixel[0];
                    if (out_channels >= 4)
                        dst[3] = (src_channels == 4) ? pixel[3] : 255;
                }
            }
            else
            {
                // Raw packet
                for (int j = 0; j < count && pixels_read < pixel_count; ++j, ++pixels_read)
                {
                    unsigned char pixel[4];
                    if (fread(pixel, 1, src_channels, f) != (size_t)src_channels)
                        break;
                    stbi_uc* dst = output + pixels_read * out_channels;
                    if (out_channels >= 1)
                        dst[0] = pixel[2];
                    if (out_channels >= 2)
                        dst[1] = pixel[1];
                    if (out_channels >= 3)
                        dst[2] = pixel[0];
                    if (out_channels >= 4)
                        dst[3] = (src_channels == 4) ? pixel[3] : 255;
                }
            }
        }
    }

    // Handle TGA origin (bit 5 of descriptor = top-left origin)
    bool top_origin = (header[17] & 0x20) != 0;
    if (!top_origin)
        stbi__vertical_flip(output, width, height, out_channels);

    if (stbi__flip_vertically)
        stbi__vertical_flip(output, width, height, out_channels);

    return output;
}

STBIDEF stbi_uc* stbi_load(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels)
{
    FILE* f = fopen(filename, "rb");
    if (!f)
    {
        stbi__failure_reason_str = "Unable to open file";
        return NULL;
    }

    stbi_uc* result = stbi_load_from_file(f, x, y, channels_in_file, desired_channels);
    fclose(f);
    return result;
}

STBIDEF stbi_uc* stbi_load_from_file(FILE* f, int* x, int* y, int* channels_in_file, int desired_channels)
{
    // Detect format by reading first bytes
    unsigned char sig[8];
    size_t sig_read = fread(sig, 1, 8, f);
    fseek(f, 0, SEEK_SET);

    if (sig_read < 2)
    {
        stbi__failure_reason_str = "File too small";
        return NULL;
    }

    // BMP: 'B' 'M'
    if (sig[0] == 'B' && sig[1] == 'M')
    {
        return stbi__load_bmp(f, x, y, channels_in_file, desired_channels);
    }

    // TGA: Check by extension or heuristic (image type byte at offset 2)
    // TGA has no magic number, but common image types are 2 (uncompressed) or 10 (RLE)
    if (sig[2] == 2 || sig[2] == 10)
    {
        // Could be TGA — try it
        return stbi__load_tga(f, x, y, channels_in_file, desired_channels);
    }

    stbi__failure_reason_str = "Image format not recognized (install full stb_image.h for PNG/JPEG/HDR support)";
    return NULL;
}

STBIDEF stbi_uc* stbi_load_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file,
                                        int desired_channels)
{
    // Minimal: only BMP from memory
    if (len >= 2 && buffer[0] == 'B' && buffer[1] == 'M')
    {
        FILE* tmp = tmpfile();
        if (!tmp)
        {
            stbi__failure_reason_str = "Failed to create temp file";
            return NULL;
        }
        fwrite(buffer, 1, len, tmp);
        fseek(tmp, 0, SEEK_SET);
        stbi_uc* result = stbi__load_bmp(tmp, x, y, channels_in_file, desired_channels);
        fclose(tmp);
        return result;
    }

    stbi__failure_reason_str = "Image format not recognized from memory (install full stb_image.h)";
    return NULL;
}

STBIDEF float* stbi_loadf(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels)
{
    (void)filename;
    (void)x;
    (void)y;
    (void)channels_in_file;
    (void)desired_channels;
    stbi__failure_reason_str = "HDR loading requires full stb_image.h";
    return NULL;
}

STBIDEF float* stbi_loadf_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file,
                                       int desired_channels)
{
    (void)buffer;
    (void)len;
    (void)x;
    (void)y;
    (void)channels_in_file;
    (void)desired_channels;
    stbi__failure_reason_str = "HDR loading requires full stb_image.h";
    return NULL;
}

STBIDEF stbi_us* stbi_load_16(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels)
{
    (void)filename;
    (void)x;
    (void)y;
    (void)channels_in_file;
    (void)desired_channels;
    stbi__failure_reason_str = "16-bit loading requires full stb_image.h";
    return NULL;
}

STBIDEF stbi_us* stbi_load_16_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file,
                                           int desired_channels)
{
    (void)buffer;
    (void)len;
    (void)x;
    (void)y;
    (void)channels_in_file;
    (void)desired_channels;
    stbi__failure_reason_str = "16-bit loading requires full stb_image.h";
    return NULL;
}

STBIDEF int stbi_info(const char* filename, int* x, int* y, int* comp)
{
    (void)filename;
    (void)x;
    (void)y;
    (void)comp;
    stbi__failure_reason_str = "stbi_info requires full stb_image.h";
    return 0;
}

STBIDEF int stbi_info_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* comp)
{
    (void)buffer;
    (void)len;
    (void)x;
    (void)y;
    (void)comp;
    return 0;
}

STBIDEF int stbi_is_hdr(const char* filename)
{
    (void)filename;
    return 0;
}

STBIDEF int stbi_is_hdr_from_memory(const stbi_uc* buffer, int len)
{
    (void)buffer;
    (void)len;
    return 0;
}

#endif // STB_IMAGE_IMPLEMENTATION

#endif // STBI_INCLUDE_STB_IMAGE_H
