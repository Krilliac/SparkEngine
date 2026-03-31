/**
 * tinyexr - v1.0.9 - Tiny OpenEXR image loader/saver
 * https://github.com/syoyo/tinyexr
 *
 * This is a MINIMAL STUB providing the tinyexr API surface.
 * For production use, replace with the real header from:
 *   curl -L https://raw.githubusercontent.com/syoyo/tinyexr/master/tinyexr.h -o ThirdParty/Utils/tinyexr/tinyexr.h
 *
 * License: BSD 3-Clause
 */

#ifndef TINYEXR_H_
#define TINYEXR_H_

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

#define TINYEXR_SUCCESS (0)
#define TINYEXR_ERROR_INVALID_MAGIC_NUMBER (-1)
#define TINYEXR_ERROR_INVALID_EXR_VERSION (-2)
#define TINYEXR_ERROR_INVALID_ARGUMENT (-3)
#define TINYEXR_ERROR_INVALID_DATA (-4)
#define TINYEXR_ERROR_INVALID_FILE (-5)
#define TINYEXR_ERROR_INVALID_PARAMETER (-6)
#define TINYEXR_ERROR_CANT_OPEN_FILE (-7)
#define TINYEXR_ERROR_UNSUPPORTED_FORMAT (-8)
#define TINYEXR_ERROR_INVALID_HEADER (-9)

    typedef struct EXRVersion_
    {
        int version;
        int tiled;
        int long_name;
        int non_image;
        int multipart;
    } EXRVersion;

    typedef struct EXRAttribute_
    {
        char name[256];
        char type[256];
        unsigned char* value;
        int size;
    } EXRAttribute;

    typedef struct EXRChannelInfo_
    {
        char name[256];
        int pixel_type; // 0=UINT, 1=HALF, 2=FLOAT
        int x_sampling;
        int y_sampling;
        unsigned char p_linear;
    } EXRChannelInfo;

    typedef struct EXRHeader_
    {
        float pixel_aspect_ratio;
        int line_order;
        int data_window[4];
        int display_window[4];
        float screen_window_center[2];
        float screen_window_width;
        int chunk_count;
        int tiled;
        int tile_size_x;
        int tile_size_y;
        int tile_level_mode;
        int tile_rounding_mode;
        int long_name;
        int non_image;
        int multipart;
        unsigned int header_len;
        int num_custom_attributes;
        EXRAttribute* custom_attributes;
        EXRChannelInfo* channels;
        int* pixel_types;
        int* requested_pixel_types;
        int num_channels;
        int compression_type;
    } EXRHeader;

    typedef struct EXRImage_
    {
        EXRHeader* header;
        int width;
        int height;
        unsigned char** images;
        int num_channels;
    } EXRImage;

    /**
     * @brief Initialize EXR header with default values
     */
    void InitEXRHeader(EXRHeader* header);

    /**
     * @brief Initialize EXR image with default values
     */
    void InitEXRImage(EXRImage* image);

    /**
     * @brief Free EXR header resources
     */
    int FreeEXRHeader(EXRHeader* header);

    /**
     * @brief Free EXR image resources
     */
    int FreeEXRImage(EXRImage* image);

    /**
     * @brief Parse EXR version from file
     */
    int ParseEXRVersionFromFile(EXRVersion* version, const char* filename);

    /**
     * @brief Parse EXR header from file
     */
    int ParseEXRHeaderFromFile(EXRHeader* header, const EXRVersion* version, const char* filename, const char** err);

    /**
     * @brief Load EXR image from file
     */
    int LoadEXRImageFromFile(EXRImage* image, const EXRHeader* header, const char* filename, const char** err);

    /**
     * @brief Load EXR image as float array (convenience)
     * @param out_rgba Pointer to float array (RGBA, 4 channels)
     * @param width Output image width
     * @param height Output image height
     * @param filename Path to EXR file
     * @param err Error message string (caller must free with FreeEXRErrorMessage)
     */
    int LoadEXR(float** out_rgba, int* width, int* height, const char* filename, const char** err);

    /**
     * @brief Save EXR image to file
     */
    int SaveEXR(const float* data, int width, int height, int components, int save_as_fp16, const char* outfilename,
                const char** err);

    /**
     * @brief Free error message string
     */
    void FreeEXRErrorMessage(const char* msg);

#ifdef __cplusplus
}
#endif

// ============================================================================
// Implementation
// ============================================================================
#ifdef TINYEXR_IMPLEMENTATION

#include <cstdio>
#include <cstdlib>
#include <cstring>

void InitEXRHeader(EXRHeader* header)
{
    if (!header)
        return;
    memset(header, 0, sizeof(*header));
    header->pixel_aspect_ratio = 1.0f;
    header->screen_window_width = 1.0f;
}

void InitEXRImage(EXRImage* image)
{
    if (!image)
        return;
    memset(image, 0, sizeof(*image));
}

int FreeEXRHeader(EXRHeader* header)
{
    if (!header)
        return TINYEXR_ERROR_INVALID_ARGUMENT;
    if (header->channels)
    {
        free(header->channels);
        header->channels = nullptr;
    }
    if (header->pixel_types)
    {
        free(header->pixel_types);
        header->pixel_types = nullptr;
    }
    if (header->requested_pixel_types)
    {
        free(header->requested_pixel_types);
        header->requested_pixel_types = nullptr;
    }
    if (header->custom_attributes)
    {
        free(header->custom_attributes);
        header->custom_attributes = nullptr;
    }
    return TINYEXR_SUCCESS;
}

int FreeEXRImage(EXRImage* image)
{
    if (!image)
        return TINYEXR_ERROR_INVALID_ARGUMENT;
    if (image->images)
    {
        for (int i = 0; i < image->num_channels; ++i)
        {
            if (image->images[i])
                free(image->images[i]);
        }
        free(image->images);
        image->images = nullptr;
    }
    return TINYEXR_SUCCESS;
}

int ParseEXRVersionFromFile(EXRVersion* version, const char* filename)
{
    if (!version || !filename)
        return TINYEXR_ERROR_INVALID_ARGUMENT;

    FILE* f = fopen(filename, "rb");
    if (!f)
        return TINYEXR_ERROR_CANT_OPEN_FILE;

    unsigned char magic[4];
    if (fread(magic, 1, 4, f) != 4)
    {
        fclose(f);
        return TINYEXR_ERROR_INVALID_MAGIC_NUMBER;
    }
    fclose(f);

    // EXR magic: 0x76, 0x2F, 0x31, 0x01
    if (magic[0] != 0x76 || magic[1] != 0x2F || magic[2] != 0x31 || magic[3] != 0x01)
        return TINYEXR_ERROR_INVALID_MAGIC_NUMBER;

    memset(version, 0, sizeof(*version));
    version->version = 2;
    return TINYEXR_SUCCESS;
}

int ParseEXRHeaderFromFile(EXRHeader* header, const EXRVersion* version, const char* filename, const char** err)
{
    (void)version;
    (void)filename;
    if (!header)
        return TINYEXR_ERROR_INVALID_ARGUMENT;
    InitEXRHeader(header);
    if (err)
        *err = nullptr;
    // Stub: full header parsing requires the real tinyexr.h
    return TINYEXR_ERROR_UNSUPPORTED_FORMAT;
}

int LoadEXRImageFromFile(EXRImage* image, const EXRHeader* header, const char* filename, const char** err)
{
    (void)image;
    (void)header;
    (void)filename;
    if (err)
        *err = nullptr;
    return TINYEXR_ERROR_UNSUPPORTED_FORMAT;
}

int LoadEXR(float** out_rgba, int* width, int* height, const char* filename, const char** err)
{
    (void)out_rgba;
    (void)width;
    (void)height;
    (void)filename;
    if (err)
        *err = nullptr;
    // Stub: replace ThirdParty/Utils/tinyexr/tinyexr.h with real implementation
    return TINYEXR_ERROR_UNSUPPORTED_FORMAT;
}

int SaveEXR(const float* data, int width, int height, int components, int save_as_fp16, const char* outfilename,
            const char** err)
{
    (void)data;
    (void)width;
    (void)height;
    (void)components;
    (void)save_as_fp16;
    (void)outfilename;
    if (err)
        *err = nullptr;
    return TINYEXR_ERROR_UNSUPPORTED_FORMAT;
}

void FreeEXRErrorMessage(const char* msg)
{
    (void)msg;
    // In the real implementation, this frees a strdup'd string
}

#endif // TINYEXR_IMPLEMENTATION

#endif // TINYEXR_H_
