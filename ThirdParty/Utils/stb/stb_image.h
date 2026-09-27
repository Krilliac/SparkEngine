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

// Every header field below is untrusted file content (this decoder backs the
// non-Windows texture loaders). Dimensions are capped, all size arithmetic is
// done in size_t after the cap, and a header may not request more pixel bytes
// than the file actually holds, so a tiny file cannot drive a huge allocation.
#ifndef STBI_MAX_DIMENSIONS
#define STBI_MAX_DIMENSIONS 16384
#endif

static unsigned int stbi__le32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int stbi__le16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

// Bytes from the current position to the end of the file, restoring the
// position. Returns 0 on success, -1 when the stream cannot be measured.
static int stbi__remaining_bytes(FILE* f, size_t* out_remaining)
{
    long here = ftell(f);
    if (here < 0 || fseek(f, 0, SEEK_END) != 0)
        return -1;
    long end = ftell(f);
    if (end < here || fseek(f, here, SEEK_SET) != 0)
        return -1;
    *out_remaining = (size_t)(end - here);
    return 0;
}

// Output channel count for a request; 0 means "as stored". Anything outside
// 0..4 is a caller error that would index past a pixel.
static int stbi__out_channels(int req_comp, int src_channels)
{
    if (req_comp < 0 || req_comp > 4)
        return 0;
    return req_comp > 0 ? req_comp : src_channels;
}

// Helper: flip image rows vertically
static void stbi__vertical_flip(void* image, int w, int h, int bytes_per_pixel)
{
    if (w <= 0 || h <= 0 || bytes_per_pixel <= 0)
        return;
    size_t stride = (size_t)w * (size_t)bytes_per_pixel;
    unsigned char* row_buffer = (unsigned char*)malloc(stride);
    if (!row_buffer)
        return;
    unsigned char* bytes = (unsigned char*)image;
    for (size_t row = 0; row < (size_t)h / 2; ++row)
    {
        unsigned char* row0 = bytes + row * stride;
        unsigned char* row1 = bytes + ((size_t)h - 1 - row) * stride;
        memcpy(row_buffer, row0, stride);
        memcpy(row0, row1, stride);
        memcpy(row1, row_buffer, stride);
    }
    free(row_buffer);
}

static void stbi__store_pixel(stbi_uc* dst, int out_channels, unsigned char r, unsigned char g, unsigned char b,
                              unsigned char a)
{
    if (out_channels >= 1)
        dst[0] = r;
    if (out_channels >= 2)
        dst[1] = g;
    if (out_channels >= 3)
        dst[2] = b;
    if (out_channels >= 4)
        dst[3] = a;
}

// Minimal BMP loader (uncompressed, bottom-up, 24 or 32 bpp)
static stbi_uc* stbi__load_bmp(FILE* f, int* x, int* y, int* comp, int req_comp)
{
    size_t file_bytes = 0;
    if (stbi__remaining_bytes(f, &file_bytes) != 0)
    {
        stbi__failure_reason_str = "Unable to measure BMP";
        return NULL;
    }

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

    // Signed 32-bit fields decoded explicitly as little-endian.
    const int32_t width = (int32_t)stbi__le32(&header[18]);
    const int32_t height = (int32_t)stbi__le32(&header[22]);
    const unsigned int bpp = stbi__le16(&header[28]);
    const unsigned int data_offset = stbi__le32(&header[10]);

    // Top-down (negative height) BMPs are not supported by this stub.
    if (width <= 0 || height <= 0 || (bpp != 24 && bpp != 32))
    {
        stbi__failure_reason_str = "Unsupported BMP format";
        return NULL;
    }
    if (width > STBI_MAX_DIMENSIONS || height > STBI_MAX_DIMENSIONS)
    {
        stbi__failure_reason_str = "BMP too large";
        return NULL;
    }

    const int src_channels = (int)(bpp / 8);
    const int out_channels = stbi__out_channels(req_comp, src_channels);
    if (out_channels == 0)
    {
        stbi__failure_reason_str = "Bad req_comp";
        return NULL;
    }

    // Dimensions are <= STBI_MAX_DIMENSIONS, so none of these products overflow size_t.
    const size_t w = (size_t)width;
    const size_t h = (size_t)height;
    const size_t row_size = ((w * (size_t)src_channels + 3) / 4) * 4; // BMP rows are 4-byte aligned
    const size_t pixel_bytes = row_size * h;
    if (data_offset < 54 || data_offset > file_bytes || pixel_bytes > file_bytes - data_offset)
    {
        stbi__failure_reason_str = "Truncated BMP";
        return NULL;
    }

    stbi_uc* output = (stbi_uc*)malloc(w * h * (size_t)out_channels);
    unsigned char* row_buf = (unsigned char*)malloc(row_size);
    if (!output || !row_buf)
    {
        free(output);
        free(row_buf);
        stbi__failure_reason_str = "Out of memory";
        return NULL;
    }

    if (fseek(f, (long)data_offset, SEEK_SET) != 0)
    {
        free(row_buf);
        free(output);
        stbi__failure_reason_str = "Truncated BMP";
        return NULL;
    }

    for (size_t row = 0; row < h; ++row)
    {
        const size_t dest_row = h - 1 - row;
        if (fread(row_buf, 1, row_size, f) != row_size)
        {
            free(row_buf);
            free(output);
            stbi__failure_reason_str = "Truncated BMP";
            return NULL;
        }
        for (size_t col = 0; col < w; ++col)
        {
            const unsigned char* src = row_buf + col * (size_t)src_channels;
            const unsigned char a = (src_channels == 4) ? src[3] : 255;
            stbi__store_pixel(output + (dest_row * w + col) * (size_t)out_channels, out_channels, src[2], src[1],
                              src[0], a);
        }
    }
    free(row_buf);

    *x = width;
    *y = height;
    *comp = src_channels;

    if (stbi__flip_vertically)
        stbi__vertical_flip(output, width, height, out_channels);

    return output;
}

// TGA loader (uncompressed and RLE true-colour)
static stbi_uc* stbi__load_tga(FILE* f, int* x, int* y, int* comp, int req_comp)
{
    size_t file_bytes = 0;
    if (stbi__remaining_bytes(f, &file_bytes) != 0)
    {
        stbi__failure_reason_str = "Unable to measure TGA";
        return NULL;
    }

    unsigned char header[18];
    if (fread(header, 1, 18, f) != 18)
    {
        stbi__failure_reason_str = "Not a TGA file";
        return NULL;
    }

    const int width = (int)stbi__le16(&header[12]);
    const int height = (int)stbi__le16(&header[14]);
    const int bpp = header[16];
    const int image_type = header[2];

    if (width <= 0 || height <= 0 || (bpp != 24 && bpp != 32) || (image_type != 2 && image_type != 10))
    {
        stbi__failure_reason_str = "Unsupported TGA format";
        return NULL;
    }
    if (width > STBI_MAX_DIMENSIONS || height > STBI_MAX_DIMENSIONS)
    {
        stbi__failure_reason_str = "TGA too large";
        return NULL;
    }

    const int src_channels = bpp / 8;
    const int out_channels = stbi__out_channels(req_comp, src_channels);
    if (out_channels == 0)
    {
        stbi__failure_reason_str = "Bad req_comp";
        return NULL;
    }

    // Skip the image ID field and any colour map (ignored for true-colour images).
    const size_t colormap_bytes = (size_t)stbi__le16(&header[5]) * (((size_t)header[7] + 7) / 8);
    const size_t skip = (size_t)header[0] + (header[1] ? colormap_bytes : 0);
    if (skip > file_bytes - 18 || (skip > 0 && fseek(f, (long)skip, SEEK_CUR) != 0))
    {
        stbi__failure_reason_str = "Truncated TGA";
        return NULL;
    }
    const size_t payload_bytes = file_bytes - 18 - skip;

    const size_t pixel_count = (size_t)width * (size_t)height;
    // An uncompressed image must be fully present before anything is allocated.
    // An RLE packet can expand to at most 128 pixels, so it needs at least one
    // header byte plus one pixel per 128 pixels.
    const size_t min_payload = (image_type == 2) ? pixel_count * (size_t)src_channels
                                                 : ((pixel_count + 127) / 128) * (1 + (size_t)src_channels);
    if (payload_bytes < min_payload)
    {
        stbi__failure_reason_str = "Truncated TGA";
        return NULL;
    }

    stbi_uc* output = (stbi_uc*)malloc(pixel_count * (size_t)out_channels);
    if (!output)
    {
        stbi__failure_reason_str = "Out of memory";
        return NULL;
    }

    size_t pixels_read = 0;
    while (pixels_read < pixel_count)
    {
        size_t run = 1;
        int repeat = 0;
        if (image_type == 10)
        {
            unsigned char packet;
            if (fread(&packet, 1, 1, f) != 1)
                break;
            run = (size_t)(packet & 0x7F) + 1;
            repeat = (packet & 0x80) != 0;
        }

        unsigned char pixel[4] = {0, 0, 0, 255};
        size_t j = 0;
        for (; j < run && pixels_read < pixel_count; ++j, ++pixels_read)
        {
            if ((j == 0 || !repeat) && fread(pixel, 1, (size_t)src_channels, f) != (size_t)src_channels)
                break;
            stbi__store_pixel(output + pixels_read * (size_t)out_channels, out_channels, pixel[2], pixel[1],
                              pixel[0], (src_channels == 4) ? pixel[3] : 255);
        }
        if (j < run && pixels_read < pixel_count)
            break; // short read inside a packet
    }

    // A truncated stream would otherwise return uninitialised heap bytes as pixels.
    if (pixels_read != pixel_count)
    {
        free(output);
        stbi__failure_reason_str = "Truncated TGA";
        return NULL;
    }

    *x = width;
    *y = height;
    *comp = src_channels;

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
    unsigned char sig[8] = {0};
    size_t sig_read = fread(sig, 1, 8, f);
    if (fseek(f, 0, SEEK_SET) != 0)
    {
        stbi__failure_reason_str = "Unable to rewind file";
        return NULL;
    }

    if (sig_read < 3)
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
    if (buffer && len >= 2 && buffer[0] == 'B' && buffer[1] == 'M')
    {
        FILE* tmp = tmpfile();
        if (!tmp)
        {
            stbi__failure_reason_str = "Failed to create temp file";
            return NULL;
        }
        if (fwrite(buffer, 1, (size_t)len, tmp) != (size_t)len || fseek(tmp, 0, SEEK_SET) != 0)
        {
            fclose(tmp);
            stbi__failure_reason_str = "Failed to stage BMP";
            return NULL;
        }
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
