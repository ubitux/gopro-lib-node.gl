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

#include "log.h" // XXX
#include "math_utils.h"
#include "nodegl.h"
#include "nodes.h"
#include "type.h"

enum {
    NOISE_CUBIC,
    NOISE_QUINTIC,
    NB_NOISE
};

typedef float (*interp_func_type)(float t);

struct noise {
    struct variable_priv var;

    int octaves;
    double lacunarity;
    double gain;
    uint32_t seed;
    int function;

    interp_func_type interp_func;
};

const struct param_choices noise_func_choices = {
    .name = "interp_noise",
    .consts = {
        {"cubic",   NOISE_CUBIC,   .desc=NGLI_DOCSTRING("cubic hermite curve, f(t)=3t²-2t³")},
        {"quintic", NOISE_QUINTIC, .desc=NGLI_DOCSTRING("quintic curve, f(t)=6t⁵-15t⁴+10x³")},
        {NULL}
    }
};

#define OFFSET(x) offsetof(struct noise, x)
static const struct node_param noise_params[] = {
    {"octaves",    PARAM_TYPE_INT, OFFSET(octaves), {.i64=3},
                   .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
                   .desc=NGLI_DOCSTRING("iterations of noise")},
    {"lacunarity", PARAM_TYPE_DBL, OFFSET(lacunarity), {.dbl=2.0},
                   .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
                   .desc=NGLI_DOCSTRING("frequency multiplier per octave")},
    {"gain",       PARAM_TYPE_DBL, OFFSET(gain), {.dbl=0.5},
                   .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
                   .desc=NGLI_DOCSTRING("amplitude multiplier per octave")},
    {"seed",       PARAM_TYPE_INT, OFFSET(seed), {.i64=0x50726e67 /* "Prng" */},
                   .desc=NGLI_DOCSTRING("random seed")},
    {"function",   PARAM_TYPE_SELECT, OFFSET(function), {.i64=NOISE_QUINTIC},
                   .choices=&noise_func_choices,
                   .desc=NGLI_DOCSTRING("interpolation function to use between noise point")},
    {NULL}
};

NGLI_STATIC_ASSERT(variable_priv_is_first, OFFSET(var) == 0);

/*
 * xorshift64s PRNG, could be replaced with something else
 */
static uint64_t hash(uint64_t x)
{
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    return x * UINT64_C(0x2545F4914F6CDD1D);
}

/*
 * Return a random double-precision float between [0;1)
 * Taken from http://prng.di.unimi.it/
 */
static double rand_u64_to_f64(uint64_t x)
{
    const union { uint64_t i; double d; } u = {.i = UINT64_C(0x3FF) << 52 | x >> 12};
    return u.d - 1.0;
}

#if 0
static float minn = 999999.0;
static float maxn = -999999.0;
#endif

static float noise(const struct noise *s, float v)
{
    const float i = floorf(v);
    const float f = v - i;
    const uint64_t x = (uint64_t)i + s->seed;

    /* random slopes found at boundaries; they are between [0;1) so we rescale
     * them to [-1;1) */
    const float s0 = rand_u64_to_f64(hash(x))     * 2. - 1.;
    const float s1 = rand_u64_to_f64(hash(x + 1)) * 2. - 1.;

    /* apply slope on each side */
    const float v0 = f * s0;
    const float v1 = (1. - f) * s1;

    const float t = s->interp_func(f);
    const float r = NGLI_MIX(v0, v1, t);

    const float n = r * 2.; // [-.5;.5) → [-1;1)

#if 0
    minn = NGLI_MIN(n, minn);
    maxn = NGLI_MAX(n, maxn);
    LOG(ERROR, "n=%f [%f,%f]", n, minn, maxn);
#endif
    return n;
}

static int noise_update(struct ngl_node *node, double t)
{
    struct noise *s = node->priv_data;
    float sum = 0.0;
    float max_amp = 0.0;
    float freq = 1.0;
    float amp = 1.0;
    for (int i = 0; i < s->octaves; i++) {
        sum += noise(s, t * freq) * amp;
        max_amp += amp;
        freq *= s->lacunarity;
        amp *= s->gain;
    }
    s->var.scalar = sum / max_amp;
    return 0;
}

static float curve_cubic(float t)
{
    return ((6.0*t - 15.0)*t + 10.0)*t*t*t;
}

static float curve_quintic(float t)
{
    return (3.0 - 2.0*t)*t*t;
}

static const interp_func_type interp_func_map[NB_NOISE] = {
    [NOISE_CUBIC]   = curve_cubic,
    [NOISE_QUINTIC] = curve_quintic,
};

static int noise_init(struct ngl_node *node)
{
    struct noise *s = node->priv_data;
    s->var.data = &s->var.scalar;
    s->var.data_size = sizeof(float);
    s->var.data_type = NGLI_TYPE_FLOAT;
    s->interp_func = interp_func_map[s->function];
    return 0;
}

const struct node_class ngli_noise_class = {
    .id        = NGL_NODE_NOISE,
    .category  = NGLI_NODE_CATEGORY_UNIFORM,
    .name      = "Noise",
    .init      = noise_init,
    .update    = noise_update,
    .priv_size = sizeof(struct noise),
    .params    = noise_params,
    .file      = __FILE__,
};
