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

#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>
#ifdef HAVE_TEXTLIBS
#include <hb.h>
#include <hb-ft.h>
#endif

#include "darray.h"
#include "hmap.h"
#include "nodes.h"

enum writing_mode {
    NGLI_TEXT_WRITING_MODE_UNDEFINED,
    NGLI_TEXT_WRITING_MODE_HORIZONTAL_TB,
    NGLI_TEXT_WRITING_MODE_VERTICAL_RL,
    NGLI_TEXT_WRITING_MODE_VERTICAL_LR,
};

enum char_category {
    NGLI_TEXT_CHAR_CATEGORY_NONE,
    NGLI_TEXT_CHAR_CATEGORY_SPACE,
    NGLI_TEXT_CHAR_CATEGORY_LINEBREAK,
};

struct char_info {
    int x, y, w, h;
    enum char_category category;
    float atlas_uvcoords[8];
};

struct text_config {
    const char *fontfile;
    int padding;
    enum writing_mode wmode;  // TODO: expose to the top-level user (in node text)
};

struct text {
    struct ngl_ctx *ctx;
    struct text_config config;
    int width;
    int height;
    struct darray chars; // struct char_info
    struct texture *atlas_ref;

#ifdef HAVE_TEXTLIBS
    FT_Library ft_library;
    FT_Face ft_face;
    hb_font_t *hb_font;
    struct darray lines; // struct line
    struct hmap *glyph_index; // struct glyph
    struct texture *owned_atlas;
#endif
};

int ngli_text_init(struct text *s, struct ngl_ctx *ctx, const struct text_config *cfg);
int ngli_text_set_string(struct text *s, const char *str);
void ngli_text_reset(struct text *s);

#endif
