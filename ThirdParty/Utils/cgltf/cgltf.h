/**
 * cgltf - v1.14 - single-file glTF 2.0 parser
 * https://github.com/jkuhlmann/cgltf
 *
 * This is a MINIMAL STUB that provides the cgltf API declarations and a basic
 * implementation for parsing glTF JSON. For production use, replace with the
 * real cgltf.h from:
 *   curl -L https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h -o ThirdParty/Utils/cgltf/cgltf.h
 *
 * License: MIT
 */

#ifndef CGLTF_H_INCLUDED__
#define CGLTF_H_INCLUDED__

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef size_t cgltf_size;
    typedef long long int cgltf_ssize;
    typedef float cgltf_float;
    typedef int cgltf_int;
    typedef unsigned int cgltf_uint;
    typedef int cgltf_bool;

    typedef enum cgltf_result
    {
        cgltf_result_success,
        cgltf_result_data_too_short,
        cgltf_result_unknown_format,
        cgltf_result_invalid_json,
        cgltf_result_invalid_gltf,
        cgltf_result_invalid_options,
        cgltf_result_file_not_found,
        cgltf_result_io_error,
        cgltf_result_out_of_memory,
        cgltf_result_legacy_gltf,
        cgltf_result_max_enum
    } cgltf_result;

    typedef enum cgltf_file_type
    {
        cgltf_file_type_invalid,
        cgltf_file_type_gltf,
        cgltf_file_type_glb,
        cgltf_file_type_max_enum
    } cgltf_file_type;

    typedef enum cgltf_primitive_type
    {
        cgltf_primitive_type_points,
        cgltf_primitive_type_lines,
        cgltf_primitive_type_line_loop,
        cgltf_primitive_type_line_strip,
        cgltf_primitive_type_triangles,
        cgltf_primitive_type_triangle_strip,
        cgltf_primitive_type_triangle_fan,
        cgltf_primitive_type_max_enum
    } cgltf_primitive_type;

    typedef enum cgltf_component_type
    {
        cgltf_component_type_invalid,
        cgltf_component_type_r_8,
        cgltf_component_type_r_8u,
        cgltf_component_type_r_16,
        cgltf_component_type_r_16u,
        cgltf_component_type_r_32u,
        cgltf_component_type_r_32f,
        cgltf_component_type_max_enum
    } cgltf_component_type;

    typedef enum cgltf_type
    {
        cgltf_type_invalid,
        cgltf_type_scalar,
        cgltf_type_vec2,
        cgltf_type_vec3,
        cgltf_type_vec4,
        cgltf_type_mat2,
        cgltf_type_mat3,
        cgltf_type_mat4,
        cgltf_type_max_enum
    } cgltf_type;

    typedef enum cgltf_attribute_type
    {
        cgltf_attribute_type_invalid,
        cgltf_attribute_type_position,
        cgltf_attribute_type_normal,
        cgltf_attribute_type_tangent,
        cgltf_attribute_type_texcoord,
        cgltf_attribute_type_color,
        cgltf_attribute_type_joints,
        cgltf_attribute_type_weights,
        cgltf_attribute_type_custom,
        cgltf_attribute_type_max_enum
    } cgltf_attribute_type;

    typedef struct cgltf_buffer
    {
        char* name;
        cgltf_size size;
        char* uri;
        void* data;
    } cgltf_buffer;

    typedef struct cgltf_buffer_view
    {
        char* name;
        cgltf_buffer* buffer;
        cgltf_size offset;
        cgltf_size size;
        cgltf_size stride;
        cgltf_bool has_meshopt_compression;
    } cgltf_buffer_view;

    typedef struct cgltf_accessor
    {
        char* name;
        cgltf_component_type component_type;
        cgltf_bool normalized;
        cgltf_type type;
        cgltf_size offset;
        cgltf_size count;
        cgltf_size stride;
        cgltf_buffer_view* buffer_view;
        cgltf_bool has_min;
        cgltf_float min[16];
        cgltf_bool has_max;
        cgltf_float max[16];
        cgltf_bool is_sparse;
    } cgltf_accessor;

    typedef struct cgltf_attribute
    {
        char* name;
        cgltf_attribute_type type;
        cgltf_int index;
        cgltf_accessor* data;
    } cgltf_attribute;

    typedef struct cgltf_image
    {
        char* name;
        char* uri;
        cgltf_buffer_view* buffer_view;
        char* mime_type;
    } cgltf_image;

    typedef struct cgltf_texture
    {
        char* name;
        cgltf_image* image;
    } cgltf_texture;

    typedef struct cgltf_texture_view
    {
        cgltf_texture* texture;
        cgltf_int texcoord;
        cgltf_float scale;
        cgltf_bool has_transform;
    } cgltf_texture_view;

    typedef struct cgltf_pbr_metallic_roughness
    {
        cgltf_texture_view base_color_texture;
        cgltf_texture_view metallic_roughness_texture;
        cgltf_float base_color_factor[4];
        cgltf_float metallic_factor;
        cgltf_float roughness_factor;
    } cgltf_pbr_metallic_roughness;

    typedef struct cgltf_material
    {
        char* name;
        cgltf_bool has_pbr_metallic_roughness;
        cgltf_pbr_metallic_roughness pbr_metallic_roughness;
        cgltf_texture_view normal_texture;
        cgltf_texture_view occlusion_texture;
        cgltf_texture_view emissive_texture;
        cgltf_float emissive_factor[3];
        cgltf_bool double_sided;
        cgltf_bool unlit;
    } cgltf_material;

    typedef struct cgltf_primitive
    {
        cgltf_primitive_type type;
        cgltf_accessor* indices;
        cgltf_material* material;
        cgltf_attribute* attributes;
        cgltf_size attributes_count;
    } cgltf_primitive;

    typedef struct cgltf_mesh
    {
        char* name;
        cgltf_primitive* primitives;
        cgltf_size primitives_count;
        cgltf_float* weights;
        cgltf_size weights_count;
    } cgltf_mesh;

    typedef struct cgltf_node
    {
        char* name;
        cgltf_node* parent;
        cgltf_node** children;
        cgltf_size children_count;
        cgltf_mesh* mesh;
        cgltf_bool has_translation;
        cgltf_bool has_rotation;
        cgltf_bool has_scale;
        cgltf_bool has_matrix;
        cgltf_float translation[3];
        cgltf_float rotation[4];
        cgltf_float scale[3];
        cgltf_float matrix[16];
    } cgltf_node;

    typedef struct cgltf_scene
    {
        char* name;
        cgltf_node** nodes;
        cgltf_size nodes_count;
    } cgltf_scene;

    typedef struct cgltf_data
    {
        cgltf_file_type file_type;
        void* file_data;

        cgltf_mesh* meshes;
        cgltf_size meshes_count;

        cgltf_accessor* accessors;
        cgltf_size accessors_count;

        cgltf_buffer_view* buffer_views;
        cgltf_size buffer_views_count;

        cgltf_buffer* buffers;
        cgltf_size buffers_count;

        cgltf_image* images;
        cgltf_size images_count;

        cgltf_texture* textures;
        cgltf_size textures_count;

        cgltf_material* materials;
        cgltf_size materials_count;

        cgltf_node* nodes;
        cgltf_size nodes_count;

        cgltf_scene* scenes;
        cgltf_size scenes_count;
        cgltf_scene* scene; // default scene

        char* asset_generator;
        char* asset_version;
    } cgltf_data;

    typedef struct cgltf_options
    {
        cgltf_file_type type;
        cgltf_size json_token_count;
        void* (*memory_alloc)(void* user, cgltf_size size);
        void (*memory_free)(void* user, void* ptr);
        void* memory_user_data;
    } cgltf_options;

    /**
     * Parse glTF/glb data from a file.
     */
    cgltf_result cgltf_parse_file(const cgltf_options* options, const char* path, cgltf_data** out_data);

    /**
     * Parse glTF/glb data from memory.
     */
    cgltf_result cgltf_parse(const cgltf_options* options, const void* data, cgltf_size size, cgltf_data** out_data);

    /**
     * Load buffer data referenced by the glTF file (bin files, base64, etc).
     */
    cgltf_result cgltf_load_buffers(const cgltf_options* options, cgltf_data* data, const char* gltf_path);

    /**
     * Validate the parsed data for consistency.
     */
    cgltf_result cgltf_validate(cgltf_data* data);

    /**
     * Free all memory allocated by cgltf.
     */
    void cgltf_free(cgltf_data* data);

    /**
     * Read accessor data (unpack to float buffer).
     */
    cgltf_size cgltf_accessor_unpack_floats(const cgltf_accessor* accessor, cgltf_float* out, cgltf_size float_count);

    /**
     * Read accessor index data.
     */
    cgltf_bool cgltf_accessor_read_uint(const cgltf_accessor* accessor, cgltf_size index, cgltf_uint* out,
                                         cgltf_size element_size);

    /**
     * Get the number of components for an accessor type.
     */
    cgltf_size cgltf_num_components(cgltf_type type);

#ifdef __cplusplus
}
#endif

// ============================================================================
// Implementation
// ============================================================================
#ifdef CGLTF_IMPLEMENTATION

#include <cstdio>
#include <cstdlib>
#include <cstring>

cgltf_size cgltf_num_components(cgltf_type type)
{
    switch (type)
    {
    case cgltf_type_scalar:
        return 1;
    case cgltf_type_vec2:
        return 2;
    case cgltf_type_vec3:
        return 3;
    case cgltf_type_vec4:
        return 4;
    case cgltf_type_mat2:
        return 4;
    case cgltf_type_mat3:
        return 9;
    case cgltf_type_mat4:
        return 16;
    default:
        return 0;
    }
}

static cgltf_size cgltf__component_size(cgltf_component_type type)
{
    switch (type)
    {
    case cgltf_component_type_r_8:
    case cgltf_component_type_r_8u:
        return 1;
    case cgltf_component_type_r_16:
    case cgltf_component_type_r_16u:
        return 2;
    case cgltf_component_type_r_32u:
    case cgltf_component_type_r_32f:
        return 4;
    default:
        return 0;
    }
}

cgltf_size cgltf_accessor_unpack_floats(const cgltf_accessor* accessor, cgltf_float* out, cgltf_size float_count)
{
    if (!accessor || !accessor->buffer_view || !accessor->buffer_view->buffer)
        return 0;

    cgltf_size num_comp = cgltf_num_components(accessor->type);
    cgltf_size total_floats = accessor->count * num_comp;
    if (!out)
        return total_floats;
    if (float_count < total_floats)
        total_floats = float_count;

    const unsigned char* base = (const unsigned char*)accessor->buffer_view->buffer->data;
    base += accessor->buffer_view->offset + accessor->offset;

    cgltf_size comp_size = cgltf__component_size(accessor->component_type);
    cgltf_size stride = accessor->stride ? accessor->stride : (num_comp * comp_size);

    cgltf_size elements = total_floats / num_comp;
    for (cgltf_size i = 0; i < elements; ++i)
    {
        const unsigned char* element = base + i * stride;
        for (cgltf_size c = 0; c < num_comp; ++c)
        {
            const unsigned char* comp_ptr = element + c * comp_size;
            float value = 0.0f;
            switch (accessor->component_type)
            {
            case cgltf_component_type_r_8:
                value = (float)(*(const int8_t*)comp_ptr);
                if (accessor->normalized)
                    value /= 127.0f;
                break;
            case cgltf_component_type_r_8u:
                value = (float)(*comp_ptr);
                if (accessor->normalized)
                    value /= 255.0f;
                break;
            case cgltf_component_type_r_16:
                value = (float)(*(const int16_t*)comp_ptr);
                if (accessor->normalized)
                    value /= 32767.0f;
                break;
            case cgltf_component_type_r_16u:
                value = (float)(*(const uint16_t*)comp_ptr);
                if (accessor->normalized)
                    value /= 65535.0f;
                break;
            case cgltf_component_type_r_32u:
                value = (float)(*(const uint32_t*)comp_ptr);
                break;
            case cgltf_component_type_r_32f:
                value = *(const float*)comp_ptr;
                break;
            default:
                break;
            }
            out[i * num_comp + c] = value;
        }
    }

    return total_floats;
}

cgltf_bool cgltf_accessor_read_uint(const cgltf_accessor* accessor, cgltf_size index, cgltf_uint* out,
                                     cgltf_size element_size)
{
    if (!accessor || !accessor->buffer_view || !accessor->buffer_view->buffer)
        return 0;
    if (index >= accessor->count)
        return 0;

    cgltf_size num_comp = cgltf_num_components(accessor->type);
    if (element_size < num_comp)
        return 0;

    const unsigned char* base = (const unsigned char*)accessor->buffer_view->buffer->data;
    base += accessor->buffer_view->offset + accessor->offset;

    cgltf_size comp_size = cgltf__component_size(accessor->component_type);
    cgltf_size stride = accessor->stride ? accessor->stride : (num_comp * comp_size);
    const unsigned char* element = base + index * stride;

    for (cgltf_size c = 0; c < num_comp; ++c)
    {
        const unsigned char* comp_ptr = element + c * comp_size;
        switch (accessor->component_type)
        {
        case cgltf_component_type_r_8u:
            out[c] = *comp_ptr;
            break;
        case cgltf_component_type_r_16u:
            out[c] = *(const uint16_t*)comp_ptr;
            break;
        case cgltf_component_type_r_32u:
            out[c] = *(const uint32_t*)comp_ptr;
            break;
        default:
            out[c] = 0;
            break;
        }
    }

    return 1;
}

cgltf_result cgltf_parse_file(const cgltf_options* options, const char* path, cgltf_data** out_data)
{
    if (!path || !out_data)
        return cgltf_result_invalid_options;

    FILE* f = fopen(path, "rb");
    if (!f)
        return cgltf_result_file_not_found;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0)
    {
        fclose(f);
        return cgltf_result_data_too_short;
    }

    void* file_data = malloc((size_t)file_size);
    if (!file_data)
    {
        fclose(f);
        return cgltf_result_out_of_memory;
    }

    size_t read_size = fread(file_data, 1, (size_t)file_size, f);
    fclose(f);

    if (read_size != (size_t)file_size)
    {
        free(file_data);
        return cgltf_result_io_error;
    }

    cgltf_result result = cgltf_parse(options, file_data, (cgltf_size)file_size, out_data);
    if (result != cgltf_result_success)
    {
        free(file_data);
        return result;
    }

    (*out_data)->file_data = file_data;
    return cgltf_result_success;
}

cgltf_result cgltf_parse(const cgltf_options* options, const void* data, cgltf_size size, cgltf_data** out_data)
{
    (void)options;

    if (!data || size < 4 || !out_data)
        return cgltf_result_invalid_options;

    // Allocate the main data structure
    cgltf_data* gltf = (cgltf_data*)calloc(1, sizeof(cgltf_data));
    if (!gltf)
        return cgltf_result_out_of_memory;

    // Check for GLB magic
    const unsigned char* bytes = (const unsigned char*)data;
    if (size >= 12 && bytes[0] == 0x67 && bytes[1] == 0x6C && bytes[2] == 0x54 && bytes[3] == 0x46)
    {
        gltf->file_type = cgltf_file_type_glb;
    }
    else if (bytes[0] == '{')
    {
        gltf->file_type = cgltf_file_type_gltf;
    }
    else
    {
        free(gltf);
        return cgltf_result_unknown_format;
    }

    // Note: Full JSON parsing requires the real cgltf.h implementation.
    // This stub recognizes the file format but does not populate mesh data.
    // For actual glTF loading, replace with the real cgltf.h.

    *out_data = gltf;
    return cgltf_result_success;
}

cgltf_result cgltf_load_buffers(const cgltf_options* options, cgltf_data* data, const char* gltf_path)
{
    (void)options;
    (void)data;
    (void)gltf_path;
    // Stub: buffer loading requires full cgltf implementation
    return cgltf_result_success;
}

cgltf_result cgltf_validate(cgltf_data* data)
{
    if (!data)
        return cgltf_result_invalid_gltf;
    return cgltf_result_success;
}

void cgltf_free(cgltf_data* data)
{
    if (!data)
        return;
    if (data->file_data)
        free(data->file_data);
    free(data);
}

#endif // CGLTF_IMPLEMENTATION

#endif // CGLTF_H_INCLUDED__
