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
#include <string.h>

#include "darray.h"
#include "log.h"
#include "math_utils.h"
#include "memory.h"
#include "nodegl.h"
#include "nodes.h"

enum {
    PATH_MODE_BEZIER3,
    PATH_MODE_CATMULL,
};

static const struct param_choices mode_choices = {
    .name = "path_mode",
    .consts = {
        {"bezier3", PATH_MODE_BEZIER3, .desc=NGLI_DOCSTRING("cubic bezier curve")},
        {"catmull", PATH_MODE_CATMULL, .desc=NGLI_DOCSTRING("catmull-rom curve")},
        {NULL}
    }
};

#define OFFSET(x) offsetof(struct path_priv, x)
static const struct node_param path_params[] = {
    {"points",    PARAM_TYPE_NODE, OFFSET(points_buffer),
                  .node_types=(const int[]){NGL_NODE_BUFFERVEC3, NGL_NODE_ANIMATEDBUFFERVEC3, -1},
                  .flags=PARAM_FLAG_NON_NULL | PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                  .desc=NGLI_DOCSTRING("anchor points the path go through")},
    {"controls",  PARAM_TYPE_NODE, OFFSET(controls_buffer),
                  .node_types=(const int[]){NGL_NODE_BUFFERVEC3, NGL_NODE_ANIMATEDBUFFERVEC3, -1},
                  .flags=PARAM_FLAG_NON_NULL | PARAM_FLAG_DOT_DISPLAY_FIELDNAME,
                  .desc=NGLI_DOCSTRING("control points (must be twice the number of `points` minus 2 with `mode`=`bezier3`, and 2 with `mode`=`catmull`)")},
    {"mode",      PARAM_TYPE_SELECT, OFFSET(mode), {.i64=PATH_MODE_BEZIER3},
                  .choices=&mode_choices,
                  .desc=NGLI_DOCSTRING("interpolation mode between points")},
    {"precision", PARAM_TYPE_INT, OFFSET(precision), {.i64=64},
                  .desc=NGLI_DOCSTRING("number of division per curve segment")},
    {"tension",   PARAM_TYPE_DBL, OFFSET(tension), {.dbl=0.5},
                  .desc=NGLI_DOCSTRING("tension between points (catmull-rom only)")},
    {NULL}
};

/*
 * Interpolate a 3D point using a cubic bezier
 * XXX: we should get rid of this, we have poly_bezier3_vec3() now
 */
static void interpolate_bezier3_vec3(float t, float *dst, const float *p0, const float *p1, const float *p2, const float *p3)
{
    const float u  = 1.f - t;
    const float f0 =       u * u * u;
    const float f1 = 3.f * u * u * t;
    const float f2 = 3.f * u * t * t;
    const float f3 =       t * t * t;
    dst[0] = f0*p0[0] + f1*p1[0] + f2*p2[0] + f3*p3[0];
    dst[1] = f0*p0[1] + f1*p1[1] + f2*p2[1] + f3*p3[1];
    dst[2] = f0*p0[2] + f1*p1[2] + f2*p2[2] + f3*p3[2];
}

/*
 * Interpolate a 3D point using the polynomes of a cubic bezier
 */
static void poly_bezier3_vec3(float t, float *dst, const float *x, const float *y, const float *z)
{
    dst[0] = ((x[0] * t + x[1]) * t + x[2]) * t + x[3];
    dst[1] = ((y[0] * t + y[1]) * t + y[2]) * t + y[3];
    dst[2] = ((z[0] * t + z[1]) * t + z[2]) * t + z[3];
}

/*
 * Build a lookup table of data points that will be used typically for
 * estimating the arc lengths of the curve.
 */
static void update_lut(float *dst, const float *p, const float *c, int nb_segments, int precision)
{
    /*
     * We're not using precision-1 for the scale because we are not computing
     * the distance for the destination point of each segment (except for the
     * last one); it is calculated as the starting point of each new segment.
     */
    const float time_scale = 1.f / ((float)precision);

    for (int i = 0; i < nb_segments; i++) { // XXX useless condition
        const float *p0 = p;
        const float *p1 = c;
        const float *p2 = c + 3;
        const float *p3 = p + 3;
        for (int k = 0; k < precision; k++) {
            const float t = k * time_scale;
            //LOG(ERROR, "i=%d k=%d t=%f", i, k, t);
            interpolate_bezier3_vec3(t, dst, p0, p1, p2, p3);
            dst += 3;
        }
        if (i == nb_segments - 1) {
            interpolate_bezier3_vec3(1., dst, p0, p1, p2, p3);
            break;
        }
        p += 3;
        c += 2 * 3;
    }
}

static void update_arc_distances(struct path_priv *s)
{
    float total_length = 0.;
    const float *lut = s->lut;
    float *dst = s->arc_distances;

    *dst++ = 0.f;
    for (int i = 0; i < s->lut_count - 1; i++) {
        const float vec[3] = {
            lut[5] - lut[2],
            lut[4] - lut[1],
            lut[3] - lut[0],
        };
        const float arc_length = ngli_vec3_length(vec);
        total_length += arc_length;
        *dst++ = total_length;
        lut += 3;
    }

    const float scale = total_length ? 1.f / total_length : 0.f;
    for (int i = 0; i < s->arc_distances_normalized_count; i++) {
        s->arc_distances_normalized[i] = s->arc_distances[i] * scale;
        //LOG(ERROR, "dist seg #%d: %f -> %f (t → d)",
        //    i, i / ((float)s->arc_distances_normalized_count - 1.f), s->arc_distances_normalized[i]);
    }
}

static int get_arc_from_dist(const float *distances, float distance, int arc_nb, int start)
{
    int ret = -1;

    for (int i = start; i < arc_nb; i++) {
        if (distances[i] > distance)
            break;
        ret = i;
    }
    return ret;
}

/*
 * Remap time according to the bezier curve distance.
 *
 * We want the time parameter to be correlated to the distance on the bézier
 * curves.  Unfortunately, there is no magic formula to get the length of a
 * bézier curve, so we rely on a simple approximation by splitting the curves
 * into many points.
 *
 * https://pomax.github.io/bezierinfo/#arclength
 * https://pomax.github.io/bezierinfo/#arclengthapprox
 * https://pomax.github.io/bezierinfo/#tracing
 */
static float distance_to_time(struct path_priv *s, float distance)
{
    const float *distances = s->arc_distances_normalized;
    const int arc_nb = s->arc_distances_normalized_count - 1; // XXX need to make sure about this

    int arc_id = get_arc_from_dist(distances, distance, arc_nb, s->current_pos);
    if (arc_id < 0)
        arc_id = get_arc_from_dist(distances, distance, arc_nb, 0);
    arc_id = NGLI_MIN(NGLI_MAX(arc_id, 0), arc_nb - 1);

    const float d0 = distances[arc_id];
    const float d1 = distances[arc_id + 1];
    const float ratio = NGLI_LINEAR_INTERP(d0, d1, distance);

    const float t0 = arc_id / ((float)arc_nb - 1.f);
    const float t1 = (arc_id + 1) / ((float)arc_nb - 1.f);
    const float t = NGLI_MIX(t0, t1, ratio);

    //LOG(ERROR, "d=%f -> ratio=%f [%f,%f] -> %f [%f,%f]", distance, ratio, d0, d1, t, t0, t1);

    s->current_pos = arc_id;
    return t;
}

static int get_knot_id(const struct path_knot *knots, int nb_segments, int start, float t)
{
    int ret = -1;

    for (int i = start; i < nb_segments; i++) {
        if (knots[i].start_time > t)
            break;
        ret = i;
    }
    return ret;
}

// XXX: make it void
int ngli_path_evaluate(struct path_priv *s, float *dst, float distance)
{
    const float t = distance_to_time(s, distance);

    const int nb_segments = s->nb_segments;

    //LOG(ERROR, "orig t=%f", t);

    int knot_id = get_knot_id(s->knots, nb_segments, s->current_knot, t);
    if (knot_id < 0)
        knot_id = get_knot_id(s->knots, nb_segments, 0, t);
    knot_id = NGLI_MIN(NGLI_MAX(knot_id, 0), nb_segments - 1);

    const struct path_knot *kn0 = &s->knots[knot_id];
    const struct path_knot *kn1 = &s->knots[knot_id + 1];

    const float seg_t = NGLI_LINEAR_INTERP(kn0->start_time, kn1->start_time, t);

    //LOG(ERROR, "t=%f between knot %d (%f) and %d (%f) at %f",
    //    t, knot_id, kn0->start_time, knot_id + 1, kn1->start_time, ti);

    //const float *p0 = kn0->start_point;
    //const float *p1 = kn0->start_control;
    //const float *p2 = kn0->end_control;
    //const float *p3 = kn1->start_point;
    const float *px = kn0->poly_x;
    const float *py = kn0->poly_y;
    const float *pz = kn0->poly_z;

    //LOG(ERROR, "p0:"NGLI_FMT_VEC3, NGLI_ARG_VEC3(p0));
    //LOG(ERROR, "p1:"NGLI_FMT_VEC3, NGLI_ARG_VEC3(p1));
    //LOG(ERROR, "p2:"NGLI_FMT_VEC3, NGLI_ARG_VEC3(p2));
    //LOG(ERROR, "p3:"NGLI_FMT_VEC3, NGLI_ARG_VEC3(p3));

    poly_bezier3_vec3(seg_t, dst, px, py, pz);
    s->current_knot = knot_id;
    return 0;
}

/*
 * Convert from:
 *   B(t) = (1-t)³ p0 + 3(1-t)²t p1 + 3(1-t)t² p2 + t³ p3
 * To polynomial form:
 *   B(t) = at³ + bt² + ct + d
 *
 * XXX: should we use an (optimized) matrix mult instead?
 * (might be relevant when updating the bezier per frame)
 */
static void get_poly_from_bezier(float *dst, float p0, float p1, float p2, float p3)
{
    const float a =     -p0 + 3.f*p1 - 3.f*p2 + p3;
    const float b =  3.f*p0 - 6.f*p1 + 3.f*p2;
    const float c = -3.f*p0 + 3.f*p1;
    const float d =      p0;
    dst[0] = a;
    dst[1] = b;
    dst[2] = c;
    dst[3] = d;
}

static int init_knots(struct path_priv *s, const float *points, const float *controls, int n)
{
    s->knots = ngli_calloc(n, sizeof(*s->knots));
    if (!s->knots)
        return NGL_ERROR_MEMORY;

    for (int i = 0; i < n; i++) {
        struct path_knot *knot = &s->knots[i];

        const float distance = s->arc_distances_normalized[i * s->precision];
        knot->start_time  = distance_to_time(s, distance);
        knot->start_point = &points[i * 3];
        //LOG(ERROR, "knot[%d]: start_time:%f (distance:%f)", i, knot->start_time, distance);
        //LOG(ERROR, "    start_point:  "NGLI_FMT_VEC3, NGLI_ARG_VEC3(knot->start_point));
        if (i != n - 1) {
            knot->start_control = &controls[i * 2 * 3];
            knot->end_control   = &controls[(i * 2 + 1) * 3];
            //LOG(ERROR, "    start_control:"NGLI_FMT_VEC3, NGLI_ARG_VEC3(knot->start_control));
            //LOG(ERROR, "    end_control:  "NGLI_FMT_VEC3, NGLI_ARG_VEC3(knot->end_control));
        //} else {
        //    LOG(ERROR, "    start_control: ø");
        //    LOG(ERROR, "    end_control: ø");
        }
    }

    for (int i = 0; i < n - 1; i++) {
        struct path_knot *kn0 = &s->knots[i];
        const struct path_knot *kn1 = &s->knots[i + 1];

        const float *p0 = kn0->start_point;
        const float *p1 = kn0->start_control;
        const float *p2 = kn0->end_control;
        const float *p3 = kn1->start_point;

        get_poly_from_bezier(kn0->poly_x, p0[0], p1[0], p2[0], p3[0]);
        get_poly_from_bezier(kn0->poly_y, p0[1], p1[1], p2[1], p3[1]);
        get_poly_from_bezier(kn0->poly_z, p0[2], p1[2], p2[2], p3[2]);
    }

    return 0;
}

static int init_catmull_data(struct path_priv *s)
{
    const struct buffer_priv *points   = s->points_buffer->priv_data;
    const struct buffer_priv *controls = s->controls_buffer->priv_data;

    const float *points_data   = (const float *)points->data;
    const float *controls_data = (const float *)controls->data;

    const int control_pairs_count = points->count - 1;
    s->catmull_controls = ngli_calloc(control_pairs_count * 2, 3 * sizeof(*s->catmull_controls));
    if (!s->catmull_controls)
        return NGL_ERROR_MEMORY;

    /*
     * With catmull interpolation, the first and last control points are user
     * defined, all the others are calculated.
     *
     * See https://pomax.github.io/bezierinfo/#catmullconv
     */

    const float *user_pfirst = &controls_data[0];
    const float *user_plast  = &controls_data[3];
    const float scale = 1.f / (s->tension * 6.f);
    float *cpoints = s->catmull_controls;

    for (int i = 0; i < control_pairs_count; i++) {
        const float *p0 = i == 0 ? user_pfirst : &points_data[(i - 1) * 3];
        const float *p1 = &points_data[i * 3];
        const float *p2 = &points_data[(i + 1) * 3];
        const float *p3 = i == control_pairs_count - 1 ? user_plast : &points_data[(i + 2) * 3];
        const float cpoint[3 * 2] = {
            /* 1st control of the point */
            p1[0] + (p2[0] - p0[0]) * scale,
            p1[1] + (p2[1] - p0[1]) * scale,
            p1[2] + (p2[2] - p0[2]) * scale,

            /* 2nd control of the point */
            p2[0] - (p3[0] - p1[0]) * scale,
            p2[1] - (p3[1] - p1[1]) * scale,
            p2[2] - (p3[2] - p1[2]) * scale,
        };
        memcpy(cpoints, cpoint, sizeof(cpoint));
        cpoints += 3 * 2;
    }

    return 0;
}

static int path_init(struct ngl_node *node)
{
    struct path_priv *s = node->priv_data;

    const struct buffer_priv *points   = s->points_buffer->priv_data;
    const struct buffer_priv *controls = s->controls_buffer->priv_data;

    if (!s->tension) {
        LOG(ERROR, "tension can not be 0");
        return NGL_ERROR_INVALID_ARG;
    }

    if (!s->precision) {
        LOG(ERROR, "precision can not be 0");
        return NGL_ERROR_INVALID_ARG;
    }

    if (points->count < 2) {
        LOG(ERROR, "at least 2 points must be defined");
        return NGL_ERROR_INVALID_ARG;
    }

    if (s->mode == PATH_MODE_BEZIER3 && controls->count != (points->count - 1) * 2) {
        LOG(ERROR, "cubic bezier curves need the number of control points to be twice the number of anchor points minus 2");
        return NGL_ERROR_INVALID_ARG;
    } else if (s->mode == PATH_MODE_CATMULL && controls->count != 2) {
        LOG(ERROR, "catmull rom need 2 control points (first and last)");
        return NGL_ERROR_INVALID_ARG;
    }

    const float *points_data   = (const float *)points->data;
    const float *controls_data = (const float *)controls->data;

    if (s->mode == PATH_MODE_CATMULL) {
        int ret = init_catmull_data(s);
        if (ret < 0)
            return ret;
        controls_data = s->catmull_controls;
    }

    s->nb_segments = points->count - 1;

    s->lut_count = s->nb_segments * s->precision + 1;
    s->lut = ngli_calloc(s->lut_count, 3 * sizeof(*s->lut));
    if (!s->lut)
        return NGL_ERROR_MEMORY;

    s->arc_distances_count = s->nb_segments * s->precision + 1;
    s->arc_distances = ngli_calloc(s->arc_distances_count, sizeof(*s->arc_distances));
    if (!s->arc_distances)
        return NGL_ERROR_MEMORY;

    s->arc_distances_normalized_count = s->nb_segments * s->precision + 1;
    s->arc_distances_normalized = ngli_calloc(s->arc_distances_normalized_count, sizeof(*s->arc_distances_normalized));
    if (!s->arc_distances_normalized)
        return NGL_ERROR_MEMORY;

    update_lut(s->lut, points_data, controls_data, s->nb_segments, s->precision);
    update_arc_distances(s);

    int ret = init_knots(s, points_data, controls_data, points->count);
    if (ret < 0)
        return ret;

    //LOG(ERROR, "%d segments for %d points", s->nb_segments, points->count);

    return 0;
}

static void path_uninit(struct ngl_node *node)
{
    struct path_priv *s = node->priv_data;

    ngli_freep(&s->knots);
    ngli_freep(&s->catmull_controls);
    ngli_freep(&s->lut);
    ngli_freep(&s->arc_distances);
    ngli_freep(&s->arc_distances_normalized);
}

const struct node_class ngli_path_class = {
    .id        = NGL_NODE_PATH,
    .name      = "Path",
    .init      = path_init,
    .uninit    = path_uninit,
    .priv_size = sizeof(struct path_priv),
    .params    = path_params,
    .file      = __FILE__,
};
