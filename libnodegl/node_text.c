/*
 * Copyright 2019 GoPro Inc.
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


#define _XOPEN_SOURCE 500 // random
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "bstr.h"
#include "memory.h"
#include "nodes.h"
#include "darray.h"
#include "drawutils.h"
#include "gctx.h"
#include "log.h"
#include "math_utils.h"
#include "pgcache.h"
#include "pgcraft.h"
#include "pipeline.h"
#include "text.h"
#include "type.h"
#include "topology.h"
#include "utils.h"

struct pipeline_desc_bg {
    struct pgcraft *crafter;
    struct pipeline *pipeline;
    int modelview_matrix_index;
    int projection_matrix_index;
    int color_index;
};

struct pipeline_desc_fg {
    struct graphicstate state;
    struct rendertarget_desc rt_desc;

    struct pgcraft *crafter;
    struct pipeline *pipeline;

    /* attr */
    int glyph_rects_index;
    int glyph_sizes_index;

    /* vert */
    int glyph_nb_index;
    int modelview_matrix_index;
    int projection_matrix_index;
    int chr_transform_index;

    /* frag */
    int chr_color_index;
    int chr_alpha_index;
    int chr_stroke_width_index;
    int chr_stroke_color_index;
    int chr_glow_width_index;
    int chr_glow_color_index;
    int chr_blur_index;
};

struct pipeline_desc {
    struct pipeline_desc_bg bg; /* Background (bounding box) */
    struct pipeline_desc_fg fg; /* Foreground (characters) */
};

struct chr_data {
    NGLI_ALIGNED_MAT(transform);
    float color[4];
    float alpha;
    float stroke_width;
    float stroke_color[4];
    float glow_width;
    float glow_color[4];
    float blur;
};

struct chr_data_pointers {
    float *transform;
    float *color;
    float *alpha;
    float *stroke_width;
    float *stroke_color;
    float *glow_width;
    float *glow_color;
    float *blur;
};

struct text_priv {
    /* options */
    char *text;
    float fg_color[4];
    float bg_color[4];
    float box_corner[3];
    float box_width[3];
    float box_height[3];
    char *font_file;
    int writing_mode;
    int padding;
    double font_scale;
    struct ngl_node **effect_nodes;
    int nb_effect_nodes;
    int valign, halign;
    int aspect_ratio[2];

    /* characters (fg) */
    struct text text_ctx;
    struct buffer *vertices;
    struct buffer *uvcoords;
    struct buffer *indices;
    int nb_indices;
    struct buffer *glyph_rects;
    struct buffer *glyph_sizes;
    int **effect_positions;
    struct chr_data_pointers chr;
    float *chars_data;
    float *chars_data_default;
    int chars_data_size;

    /* background box */
    struct buffer *bg_vertices;
    struct buffer *bg_indices;
    int nb_bg_indices;

    struct darray pipeline_descs;
    int live_changed;
};

#define VALIGN_CENTER 0
#define VALIGN_TOP    1
#define VALIGN_BOTTOM 2

#define HALIGN_CENTER 0
#define HALIGN_RIGHT  1
#define HALIGN_LEFT   2

static const struct param_choices valign_choices = {
    .name = "valign",
    .consts = {
        {"center", VALIGN_CENTER, .desc=NGLI_DOCSTRING("vertically centered")},
        {"bottom", VALIGN_BOTTOM, .desc=NGLI_DOCSTRING("bottom positioned")},
        {"top",    VALIGN_TOP,    .desc=NGLI_DOCSTRING("top positioned")},
        {NULL}
    }
};

static const struct param_choices halign_choices = {
    .name = "halign",
    .consts = {
        {"center", HALIGN_CENTER, .desc=NGLI_DOCSTRING("horizontally centered")},
        {"right",  HALIGN_RIGHT,  .desc=NGLI_DOCSTRING("right positioned")},
        {"left",   HALIGN_LEFT,   .desc=NGLI_DOCSTRING("left positioned")},
        {NULL}
    }
};

static const struct param_choices writing_mode_choices = {
    .name = "writing_mode",
    .consts = {
        {"undefined",     NGLI_TEXT_WRITING_MODE_UNDEFINED,
                          .desc=NGLI_DOCSTRING("undefined (automatic)")},
        {"horizontal-tb", NGLI_TEXT_WRITING_MODE_HORIZONTAL_TB,
                          .desc=NGLI_DOCSTRING("LTR: left-to-right flow then top-to-bottom per line, "
                                               "RTL: right-to-left flow then top-to-bottom per line")},
        {"vertical-rl",   NGLI_TEXT_WRITING_MODE_VERTICAL_LR,
                          .desc=NGLI_DOCSTRING("LTR: top-to-bottom flow then right-to-left per line, "
                                               "RTL: bottom-to-top flow then left-to-right per line")},
        {"vertical-lr",   NGLI_TEXT_WRITING_MODE_VERTICAL_RL,
                          .desc=NGLI_DOCSTRING("LTR: top-to-bottom flow then left-to-right per line, "
                                               "RTL: bottom-to-top flow then right-to-left per line")},
        {NULL}
    }
};

static int set_live_changed(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;
    s->live_changed = 1;
    return 0;
}

#define OFFSET(x) offsetof(struct text_priv, x)
static const struct node_param text_params[] = {
    {"text",         PARAM_TYPE_STR, OFFSET(text), {.str=""},
                     .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE | PARAM_FLAG_NON_NULL,
                     .update_func=set_live_changed,
                     .desc=NGLI_DOCSTRING("text string to rasterize")},
    {"fg_color",     PARAM_TYPE_VEC4, OFFSET(fg_color), {.vec={1.0, 1.0, 1.0, 1.0}},
                     .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("foreground text color")},
    {"bg_color",     PARAM_TYPE_VEC4, OFFSET(bg_color), {.vec={0.0, 0.0, 0.0, 0.8}},
                     .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .desc=NGLI_DOCSTRING("background text color")},
    {"box_corner",   PARAM_TYPE_VEC3, OFFSET(box_corner), {.vec={-1.0, -1.0, 0.0}},
                     .desc=NGLI_DOCSTRING("origin coordinates of `box_width` and `box_height` vectors")},
    {"box_width",    PARAM_TYPE_VEC3, OFFSET(box_width), {.vec={2.0, 0.0, 0.0}},
                     .desc=NGLI_DOCSTRING("box width vector")},
    {"box_height",   PARAM_TYPE_VEC3, OFFSET(box_height), {.vec={0.0, 2.0, 0.0}},
                     .desc=NGLI_DOCSTRING("box height vector")},
    {"font_file",    PARAM_TYPE_STR, OFFSET(font_file),
                     .desc=NGLI_DOCSTRING("path to font file (require build with external text libraries)")},
    {"writing_mode", PARAM_TYPE_SELECT, OFFSET(writing_mode), {.i64=NGLI_TEXT_WRITING_MODE_UNDEFINED},
                     .choices=&writing_mode_choices,
                     .desc=NGLI_DOCSTRING("direction flow per character and line")},
    {"padding",      PARAM_TYPE_INT, OFFSET(padding), {.i64=3},
                     .desc=NGLI_DOCSTRING("pixel padding around the text")},
    {"font_scale",   PARAM_TYPE_DBL, OFFSET(font_scale), {.dbl=1.0},
                     .desc=NGLI_DOCSTRING("scaling of the font")},
    {"effects",      PARAM_TYPE_NODELIST, OFFSET(effect_nodes),
                     .node_types=(const int[]){NGL_NODE_TEXTEFFECT, -1},
                     .desc=NGLI_DOCSTRING("stack of effects")},
    {"valign",       PARAM_TYPE_SELECT, OFFSET(valign), {.i64=VALIGN_CENTER},
                     .choices=&valign_choices,
                     .desc=NGLI_DOCSTRING("vertical alignment of the text in the box")},
    {"halign",       PARAM_TYPE_SELECT, OFFSET(halign), {.i64=HALIGN_CENTER},
                     .choices=&halign_choices,
                     .desc=NGLI_DOCSTRING("horizontal alignment of the text in the box")},
    {"aspect_ratio", PARAM_TYPE_RATIONAL, OFFSET(aspect_ratio),
                     .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
                     .update_func=set_live_changed,
                     .desc=NGLI_DOCSTRING("box aspect ratio")},
    {NULL}
};

static const char * const bg_vertex_data =
    "void main()"                                                           "\n"
    "{"                                                                     "\n"
    "    ngl_out_pos = projection_matrix * modelview_matrix * position;"    "\n"
    "}";

static const char * const bg_fragment_data =
    "void main()"                                                           "\n"
    "{"                                                                     "\n"
    "    ngl_out_color = color;"                                            "\n"
    "}";

#define GLSL(s) #s "\n"

static const char * const glyph_vertex_data =
    GLSL(void main() {)
    GLSL(    var_tex_coord = uvcoord;)
    GLSL(    var_glyph_id = gl_VertexID / 4;)
    GLSL(    var_glyph_nb = ngl_glyph_nb;)
    GLSL(    var_glyph_rect = ngl_glyph_rect;)
    GLSL(    var_glyph_size = ngl_glyph_size;)
    GLSL(    mat4 transform = chr_transform[var_glyph_id];)
    GLSL(    ngl_out_pos = projection_matrix * transform * modelview_matrix * position;)
    GLSL(});

static const char * const glyph_fragment_data =
    GLSL(void main() {)
    GLSL(    float v = ngl_tex2d(tex, var_tex_coord).r;)
    GLSL(    float dist = v - 0.5;)

    GLSL(    vec4 color  = chr_color[var_glyph_id];)
    GLSL(    float alpha = chr_alpha[var_glyph_id];)
    GLSL(    float blur  = chr_blur[var_glyph_id];)
    GLSL(    float glow = chr_glow_width[var_glyph_id];)
    GLSL(    vec4 glow_color = chr_glow_color[var_glyph_id];)

    GLSL(    float a = blur > 0.0 ? smoothstep(-blur, blur, dist)
                                  : clamp(dist / fwidth(dist) + 0.5, 0.0, 1.0);)

    GLSL(    a *= alpha;)

    GLSL(    vec4 out_color = vec4(color.rgb, a);)

    GLSL(    if (glow > 0.0) {)
    GLSL(        float glow_a = smoothstep(-.5, .5, dist);)
    GLSL(        glow_a *= (glow + 1.) * .5f * 2.;)
    GLSL(        out_color = mix(out_color, glow_color, glow_a);)
    GLSL(    })

    GLSL(    ngl_out_color = out_color;)
    GLSL(});

static const struct pgcraft_iovar glyph_io_vars[] = {
    {.name = "var_tex_coord",  .type = NGLI_TYPE_VEC2},
    {.name = "var_glyph_id",   .type = NGLI_TYPE_INT},
    {.name = "var_glyph_nb",   .type = NGLI_TYPE_FLOAT},
    {.name = "var_glyph_rect", .type = NGLI_TYPE_VEC4},
    {.name = "var_glyph_size", .type = NGLI_TYPE_VEC2},
};

#define BC(index) s->box_corner[index]
#define BW(index) s->box_width[index]
#define BH(index) s->box_height[index]

#define C(index) chr_corner[index]
#define W(index) chr_width[index]
#define H(index) chr_height[index]

static void shuffle_positions(int *positions, int n)
{
    for (int i = 0; i < n - 1; i++) {
        const int r = i + random() % (n - i);
        const int cur = positions[i];
        positions[i] = positions[r];
        positions[r] = cur;
    }
}

static int set_positions(struct text_priv *s, int nb_elems)
{
    for (int i = 0; i < s->nb_effect_nodes; i++) {
        struct ngl_node *effect_node = s->effect_nodes[i];
        struct texteffect_priv *effect_priv = effect_node->priv_data;

        s->effect_positions[i] = ngli_calloc(nb_elems, sizeof(*s->effect_positions[i]));
        if (!s->effect_positions[i])
            return NGL_ERROR_MEMORY;

        for (int j = 0; j < nb_elems; j++)
            s->effect_positions[i][j] = j;

        if (effect_priv->random) {
            if (effect_priv->random_seed >= 0)
                srandom(effect_priv->random_seed);
            shuffle_positions(s->effect_positions[i], nb_elems);
        }
    }

    return 0;
}

static int set_f32_from_node(float *dst, struct ngl_node *node, double t)
{
    if (!node)
        return 0;
    int ret = ngli_node_update(node, t);
    if (ret < 0)
        return ret;
    const struct variable_priv *v = node->priv_data;
    memcpy(dst, &v->scalar, sizeof(*dst));
    return 0;
}

static int set_vec4_from_node(float *dst, struct ngl_node *node, double t)
{
    if (!node)
        return 0;
    int ret = ngli_node_update(node, t);
    if (ret < 0)
        return ret;
    const struct variable_priv *v = node->priv_data;
    memcpy(dst, v->vector, 4 * sizeof(*dst));
    return 0;
}

// XXX: need to make sure there is only a chain of transform nodes, maybe at
// init
// XXX: write directly into dst? need to malloc aligned!
static int set_transform_from_node(float *dst, struct ngl_node *node, double t)
{
    if (!node)
        return 0;
    int ret = ngli_node_update(node, t);
    if (ret < 0)
        return ret;
    NGLI_ALIGNED_MAT(matrix) = NGLI_MAT4_IDENTITY;
    while (node->class->id != NGL_NODE_IDENTITY) {
        const struct transform_priv *trf = node->priv_data;
        ngli_mat4_mul(matrix, matrix, trf->matrix);
        node = trf->child;
    }
    memcpy(dst, matrix, sizeof(matrix));
    return 0;
}

static int update_character_data(struct text_priv *s, struct texteffect_priv *effect, int c, double t)
{
    int ret;
    if ((ret = set_transform_from_node(s->chr.transform    + c * 4 * 4, effect->transform_chain,   t)) < 0 ||
        (ret = set_vec4_from_node(     s->chr.color        + c * 4,     effect->color_node,        t)) < 0 ||
        (ret = set_f32_from_node(      s->chr.alpha        + c,         effect->alpha_node,        t)) < 0 ||
        (ret = set_f32_from_node(      s->chr.stroke_width + c,         effect->stroke_width_node, t)) < 0 ||
        (ret = set_vec4_from_node(     s->chr.stroke_color + c * 4,     effect->stroke_color_node, t)) < 0 ||
        (ret = set_f32_from_node(      s->chr.glow_width   + c,         effect->glow_width_node,   t)) < 0 ||
        (ret = set_vec4_from_node(     s->chr.glow_color   + c * 4,     effect->glow_color_node,   t)) < 0 ||
        (ret = set_f32_from_node(      s->chr.blur         + c,         effect->blur_node,         t)) < 0)
        return ret;
    return 0;
}

static void reset_chars_data_to_defaults(struct text_priv *s)
{
    memcpy(s->chars_data, s->chars_data_default, s->chars_data_size);
}

static float *assign_chr_data_pointers(float *base, struct chr_data_pointers *chr, int text_nbchr)
{
    chr->transform    = base;
    chr->color        = chr->transform    + text_nbchr * 4 * 4;
    chr->alpha        = chr->color        + text_nbchr * 4;
    chr->stroke_width = chr->alpha        + text_nbchr;
    chr->stroke_color = chr->stroke_width + text_nbchr;
    chr->glow_width   = chr->stroke_color + text_nbchr * 4;
    chr->glow_color   = chr->glow_width   + text_nbchr;
    chr->blur         = chr->glow_color   + text_nbchr * 4;
    return chr->blur                      + text_nbchr;
}

static int init_characters_data(struct text_priv *s, int text_nbchr)
{
    const struct chr_data chr_data_default = {
        .transform    = NGLI_MAT4_IDENTITY,
        .color        = {NGLI_ARG_VEC4(s->fg_color)},
        .alpha        = 1.,
        .stroke_width = 0.,
        .stroke_color = {0., 0., 0., 0.},
        .glow_width   = 0.,
        .glow_color   = {0., 0., 0., 0.},
        .blur         = 0.,
    };

    /*
     * We can not allocate an array of struct chr_data because each field
     * must be an array. This could be avoided if we had support for array of
     * struct with all backends.
     *
     * The x2 is because we duplicate the data for the defaults, which is the
     * reference data we use to reset all the characters properties at every
     * frame. The default data is positioned first for a more predictible
     * read/write memory access on copy.
     *
     * Note: the allocation may overshoot a bit due to struct padding.
     */
    s->chars_data_size = text_nbchr * sizeof(chr_data_default);
    s->chars_data_default = ngli_calloc(2, s->chars_data_size);
    if (!s->chars_data_default)
        return NGL_ERROR_MEMORY;

    struct chr_data_pointers defaults;
    s->chars_data = assign_chr_data_pointers(s->chars_data_default, &defaults, text_nbchr);

    for (int i = 0; i < text_nbchr; i++) {
        memcpy(defaults.transform    + i * 4 * 4, chr_data_default.transform,     sizeof(chr_data_default.transform));
        memcpy(defaults.color        + i * 4,     chr_data_default.color,         sizeof(chr_data_default.color));
        memcpy(defaults.alpha        + i,         &chr_data_default.alpha,        sizeof(chr_data_default.alpha));
        memcpy(defaults.stroke_width + i,         &chr_data_default.stroke_width, sizeof(chr_data_default.stroke_width));
        memcpy(defaults.stroke_color + i * 4,     chr_data_default.stroke_color,  sizeof(chr_data_default.stroke_color));
        memcpy(defaults.glow_width   + i,         &chr_data_default.glow_width,   sizeof(chr_data_default.glow_width));
        memcpy(defaults.glow_color   + i * 4,     chr_data_default.glow_color,    sizeof(chr_data_default.glow_color));
        memcpy(defaults.blur         + i,         &chr_data_default.blur,         sizeof(chr_data_default.blur));
    }

    assign_chr_data_pointers(s->chars_data, &s->chr, text_nbchr);
    reset_chars_data_to_defaults(s);

    return 0;
}

struct target_range {
    int start_chr;
    int end_chr;
    float overlap;
};

static int apply_effects_char(struct text_priv *s, const struct target_range *range, double effect_t, int effect_id)
{
    struct ngl_node *effect_node = s->effect_nodes[effect_id];
    struct texteffect_priv *effect_priv = effect_node->priv_data;

    const int text_nbchr  = ngli_darray_count(&s->text_ctx.chars);
    const float target_duration  = text_nbchr - range->overlap * (text_nbchr - 1);
    const float target_timescale = (1. - range->overlap) / target_duration;

    for (int c = range->start_chr; c < range->end_chr; c++) {
        const int c_pos = s->effect_positions[effect_id][c];
        const float t_prv = target_timescale * c_pos;
        const float t_nxt = t_prv + 1. / target_duration;
        const double target_t = NGLI_LINEAR_INTERP(t_prv, t_nxt, effect_t);
        int ret = update_character_data(s, effect_priv, c, target_t);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int apply_effects_text(struct text_priv *s, const struct target_range *range, double effect_t, int effect_id)
{
    struct ngl_node *effect_node = s->effect_nodes[effect_id];
    struct texteffect_priv *effect_priv = effect_node->priv_data;

    for (int c = range->start_chr; c < range->end_chr; c++) {
        int ret = update_character_data(s, effect_priv, c, effect_t);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int set_target_range(struct text_priv *s, struct texteffect_priv *effect, double t, struct target_range *r)
{
    int ret;
    float start_pos = 0.;
    float end_pos   = 1.;
    r->overlap = 0.;

    if ((ret = set_f32_from_node(&start_pos,  effect->start_pos_node, t)) < 0 ||
        (ret = set_f32_from_node(&end_pos,    effect->end_pos_node,   t)) < 0 ||
        (ret = set_f32_from_node(&r->overlap, effect->overlap_node,   t)) < 0)
        return ret;

    const int text_nbchr = ngli_darray_count(&s->text_ctx.chars);
    r->start_chr = NGLI_MAX(lrintf(text_nbchr * start_pos), 0);
    r->end_chr   = NGLI_MIN(lrintf(text_nbchr * end_pos), text_nbchr);
    return 0;
}

static int apply_effects(struct text_priv *s, double t)
{
    int ret;

    reset_chars_data_to_defaults(s);

    for (int i = 0; i < s->nb_effect_nodes; i++) {
        struct ngl_node *effect_node = s->effect_nodes[i];
        struct texteffect_priv *effect_priv = effect_node->priv_data;

        if (t < effect_priv->start_time || t > effect_priv->end_time)
            continue;

        const double effect_t = NGLI_LINEAR_INTERP(effect_priv->start_time, effect_priv->end_time, t);

        struct target_range range;
        ret = set_target_range(s, effect_priv, effect_t, &range);
        if (ret < 0)
            return ret;

        switch (effect_priv->target) {
        case NGLI_TEXT_EFFECT_CHAR: ret = apply_effects_char(s, &range, effect_t, i); break;
        case NGLI_TEXT_EFFECT_TEXT: ret = apply_effects_text(s, &range, effect_t, i); break;
        default:
            ngli_assert(0); // TODO
        }
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int init_characters_pipeline(struct ngl_node *node, struct pipeline_desc_fg *desc)
{
    struct ngl_ctx *ctx = node->ctx;
    struct text_priv *s = node->priv_data;
    struct gctx *gctx = ctx->gctx;

    const struct pgcraft_texture textures[] = {
        {
            .name     = "tex",
            .type     = NGLI_PGCRAFT_SHADER_TEX_TYPE_TEXTURE2D,
            .stage    = NGLI_PROGRAM_SHADER_FRAG,
            .texture  = s->text_ctx.atlas_ref,
        },
    };

    const struct pgcraft_attribute attributes[] = {
        {
            .name     = "position",
            .type     = NGLI_TYPE_VEC4,
            .format   = NGLI_FORMAT_R32G32B32_SFLOAT,
            .stride   = 3 * 4,
            .buffer   = s->vertices,
        },
        {
            .name     = "uvcoord",
            .type     = NGLI_TYPE_VEC2,
            .format   = NGLI_FORMAT_R32G32_SFLOAT,
            .stride   = 2 * 4,
            .buffer   = s->uvcoords,
        },
        {
            .name     = "ngl_glyph_rect",
            .type     = NGLI_TYPE_VEC4,
            .format   = NGLI_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * 4,
            .buffer   = s->glyph_rects,
        },
        {
            .name     = "ngl_glyph_size",
            .type     = NGLI_TYPE_VEC2,
            .format   = NGLI_FORMAT_R32G32_SFLOAT,
            .stride   = 2 * 4,
            .buffer   = s->glyph_sizes,
        },
    };

    const int text_nbchr = ngli_darray_count(&s->text_ctx.chars);
    const struct pgcraft_uniform uniforms[] = {
        {.name = "ngl_glyph_nb",      .type = NGLI_TYPE_INT,   .stage = NGLI_PROGRAM_SHADER_VERT},
        {.name = "modelview_matrix",  .type = NGLI_TYPE_MAT4,  .stage = NGLI_PROGRAM_SHADER_VERT},
        {.name = "projection_matrix", .type = NGLI_TYPE_MAT4,  .stage = NGLI_PROGRAM_SHADER_VERT},
        {.name = "chr_transform",     .type = NGLI_TYPE_MAT4,  .stage = NGLI_PROGRAM_SHADER_VERT, .count = text_nbchr},
        {.name = "chr_color",         .type = NGLI_TYPE_VEC4,  .stage = NGLI_PROGRAM_SHADER_FRAG, .count = text_nbchr},
        {.name = "chr_alpha",         .type = NGLI_TYPE_FLOAT, .stage = NGLI_PROGRAM_SHADER_FRAG, .count = text_nbchr},
        {.name = "chr_stroke_width",  .type = NGLI_TYPE_FLOAT, .stage = NGLI_PROGRAM_SHADER_FRAG, .count = text_nbchr},
        {.name = "chr_stroke_color",  .type = NGLI_TYPE_VEC4,  .stage = NGLI_PROGRAM_SHADER_FRAG, .count = text_nbchr},
        {.name = "chr_glow_width",    .type = NGLI_TYPE_FLOAT, .stage = NGLI_PROGRAM_SHADER_FRAG, .count = text_nbchr},
        {.name = "chr_glow_color",    .type = NGLI_TYPE_VEC4,  .stage = NGLI_PROGRAM_SHADER_FRAG, .count = text_nbchr},
        {.name = "chr_blur",          .type = NGLI_TYPE_FLOAT, .stage = NGLI_PROGRAM_SHADER_FRAG, .count = text_nbchr},
    };

    /* This controls how the characters blend onto the background */
    struct graphicstate state = desc->state;
    state.blend = 1;
    state.blend_src_factor   = NGLI_BLEND_FACTOR_SRC_ALPHA;
    state.blend_dst_factor   = NGLI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    state.blend_src_factor_a = NGLI_BLEND_FACTOR_SRC_ALPHA;
    state.blend_dst_factor_a = NGLI_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

    struct pipeline_params pipeline_params = {
        .type          = NGLI_PIPELINE_TYPE_GRAPHICS,
        .graphics      = {
            .topology       = NGLI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state          = state,
            .rt_desc        = desc->rt_desc,
        }
    };

    const struct pgcraft_params crafter_params = {
        .vert_base        = glyph_vertex_data,
        .frag_base        = glyph_fragment_data,
        .uniforms         = uniforms,
        .nb_uniforms      = NGLI_ARRAY_NB(uniforms),
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = glyph_io_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(glyph_io_vars),
    };

    desc->crafter = ngli_pgcraft_create(ctx);
    if (!desc->crafter)
        return NGL_ERROR_MEMORY;

    struct pipeline_resource_params pipeline_resource_params = {0};
    int ret = ngli_pgcraft_craft(desc->crafter, &pipeline_params, &pipeline_resource_params, &crafter_params);
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

    //for (int i = 0; i < 4; i++)
    //    LOG(ERROR, "%d: %s", i, pipeline_params.attributes_desc[i].name);
    ngli_assert(!strcmp("position", pipeline_params.attributes_desc[0].name));
    ngli_assert(!strcmp("uvcoord", pipeline_params.attributes_desc[1].name));

    // XXX: those may not be defined if not used in shaders
    for (int i = 0; i < pipeline_params.nb_attributes; i++) {
        if (!strcmp(pipeline_params.attributes_desc[0].name, "ngl_glyph_rect"))
            desc->glyph_rects_index = i;
        else if (!strcmp(pipeline_params.attributes_desc[0].name, "ngl_glyph_size"))
            desc->glyph_sizes_index = i;
    }

    desc->glyph_nb_index          = ngli_pgcraft_get_uniform_index(desc->crafter, "ngl_glyph_nb",      NGLI_PROGRAM_SHADER_VERT);
    desc->modelview_matrix_index  = ngli_pgcraft_get_uniform_index(desc->crafter, "modelview_matrix",  NGLI_PROGRAM_SHADER_VERT);
    desc->projection_matrix_index = ngli_pgcraft_get_uniform_index(desc->crafter, "projection_matrix", NGLI_PROGRAM_SHADER_VERT);
    desc->chr_transform_index     = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_transform",     NGLI_PROGRAM_SHADER_VERT);
    desc->chr_color_index         = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_color",         NGLI_PROGRAM_SHADER_FRAG);
    desc->chr_alpha_index         = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_alpha",         NGLI_PROGRAM_SHADER_FRAG);
    desc->chr_stroke_width_index  = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_stroke_width",  NGLI_PROGRAM_SHADER_FRAG);
    desc->chr_stroke_color_index  = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_stroke_color",  NGLI_PROGRAM_SHADER_FRAG);
    desc->chr_glow_width_index    = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_glow_width",    NGLI_PROGRAM_SHADER_FRAG);
    desc->chr_glow_color_index    = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_glow_color",    NGLI_PROGRAM_SHADER_FRAG);
    desc->chr_blur_index          = ngli_pgcraft_get_uniform_index(desc->crafter, "chr_blur",          NGLI_PROGRAM_SHADER_FRAG);

    return 0;
}

static void destroy_characters_resources(struct text_priv *s)
{
    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    const int nb_descs = ngli_darray_count(&s->pipeline_descs);
    for (int i = 0; i < nb_descs; i++) {
        struct pipeline_desc_fg *desc = &descs[i].fg;
        ngli_pipeline_freep(&desc->pipeline);
        ngli_pgcraft_freep(&desc->crafter);

        desc->glyph_rects_index = 0;
        desc->glyph_sizes_index = 0;
        desc->glyph_nb_index = 0;
        desc->modelview_matrix_index = 0;
        desc->projection_matrix_index = 0;
        desc->chr_transform_index = 0;
        desc->chr_color_index = 0;
        desc->chr_alpha_index = 0;
        desc->chr_stroke_width_index = 0;
        desc->chr_stroke_color_index = 0;
        desc->chr_glow_width_index = 0;
        desc->chr_glow_color_index = 0;
        desc->chr_blur_index = 0;
    }
    ngli_buffer_freep(&s->vertices);
    ngli_buffer_freep(&s->uvcoords);
    ngli_buffer_freep(&s->indices);
    s->nb_indices = 0;
    ngli_buffer_freep(&s->glyph_rects);
    ngli_buffer_freep(&s->glyph_sizes);
    for (int i = 0; i < s->nb_effect_nodes; i++)
        ngli_freep(&s->effect_positions[i]);
    s->chars_data = NULL;
    ngli_freep(&s->chars_data_default);
    s->chars_data_size = 0;
}

static int update_character_geometries(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct gctx *gctx = ctx->gctx;
    struct text_priv *s = node->priv_data;

    struct text *text = &s->text_ctx;
    int ret = ngli_text_set_string(text, s->text);
    if (ret < 0)
        return ret;

    const int text_nbchr = ngli_darray_count(&text->chars);
    if (!text_nbchr) {
        destroy_characters_resources(s);
        return 0;
    }

    // XXX: rename nb to sizes or something
    const int nb_vertices = text_nbchr * 4 * 3;
    const int nb_uvcoords = text_nbchr * 4 * 2;
    const int nb_indices  = text_nbchr * 6;
    const int nb_glyph_rects = text_nbchr * 4 * 4;
    const int nb_glyph_sizes = text_nbchr * 4 * 2;
    float *vertices = ngli_calloc(nb_vertices, sizeof(*vertices));
    float *uvcoords = ngli_calloc(nb_uvcoords, sizeof(*uvcoords));
    short *indices  = ngli_calloc(nb_indices, sizeof(*indices));
    float *glyph_rects = ngli_calloc(nb_glyph_rects, sizeof(*glyph_rects));
    float *glyph_sizes = ngli_calloc(nb_glyph_sizes, sizeof(*glyph_sizes));
    if (!vertices || !uvcoords || !indices || !glyph_rects || !glyph_sizes) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    /* Text/Box ratio */
    const float box_width_len  = ngli_vec3_length(s->box_width);
    const float box_height_len = ngli_vec3_length(s->box_height);
    static const int default_ar[2] = {1, 1};
    const int *ar = s->aspect_ratio[1] ? s->aspect_ratio : default_ar;
    const float box_ratio = ar[0] * box_width_len / (float)(ar[1] * box_height_len);
    const float text_ratio = text->width / (float)text->height;

    float ratio_w, ratio_h;
    if (text_ratio < box_ratio) {
        ratio_w = text_ratio / box_ratio;
        ratio_h = 1.0;
    } else {
        ratio_w = 1.0;
        ratio_h = box_ratio / text_ratio;
    }

    /* Apply aspect ratio and font scaling */
    float width[3];
    float height[3];
    ngli_vec3_scale(width, s->box_width, ratio_w * s->font_scale);
    ngli_vec3_scale(height, s->box_height, ratio_h * s->font_scale);

    /* Adjust text position according to alignment settings */
    float align_padw[3];
    float align_padh[3];
    ngli_vec3_sub(align_padw, s->box_width,  width);
    ngli_vec3_sub(align_padh, s->box_height, height);

    const float spx = (s->halign == HALIGN_CENTER ? .5f :
                       s->halign == HALIGN_RIGHT  ? 1.f :
                       0.f);
    const float spy = (s->valign == VALIGN_CENTER ? .5f :
                       s->valign == VALIGN_TOP    ? 1.f :
                       0.f);

    const float corner[3] = {
        BC(0) + align_padw[0] * spx + align_padh[0] * spy,
        BC(1) + align_padw[1] * spx + align_padh[1] * spy,
        BC(2) + align_padw[2] * spx + align_padh[2] * spy,
    };

    const struct char_info *chars = ngli_darray_data(&text->chars);
    for (int n = 0; n < text_nbchr; n++) {
        const struct char_info *chr = &chars[n];

        /* character dimension */
        float chr_width[3], chr_height[3];
        const float rw = chr->w / (float)text->width;
        const float rh = chr->h / (float)text->height;
        ngli_vec3_scale(chr_width, width, rw);
        ngli_vec3_scale(chr_height, height, rh);

        /* character position */
        const float rx = chr->x / (float)text->width;
        const float ry = chr->y / (float)text->height;

        /* quad vertices */
        const float chr_corner[3] = {
            corner[0] + width[0] * rx + height[0] * ry,
            corner[1] + width[1] * rx + height[1] * ry,
            corner[2] + width[2] * rx + height[2] * ry,
        };
        const float chr_vertices[] = {
            C(0),               C(1),               C(2),
            C(0) + W(0),        C(1) + W(1),        C(2) + W(2),
            C(0) + H(0) + W(0), C(1) + H(1) + W(1), C(2) + H(2) + W(2),
            C(0) + H(0),        C(1) + H(1),        C(2) + H(2),
        };
        memcpy(vertices + 4 * 3 * n, chr_vertices, sizeof(chr_vertices));

        /* focus uvcoords on the character in the atlas texture */
        memcpy(uvcoords + 4 * 2 * n, chr->atlas_uvcoords, sizeof(chr->atlas_uvcoords));

        /* glyph info (repeated for the 4 vertices of the char) */
        const float rect[] = {
            NGLI_MIN(
                NGLI_MIN(chr->atlas_uvcoords[0*2 + 0], chr->atlas_uvcoords[1*2 + 0]),
                NGLI_MIN(chr->atlas_uvcoords[2*2 + 0], chr->atlas_uvcoords[3*2 + 0])
            ),
            NGLI_MIN(
                NGLI_MIN(chr->atlas_uvcoords[0*2 + 1], chr->atlas_uvcoords[1*2 + 1]),
                NGLI_MIN(chr->atlas_uvcoords[2*2 + 1], chr->atlas_uvcoords[3*2 + 1])
            ),
            NGLI_MAX(
                NGLI_MAX(chr->atlas_uvcoords[0*2 + 0], chr->atlas_uvcoords[1*2 + 0]),
                NGLI_MAX(chr->atlas_uvcoords[2*2 + 0], chr->atlas_uvcoords[3*2 + 0])
            ),
            NGLI_MAX(
                NGLI_MAX(chr->atlas_uvcoords[0*2 + 1], chr->atlas_uvcoords[1*2 + 1]),
                NGLI_MAX(chr->atlas_uvcoords[2*2 + 1], chr->atlas_uvcoords[3*2 + 1])
            ),
        };
        //LOG(ERROR, "%f,%f %f,%f", rect[0], rect[1], rect[2], rect[3]);
        const float rects[] = {
            rect[0], rect[1], rect[2], rect[3],
            rect[0], rect[1], rect[2], rect[3],
            rect[0], rect[1], rect[2], rect[3],
            rect[0], rect[1], rect[2], rect[3],
        };
        const float sizes[] = {
            chr->w, chr->h,
            chr->w, chr->h,
            chr->w, chr->h,
            chr->w, chr->h,
        };
        //LOG(ERROR, "%d %d", chr->w, chr->h);
        memcpy(glyph_rects + 4 * 4 * n, rects, sizeof(rects));
        memcpy(glyph_sizes + 4 * 2 * n, sizes, sizeof(sizes));

        /* quad for each character is made of 2 triangles */
        const short chr_indices[] = { n*4 + 0, n*4 + 1, n*4 + 2, n*4 + 0, n*4 + 2, n*4 + 3 };
        memcpy(indices + n * NGLI_ARRAY_NB(chr_indices), chr_indices, sizeof(chr_indices));
    }

    if (nb_indices > s->nb_indices) { // need re-alloc
        destroy_characters_resources(s);

        ret = set_positions(s, text_nbchr);
        if (ret < 0)
            return ret;

        ret = init_characters_data(s, text_nbchr);
        if (ret < 0)
            return ret;

        s->vertices = ngli_buffer_create(gctx);
        s->uvcoords = ngli_buffer_create(gctx);
        s->indices  = ngli_buffer_create(gctx);
        s->glyph_rects = ngli_buffer_create(gctx);
        s->glyph_sizes = ngli_buffer_create(gctx);
        if (!s->vertices || !s->uvcoords || !s->indices || !s->glyph_rects || !s->glyph_sizes) {
            ret = NGL_ERROR_MEMORY;
            goto end;
        }

        if ((ret = ngli_buffer_init(s->vertices, nb_vertices * sizeof(*vertices), NGLI_BUFFER_USAGE_DYNAMIC)) < 0 ||
            (ret = ngli_buffer_init(s->uvcoords, nb_uvcoords * sizeof(*uvcoords), NGLI_BUFFER_USAGE_DYNAMIC)) < 0 ||
            (ret = ngli_buffer_init(s->indices,  nb_indices  * sizeof(*indices),  NGLI_BUFFER_USAGE_DYNAMIC)) < 0 ||
            (ret = ngli_buffer_init(s->glyph_rects, nb_glyph_rects * sizeof(*glyph_rects), NGLI_BUFFER_USAGE_DYNAMIC)) < 0 ||
            (ret = ngli_buffer_init(s->glyph_sizes, nb_glyph_sizes * sizeof(*glyph_sizes), NGLI_BUFFER_USAGE_DYNAMIC)) < 0)
            goto end;

        struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
        const int nb_descs = ngli_darray_count(&s->pipeline_descs);
        for (int i = 0; i < nb_descs; i++) {
            struct pipeline_desc_fg *desc = &descs[i].fg;

            ret = init_characters_pipeline(node, desc);
            if (ret < 0)
                goto end;

            ngli_pipeline_update_attribute(desc->pipeline, 0, s->vertices);
            ngli_pipeline_update_attribute(desc->pipeline, 1, s->uvcoords);
            ngli_pipeline_update_attribute(desc->pipeline, desc->glyph_rects_index, s->glyph_rects);
            ngli_pipeline_update_attribute(desc->pipeline, desc->glyph_sizes_index, s->glyph_sizes);
        }
    }

    if ((ret = ngli_buffer_upload(s->vertices, vertices, nb_vertices * sizeof(*vertices))) < 0 ||
        (ret = ngli_buffer_upload(s->uvcoords, uvcoords, nb_uvcoords * sizeof(*uvcoords))) < 0 ||
        (ret = ngli_buffer_upload(s->indices, indices, nb_indices * sizeof(*indices))) < 0 ||
        (ret = ngli_buffer_upload(s->glyph_rects, glyph_rects, nb_glyph_rects * sizeof(*glyph_rects))) < 0 ||
        (ret = ngli_buffer_upload(s->glyph_sizes, glyph_sizes, nb_glyph_sizes * sizeof(*glyph_sizes))) < 0)
        goto end;

    s->nb_indices = nb_indices;

end:
    ngli_free(vertices);
    ngli_free(uvcoords);
    ngli_free(indices);
    ngli_free(glyph_rects);
    ngli_free(glyph_sizes);
    return ret;
}

static int init_bounding_box_geometry(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct gctx *gctx = ctx->gctx;
    struct text_priv *s = node->priv_data;

    static const short indices[] = { 0, 1, 2, 0, 2, 3 };
    const float vertices[] = {
        BC(0),                 BC(1),                 BC(2),
        BC(0) + BW(0),         BC(1) + BW(1),         BC(2) + BW(2),
        BC(0) + BH(0) + BW(0), BC(1) + BH(1) + BW(1), BC(2) + BH(2) + BW(2),
        BC(0) + BH(0),         BC(1) + BH(1),         BC(2) + BH(2),
    };

    s->bg_vertices = ngli_buffer_create(gctx);
    s->bg_indices  = ngli_buffer_create(gctx);
    if (!s->bg_vertices || !s->bg_indices)
        return NGL_ERROR_MEMORY;

    int ret;
    if ((ret = ngli_buffer_init(s->bg_vertices, sizeof(vertices), NGLI_BUFFER_USAGE_STATIC)) < 0 ||
        (ret = ngli_buffer_init(s->bg_indices,  sizeof(indices),  NGLI_BUFFER_USAGE_STATIC)) < 0)
        return ret;

    if ((ret = ngli_buffer_upload(s->bg_vertices, vertices, sizeof(vertices))) < 0 ||
        (ret = ngli_buffer_upload(s->bg_indices,  indices,  sizeof(indices))) < 0)
        return ret;

    s->nb_bg_indices = NGLI_ARRAY_NB(indices);

    return 0;
}

static int text_init(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;

    struct text_config text_cfg = {
        .padding  = s->padding,
        .fontfile = s->font_file,
        .wmode    = s->writing_mode,
    };
    int ret = ngli_text_init(&s->text_ctx, node->ctx, &text_cfg);
    if (ret < 0)
        return ret;

    ngli_darray_init(&s->pipeline_descs, sizeof(struct pipeline_desc), 0);

    ret = init_bounding_box_geometry(node);
    if (ret < 0)
        return ret;

    if (s->nb_effect_nodes) {
        s->effect_positions = ngli_calloc(s->nb_effect_nodes, sizeof(*s->effect_positions));
        if (!s->effect_positions)
            return NGL_ERROR_MEMORY;
    }

    ret = update_character_geometries(node);
    if (ret < 0)
        return ret;

    return 0;
}

static int bg_prepare(struct ngl_node *node, struct pipeline_desc_bg *desc)
{
    struct ngl_ctx *ctx = node->ctx;
    struct rnode *rnode = ctx->rnode_pos;
    struct text_priv *s = node->priv_data;
    struct gctx *gctx = ctx->gctx;

    const struct pgcraft_uniform uniforms[] = {
        {.name = "modelview_matrix",  .type = NGLI_TYPE_MAT4, .stage = NGLI_PROGRAM_SHADER_VERT, .data = NULL},
        {.name = "projection_matrix", .type = NGLI_TYPE_MAT4, .stage = NGLI_PROGRAM_SHADER_VERT, .data = NULL},
        {.name = "color",             .type = NGLI_TYPE_VEC4, .stage = NGLI_PROGRAM_SHADER_FRAG, .data = s->bg_color},
    };

    const struct pgcraft_attribute attributes[] = {
        {
            .name     = "position",
            .type     = NGLI_TYPE_VEC4,
            .format   = NGLI_FORMAT_R32G32B32_SFLOAT,
            .stride   = 3 * 4,
            .buffer   = s->bg_vertices,
        },
    };

    struct pipeline_params pipeline_params = {
        .type          = NGLI_PIPELINE_TYPE_GRAPHICS,
        .graphics      = {
            .topology       = NGLI_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state          = rnode->graphicstate,
            .rt_desc        = rnode->rendertarget_desc,
        }
    };

    const struct pgcraft_params crafter_params = {
        .vert_base        = bg_vertex_data,
        .frag_base        = bg_fragment_data,
        .uniforms         = uniforms,
        .nb_uniforms      = NGLI_ARRAY_NB(uniforms),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
    };

    desc->crafter = ngli_pgcraft_create(ctx);
    if (!desc->crafter)
        return NGL_ERROR_MEMORY;

    struct pipeline_resource_params pipeline_resource_params = {0};
    int ret = ngli_pgcraft_craft(desc->crafter, &pipeline_params, &pipeline_resource_params, &crafter_params);
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
    desc->color_index = ngli_pgcraft_get_uniform_index(desc->crafter, "color", NGLI_PROGRAM_SHADER_FRAG);

    return 0;
}

static int fg_prepare(struct ngl_node *node, struct pipeline_desc_fg *desc)
{
    struct ngl_ctx *ctx = node->ctx;
    struct rnode *rnode = ctx->rnode_pos;
    struct text_priv *s = node->priv_data;

    desc->state   = rnode->graphicstate;
    desc->rt_desc = rnode->rendertarget_desc;

    const int text_nbchr = ngli_darray_count(&s->text_ctx.chars);
    if (!text_nbchr)
        return 0;

    return init_characters_pipeline(node, desc);
}

static int text_prepare(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct text_priv *s = node->priv_data;

    struct pipeline_desc *desc = ngli_darray_push(&s->pipeline_descs, NULL);
    if (!desc)
        return NGL_ERROR_MEMORY;
    ctx->rnode_pos->id = ngli_darray_count(&s->pipeline_descs) - 1;

    memset(desc, 0, sizeof(*desc));

    int ret = bg_prepare(node, &desc->bg);
    if (ret < 0)
        return ret;

    ret = fg_prepare(node, &desc->fg);
    if (ret < 0)
        return ret;

    return 0;
}

static int text_update(struct ngl_node *node, double t)
{
    int ret;
    struct text_priv *s = node->priv_data;

    if (s->live_changed) {
        ret = update_character_geometries(node);
        if (ret < 0)
            return ret;
        s->live_changed = 0;
    }

    ret = apply_effects(s, t);
    if (ret < 0)
        return ret;

    return 0;
}

static void text_draw(struct ngl_node *node)
{
    struct ngl_ctx *ctx = node->ctx;
    struct text_priv *s = node->priv_data;

    const float *modelview_matrix  = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const float *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);

    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    struct pipeline_desc *desc = &descs[ctx->rnode_pos->id];

    if (ctx->begin_render_pass) {
        struct gctx *gctx = ctx->gctx;
        ngli_gctx_begin_render_pass(gctx, ctx->current_rendertarget);
        ctx->begin_render_pass = 0;
    }

    struct pipeline_desc_bg *bg_desc = &desc->bg;
    ngli_pipeline_update_uniform(bg_desc->pipeline, bg_desc->modelview_matrix_index, modelview_matrix);
    ngli_pipeline_update_uniform(bg_desc->pipeline, bg_desc->projection_matrix_index, projection_matrix);
    ngli_pipeline_update_uniform(bg_desc->pipeline, bg_desc->color_index, s->bg_color);
    ngli_pipeline_draw_indexed(bg_desc->pipeline, s->bg_indices, NGLI_FORMAT_R16_UNORM, s->nb_bg_indices, 1);

    if (s->nb_indices) {
        struct pipeline_desc_fg *fg_desc = &desc->fg;
        const int nb_glyph = ngli_darray_count(&s->text_ctx.chars);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->glyph_nb_index,          &nb_glyph);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->modelview_matrix_index,  modelview_matrix);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->projection_matrix_index, projection_matrix);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_transform_index,     s->chr.transform);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_color_index,         s->chr.color);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_alpha_index,         s->chr.alpha);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_stroke_width_index,  s->chr.stroke_width);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_stroke_color_index,  s->chr.stroke_color);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_glow_width_index,    s->chr.glow_width);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_glow_color_index,    s->chr.glow_color);
        ngli_pipeline_update_uniform(fg_desc->pipeline, fg_desc->chr_blur_index,          s->chr.blur);
        ngli_pipeline_draw_indexed(fg_desc->pipeline, s->indices, NGLI_FORMAT_R16_UNORM, s->nb_indices, 1);
    }
}

static void text_uninit(struct ngl_node *node)
{
    struct text_priv *s = node->priv_data;
    struct pipeline_desc *descs = ngli_darray_data(&s->pipeline_descs);
    const int nb_descs = ngli_darray_count(&s->pipeline_descs);
    for (int i = 0; i < nb_descs; i++) {
        struct pipeline_desc *desc = &descs[i];
        ngli_pipeline_freep(&desc->bg.pipeline);
        ngli_pgcraft_freep(&desc->bg.crafter);
    }
    ngli_buffer_freep(&s->bg_vertices);
    ngli_buffer_freep(&s->bg_indices);

    destroy_characters_resources(s);
    ngli_freep(&s->effect_positions);
    ngli_darray_reset(&s->pipeline_descs);
    ngli_text_reset(&s->text_ctx);
}

const struct node_class ngli_text_class = {
    .id        = NGL_NODE_TEXT,
    .name      = "Text",
    .init      = text_init,
    .prepare   = text_prepare,
    .update    = text_update,
    .draw      = text_draw,
    .uninit    = text_uninit,
    .priv_size = sizeof(struct text_priv),
    .params    = text_params,
    .file      = __FILE__,
};
