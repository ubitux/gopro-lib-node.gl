/*
 * Copyright 2020 GoPro Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <float.h>
#include <stddef.h>
#include <string.h>

#include "drawutils.h"
#include "gctx.h"
#include "log.h"
#include "memory.h"
#include "nodegl.h"
#include "nodes.h"
#include "pgcraft.h"
#include "pipeline.h"
#include "root_finder.h"
#include "texture.h"
#include "topology.h"
#include "type.h"
#include "utils.h"


struct pipeline_desc {
    struct pgcraft *crafter;
    struct pipeline *pipeline;
    int modelview_matrix_index;
    int projection_matrix_index;
    //int color_index;
};

struct pathdraw_priv {
    struct ngl_node *path_node;

    struct texture *distmap;
    //int fill;
    //struct ngl_node *start_pos_node;
    //struct ngl_node *end_pos_node;
    //struct ngl_node *thickness_node;
    //struct ngl_node *colors_node;

    struct buffer *vertices;
    struct buffer *indices;
    int nb_indices;
    struct darray pipeline_descs;
};

//#define FLOAT_NODE_TYPES (const int[]){NGL_NODE_UNIFORMFLOAT, NGL_NODE_ANIMATEDFLOAT, NGL_NODE_NOISE, -1}

#define DISTMAP_SIZE 256
#define DISTMAP_DEBUG 0

#define OFFSET(x) offsetof(struct pathdraw_priv, x)
static const struct node_param pathdraw_params[] = {
    {"path",         PARAM_TYPE_NODE, OFFSET(path_node),
                     .node_types=(const int[]){NGL_NODE_PATH, -1},
                     .flags=PARAM_FLAG_NON_NULL,
                     .desc=NGLI_DOCSTRING("path to draw")},
    //{"fill",         PARAM_TYPE_BOOL, OFFSET(fill),
    //                 .desc=NGLI_DOCSTRING("fill the content of the path")},
    //{"start_pos",    PARAM_TYPE_NODE, OFFSET(start_pos_node),
    //                 .node_types=FLOAT_NODE_TYPES,
    //                 .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
    //                 .desc=NGLI_DOCSTRING("path position where the drawing starts")},
    //{"end_pos",      PARAM_TYPE_NODE, OFFSET(end_pos_node),
    //                 .node_types=FLOAT_NODE_TYPES,
    //                 .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
    //                 .desc=NGLI_DOCSTRING("path position where the drawing ends")},
    //{"thickness",    PARAM_TYPE_NODE, OFFSET(thickness_node),
    //                 .node_types=FLOAT_NODE_TYPES,
    //                 .desc=NGLI_DOCSTRING("thickness of the drawn path")},
    //{"colors",       PARAM_TYPE_NODE, OFFSET(colors_node),
    //                 .node_types=(const int[]){
    //                     NGL_NODE_UNIFORMVEC4,
    //                     NGL_NODE_ANIMATEDVEC4,
    //                     NGL_NODE_BUFFERVEC4,
    //                     NGL_NODE_ANIMATEDBUFFERVEC4,
    //                     -1
    //                 },
    //                 .desc=NGLI_DOCSTRING("path color if single vec4, color at every point if buffer")},
    // corner style
    // cap style
    {NULL}
};

static inline float poly4(float a, float b, float c, float d, float e, float t)
{
    return (((a * t + b) * t + c) * t + d) * t + e;
}

static inline float poly3(float a, float b, float c, float d, float t)
{
    return ((a * t + b) * t + c) * t + d;
}

static float get_distance(const struct path_knot *knots, int nb_knots, int x, int y, int w, int h)
{
    float min_dist = FLT_MAX;

    // XXX: we need to optimise which knots to evaluate (discard the far ones)
    for (int i = 0; i < nb_knots - 1; i++) {
        const struct path_knot *knot = &knots[i];

        // XXX: we ignore the 3rd dimension: need an API change, a warning or
        // something
        const float ax = knot->poly_x[0];
        const float bx = knot->poly_x[1];
        const float cx = knot->poly_x[2];
        const float dx = knot->poly_x[3];

        const float ay = knot->poly_y[0];
        const float by = knot->poly_y[1];
        const float cy = knot->poly_y[2];
        const float dy = knot->poly_y[3];

        // XXX: rework coordinate to handle the bounding box instead of the default resolution
        const float px = x / (float)w * 2.f - 1.f;
        const float py = y / (float)h * 2.f - 1.f;

        /*
         * Calculate coefficients for the derivate d'(t) (degree 5) of d(t)
         * d(t) is the distance squared
         * See https://stackoverflow.com/questions/2742610/closest-point-on-a-cubic-bezier-curve/57315396#57315396
         */
        const float dt_a =  6.f*(ax*ax + ay*ay);
        const float dt_b = 10.f*(ax*bx + ay*by);
        const float dt_c =  4.f*(2.f*(ax*cx + ay*cy) + bx*bx + by*by);
        const float dt_d =  6.f*(ax*(dx-px) + bx*cx + ay*(dy-py) + by*cy);
        const float dt_e =  2.f*(2.f*(bx*dx - bx*px + by*dy - by*py) + cx*cx + cy*cy);
        const float dt_f =  2.f*(cx*dx - cx*px + cy*dy - cy*py);

        /*
         * Calculate the derivate d''(t) (degree 4)
         */
        const float ddt_a = 5.f*dt_a;
        const float ddt_b = 4.f*dt_b;
        const float ddt_c = 3.f*dt_c;
        const float ddt_d = 2.f*dt_d;
        const float ddt_e =     dt_e;

        float roots[2+5] = {0.f, 1.f}; /* also include start and end points */
        const int nb_roots = ngli_root_find5(roots + 2, dt_a, dt_b, dt_c, dt_d, dt_e, dt_f) + 2;
        for (int r = 0; r < nb_roots; r++) {
            const float t = roots[r];
            if (t < 0.f || t > 1.f) /* ignore out of bounds roots */
                continue;

            // Check for d''(t)≥0
            // XXX: doesn't actually look needed for some reason: worse, it gliches shit
            //if (poly4(ddt_a, ddt_b, ddt_c, ddt_d, ddt_e, t) < 0.f)
            //    continue;

            const float xmp = px - poly3(ax, bx, cx, dx, t);
            const float ymp = py - poly3(ay, by, cy, dy, t);
            const float dist = xmp*xmp + ymp*ymp;

            min_dist = NGLI_MIN(min_dist, dist);
        }
    }

    // XXX: need to normalize?
    return sqrtf(min_dist);
}

// XXX: refactor in the codebase somewhere?
#define CLAMP_U8(x) ((uint8_t)NGLI_MIN(NGLI_MAX(x, 0), 255))

static void build_distmap(struct pathdraw_priv *s, struct canvas *canvas)
{
    const struct path_priv *path = s->path_node->priv_data;
    const struct path_knot *knots = path->knots;
    const struct buffer_priv *points = path->points_buffer->priv_data; // XXX: add a path->nb_knots
    const int nb_knots = points->count;

    float *buf = (float *)canvas->buf;
    for (int y = 0; y < canvas->h; y++) {
        for (int x = 0; x < canvas->w; x++) {
            const float dist = get_distance(knots, nb_knots, x, y, canvas->w, canvas->h);
            buf[x] = dist;
            //buf[x] = CLAMP_U8(lrintf(dist * 255));
        }
        buf += canvas->w;
    }
}

static int pathdraw_init(struct ngl_node *node)
{
    int ret = 0;
    struct pathdraw_priv *s = node->priv_data;

    //if (s->colors_node) {
    //    if (s->colors_node->class->category == NGLI_NODE_CATEGORY_BUFFER) {
    //        const struct pathdraw_priv *path = s->path_node->priv_data;
    //        const struct buffer_priv *points = path->points_buffer->priv_data;
    //        const struct buffer_priv *colors = s->colors_node->priv_data;
    //        if (colors->count != points->count) {
    //            LOG(ERROR, "the number of colors in the buffer (%d) must match the number of points in the path (%d)",
    //                colors->count, points->count);
    //            return NGL_ERROR_INVALID_ARG;
    //        }
    //    }
    //}

    ngli_darray_init(&s->pipeline_descs, sizeof(struct pipeline_desc), 0);

    struct canvas canvas = {.w=DISTMAP_SIZE, .h=DISTMAP_SIZE};
    canvas.buf = ngli_calloc(canvas.w, canvas.h * sizeof(float));
    if (!canvas.buf)
        return NGL_ERROR_MEMORY;

    build_distmap(s, &canvas);

    const struct texture_params tex_params = {
        .type          = NGLI_TEXTURE_TYPE_2D,
        .width         = canvas.w,
        .height        = canvas.h,
        .format        = NGLI_FORMAT_R32_SFLOAT,
#if !DISTMAP_DEBUG
        .min_filter    = NGLI_FILTER_LINEAR,
        .mag_filter    = NGLI_FILTER_LINEAR,
#endif
        //.mipmap_filter = NGLI_MIPMAP_FILTER_LINEAR,
    };

    s->distmap = ngli_texture_create(node->ctx->gctx);
    if (!s->distmap) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    ret = ngli_texture_init(s->distmap, &tex_params);
    if (ret < 0)
        goto end;

    ret = ngli_texture_upload(s->distmap, canvas.buf, 0);
    if (ret < 0)
        goto end;

end:
    ngli_free(canvas.buf);
    return ret;
}

static const char * const vertex_data =
    "void main()"                                                               "\n"
    "{"                                                                         "\n"
    "    ngl_out_pos = projection_matrix * modelview_matrix * vec4(position.xy, 0.0, 1.0);"                            "\n"
    "    var_tex_coord = (/* tex_coord_matrix * */ vec4(position.zw, 0.0, 1.0)).xy;"  "\n"
    "}";

static const char * const fragment_data =
    "void main()"                                                           "\n"
    "{"                                                                     "\n"
    "    float v = ngl_tex2d(tex, var_tex_coord).r;"                        "\n"
#if DISTMAP_DEBUG
    "    ngl_out_color = vec4(vec3(v), 1.0);"                               "\n"
#else
    "    float d = v - .01;"                                                "\n"
    "    float a = 1. - clamp(d / fwidth(d) + .5, 0.0, 1.0);"               "\n"
    "    ngl_out_color = vec4(vec3(a), 1.0);"                               "\n"
#endif
    "}";

static const struct pgcraft_iovar io_vars[] = {
    {.name = "var_tex_coord", .type = NGLI_TYPE_VEC2},
};

static int pathdraw_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct pathdraw_priv *s = node->priv_data;

    struct pipeline_desc *desc = ngli_darray_push(&s->pipeline_descs, NULL);
    if (!desc)
        return NGL_ERROR_MEMORY;
    ctx->rnode_pos->id = ngli_darray_count(&s->pipeline_descs) - 1;

    memset(desc, 0, sizeof(*desc));

    struct rnode *rnode = ctx->rnode_pos;
    struct gctx *gctx = ctx->gctx;

    static const float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };

    // XXX: memleak alert! each prepare will override the existing pointers
    s->vertices = ngli_buffer_create(gctx);
    if (!s->vertices)
        return NGL_ERROR_MEMORY;

    int ret = ngli_buffer_init(s->vertices, sizeof(vertices), NGLI_BUFFER_USAGE_STATIC);
    if (ret < 0)
        return ret;

    ret = ngli_buffer_upload(s->vertices, vertices, sizeof(vertices));
    if (ret < 0)
        return ret;

    struct pgcraft_texture textures[] = {
        {.name = "tex", .type = NGLI_PGCRAFT_SHADER_TEX_TYPE_TEXTURE2D, .stage = NGLI_PROGRAM_SHADER_FRAG, .texture = s->distmap},
    };

    const struct pgcraft_uniform uniforms[] = {
        {.name = "modelview_matrix",  .type = NGLI_TYPE_MAT4, .stage = NGLI_PROGRAM_SHADER_VERT, .data = NULL},
        {.name = "projection_matrix", .type = NGLI_TYPE_MAT4, .stage = NGLI_PROGRAM_SHADER_VERT, .data = NULL},
    };

    const struct pgcraft_attribute attributes[] = {
        {
            .name     = "position",
            .type     = NGLI_TYPE_VEC4,
            .format   = NGLI_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * 4,
            .buffer   = s->vertices,
        },
    };

    struct pipeline_params pipeline_params = {
        .type          = NGLI_PIPELINE_TYPE_GRAPHICS,
        .graphics      = {
            .topology       = NGLI_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .state          = rnode->graphicstate,
            .rt_desc        = rnode->rendertarget_desc,
        }
    };

    const struct pgcraft_params crafter_params = {
        .vert_base        = vertex_data,
        .frag_base        = fragment_data,
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .uniforms         = uniforms,
        .nb_uniforms      = NGLI_ARRAY_NB(uniforms),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = io_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(io_vars),
    };

    desc->crafter = ngli_pgcraft_create(ctx);
    if (!desc->crafter)
        return NGL_ERROR_MEMORY;

    struct pipeline_resource_params pipeline_resource_params = {0};
    ret = ngli_pgcraft_craft(desc->crafter, &pipeline_params, &pipeline_resource_params, &crafter_params);
    if (ret < 0)
        return ret;

    desc->pipeline = ngli_pipeline_create(gctx);
    if (!desc->pipeline)
        return NGL_ERROR_MEMORY;

    ret = ngli_pipeline_init(desc->pipeline, &pipeline_params);
    if (ret < 0)
        return ret;

    ret = ngli_pipeline_set_resources(desc->pipeline, &pipeline_resource_params);
    if (ret < 0)
        return ret;

    desc->modelview_matrix_index = ngli_pgcraft_get_uniform_index(desc->crafter, "modelview_matrix", NGLI_PROGRAM_SHADER_VERT);
    desc->projection_matrix_index = ngli_pgcraft_get_uniform_index(desc->crafter, "projection_matrix", NGLI_PROGRAM_SHADER_VERT);
    //desc->color_index = ngli_pgcraft_get_uniform_index(desc->crafter, "color", NGLI_PROGRAM_SHADER_FRAG);

    return 0;
}

static int pathdraw_update(struct ngl_node *node, double t)
{
    //struct pathdraw_priv *s = node->priv_data;

    //TODO: morphing path
    //int ret = ngli_node_update(s->path_node, t);
    //if (ret < 0)
    //    return ret;

    return 0;
}

static void pathdraw_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct pathdraw_priv *s = node->priv_data;

    const float *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    struct pipeline_desc *desc = &descs[ctx->rnode_pos->id];

    if (ctx->begin_render_pass) {
        struct gctx *gctx = ctx->gctx;
        ngli_gctx_begin_render_pass(gctx, ctx->current_rendertarget);
        ctx->begin_render_pass = 0;
    }

    ngli_pipeline_update_uniform(desc->pipeline, desc->modelview_matrix_index, modelview_matrix);
    ngli_pipeline_update_uniform(desc->pipeline, desc->projection_matrix_index, projection_matrix);
    //ngli_pipeline_update_uniform(desc->pipeline, desc->color_index, s->color);
    ngli_pipeline_draw(desc->pipeline, 4, 1);
}

static void pathdraw_uninit(struct ngl_node *node)
{
    struct pathdraw_priv *s = node->priv_data;
    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    const int nb_descs = ngli_darray_count(&s->pipeline_descs);
    for (int i = 0; i < nb_descs; i++) {
        struct pipeline_desc *desc = &descs[i];
        ngli_pipeline_freep(&desc->pipeline);
        ngli_pgcraft_freep(&desc->crafter);
    }
    ngli_buffer_freep(&s->vertices);
    ngli_buffer_freep(&s->indices);
    ngli_darray_reset(&s->pipeline_descs);
}

const struct node_class ngli_pathdraw_class = {
    .id        = NGL_NODE_PATHDRAW,
    .name      = "PathDraw",
    .init      = pathdraw_init,
    .prepare   = pathdraw_prepare,
    .update    = pathdraw_update,
    .draw      = pathdraw_draw,
    .uninit    = pathdraw_uninit,
    .priv_size = sizeof(struct pathdraw_priv),
    .params    = pathdraw_params,
    .file      = __FILE__,
};
