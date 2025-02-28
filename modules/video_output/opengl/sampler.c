/*****************************************************************************
 * sampler.c
 *****************************************************************************
 * Copyright (C) 2020 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "sampler_priv.h"

#include <vlc_common.h>
#include <vlc_memstream.h>
#include <vlc_opengl.h>

#ifdef HAVE_LIBPLACEBO
#include <libplacebo/shaders.h>
#include <libplacebo/shaders/colorspace.h>
#include "../libplacebo/utils.h"
#endif

#include "gl_api.h"
#include "gl_common.h"
#include "gl_util.h"
#include "interop.h"

struct vlc_gl_sampler_priv {
    struct vlc_gl_sampler sampler;

    struct vlc_gl_t *gl;
    const struct vlc_gl_api *api;
    const opengl_vtable_t *vt; /* for convenience, same as &api->vt */

    struct {
        GLint Textures[PICTURE_PLANE_MAX];
        GLint TexSizes[PICTURE_PLANE_MAX]; /* for GL_TEXTURE_RECTANGLE */
        GLint ConvMatrix;
        GLint *pl_vars; /* for pl_sh_res */
    } uloc;

    bool yuv_color;
    GLfloat conv_matrix[4*4];

    /* libplacebo context */
    struct pl_context *pl_ctx;
    struct pl_shader *pl_sh;
    const struct pl_shader_res *pl_sh_res;

    GLsizei tex_widths[PICTURE_PLANE_MAX];
    GLsizei tex_heights[PICTURE_PLANE_MAX];

    GLsizei visible_widths[PICTURE_PLANE_MAX];
    GLsizei visible_heights[PICTURE_PLANE_MAX];

    GLuint textures[PICTURE_PLANE_MAX];

    GLenum tex_target;

    struct {
        unsigned int i_x_offset;
        unsigned int i_y_offset;
        unsigned int i_visible_width;
        unsigned int i_visible_height;
    } last_source;

    /* A sampler supports 2 kinds of input.
     *  - created with _NewFromInterop(), it receives input pictures from VLC
     *    (picture_t) via _UpdatePicture();
     *  - created with _NewFromTexture2D() (interop is NULL), it receives
     *    directly OpenGL textures via _UpdateTextures().
     */
    struct vlc_gl_interop *interop;

    /* Only used for "direct" sampler (when interop == NULL) */
    video_format_t direct_fmt;

    /* If set, vlc_texture() exposes a single plane (without chroma
     * conversion), selected by vlc_gl_sampler_SetCurrentPlane(). */
    bool expose_planes;
    unsigned plane;

    /* All matrices below are stored in column-major order. */

    float mtx_orientation[3*2];
    float mtx_coords_map[3*2];

    float mtx_transform[3*2];
    bool mtx_transform_defined;

    /**
     * tex_coords =   mtx_all  × pic_coords
     *
     *  / tex_x \    / a b c \   / pic_x \
     *  \ tex_y / =  \ d e f / × | pic_y |
     *                           \   1   /
     *
     * Semantically, it represents the result of:
     *
     *     get_transform_matrix() * mtx_coords_map * mtx_orientation
     *
     * (The intermediate matrices are implicitly expanded to 3x3 with [0 0 1]
     * as the last row.)
     *
     * It is stored in column-major order: [a, d, b, e, c, f].
     */
    float mtx_all[3*2];
    bool mtx_all_defined;
    bool mtx_all_has_changed; /* since the previous picture */
};

static inline struct vlc_gl_sampler_priv *
PRIV(struct vlc_gl_sampler *sampler)
{
    return container_of(sampler, struct vlc_gl_sampler_priv, sampler);
}

static const float MATRIX_COLOR_RANGE_LIMITED[4*3] = {
    255.0/219,         0,         0, -255.0/219 *  16.0/255,
            0, 255.0/224,         0, -255.0/224 * 128.0/255,
            0,         0, 255.0/224, -255.0/224 * 128.0/255,
};

static const float MATRIX_COLOR_RANGE_FULL[4*3] = {
    1, 0, 0,          0,
    0, 1, 0, -128.0/255,
    0, 0, 1, -128.0/255,
};

/*
 * Construct the transformation matrix from the luma weight of the red and blue
 * component (the green component is deduced).
 */
#define MATRIX_YUV_TO_RGB(KR, KB) \
    MATRIX_YUV_TO_RGB_(KR, (1-(KR)-(KB)), KB)

/*
 * Construct the transformation matrix from the luma weight of the RGB
 * components.
 *
 * KR: luma weight of the red component
 * KG: luma weight of the green component
 * KB: luma weight of the blue component
 *
 * By definition, KR + KG + KB == 1.
 *
 * Ref: <https://en.wikipedia.org/wiki/YCbCr#ITU-R_BT.601_conversion>
 * Ref: libplacebo: src/colorspace.c:luma_coeffs()
 * */
#define MATRIX_YUV_TO_RGB_(KR, KG, KB) { \
    1,                         0,              2*(1.0-(KR)), \
    1, -2*(1.0-(KB))*((KB)/(KG)), -2*(1.0-(KR))*((KR)/(KG)), \
    1,              2*(1.0-(KB)),                         0, \
}

static const float MATRIX_BT601[3*3] = MATRIX_YUV_TO_RGB(0.299, 0.114);
static const float MATRIX_BT709[3*3] = MATRIX_YUV_TO_RGB(0.2126, 0.0722);
static const float MATRIX_BT2020[3*3] = MATRIX_YUV_TO_RGB(0.2627, 0.0593);

static void
init_conv_matrix(float conv_matrix_out[],
                 video_color_space_t color_space,
                 video_color_range_t color_range)
{
    const float *space_matrix;
    switch (color_space) {
        case COLOR_SPACE_BT601:
            space_matrix = MATRIX_BT601;
            break;
        case COLOR_SPACE_BT2020:
            space_matrix = MATRIX_BT2020;
            break;
        default:
            space_matrix = MATRIX_BT709;
    }

    /* Init the conversion matrix in column-major order (OpenGL expects
     * column-major order by default, and OpenGL ES does not support row-major
     * order at all). */

    const float *range_matrix = color_range == COLOR_RANGE_FULL
                              ? MATRIX_COLOR_RANGE_FULL
                              : MATRIX_COLOR_RANGE_LIMITED;
    /* Multiply the matrices on CPU once for all */
    for (int x = 0; x < 4; ++x)
    {
        for (int y = 0; y < 3; ++y)
        {
            /* Perform intermediate computation in double precision even if the
             * result is in single-precision, to avoid unnecessary errors. */
            double sum = 0;
            for (int k = 0; k < 3; ++k)
                sum += space_matrix[y * 3 + k] * range_matrix[k * 4 + x];
            /* Notice the reversed indices: x is now the row, y is the
             * column. */
            conv_matrix_out[x * 4 + y] = sum;
        }
    }

    /* Add a row to fill a 4x4 matrix (remember it's in column-major order).
     * (non-square matrices are not supported on old OpenGL ES versions) */
    conv_matrix_out[3] = 0;
    conv_matrix_out[7] = 0;
    conv_matrix_out[11] = 0;
    conv_matrix_out[15] = 1;
}

static int
sampler_yuv_base_init(struct vlc_gl_sampler *sampler, vlc_fourcc_t chroma,
                      const vlc_chroma_description_t *desc,
                      video_color_space_t yuv_space)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    /* The current implementation always converts from limited to full range. */
    const video_color_range_t range = COLOR_RANGE_LIMITED;
    float *matrix = priv->conv_matrix;
    init_conv_matrix(matrix, yuv_space, range);

    if (desc->pixel_size == 2)
    {
        if (chroma != VLC_CODEC_P010 && chroma != VLC_CODEC_P016) {
            /* Do a bit shift if samples are stored on LSB. */
            float yuv_range_correction = (float)((1 << 16) - 1)
                                         / ((1 << desc->pixel_bits) - 1);
            /* We want to transform the input color (y, u, v, 1) to
             * (r*y, r*u, r*v, 1), where r = yuv_range_correction.
             *
             * This can be done by left-multiplying the color vector by a
             * matrix R:
             *
             *                 R
             *  / r*y \   / r 0 0 0 \   / y \
             *  | r*u | = | 0 r 0 0 | * | u |
             *  | r*v |   | 0 0 r 0 |   | v |
             *  \  1  /   \ 0 0 0 1 /   \ 1 /
             *
             * Combine this transformation with the color conversion matrix:
             *
             *     matrix := matrix * R
             *
             * This is equivalent to multipying the 3 first rows by r
             * (yuv_range_conversion).
             */
            for (int i = 0; i < 4*3; ++i)
                matrix[i] *= yuv_range_correction;
        }
    }

    priv->yuv_color = true;

    /* Some formats require to swap the U and V components.
     *
     * This can be done by left-multiplying the color vector by a matrix S:
     *
     *               S
     *  / y \   / 1 0 0 0 \   / y \
     *  | v | = | 0 0 1 0 | * | u |
     *  | u |   | 0 1 0 0 |   | v |
     *  \ 1 /   \ 0 0 0 1 /   \ 1 /
     *
     * Combine this transformation with the color conversion matrix:
     *
     *     matrix := matrix * S
     *
     * This is equivalent to swap columns 1 and 2.
     */
    bool swap_uv = chroma == VLC_CODEC_YV12 || chroma == VLC_CODEC_YV9 ||
                   chroma == VLC_CODEC_NV21;
    if (swap_uv)
    {
        /* Remember, the matrix in column-major order */
        float tmp[4];
        /* tmp <- column1 */
        memcpy(tmp, matrix + 4, sizeof(tmp));
        /* column1 <- column2 */
        memcpy(matrix + 4, matrix + 8, sizeof(tmp));
        /* column2 <- tmp */
        memcpy(matrix + 8, tmp, sizeof(tmp));
    }
    return VLC_SUCCESS;
}

static void
sampler_base_fetch_locations(struct vlc_gl_sampler *sampler, GLuint program)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    const opengl_vtable_t *vt = priv->vt;

    if (priv->yuv_color)
    {
        priv->uloc.ConvMatrix = vt->GetUniformLocation(program, "ConvMatrix");
        assert(priv->uloc.ConvMatrix != -1);
    }

    assert(sampler->tex_count < 10); /* to guarantee variable names length */
    for (unsigned int i = 0; i < sampler->tex_count; ++i)
    {
        char name[sizeof("Textures[X]")];

        snprintf(name, sizeof(name), "Textures[%1u]", i);
        priv->uloc.Textures[i] = vt->GetUniformLocation(program, name);
        assert(priv->uloc.Textures[i] != -1);

        if (priv->tex_target == GL_TEXTURE_RECTANGLE)
        {
            snprintf(name, sizeof(name), "TexSizes[%1u]", i);
            priv->uloc.TexSizes[i] = vt->GetUniformLocation(program, name);
            assert(priv->uloc.TexSizes[i] != -1);
        }
    }

#ifdef HAVE_LIBPLACEBO
    const struct pl_shader_res *res = priv->pl_sh_res;
    for (int i = 0; res && i < res->num_variables; i++) {
        struct pl_shader_var sv = res->variables[i];
        priv->uloc.pl_vars[i] = vt->GetUniformLocation(program, sv.var.name);
    }
#endif
}

static const GLfloat *
GetTransformMatrix(const struct vlc_gl_interop *interop)
{
    const GLfloat *tm = NULL;
    if (interop && interop->ops && interop->ops->get_transform_matrix)
        tm = interop->ops->get_transform_matrix(interop);
    return tm;
}

static void
sampler_base_load(struct vlc_gl_sampler *sampler)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    const opengl_vtable_t *vt = priv->vt;

    if (priv->yuv_color)
        vt->UniformMatrix4fv(priv->uloc.ConvMatrix, 1, GL_FALSE,
                             priv->conv_matrix);

    for (unsigned i = 0; i < sampler->tex_count; ++i)
    {
        vt->Uniform1i(priv->uloc.Textures[i], i);

        assert(priv->textures[i] != 0);
        vt->ActiveTexture(GL_TEXTURE0 + i);
        vt->BindTexture(priv->tex_target, priv->textures[i]);

    }

    if (priv->tex_target == GL_TEXTURE_RECTANGLE)
    {
        for (unsigned i = 0; i < sampler->tex_count; ++i)
            vt->Uniform2f(priv->uloc.TexSizes[i], priv->tex_widths[i],
                          priv->tex_heights[i]);
    }

#ifdef HAVE_LIBPLACEBO
    const struct pl_shader_res *res = priv->pl_sh_res;
    for (int i = 0; res && i < res->num_variables; i++) {
        GLint loc = priv->uloc.pl_vars[i];
        if (loc == -1) // uniform optimized out
            continue;

        struct pl_shader_var sv = res->variables[i];
        struct pl_var var = sv.var;
        // libplacebo doesn't need anything else anyway
        if (var.type != PL_VAR_FLOAT)
            continue;
        if (var.dim_m > 1 && var.dim_m != var.dim_v)
            continue;

        const float *f = sv.data;
        switch (var.dim_m) {
        case 4: vt->UniformMatrix4fv(loc, 1, GL_FALSE, f); break;
        case 3: vt->UniformMatrix3fv(loc, 1, GL_FALSE, f); break;
        case 2: vt->UniformMatrix2fv(loc, 1, GL_FALSE, f); break;

        case 1:
            switch (var.dim_v) {
            case 1: vt->Uniform1f(loc, f[0]); break;
            case 2: vt->Uniform2f(loc, f[0], f[1]); break;
            case 3: vt->Uniform3f(loc, f[0], f[1], f[2]); break;
            case 4: vt->Uniform4f(loc, f[0], f[1], f[2], f[3]); break;
            }
            break;
        }
    }
#endif
}

static void
sampler_xyz12_fetch_locations(struct vlc_gl_sampler *sampler, GLuint program)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    const opengl_vtable_t *vt = priv->vt;

    priv->uloc.Textures[0] = vt->GetUniformLocation(program, "Textures[0]");
    assert(priv->uloc.Textures[0] != -1);
}

static void
sampler_xyz12_load(struct vlc_gl_sampler *sampler)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    const opengl_vtable_t *vt = priv->vt;

    vt->Uniform1i(priv->uloc.Textures[0], 0);

    assert(priv->textures[0] != 0);
    vt->ActiveTexture(GL_TEXTURE0);
    vt->BindTexture(priv->tex_target, priv->textures[0]);
}

static int
xyz12_shader_init(struct vlc_gl_sampler *sampler)
{
    static const struct vlc_gl_sampler_ops ops = {
        .fetch_locations = sampler_xyz12_fetch_locations,
        .load = sampler_xyz12_load,
    };
    sampler->ops = &ops;

    /* Shader for XYZ to RGB correction
     * 3 steps :
     *  - XYZ gamma correction
     *  - XYZ to RGB matrix conversion
     *  - reverse RGB gamma correction
     */
    static const char *template =
        "uniform sampler2D Textures[1];"
        "uniform vec4 xyz_gamma = vec4(2.6);"
        "uniform vec4 rgb_gamma = vec4(1.0/2.2);"
        /* WARN: matrix Is filled column by column (not row !) */
        "uniform mat4 matrix_xyz_rgb = mat4("
        "    3.240454 , -0.9692660, 0.0556434, 0.0,"
        "   -1.5371385,  1.8760108, -0.2040259, 0.0,"
        "    -0.4985314, 0.0415560, 1.0572252,  0.0,"
        "    0.0,      0.0,         0.0,        1.0 "
        " );"

        "vec4 vlc_texture(vec2 tex_coords)\n"
        "{ "
        " vec4 v_in, v_out;"
        " v_in  = texture2D(Textures[0], tex_coords);\n"
        " v_in = pow(v_in, xyz_gamma);"
        " v_out = matrix_xyz_rgb * v_in ;"
        " v_out = pow(v_out, rgb_gamma) ;"
        " v_out = clamp(v_out, 0.0, 1.0) ;"
        " return v_out;"
        "}\n";

    sampler->shader.body = strdup(template);
    if (!sampler->shader.body)
        return VLC_ENOMEM;

    return VLC_SUCCESS;
}

static int
opengl_init_swizzle(struct vlc_gl_sampler *sampler,
                    const char *swizzle_per_tex[],
                    vlc_fourcc_t chroma,
                    const vlc_chroma_description_t *desc)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    GLint oneplane_texfmt;
    if (vlc_gl_StrHasToken(priv->api->extensions, "GL_ARB_texture_rg"))
        oneplane_texfmt = GL_RED;
    else
        oneplane_texfmt = GL_LUMINANCE;

    if (desc->plane_count == 3)
        swizzle_per_tex[0] = swizzle_per_tex[1] = swizzle_per_tex[2] = "r";
    else if (desc->plane_count == 2)
    {
        if (oneplane_texfmt == GL_RED)
        {
            swizzle_per_tex[0] = "r";
            swizzle_per_tex[1] = "rg";
        }
        else
        {
            swizzle_per_tex[0] = "x";
            swizzle_per_tex[1] = "xa";
        }
    }
    else if (desc->plane_count == 1)
    {
        /*
         * Set swizzling in Y1 U V order
         * R  G  B  A
         * U  Y1 V  Y2 => GRB
         * Y1 U  Y2 V  => RGA
         * V  Y1 U  Y2 => GBR
         * Y1 V  Y2 U  => RAG
         */
        switch (chroma)
        {
            case VLC_CODEC_UYVY:
                swizzle_per_tex[0] = "grb";
                break;
            case VLC_CODEC_YUYV:
                swizzle_per_tex[0] = "rga";
                break;
            case VLC_CODEC_VYUY:
                swizzle_per_tex[0] = "gbr";
                break;
            case VLC_CODEC_YVYU:
                swizzle_per_tex[0] = "rag";
                break;
            default:
                assert(!"missing chroma");
                return VLC_EGENERIC;
        }
    }
    return VLC_SUCCESS;
}

static void
InitOrientationMatrix(float matrix[static 3*2], video_orientation_t orientation)
{
/**
 * / C0R0  C1R0  C3R0 \
 * \ C0R1  C1R1  C3R1 /
 *
 * (note that in memory, the matrix is stored in column-major order)
 */
#define MATRIX_SET(C0R0, C1R0, C3R0, \
                   C0R1, C1R1, C3R1) \
    matrix[0*2 + 0] = C0R0; \
    matrix[1*2 + 0] = C1R0; \
    matrix[2*2 + 0] = C3R0; \
    matrix[0*2 + 1] = C0R1; \
    matrix[1*2 + 1] = C1R1; \
    matrix[2*2 + 1] = C3R1;

    /**
     * The following schemas show how the video picture is oriented in the
     * texture, according to the "orientation" value:
     *
     *     video         texture
     *    picture        storage
     *
     *     1---2          2---3
     *     |   |   --->   |   |
     *     4---3          1---4
     *
     * In addition, they show how the orientation transforms video picture
     * coordinates axis (x,y) into texture axis (X,Y):
     *
     *   y         --->         X
     *   |                      |
     *   +---x              Y---+
     *
     * The resulting coordinates undergo the reverse of the transformation
     * applied to the axis, so expressing (x,y) in terms of (X,Y) gives the
     * orientation matrix coefficients.
     */

    switch (orientation) {
        case ORIENT_ROTATED_90:
            /**
             *     1---2          2---3
             *   y |   |   --->   |   | X
             *   | 4---3          1---4 |
             *   +---x              Y---+
             *
             *          x = 1-Y
             *          y = X
             */
                     /* X  Y  1 */
            MATRIX_SET( 0,-1, 1, /* 1-Y */
                        1, 0, 0) /* X */
            break;
        case ORIENT_ROTATED_180:
            /**
             *                      X---+
             *     1---2          3---4 |
             *   y |   |   --->   |   | Y
             *   | 4---3          2---1
             *   +---x
             *
             *          x = 1-X
             *          y = 1-Y
             */
                     /* X  Y  1 */
            MATRIX_SET(-1, 0, 1, /* 1-X */
                        0,-1, 1) /* 1-Y */
            break;
        case ORIENT_ROTATED_270:
            /**
             *                    +---Y
             *     1---2          | 4---1
             *   y |   |   --->   X |   |
             *   | 4---3            3---2
             *   +---x
             *
             *          x = Y
             *          y = 1-X
             */
                     /* X  Y  1 */
            MATRIX_SET( 0, 1, 0, /* Y */
                       -1, 0, 1) /* 1-X */
            break;
        case ORIENT_HFLIPPED:
            /**
             *     1---2          2---1
             *   y |   |   --->   |   | Y
             *   | 4---3          3---4 |
             *   +---x              X---+
             *
             *          x = 1-X
             *          y = Y
             */
                     /* X  Y  1 */
            MATRIX_SET(-1, 0, 1, /* 1-X */
                        0, 1, 0) /* Y */
            break;
        case ORIENT_VFLIPPED:
            /**
             *                    +---X
             *     1---2          | 4---3
             *   y |   |   --->   Y |   |
             *   | 4---3            1---2
             *   +---x
             *
             *          x = X
             *          y = 1-Y
             */
                     /* X  Y  1 */
            MATRIX_SET( 1, 0, 0, /* X */
                        0,-1, 1) /* 1-Y */
            break;
        case ORIENT_TRANSPOSED:
            /**
             *                      Y---+
             *     1---2          1---4 |
             *   y |   |   --->   |   | X
             *   | 4---3          2---3
             *   +---x
             *
             *          x = 1-Y
             *          y = 1-X
             */
                     /* X  Y  1 */
            MATRIX_SET( 0,-1, 1, /* 1-Y */
                       -1, 0, 1) /* 1-X */
            break;
        case ORIENT_ANTI_TRANSPOSED:
            /**
             *     1---2            3---2
             *   y |   |   --->   X |   |
             *   | 4---3          | 4---1
             *   +---x            +---Y
             *
             *          x = Y
             *          y = X
             */
                     /* X  Y  1 */
            MATRIX_SET( 0, 1, 0, /* Y */
                        1, 0, 0) /* X */
            break;
        default:
            break;
    }
}

static void
GetNames(GLenum tex_target, const char **glsl_sampler, const char **texture)
{
    switch (tex_target)
    {
        case GL_TEXTURE_EXTERNAL_OES:
            *glsl_sampler = "samplerExternalOES";
            *texture = "texture2D";
            break;
        case GL_TEXTURE_2D:
            *glsl_sampler = "sampler2D";
            *texture = "texture2D";
            break;
        case GL_TEXTURE_RECTANGLE:
            *glsl_sampler = "sampler2DRect";
            *texture = "texture2DRect";
            break;
        default:
            vlc_assert_unreachable();
    }
}

static int
InitShaderExtensions(struct vlc_gl_sampler *sampler, GLenum tex_target)
{
    if (tex_target == GL_TEXTURE_EXTERNAL_OES)
    {
        sampler->shader.extensions =
            strdup("#extension GL_OES_EGL_image_external : require\n");
        if (!sampler->shader.extensions)
            return VLC_EGENERIC;
    }
    else
        sampler->shader.extensions = NULL;

    return VLC_SUCCESS;
}

static void
sampler_planes_fetch_locations(struct vlc_gl_sampler *sampler, GLuint program)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    const opengl_vtable_t *vt = priv->vt;

    priv->uloc.Textures[0] = vt->GetUniformLocation(program, "Texture");
    assert(priv->uloc.Textures[0] != -1);

    if (priv->tex_target == GL_TEXTURE_RECTANGLE)
    {
        priv->uloc.TexSizes[0] = vt->GetUniformLocation(program, "TexSize");
        assert(priv->uloc.TexSizes[0] != -1);
    }
}

static void
sampler_planes_load(struct vlc_gl_sampler *sampler)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    unsigned plane = priv->plane;

    const opengl_vtable_t *vt = priv->vt;

    vt->Uniform1i(priv->uloc.Textures[0], 0);

    assert(priv->textures[plane] != 0);
    vt->ActiveTexture(GL_TEXTURE0);
    vt->BindTexture(priv->tex_target, priv->textures[plane]);

    if (priv->tex_target == GL_TEXTURE_RECTANGLE)
    {
        vt->Uniform2f(priv->uloc.TexSizes[0], priv->tex_widths[plane],
                      priv->tex_heights[plane]);
    }
}

static int
sampler_planes_init(struct vlc_gl_sampler *sampler)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    GLenum tex_target = priv->tex_target;

    struct vlc_memstream ms;
    if (vlc_memstream_open(&ms))
        return VLC_EGENERIC;

#define ADD(x) vlc_memstream_puts(&ms, x)
#define ADDF(x, ...) vlc_memstream_printf(&ms, x, ##__VA_ARGS__)

    const char *sampler_type;
    const char *texture_fn;
    GetNames(tex_target, &sampler_type, &texture_fn);

    ADDF("uniform %s Texture;\n", sampler_type);

    if (tex_target == GL_TEXTURE_RECTANGLE)
        ADD("uniform vec2 TexSize;\n");

    ADD("vec4 vlc_texture(vec2 tex_coords) {\n");

    if (tex_target == GL_TEXTURE_RECTANGLE)
    {
        /* The coordinates are in texels values, not normalized */
        ADD(" tex_coords = TexSize * tex_coords;\n");
    }

    ADDF("  return %s(Texture, tex_coords);\n", texture_fn);
    ADD("}\n");

#undef ADD
#undef ADDF

    if (vlc_memstream_close(&ms) != 0)
        return VLC_EGENERIC;

    int ret = InitShaderExtensions(sampler, tex_target);
    if (ret != VLC_SUCCESS)
    {
        free(ms.ptr);
        return VLC_EGENERIC;
    }
    sampler->shader.body = ms.ptr;

    static const struct vlc_gl_sampler_ops ops = {
        .fetch_locations = sampler_planes_fetch_locations,
        .load = sampler_planes_load,
    };
    sampler->ops = &ops;

    return VLC_SUCCESS;
}

static int
opengl_fragment_shader_init(struct vlc_gl_sampler *sampler, GLenum tex_target,
                            const video_format_t *fmt, bool expose_planes)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    priv->tex_target = tex_target;
    priv->expose_planes = expose_planes;
    priv->plane = 0;

    vlc_fourcc_t chroma = fmt->i_chroma;
    video_color_space_t yuv_space = fmt->space;
    video_orientation_t orientation = fmt->orientation;

    const char *swizzle_per_tex[PICTURE_PLANE_MAX] = { NULL, };
    const bool is_yuv = vlc_fourcc_IsYUV(chroma);
    int ret;

    const vlc_chroma_description_t *desc = vlc_fourcc_GetChromaDescription(chroma);
    if (desc == NULL)
        return VLC_EGENERIC;

    unsigned tex_count = desc->plane_count;
    sampler->tex_count = tex_count;

    InitOrientationMatrix(priv->mtx_orientation, orientation);

    if (expose_planes)
        return sampler_planes_init(sampler);

    if (chroma == VLC_CODEC_XYZ12)
        return xyz12_shader_init(sampler);

    if (is_yuv)
    {
        ret = sampler_yuv_base_init(sampler, chroma, desc, yuv_space);
        if (ret != VLC_SUCCESS)
            return ret;
        ret = opengl_init_swizzle(sampler, swizzle_per_tex, chroma, desc);
        if (ret != VLC_SUCCESS)
            return ret;
    }

    const char *glsl_sampler, *lookup;
    GetNames(tex_target, &glsl_sampler, &lookup);

    struct vlc_memstream ms;
    if (vlc_memstream_open(&ms) != 0)
        return VLC_EGENERIC;

#define ADD(x) vlc_memstream_puts(&ms, x)
#define ADDF(x, ...) vlc_memstream_printf(&ms, x, ##__VA_ARGS__)

    ADDF("uniform %s Textures[%u];\n", glsl_sampler, tex_count);

#ifdef HAVE_LIBPLACEBO
    if (priv->pl_sh) {
        struct pl_shader *sh = priv->pl_sh;
        struct pl_color_map_params color_params = pl_color_map_default_params;
        color_params.intent = var_InheritInteger(priv->gl, "rendering-intent");
        color_params.tone_mapping_algo = var_InheritInteger(priv->gl, "tone-mapping");
        color_params.tone_mapping_param = var_InheritFloat(priv->gl, "tone-mapping-param");
        color_params.desaturation_strength = var_InheritFloat(priv->gl, "desat-strength");
        color_params.desaturation_exponent = var_InheritFloat(priv->gl, "desat-exponent");
        color_params.desaturation_base = var_InheritFloat(priv->gl, "desat-base");
        color_params.gamut_warning = var_InheritBool(priv->gl, "tone-mapping-warn");

        struct pl_color_space dst_space = pl_color_space_unknown;
        dst_space.primaries = var_InheritInteger(priv->gl, "target-prim");
        dst_space.transfer = var_InheritInteger(priv->gl, "target-trc");

        pl_shader_color_map(sh, &color_params,
                vlc_placebo_ColorSpace(fmt),
                dst_space, NULL, false);

        struct pl_shader_obj *dither_state = NULL;
        int method = var_InheritInteger(priv->gl, "dither-algo");
        if (method >= 0) {

            unsigned out_bits = 0;
            int override = var_InheritInteger(priv->gl, "dither-depth");
            if (override > 0)
                out_bits = override;
            else
            {
                GLint fb_depth = 0;
#if !defined(USE_OPENGL_ES2)
                const opengl_vtable_t *vt = priv->vt;
                /* fetch framebuffer depth (we are already bound to the default one). */
                if (vt->GetFramebufferAttachmentParameteriv != NULL)
                    vt->GetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT,
                                                            GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE,
                                                            &fb_depth);
#endif
                if (fb_depth <= 0)
                    fb_depth = 8;
                out_bits = fb_depth;
            }

            pl_shader_dither(sh, out_bits, &dither_state, &(struct pl_dither_params) {
                .method   = method,
                .lut_size = 4, // avoid too large values, since this gets embedded
            });
        }

        const struct pl_shader_res *res = priv->pl_sh_res = pl_shader_finalize(sh);
        pl_shader_obj_destroy(&dither_state);

        FREENULL(priv->uloc.pl_vars);
        priv->uloc.pl_vars = calloc(res->num_variables, sizeof(GLint));
        for (int i = 0; i < res->num_variables; i++) {
            struct pl_shader_var sv = res->variables[i];
            const char *glsl_type_name = pl_var_glsl_type_name(sv.var);
            ADDF("uniform %s %s;\n", glsl_type_name, sv.var.name);
        }

        // We can't handle these yet, but nothing we use requires them, either
        assert(res->num_vertex_attribs == 0);
        assert(res->num_descriptors == 0);

        ADD(res->glsl);
    }
#else
    if (fmt->transfer == TRANSFER_FUNC_SMPTE_ST2084 ||
        fmt->primaries == COLOR_PRIMARIES_BT2020)
    {
        // no warning for HLG because it's more or less backwards-compatible
        msg_Warn(priv->gl, "VLC needs to be built with support for libplacebo "
                 "in order to display wide gamut or HDR signals correctly.");
    }
#endif

    if (tex_target == GL_TEXTURE_RECTANGLE)
        ADDF("uniform vec2 TexSizes[%u];\n", tex_count);

    if (is_yuv)
        ADD("uniform mat4 ConvMatrix;\n");

    ADD("vec4 vlc_texture(vec2 tex_coords) {\n");

    unsigned color_count;
    if (is_yuv) {
        ADD(" vec4 pixel = vec4(\n");
        color_count = 0;
        for (unsigned i = 0; i < tex_count; ++i)
        {
            const char *swizzle = swizzle_per_tex[i];
            assert(swizzle);
            color_count += strlen(swizzle);
            assert(color_count < PICTURE_PLANE_MAX);
            if (tex_target == GL_TEXTURE_RECTANGLE)
            {
                /* The coordinates are in texels values, not normalized */
                ADDF("  %s(Textures[%u], TexSizes[%u] * tex_coords).%s,\n", lookup, i, i, swizzle);
            }
            else
            {
                ADDF("  %s(Textures[%u], tex_coords).%s,\n", lookup, i, swizzle);
            }
        }
        ADD("  1.0);\n");
        ADD(" vec4 result = ConvMatrix * pixel;\n");
    }
    else
    {
        if (tex_target == GL_TEXTURE_RECTANGLE)
            ADD(" tex_coords *= TexSizes[0];\n");

        ADDF(" vec4 result = %s(Textures[0], tex_coords);\n", lookup);
        color_count = 1;
    }
    assert(yuv_space == COLOR_SPACE_UNDEF || color_count == 3);

#ifdef HAVE_LIBPLACEBO
    if (priv->pl_sh_res) {
        const struct pl_shader_res *res = priv->pl_sh_res;
        assert(res->input  == PL_SHADER_SIG_COLOR);
        assert(res->output == PL_SHADER_SIG_COLOR);
        ADDF(" result = %s(result);\n", res->name);
    }
#endif

    ADD(" return result;\n"
        "}\n");

#undef ADD
#undef ADDF

    if (vlc_memstream_close(&ms) != 0)
        return VLC_EGENERIC;

    ret = InitShaderExtensions(sampler, tex_target);
    if (ret != VLC_SUCCESS)
    {
        free(ms.ptr);
        return VLC_EGENERIC;
    }
    sampler->shader.body = ms.ptr;

    static const struct vlc_gl_sampler_ops ops = {
        .fetch_locations = sampler_base_fetch_locations,
        .load = sampler_base_load,
    };
    sampler->ops = &ops;

    return VLC_SUCCESS;
}

static struct vlc_gl_sampler *
CreateSampler(struct vlc_gl_interop *interop, struct vlc_gl_t *gl,
              const struct vlc_gl_api *api, const video_format_t *fmt,
              unsigned tex_target, bool expose_planes)
{
    struct vlc_gl_sampler_priv *priv = calloc(1, sizeof(*priv));
    if (!priv)
        return NULL;

    struct vlc_gl_sampler *sampler = &priv->sampler;

    priv->uloc.pl_vars = NULL;
    priv->pl_ctx = NULL;
    priv->pl_sh = NULL;
    priv->pl_sh_res = NULL;

    priv->interop = interop;
    priv->gl = gl;
    priv->api = api;
    priv->vt = &api->vt;

    priv->mtx_transform_defined = false;
    sampler->pic_to_tex_matrix = NULL;
    priv->mtx_all_defined = false;
    priv->mtx_all_has_changed = false;

    /* Formats with palette are not supported. This also allows to copy
     * video_format_t without possibility of failure. */
    assert(!sampler->fmt.p_palette);

    sampler->fmt = *fmt;

    sampler->shader.extensions = NULL;
    sampler->shader.body = NULL;

    /* Expose the texture sizes publicly */
    sampler->tex_widths = priv->tex_widths;
    sampler->tex_heights = priv->tex_heights;

#ifdef HAVE_LIBPLACEBO
    // Create the main libplacebo context
    priv->pl_ctx = vlc_placebo_CreateContext(VLC_OBJECT(gl));
    if (priv->pl_ctx) {
        priv->pl_sh = pl_shader_alloc(priv->pl_ctx, &(struct pl_shader_params) {
            .glsl = {
#   ifdef USE_OPENGL_ES2
                .version = 100,
                .gles = true,
#   else
                .version = 120,
#   endif
            },
        });
    }
#endif

    int ret = opengl_fragment_shader_init(sampler, tex_target, fmt,
                                          expose_planes);
    if (ret != VLC_SUCCESS)
    {
        free(sampler);
        return NULL;
    }

    unsigned tex_count = sampler->tex_count;
    assert(!interop || interop->tex_count == tex_count);

    /* This might be updated in UpdatePicture for non-direct samplers */
    memcpy(&priv->mtx_coords_map, MATRIX3x2_IDENTITY,
           sizeof(MATRIX3x2_IDENTITY));

    if (interop)
    {
        /* Texture size */
        for (unsigned j = 0; j < interop->tex_count; j++) {
            const GLsizei w = interop->fmt_out.i_visible_width  * interop->texs[j].w.num
                            / interop->texs[j].w.den;
            const GLsizei h = interop->fmt_out.i_visible_height * interop->texs[j].h.num
                            / interop->texs[j].h.den;
            priv->visible_widths[j] = w;
            priv->visible_heights[j] = h;
            if (interop->api->supports_npot) {
                priv->tex_widths[j]  = w;
                priv->tex_heights[j] = h;
            } else {
                priv->tex_widths[j]  = vlc_align_pot(w);
                priv->tex_heights[j] = vlc_align_pot(h);
            }
        }

        if (!interop->handle_texs_gen)
        {
            ret = vlc_gl_interop_GenerateTextures(interop, priv->tex_widths,
                                                  priv->tex_heights,
                                                  priv->textures);
            if (ret != VLC_SUCCESS)
            {
                free(sampler);
                return NULL;
            }
        }
    }

    return sampler;
}

struct vlc_gl_sampler *
vlc_gl_sampler_NewFromInterop(struct vlc_gl_interop *interop,
                              bool expose_planes)
{
    return CreateSampler(interop, interop->gl, interop->api, &interop->fmt_out,
                         interop->tex_target, expose_planes);
}

struct vlc_gl_sampler *
vlc_gl_sampler_NewFromTexture2D(struct vlc_gl_t *gl,
                                const struct vlc_gl_api *api,
                                const video_format_t *fmt, bool expose_planes)
{
    return CreateSampler(NULL, gl, api, fmt, GL_TEXTURE_2D, expose_planes);
}

void
vlc_gl_sampler_Delete(struct vlc_gl_sampler *sampler)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    struct vlc_gl_interop *interop = priv->interop;
    if (interop && !interop->handle_texs_gen)
    {
        const opengl_vtable_t *vt = interop->vt;
        vt->DeleteTextures(interop->tex_count, priv->textures);
    }

#ifdef HAVE_LIBPLACEBO
    FREENULL(priv->uloc.pl_vars);
    if (priv->pl_ctx)
        pl_context_destroy(&priv->pl_ctx);
#endif

    free(sampler->shader.extensions);
    free(sampler->shader.body);

    free(priv);
}

/**
 * Compute out = a * b, as if the 3x2 matrices were expanded to 3x3 with
 *  [0 0 1] as the last row.
 */
static void
MatrixMultiply(float out[static 3*2],
               const float a[static 3*2], const float b[static 3*2])
{
    /* All matrices are stored in column-major order. */
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned j = 0; j < 2; ++j)
            out[i*2+j] = a[0*2+j] * b[i*2+0]
                       + a[1*2+j] * b[i*2+1]
                       + a[2*2+j];
}

static void
UpdateMatrixAll(struct vlc_gl_sampler_priv *priv)
{
    float tmp[3*2];

    float *out = priv->mtx_transform_defined ? tmp : priv->mtx_all;
    /* out = mtx_coords_map * mtx_orientation */
    MatrixMultiply(out, priv->mtx_coords_map, priv->mtx_orientation);

    if (priv->mtx_transform_defined)
        /* mtx_all = mtx_transform * tmp */
        MatrixMultiply(priv->mtx_all, priv->mtx_transform, tmp);
}

int
vlc_gl_sampler_UpdatePicture(struct vlc_gl_sampler *sampler, picture_t *picture)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);

    const struct vlc_gl_interop *interop = priv->interop;
    assert(interop);

    const video_format_t *source = &picture->format;

    bool mtx_changed = false;

    if (!priv->mtx_all_defined
     || source->i_x_offset != priv->last_source.i_x_offset
     || source->i_y_offset != priv->last_source.i_y_offset
     || source->i_visible_width != priv->last_source.i_visible_width
     || source->i_visible_height != priv->last_source.i_visible_height)
    {
        memset(priv->mtx_coords_map, 0, sizeof(priv->mtx_coords_map));

        /* The transformation is the same for all planes, even with power-of-two
         * textures. */
        float scale_w = priv->tex_widths[0];
        float scale_h = priv->tex_heights[0];

        /* Warning: if NPOT is not supported a larger texture is
           allocated. This will cause right and bottom coordinates to
           land on the edge of two texels with the texels to the
           right/bottom uninitialized by the call to
           glTexSubImage2D. This might cause a green line to appear on
           the right/bottom of the display.
           There are two possible solutions:
           - Manually mirror the edges of the texture.
           - Add a "-1" when computing right and bottom, however the
           last row/column might not be displayed at all.
        */
        float left   = (source->i_x_offset +                       0 ) / scale_w;
        float top    = (source->i_y_offset +                       0 ) / scale_h;
        float right  = (source->i_x_offset + source->i_visible_width ) / scale_w;
        float bottom = (source->i_y_offset + source->i_visible_height) / scale_h;

        /**
         * This matrix converts from picture coordinates (in range [0; 1])
         * to textures coordinates where the picture is actually stored
         * (removing paddings).
         *
         *        texture           (in texture coordinates)
         *       +----------------+--- 0.0
         *       |                |
         *       |  +---------+---|--- top
         *       |  | picture |   |
         *       |  +---------+---|--- bottom
         *       |  .         .   |
         *       |  .         .   |
         *       +----------------+--- 1.0
         *       |  .         .   |
         *      0.0 left  right  1.0  (in texture coordinates)
         *
         * In particular:
         *  - (0.0, 0.0) is mapped to (left, top)
         *  - (1.0, 1.0) is mapped to (right, bottom)
         *
         * This is an affine 2D transformation, so the input coordinates
         * are given as a 3D vector in the form (x, y, 1), and the output
         * is (x', y').
         *
         * The paddings are l (left), r (right), t (top) and b (bottom).
         *
         *      matrix = / (r-l)   0     l \
         *               \   0   (b-t)   t /
         *
         * It is stored in column-major order.
         */
        float *matrix = priv->mtx_coords_map;
#define COL(x) (x*2)
#define ROW(x) (x)
        matrix[COL(0) + ROW(0)] = right - left;
        matrix[COL(1) + ROW(1)] = bottom - top;
        matrix[COL(2) + ROW(0)] = left;
        matrix[COL(2) + ROW(1)] = top;
#undef COL
#undef ROW

        mtx_changed = true;

        priv->last_source.i_x_offset = source->i_x_offset;
        priv->last_source.i_y_offset = source->i_y_offset;
        priv->last_source.i_visible_width = source->i_visible_width;
        priv->last_source.i_visible_height = source->i_visible_height;
    }

    /* Update the texture */
    int ret = interop->ops->update_textures(interop, priv->textures,
                                            priv->visible_widths,
                                            priv->visible_heights, picture,
                                            NULL);

    const float *tm = GetTransformMatrix(interop);
    if (tm) {
        memcpy(priv->mtx_transform, tm, sizeof(priv->mtx_transform));
        priv->mtx_transform_defined = true;
        mtx_changed = true;
    }
    else if (priv->mtx_transform_defined)
    {
        priv->mtx_transform_defined = false;
        mtx_changed = true;
    }

    if (!priv->mtx_all_defined || mtx_changed)
    {
        UpdateMatrixAll(priv);
        priv->mtx_all_defined = true;
        sampler->pic_to_tex_matrix = priv->mtx_all;
        priv->mtx_all_has_changed = true;
    }
    else
        priv->mtx_all_has_changed = false;

    return ret;
}

int
vlc_gl_sampler_UpdateTextures(struct vlc_gl_sampler *sampler, GLuint textures[],
                              GLsizei tex_widths[], GLsizei tex_heights[])
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    assert(!priv->interop);

    if (!priv->mtx_all_defined)
    {
        memcpy(priv->mtx_all, MATRIX3x2_IDENTITY, sizeof(MATRIX3x2_IDENTITY));
        priv->mtx_all_defined = true;
        priv->mtx_all_has_changed = true;

        sampler->pic_to_tex_matrix = priv->mtx_all;
    }
    else
        priv->mtx_all_has_changed = false;

    unsigned tex_count = sampler->tex_count;
    memcpy(priv->textures, textures, tex_count * sizeof(textures[0]));
    memcpy(priv->tex_widths, tex_widths, tex_count * sizeof(tex_widths[0]));
    memcpy(priv->tex_heights, tex_heights, tex_count * sizeof(tex_heights[0]));

    return VLC_SUCCESS;
}

void
vlc_gl_sampler_SelectPlane(struct vlc_gl_sampler *sampler, unsigned plane)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    priv->plane = plane;
}

void
vlc_gl_sampler_PicToTexCoords(struct vlc_gl_sampler *sampler,
                              unsigned coords_count, const float *pic_coords,
                              float *tex_coords_out)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    const float *mtx = priv->mtx_all;
#define MTX(col,row) mtx[(col*2)+row]
    for (unsigned i = 0; i < coords_count; ++i)
    {
        /* Store the coordinates, in case the transform must be applied in
         * place (i.e. with pic_coords == tex_coords_out) */
        float x = pic_coords[0];
        float y = pic_coords[1];
        tex_coords_out[0] = MTX(0,0) * x + MTX(1,0) * y + MTX(2,0);
        tex_coords_out[1] = MTX(0,1) * x + MTX(1,1) * y + MTX(2,1);
        pic_coords += 2;
        tex_coords_out += 2;
    }
}

void
vlc_gl_sampler_ComputeDirectionMatrix(struct vlc_gl_sampler *sampler,
                                      float direction[static 2*2])
{
    /**
     * The direction matrix is extracted from priv->mtx_all:
     *
     *    mtx_all = / a b c \
     *              \ d e f /
     *
     * The last column (the offset part of the affine transformation) is
     * discarded, and the 2 remaining column vectors are normalized to remove
     * any scaling:
     *
     *    direction = / a/unorm  b/vnorm \
     *                \ d/unorm  e/vnorm /
     *
     * where unorm = norm( / a \ ) and vnorm = norm( / b \ ).
     *                     \ d /                     \ e /
     */

    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    assert(priv->mtx_all_defined);

    float ux = priv->mtx_all[0];
    float uy = priv->mtx_all[1];
    float vx = priv->mtx_all[2];
    float vy = priv->mtx_all[3];

    float unorm = sqrt(ux * ux + uy * uy);
    float vnorm = sqrt(vx * vx + vy * vy);

    direction[0] = ux / unorm;
    direction[1] = uy / unorm;
    direction[2] = vx / vnorm;
    direction[3] = vy / vnorm;
}

bool
vlc_gl_sampler_MustRecomputeCoords(struct vlc_gl_sampler *sampler)
{
    struct vlc_gl_sampler_priv *priv = PRIV(sampler);
    return priv->mtx_all_has_changed;
}
