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

#include <complex.h>
#include <math.h>

#include "utils.h"
#include "math_utils.h"
#include "root_finder.h"

#define EPS 1e-13
#define CHECK_ZERO(x) (fabs(x) < EPS)

/* Linear: f(x)=ax+b */
static int root_find1(float *roots, float a, float b)
{
    if (!a)
        return 0;
    roots[0] = -b / a;
    return 1;
}

/* Quadratic monic: f(x)=x²+ax+b */
static int root_find2_monic(float *roots, float a, float b)
{
    /* depressed: t²+p with x=t-a/2 */
    const float offset = -a / 2.f;
    const float p = -a*a/4.f + b;
    const float delta = -4.f * p;

    if (CHECK_ZERO(delta)) {
        roots[0] = offset;
        return 1;
    }

    if (delta < 0.f)
        return 0;

    const float z = sqrtf(delta) / 2.f;
    roots[0] = offset - z;
    roots[1] = offset + z;
    return 2;
}

/* Quadratic: f(x)=ax²+bx+c */
static int root_find2(float *roots, float a, float b, float c)
{
    return a ? root_find2_monic(roots, b / a, c / a)
             : root_find1(roots, b, c);
}

/* Cubic monic: f(x)=x³+ax²+bx+c */
static int root_find3_monic(float *roots, float a, float b, float c)
{
    /* depressed: t³+pt+q with x=t-a/3 */
    const float offset = -a / 3.f;
    const float p = b - a*a / 3.f;
    const float q = a*a*a*2.f/27.f - a*b/3.f + c;
    const float q2 = q / 2.f;
    const float p3 = p / 3.f;
    const float delta = q2*q2 + p3*p3*p3; // simplified discriminant

    if (CHECK_ZERO(p) && CHECK_ZERO(q)) {
        roots[0] = offset;
        return 1;
    }

    if (CHECK_ZERO(delta)) {
        const float u = cbrtf(q2);
        roots[0] = offset + 2.f * u;
        roots[1] = offset - u;
        return 2;
    }

    if (delta > 0.f) {
        const float z = sqrtf(delta);
        const float u = cbrtf(-q2 + z);
        const float v = cbrtf(-q2 - z);
        roots[0] = u + v + offset;
        return 1;
    }

    /* see https://en.wikipedia.org/wiki/Cubic_equation#Trigonometric_and_hyperbolic_solutions */
    const float u = 2.f * sqrtf(-p3);
    const float v = acosf(3.f*q / (2.f*p) * sqrtf(-1.f / p3)) / 3.f;
    roots[0] = offset + u * cosf(v);
    roots[1] = offset + u * cosf(v + 2.f*M_PI/3.f);
    roots[2] = offset + u * cosf(v + 4.f*M_PI/3.f);
    return 3;
}

/* Cubic: f(x)=ax³+bx²+cx+d */
static int root_find3(float *roots, float a, float b, float c, float d)
{
    return a ? root_find3_monic(roots, b / a, c / a, d / a)
             : root_find2(roots, b, c, d);
}

/* Quartic monic: f(x)=x⁴+ax³+bx²+c */
static int root_find4_monic(float *roots, float a, float b, float c, float d)
{
    /* depressed: t⁴+pt²+qt+r with x=t-a/4 */
    const float offset = -a / 4.f;
    const float p = -3.f*a*a/8.f + b;
    const float q = a*a*a/8.f - a*b/2.f + c;
    const float r = -3.f*a*a*a*a/256.f + a*a*b/16.f - a*c/4.f + d;

    int nroot;

    if (CHECK_ZERO(r)) {
        roots[0] = 0.f;
        nroot = 1 + root_find3_monic(roots + 1, 0.f, p, q);
    } else {
        nroot = root_find3_monic(roots, -p/2.f, -r, p*r/2.f - q*q/8.f);

        /* a cubic monic will always cross the x axis at some point, so there
         * is always at least one root */
        const float z = roots[0];
        const float s = z*z - r;
        const float t = 2.f*z - p;

        /* s and t are the same sign (because st=q²/4), so technically only one
         * if is necessary; both are kept for consistency. */
        if (s < 0.f || t < 0.f)
            return 0;

        const float u = sqrtf(s);
        const float v = sqrtf(t);
        const float sv = q < 0.f ? -v : v;
        nroot  = root_find2_monic(roots,          sv, z - u);
        nroot += root_find2_monic(roots + nroot, -sv, z + u);
    }

    roots[0] += offset;
    roots[1] += offset;
    roots[2] += offset;
    roots[3] += offset;
    return nroot;
}

/* Quartic: f(x)=ax⁴+bx³+cx²+d */
static int root_find4(float *roots, float a, float b, float c, float d, float e)
{
    return a ? root_find4_monic(roots, b / a, c / a, d / a, e / a)
             : root_find3(roots, b, c, d, e);
}

/*
 * Generated with:
 *     import math
 *     n = 5
 *     for k in range(n):
 *         angle = 2*math.pi/n
 *         offset = math.pi/(2*n)
 *         z = angle * k + offset
 *         c, s = math.cos(z), math.sin(z)
 *         print(f'#define C{k} {c:.15f}')
 *         print(f'#define S{k} {s:.15f}')
 */

#define C5_0  0.951056516295154
#define S5_0  0.309016994374947
#define C5_1  0.000000000000000
#define S5_1  1.000000000000000
#define C5_2 -0.951056516295154
#define S5_2  0.309016994374948
#define C5_3 -0.587785252292473
#define S5_3 -0.809016994374947
#define C5_4  0.587785252292473
#define S5_4 -0.809016994374948

#define MAX_ITERATION 16

static inline float complex poly1(float a, float b, float complex x)
{
    return a * x + b;
}

static inline float complex poly2(float a, float b, float c, float complex x)
{
    return poly1(a, b, x) * x + c;
}

static inline float complex poly3(float a, float b, float c, float d, float complex x)
{
    return poly2(a, b, c, x) * x + d;
}

static inline float complex poly4(float a, float b, float c, float d, float e, float complex x)
{
    return poly3(a, b, c, d, x) * x + e;
}

static inline float complex poly5(float a, float b, float c, float d, float e, float f, float complex x)
{
    return poly4(a, b, c, d, e, x) * x + f;
}

static float get_err_sq(const float complex *a, const float complex *b)
{
    float err = 0.f;
    for (int i = 0; i < 5; i++) {
        const float d = cabsf(a[i] - b[i]);
        err += d * d;
    }
    return err;
}

/* https://en.wikipedia.org/wiki/Aberth_method */
static int alberth_ehrlich_p5(float *roots, float a, float b, float c, float d, float e, float f)
{
    const float r = powf(fabsf(f / a), .2f);

    float complex cur_buffer[5];
    float complex prv_buffer[5] = {
        r * C5_0 + r * S5_0 * I,
        r * C5_1 + r * S5_1 * I,
        r * C5_2 + r * S5_2 * I,
        r * C5_3 + r * S5_3 * I,
        r * C5_4 + r * S5_4 * I,
    };
    float complex *cur = cur_buffer;
    float complex *prv = prv_buffer;

    for (int m = 1; m <= MAX_ITERATION; m++) {
        const float complex sum[5] = {
            1.f/(prv[0]-prv[1]) + 1.f/(prv[0]-prv[2]) + 1.f/(prv[0]-prv[3]) + 1.f/(prv[0]-prv[4]),
            1.f/(prv[1]-prv[0]) + 1.f/(prv[1]-prv[2]) + 1.f/(prv[1]-prv[3]) + 1.f/(prv[1]-prv[4]),
            1.f/(prv[2]-prv[0]) + 1.f/(prv[2]-prv[1]) + 1.f/(prv[2]-prv[3]) + 1.f/(prv[2]-prv[4]),
            1.f/(prv[3]-prv[0]) + 1.f/(prv[3]-prv[1]) + 1.f/(prv[3]-prv[2]) + 1.f/(prv[3]-prv[4]),
            1.f/(prv[4]-prv[0]) + 1.f/(prv[4]-prv[1]) + 1.f/(prv[4]-prv[2]) + 1.f/(prv[4]-prv[3]),
        };

        for (int i = 0; i < 5; i++) {
            const float complex p5 = poly5(a, b, c, d, e, f, prv[i]);
            const float complex p4 = poly4(5.f * a, 4.f * b, 3.f * c, 2.f * d, e, prv[i]);
            const float complex pod = p5 / p4;
            cur[i] = prv[i] - pod / (1.f - pod * sum[i]);
        }

        NGLI_SWAP(float complex *, cur, prv);

        const float err = get_err_sq(cur, prv);
        if (err < EPS*EPS)
            break;
    }

    int nroot = 0;
    for (int i = 0; i < 5; i++)
        if (fabs(cimag(prv[i])) <= EPS)
            roots[nroot++] = creal(prv[i]);

    return nroot;
}

/* Quintic monic: f(x)=x⁵+ax⁴+bx³+cx²+dx+e */
static int root_find5_monic(float *roots, float a, float b, float c, float d, float e)
{
    return alberth_ehrlich_p5(roots, 1.f, a, b, c, d, e);
}

/* Quintic: f(x)=ax⁵+bx⁴+cx³+dx²+ex+f */
int ngli_root_find5(float *roots, float a, float b, float c, float d, float e, float f)
{
    return a ? root_find5_monic(roots, b / a, c / a, d / a, e / a, f / a)
             : root_find4(roots, b, c, d, e, f);
}
