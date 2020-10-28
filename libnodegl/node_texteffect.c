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

#include <stddef.h>

#include "log.h"
#include "nodes.h"

static const struct param_choices target_choices = {
    .name = "text_target",
    .consts = {
        {"char",         NGLI_TEXT_EFFECT_CHAR,         .desc=NGLI_DOCSTRING("characters")},
        {"char_nospace", NGLI_TEXT_EFFECT_CHAR_NOSPACE, .desc=NGLI_DOCSTRING("characters without space")},
        {"word",         NGLI_TEXT_EFFECT_WORD,         .desc=NGLI_DOCSTRING("words")},
        {"line",         NGLI_TEXT_EFFECT_LINE,         .desc=NGLI_DOCSTRING("lines")},
        {"text",         NGLI_TEXT_EFFECT_TEXT,         .desc=NGLI_DOCSTRING("whole text")},
        {NULL}
    }
};

#define FLOAT_NODE_TYPES (const int[]){NGL_NODE_UNIFORMFLOAT, NGL_NODE_ANIMATEDFLOAT, NGL_NODE_NOISE, -1}
#define VEC4_NODE_TYPES  (const int[]){NGL_NODE_UNIFORMVEC4,  NGL_NODE_ANIMATEDVEC4,  -1}

#define OFFSET(x) offsetof(struct texteffect_priv, x)
static const struct node_param texteffect_params[] = {
    {"start",        PARAM_TYPE_DBL, OFFSET(start_time), {.dbl=0},
                     .desc=NGLI_DOCSTRING("absolute start time of the effect")},
    {"end",          PARAM_TYPE_DBL, OFFSET(end_time), {.dbl=5},
                     .desc=NGLI_DOCSTRING("absolute end time of the effect")},
    {"target",       PARAM_TYPE_SELECT, OFFSET(target), {.i64=NGLI_TEXT_EFFECT_TEXT},
                     .choices=&target_choices,
                     .desc=NGLI_DOCSTRING("segmentation target of the effect")},
    {"random",       PARAM_TYPE_BOOL, OFFSET(random),
                     .desc=NGLI_DOCSTRING("randomize the order the effect are applied on the target")},
    {"random_seed",  PARAM_TYPE_INT, OFFSET(random_seed),
                     .desc=NGLI_DOCSTRING("random seed, use < 0 to disable it")},
    {"start_pos",    PARAM_TYPE_NODE, OFFSET(start_pos_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("text position where the effect starts")},
    {"end_pos",      PARAM_TYPE_NODE, OFFSET(end_pos_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("text position where the effect ends")},
    {"overlap",      PARAM_TYPE_NODE, OFFSET(overlap_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("overlap factor between target elements")},
    {"transform",    PARAM_TYPE_NODE, OFFSET(transform_chain), .node_types=TRANSFORM_TYPES_LIST,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("transformation chain")},
    {"line_spacing", PARAM_TYPE_NODE, OFFSET(line_spacing_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("extra line spacing")},
    {"char_spacing", PARAM_TYPE_NODE, OFFSET(char_spacing_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("extra character spacing")},
    {"alpha",        PARAM_TYPE_NODE, OFFSET(alpha_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("alpha/opacity")},
    {"color",        PARAM_TYPE_NODE, OFFSET(color_node),
                     .node_types=VEC4_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("characters fill color")},
    {"stroke_width", PARAM_TYPE_NODE, OFFSET(stroke_width_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("characters stroke width")},
    {"stroke_color", PARAM_TYPE_NODE, OFFSET(stroke_color_node),
                     .node_types=VEC4_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("characters stroke color")},
    {"glow_width",   PARAM_TYPE_NODE, OFFSET(glow_width_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("characters stroke width")},
    {"glow_color",   PARAM_TYPE_NODE, OFFSET(glow_color_node),
                     .node_types=VEC4_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("characters stroke color")},
    {"blur",         PARAM_TYPE_NODE, OFFSET(blur_node),
                     .node_types=FLOAT_NODE_TYPES,
                     .flags=PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                     .desc=NGLI_DOCSTRING("characters blur")},
    {NULL}
};

static int texteffect_init(struct ngl_node *node)
{
    struct texteffect_priv *s = node->priv_data;
    if (s->start_time >= s->end_time) {
        LOG(ERROR, "end time must be strictly superior to start time");
        return NGL_ERROR_INVALID_ARG;
    }
    return 0;
}

const struct node_class ngli_texteffect_class = {
    .id        = NGL_NODE_TEXTEFFECT,
    .name      = "TextEffect",
    .init      = texteffect_init,
    .priv_size = sizeof(struct texteffect_priv),
    .params    = texteffect_params,
    .file      = __FILE__,
};
