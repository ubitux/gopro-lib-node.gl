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
#include <string.h>

#include "drawutils.h"
#include "hmap.h"
#include "log.h"
#include "memory.h"
#include "text.h"

#ifndef HAVE_TEXTLIBS
static int text_set_string_libs(struct text *s, const char *str)
{
    return NGL_ERROR_BUG;
}

static int text_init_from_libs(struct text *s)
{
    LOG(ERROR, "node.gl is not compiled with text libraries support "
        "(use ENABLE_TEXTLIBS=yes at build time)");
    return NGL_ERROR_UNSUPPORTED;
}
#else
#define FONT_SCALE (1<<6)
#define FONT_SIZE 72 /* internal resolution */

static int text_init_from_libs(struct text *s)
{
    FT_Error ft_error;
    const FT_F26Dot6 char_size = FONT_SIZE * FONT_SCALE;
    if ((ft_error = FT_Init_FreeType(&s->ft_library)) ||
        (ft_error = FT_New_Face(s->ft_library, s->config.fontfile, 0, &s->ft_face)) ||
        (ft_error = FT_Set_Char_Size(s->ft_face, char_size, 0, 0, 0))) {
        LOG(ERROR, "unable to initialize FreeType");
        return NGL_ERROR_EXTERNAL;
    }

    s->hb_font = hb_ft_font_create(s->ft_face, NULL);
    if (!s->hb_font)
        return NGL_ERROR_MEMORY; // XXX: is this the proper way of checking?

    return 0;
}

struct glyph {
    uint8_t *buf;
    int w, h;
    int linesize;
    int bearing_x, bearing_y;
    float uvcoords[8];
};

static void free_glyph(void *user_arg, void *data)
{
    struct glyph *glyph = data;
    ngli_freep(&glyph->buf);
    ngli_freep(&glyph);
}

/*
 * Pad/Spread is arbitrary: it represents how far an effect such as glowing
 * could be applied, but it's also used for padding around the glyph to be that
 * the extremities of the distance map are always black, and thus not affect
 * neighbor glyph, typically when relying on mipmapping.
 */
#define DF_CHAR_PAD 16

#define INF FLT_MAX
#define SQ(x) ((x) * (x))
#define SRC(x) src[x * src_stride]
#define PARABOLLA(k) ((SRC(q) + SQ(q)) - (SRC(v[k]) + SQ(v[k]))) / (2 * q - 2 * v[k]);

/*
 * Direct implementation of the DT(f) algorithm presented in "Distance
 * Transforms of Sampled Functions" by Pedro F. Felzenszwalb and Daniel P.
 * Huttenlocher (2012).
 */
static void dt_1d(float *dst, int dst_stride, const float *src, int src_stride, int n, int *v, float *z)
{
    int k = 0;

    memset(v, 0, n * sizeof(*v));

    z[0] = -INF;
    z[1] =  INF;
    for (int q = 1; q < n; q++) {
        float s = PARABOLLA(k);
        while (s <= z[k]) {
            k--;
            s = PARABOLLA(k);
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = INF;
    }

    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q)
            k++;
        dst[q * dst_stride] = SQ(q - v[k]) + SRC(v[k]);
    }
}

static float clamp(double v, double min, double max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

void computegradient(double *img, int w, int h, double *gx, double *gy);
void edtaa3(double *img, double *gx, double *gy, int w, int h, short *distx, short *disty, double *dist);

static int distmap_create(struct glyph *glyph, const FT_GlyphSlot slot)
{
    int ret = 0;

    glyph->w = slot->bitmap.width + DF_CHAR_PAD * 2;
    glyph->h = slot->bitmap.rows  + DF_CHAR_PAD * 2;
    glyph->bearing_x = slot->bitmap_left;
    glyph->bearing_y = slot->bitmap_top;
    glyph->linesize = glyph->w; // XXX: align?

    if (glyph->w <= 0 || glyph->h <= 0)
        return 0;

    glyph->buf = ngli_calloc(glyph->h, glyph->linesize);
    if (!glyph->buf)
        return NGL_ERROR_MEMORY;

#if 1
    const int dt_size = glyph->w * glyph->h;
    short *xdist    = ngli_malloc(dt_size * sizeof(*xdist));
    short *ydist    = ngli_malloc(dt_size * sizeof(*ydist));
    double *gx      = ngli_calloc(dt_size, sizeof(*gx));
    double *gy      = ngli_calloc(dt_size, sizeof(*gy));
    double *dt_ref  = ngli_calloc(dt_size, sizeof(*dt_ref));
    double *dt_inv  = ngli_calloc(dt_size, sizeof(*dt_inv));
    double *outside = ngli_calloc(dt_size, sizeof(*outside));
    double *inside  = ngli_calloc(dt_size, sizeof(*inside));

    if (!xdist || !ydist || !gx || !gy || !dt_ref || !dt_inv || !outside || !inside) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    for (int y = 0; y < glyph->h; y++) {
        for (int x = 0; x < glyph->w; x++) {
            int bit = 0;
            const int inside_glyph = x >= DF_CHAR_PAD && x < glyph->w - DF_CHAR_PAD &&
                                     y >= DF_CHAR_PAD && y < glyph->h - DF_CHAR_PAD;
            if (inside_glyph) {
                const int glyph_x = x - DF_CHAR_PAD;
                const int glyph_y = y - DF_CHAR_PAD;
                bit = slot->bitmap.buffer[slot->bitmap.pitch * glyph_y + glyph_x];
            }

            dt_ref[y * glyph->w + x] = bit / 255.;
            dt_inv[y * glyph->w + x] = 1. - bit / 255.;
        }
    }

    computegradient(dt_ref, glyph->w, glyph->h, gx, gy);
    edtaa3(dt_ref, gx, gy, glyph->w, glyph->h, xdist, ydist, outside);
    for (int i = 0; i < dt_size; i++)
        if (outside[i] < 0)
            outside[i] = 0.0;

    memset(gx, 0, sizeof(*gx) * dt_size);
    memset(gy, 0, sizeof(*gy) * dt_size);

    computegradient(dt_inv, glyph->w, glyph->h, gx, gy);
    edtaa3(dt_inv, gx, gy, glyph->w, glyph->h, xdist, ydist, inside);
    for (int i = 0; i < dt_size; i++)
        if (inside[i] < 0)
            inside[i] = 0.0;

    const float scale = 1.f / DF_CHAR_PAD;
    uint8_t *dst = glyph->buf;
    for (int y = 0; y < glyph->h; y++) {
        for (int x = 0; x < glyph->w; x++) {
            const int i = y * glyph->w + x;
            const double src = (outside[i] - inside[i]) * scale;
            const float scaled_v = 1. - (src + 1.f) * .5f;
            const long int v = lrintf(scaled_v * 255);
            dst[x] = NGLI_MIN(NGLI_MAX(v, 0), 255);
        }
        dst += glyph->linesize;
    }

end:
    ngli_free(xdist);
    ngli_free(ydist);
    ngli_free(gx);
    ngli_free(gy);
    ngli_free(dt_inv);
    ngli_free(dt_ref);
    ngli_free(outside);
    ngli_free(inside);

#else
    const int dt_size = glyph->w * glyph->h;
    const int dt_size_1d = NGLI_MAX(glyph->w, glyph->h);

    int *v     = ngli_calloc(dt_size_1d, sizeof(*v));
    float *z   = ngli_calloc(dt_size_1d + 1, sizeof(*z));
    float *dts = ngli_calloc(dt_size * 3, sizeof(*dts));
    if (!v || !z || !dts) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    float *dt_ref = dts;
    float *dt_inv = dts + dt_size;
    float *dt_tmp = dts + dt_size * 2;

    for (int y = 0; y < glyph->h; y++) {
        for (int x = 0; x < glyph->w; x++) {
            int bit = 0;
            const int inside_glyph = x >= DF_CHAR_PAD && x < glyph->w - DF_CHAR_PAD &&
                                     y >= DF_CHAR_PAD && y < glyph->h - DF_CHAR_PAD;
            if (inside_glyph) {
                const int glyph_x = x - DF_CHAR_PAD;
                const int glyph_y = y - DF_CHAR_PAD;
                // XXX: we probably need something better for handling anti-aliased edges
                bit = slot->bitmap.buffer[slot->bitmap.pitch * glyph_y + glyph_x] > 127;
            }

            /* Set distance to 0 for the character, and infinite for the rest */
            dt_ref[y * glyph->w + x] = bit ? 0 : INF;

            /* Inverse of the above to be used for getting signed distances */
            dt_inv[y * glyph->w + x] = bit ? INF : 0;
        }
    }

    /* Vertical pass followed by Horizontal pass (outside) */
    for (int x = 0; x < glyph->w; x++)
        dt_1d(dt_tmp + x, glyph->w, dt_ref + x, glyph->w, glyph->h, v, z);
    for (int y = 0; y < glyph->h; y++)
        dt_1d(dt_ref + y*glyph->w, 1, dt_tmp + y*glyph->w, 1, glyph->w, v, z);
#if 0
    for (int i = 0; i < dt_size; i++)
        if (dt_ref[i] < 0)
            dt_ref[i] = 0.0;
#endif

    /* Same thing with the inverse distances (inside) */
    for (int x = 0; x < glyph->w; x++)
        dt_1d(dt_tmp + x, glyph->w, dt_inv + x, glyph->w, glyph->h, v, z);
    for (int y = 0; y < glyph->h; y++)
        dt_1d(dt_inv + y*glyph->w, 1, dt_tmp + y*glyph->w, 1, glyph->w, v, z);
#if 0
    for (int i = 0; i < dt_size; i++)
        if (dt_inv[i] < 0)
            dt_inv[i] = 0.0;
#endif

    /* Subtract inverse Dt from the ref to get the signed distance field (SDF).
     * The square root computations and normalization are also slipped in at
     * this step. */
    const float scale = -1.f / DF_CHAR_PAD;
    for (int i = 0; i < dt_size; i++)
        dt_ref[i] = clamp((sqrtf(dt_ref[i]) - sqrtf(dt_inv[i])) * scale, -1.f, 1.f);

    // Rescale from [-1;1] to [0;255] and write to the destination
    // XXX: clarify far-inside, far-outside
    const float *src = dt_ref;
    uint8_t *dst = glyph->buf; // + DF_CHAR_PAD * glyph->linesize + DF_CHAR_PAD;
    for (int y = 0; y < glyph->h; y++) {
        for (int x = 0; x < glyph->w; x++) {
            const float scaled_v = (*src++ + 1.f) * .5f; // [-1;1] → [0;1]
            const long int v = lrintf(scaled_v * 255);
            dst[x] = NGLI_MIN(NGLI_MAX(v, 0), 255);
        }
        dst += glyph->linesize;
    }

end:
    ngli_free(v);
    ngli_free(z);
    ngli_free(dts);
#endif

    return ret;
}

static struct glyph *create_glyph(const FT_GlyphSlot slot)
{
    struct glyph *glyph = ngli_calloc(1, sizeof(*glyph));
    if (!glyph)
        return NULL;

    int ret = distmap_create(glyph, slot);
    if (ret < 0) {
        free_glyph(NULL, glyph);
        return NULL;
    }

    return glyph;
}

static int make_glyph_index(struct text *s)
{
    ngli_hmap_freep(&s->glyph_index);
    s->glyph_index = ngli_hmap_create();
    if (!s->glyph_index)
        return NGL_ERROR_MEMORY;
    ngli_hmap_set_free(s->glyph_index, free_glyph, NULL);

    hb_buffer_t **buffers = ngli_darray_data(&s->lines);
    for (int i = 0; i < ngli_darray_count(&s->lines); i++) {
        hb_buffer_t *buffer = buffers[i]; // can't be const because of hb_buffer_get_length()
        const unsigned int nb_glyphs = hb_buffer_get_length(buffer);
        const hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buffer, NULL);
        for (int cp = 0; cp < nb_glyphs; cp++) {
            /*
             * Check if glyph is not already registered in the index. We use a
             * unique string identifier.
             *
             * We can't use hb_font_get_glyph_name() since the result is not
             * unique. With some font, it may return an empty string for all
             * the glyph (see ttf-hanazono 20170904 for an example of this).
             *
             * TODO: make a variant of the hmap with an int to save this int->str
             */
            char glyph_name[32];
            const hb_codepoint_t glyph_id = glyph_info[cp].codepoint;
            snprintf(glyph_name, sizeof(glyph_name), "%u", glyph_id);
            if (ngli_hmap_get(s->glyph_index, glyph_name))
                continue;

            /* Rasterize the glyph with FreeType */
            // TODO: error checking?
            FT_Load_Glyph(s->ft_face, glyph_id, FT_LOAD_DEFAULT);
            FT_GlyphSlot slot = s->ft_face->glyph;
            //FT_Render_Glyph(slot, FT_RENDER_MODE_MONO);
            FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL);

            /* Save the rasterized glyph in the index */
            struct glyph *glyph = create_glyph(slot);
            if (!glyph)
                return NGL_ERROR_MEMORY;
            int ret = ngli_hmap_set(s->glyph_index, glyph_name, glyph);
            if (ret < 0) {
                free_glyph(NULL, glyph);
                return NGL_ERROR_MEMORY;
            }
        }
    }

    return 0;
}

#define ATLAS_CHAR_PAD 1 // prevent interpolation overlap issues with texture picking

static void get_max_glyph_dimensions(const struct hmap *glyph_index, int *w, int *h)
{
    int max_glyph_w = 0, max_glyph_h = 0;
    const struct hmap_entry *entry = NULL;
    while ((entry = ngli_hmap_next(glyph_index, entry))) {
        const struct glyph *glyph = entry->data;
        if (!glyph->buf)
            continue;
        max_glyph_w = NGLI_MAX(max_glyph_w, glyph->w);
        max_glyph_h = NGLI_MAX(max_glyph_h, glyph->h);
    }
    *w = max_glyph_w;
    *h = max_glyph_h;
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static int save_ppm(const char *filename, uint8_t *data, int width, int height)
{
    int ret = 0;
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0600);
    if (fd == -1) {
        fprintf(stderr, "Unable to open '%s'\n", filename);
        return -1;
    }

    uint8_t *buf = malloc(32 + width * height * 3);
    if (!buf) {
        ret = -1;
        goto end;
    }

    const int header_size = snprintf((char *)buf, 32, "P5 %d %d 255\n", width, height);
    if (header_size < 0) {
        ret = -1;
        fprintf(stderr, "Failed to write PPM header\n");
        goto end;
    }

    uint8_t *dst = buf + header_size;
    memcpy(dst, data, width * height);

    const int size = header_size + width * height;
    ret = write(fd, buf, size);
    if (ret < 0) {
        fprintf(stderr, "Failed to write PPM data\n");
        goto end;
    }

end:
    free(buf);
    close(fd);
    return ret;
}

static int atlas_create_libs(struct text *s)
{
    int ret = make_glyph_index(s);
    if (ret < 0)
        return ret;

    /*
     * Allocate a (mostly) squared canvas for the atlas texture.
     * We pick the largest glyph dimensions as reference for a grid cell size.
     */
    int max_glyph_w, max_glyph_h;
    get_max_glyph_dimensions(s->glyph_index, &max_glyph_w, &max_glyph_h);
    const int nb_glyphs = ngli_hmap_count(s->glyph_index);
    const int nb_rows = (int)lrintf(sqrtf(nb_glyphs));
    const int nb_cols = ceilf(nb_glyphs / (float)nb_rows);
    ngli_assert(nb_rows * nb_cols >= nb_glyphs);
    const int glyph_w_padded = max_glyph_w + 2 * ATLAS_CHAR_PAD;
    const int glyph_h_padded = max_glyph_h + 2 * ATLAS_CHAR_PAD;
    struct canvas canvas = {
        .w = glyph_w_padded * nb_cols,
        .h = glyph_h_padded * nb_rows,
    };
    //LOG(ERROR, "%d glyphs -> %dx%d", nb_glyphs, nb_rows, nb_cols);
    canvas.buf = ngli_calloc(canvas.w, canvas.h);
    if (!canvas.buf)
        return NGL_ERROR_MEMORY;

    /*
     * Pack rasterized glyphs from the index into the texture, and reference
     * the atlas coordinates back into the glyph index.
     */
    const float scale_w = 1.f / canvas.w;
    const float scale_h = 1.f / canvas.h;
    int col = 0, row = 0;
    struct hmap_entry *entry = NULL;
    while ((entry = ngli_hmap_next(s->glyph_index, entry))) {
        struct glyph *glyph = entry->data;
        if (!glyph->buf)
            continue;

        const int px = col * glyph_w_padded + ATLAS_CHAR_PAD;
        const int py = row * glyph_h_padded + ATLAS_CHAR_PAD;

        /*
         * Translate its pixel position in the canvas to texture UV
         * coordinates.
         */
        const float gx = px * scale_w;
        const float gy = py * scale_h;
        //LOG(ERROR, "gx=%g gy=%g", gx, gy);
        const float gw = glyph->w * scale_w;
        const float gh = glyph->h * scale_h;
        const float g_uvs[] = {
            gx,      gy + gh,
            gx + gw, gy + gh,
            gx + gw, gy,
            gx,      gy,
        };
        memcpy(glyph->uvcoords, g_uvs, sizeof(glyph->uvcoords));

        /*
         * Insert glyph bitmap into the canvas
         */
        uint8_t *dst = canvas.buf + py * canvas.w + px;
        const uint8_t *src = glyph->buf;
        for (int y = 0; y < glyph->h; y++) {
            memcpy(dst, src, glyph->w);
            src += glyph->linesize;
            dst += canvas.w;
        }

        col++;
        if (col == nb_cols) {
            col = 0;
            row++;
        }
    }

    /*
     * Create texture from the canvas
     */
    struct texture_params tex_params = {
        .type          = NGLI_TEXTURE_TYPE_2D,
        .width         = canvas.w,
        .height        = canvas.h,
        .format        = NGLI_FORMAT_R8_UNORM,
        .min_filter    = NGLI_FILTER_LINEAR,
        .mag_filter    = NGLI_FILTER_LINEAR,
        //.mipmap_filter = NGLI_MIPMAP_FILTER_LINEAR,
    };

    s->owned_atlas = ngli_texture_create(s->ctx->gctx);
    if (!s->owned_atlas) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    ret = ngli_texture_init(s->owned_atlas, &tex_params);
    if (ret < 0)
        goto end;

    ret = ngli_texture_upload(s->owned_atlas, canvas.buf, 0);
    if (ret < 0)
        goto end;

    s->atlas_ref = s->owned_atlas;

end:
    save_ppm("/tmp/atlas.ppm", canvas.buf, canvas.w, canvas.h);
    ngli_free(canvas.buf);
    return ret;
}

static void free_lines(struct darray *lines)
{
    hb_buffer_t **buffers = ngli_darray_data(lines);
    for (int i = 0; i < ngli_darray_count(lines); i++)
        hb_buffer_destroy(buffers[i]);
    ngli_darray_reset(lines);
}

/*
 * Split text into lines, where each line is a harfbuzz buffer
 *
 * TODO: need another layer of segmentation: sub-segments per line to handle
 * bidirectional
 */
static int split_text(struct text *s, const char *str)
{
    free_lines(&s->lines); // make it re-entrant (for live-update of the text)
    ngli_darray_init(&s->lines, sizeof(hb_buffer_t *), 0);

    const size_t len = strlen(str);
    size_t start = 0;

    for (size_t i = 0; i <= len; i++) {
        if (i != len && str[i] != '\n')
            continue;

        hb_buffer_t *buffer = hb_buffer_create();
        if (!hb_buffer_allocation_successful(buffer))
            return NGL_ERROR_MEMORY;

        /*
         * Shape segment
         */
        const size_t segment_len = i - start;
        //LOG(ERROR, "add segment:[%.*s]", (int)segment_len, &str[start]);
        hb_buffer_add_utf8(buffer, &str[start], segment_len, 0, segment_len);

        // TODO: need to probe rtl/ltr script
        if (s->config.wmode == NGLI_TEXT_WRITING_MODE_VERTICAL_LR ||
            s->config.wmode == NGLI_TEXT_WRITING_MODE_VERTICAL_RL) {
            hb_buffer_set_direction(buffer, HB_DIRECTION_TTB);
        } else if (s->config.wmode == NGLI_TEXT_WRITING_MODE_HORIZONTAL_TB) {
            hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
        }

        //XXX: expose top user?
        //hb_buffer_set_script(buffer, script);
        //hb_buffer_set_language(buffer, language);

        hb_buffer_guess_segment_properties(buffer); // this is guessing direction/script/language
        hb_shape(s->hb_font, buffer, NULL, 0);
        start = i + 1;

        if (!ngli_darray_push(&s->lines, &buffer)) {
            hb_buffer_reset(buffer);
            return NGL_ERROR_MEMORY;
        }
    }

    return 0;
}

static int text_set_string_libs(struct text *s, const char *str)
{
    int ret = split_text(s, str);
    if (ret < 0)
        return ret;

    ret = atlas_create_libs(s);
    if (ret < 0)
        return ret;

    int x_min = INT_MAX, y_min = INT_MAX;
    int x_max = INT_MIN, y_max = INT_MIN;

    float x_cur = s->config.padding;
    float y_cur = s->config.padding;

    const int line_advance = s->ft_face->size->metrics.height / 64.;

    hb_buffer_t **buffers = ngli_darray_data(&s->lines);
    for (int i = 0; i < ngli_darray_count(&s->lines); i++) {
        hb_buffer_t *buffer = buffers[i]; // can't be const because of hb_buffer_get_length()

        const unsigned int len = hb_buffer_get_length(buffer);
        const hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buffer, NULL);
        const hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buffer, NULL);

        for (int cp = 0; cp < len; cp++) {
            char glyph_name[32];
            const hb_codepoint_t glyph_id = glyph_info[cp].codepoint;
            snprintf(glyph_name, sizeof(glyph_name), "%u", glyph_id);
            const struct glyph *glyph = ngli_hmap_get(s->glyph_index, glyph_name);
            if (!glyph)
                continue;

            const hb_glyph_position_t *pos = &glyph_pos[cp];
            const float x_adv = pos->x_advance / 64.;
            const float y_adv = pos->y_advance / 64.;
            const float x_off = pos->x_offset / 64.;
            const float y_off = pos->y_offset / 64.;

            // XXX: do we need to floor() or something?
            struct char_info chr = {
                .x = x_cur + glyph->bearing_x + x_off,
                .y = y_cur + glyph->bearing_y + y_off - glyph->h,
                .w = glyph->w,
                .h = glyph->h,
            };
            memcpy(chr.atlas_uvcoords, glyph->uvcoords, sizeof(chr.atlas_uvcoords));

            x_min = NGLI_MIN(x_min, chr.x + DF_CHAR_PAD);
            y_min = NGLI_MIN(y_min, chr.y + DF_CHAR_PAD);
            x_max = NGLI_MAX(x_max, chr.x + chr.w - DF_CHAR_PAD);
            y_max = NGLI_MAX(y_max, chr.y + chr.h - DF_CHAR_PAD);

            //LOG(ERROR, "chr{%s}: [off:%d %d] [adv:%d %d] [bearing:%d %d] "
            //    "x=%d y=%d w=%d h=%d "
            //    "[uv:%g %g %g %g %g %g %g %g]",
            //    glyph_name,
            //    pos->x_offset, pos->y_offset,
            //    pos->x_advance, pos->y_advance,
            //    glyph->bearing_x, glyph->bearing_y,
            //    chr.x, chr.y, chr.w, chr.h,
            //    chr.atlas_uvcoords[0], chr.atlas_uvcoords[1], chr.atlas_uvcoords[2], chr.atlas_uvcoords[3],
            //    chr.atlas_uvcoords[4], chr.atlas_uvcoords[5], chr.atlas_uvcoords[6], chr.atlas_uvcoords[7]);

            // XXX: move all the stuff inside that if?
            if (glyph->w > 0 && glyph->h > 0) {
                if (!ngli_darray_push(&s->chars, &chr))
                    return NGL_ERROR_MEMORY;
            }

            x_cur += x_adv;
            y_cur += y_adv;
        }

        /* Jump to next line or column */
        if (HB_DIRECTION_IS_HORIZONTAL(hb_buffer_get_direction(buffer))) {
            x_cur = s->config.padding;
            y_cur -= line_advance;
        } else {
            y_cur = s->config.padding;
            x_cur -= line_advance; // is this OK?
        }
    }

    //LOG(ERROR, "xmin=%d ymin=%d", x_min, y_min);
    //LOG(ERROR, "xmax=%d ymax=%d", x_max, y_max);

    s->width  = x_max - x_min + s->config.padding;
    s->height = y_max - y_min + s->config.padding;
    //LOG(ERROR, "text size: %dx%d", s->width, s->height);

    struct char_info *chars = ngli_darray_data(&s->chars);
    const int nb_chars = ngli_darray_count(&s->chars);
    for (int i = 0; i < nb_chars; i++) {
        struct char_info *chr = &chars[i];
        chr->x -= x_min;
        chr->y -= y_min;
    }

    return 0;
}
#endif

static void get_char_box_dim(const char *s, int *wp, int *hp, int *np)
{
    int w = 0, h = 1;
    int cur_w = 0;
    int n = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '\n') {
            cur_w = 0;
            h++;
        } else {
            cur_w++;
            w = NGLI_MAX(w, cur_w);
            n++;
        }
    }
    *wp = w;
    *hp = h;
    *np = n;
}

static int atlas_create(struct text *s)
{
    struct ngl_ctx *ctx = s->ctx;
    struct gctx *gctx = ctx->gctx;

    if (ctx->font_atlas)
        return 0;

    struct canvas canvas = {0};
    int ret = ngli_drawutils_get_font_atlas(&canvas);
    if (ret < 0)
        goto end;

    struct texture_params tex_params = {
        .width         = canvas.w,
        .height        = canvas.h,
        .format        = NGLI_FORMAT_R8_UNORM,
        .min_filter    = NGLI_FILTER_LINEAR,
        .mag_filter    = NGLI_FILTER_NEAREST,
        .mipmap_filter = NGLI_MIPMAP_FILTER_LINEAR,
    };

    ctx->font_atlas = ngli_texture_create(gctx); // freed at context reconfiguration/destruction
    if (!ctx->font_atlas) {
        ret = NGL_ERROR_MEMORY;
        goto end;
    }

    ret = ngli_texture_init(ctx->font_atlas, &tex_params);
    if (ret < 0)
        goto end;

    ret = ngli_texture_upload(ctx->font_atlas, canvas.buf, 0);
    if (ret < 0)
        goto end;

end:
    ngli_free(canvas.buf);
    return ret;
}

static int text_init_builtin(struct text *s)
{
    if (s->config.wmode != NGLI_TEXT_WRITING_MODE_UNDEFINED) {
        LOG(ERROR, "writing mode is not supported without a font");
        return NGL_ERROR_UNSUPPORTED;
    }

    int ret = atlas_create(s);
    if (ret < 0)
        return ret;

    s->atlas_ref = s->ctx->font_atlas;

    return 0;
}

int ngli_text_init(struct text *s, struct ngl_ctx *ctx, const struct text_config *cfg)
{
    ngli_assert(!s->ctx);
    s->ctx = ctx;
    s->config = *cfg;
    ngli_darray_init(&s->chars, sizeof(struct char_info), 0);
    return cfg->fontfile ? text_init_from_libs(s) : text_init_builtin(s);
}

int ngli_text_set_string(struct text *s, const char *str)
{
    ngli_darray_clear(&s->chars);

    if (s->config.fontfile)
        return text_set_string_libs(s, str);

    int text_cols, text_rows, text_nbchr;
    get_char_box_dim(str, &text_cols, &text_rows, &text_nbchr);

    s->width  = text_cols * NGLI_FONT_W + 2 * s->config.padding;
    s->height = text_rows * NGLI_FONT_H + 2 * s->config.padding;

    int px = 0, py = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] == '\n') {
            py++;
            px = 0;
            continue;
        }

        struct char_info chr = {
            .x = s->config.padding + NGLI_FONT_W * px,
            .y = s->config.padding + NGLI_FONT_H * (text_rows - py - 1),
            .w = NGLI_FONT_W,
            .h = NGLI_FONT_H,
        };
        ngli_drawutils_get_atlas_uvcoords(str[i], chr.atlas_uvcoords);
        if (!ngli_darray_push(&s->chars, &chr))
            return NGL_ERROR_MEMORY;
        px++;
    }
    return 0;
}

void ngli_text_reset(struct text *s)
{
    if (!s->ctx)
        return;
#ifdef HAVE_TEXTLIBS
    free_lines(&s->lines);
    hb_font_destroy(s->hb_font);
    FT_Done_Face(s->ft_face);
    FT_Done_FreeType(s->ft_library);
    ngli_hmap_freep(&s->glyph_index);
    ngli_texture_freep(&s->owned_atlas);
#endif
    ngli_darray_reset(&s->chars);
    memset(s, 0, sizeof(*s));
}
