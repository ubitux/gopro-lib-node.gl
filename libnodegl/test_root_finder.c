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

#include <math.h>

#include "root_finder.h"
#include "utils.h"

static const struct polytest {
    const char *label;
    float coeffs[6];
    float roots[5];
    int nb_roots;
} polytests[] = {
    // XXX: need more tests for specific corner cases
    {
        .label    = "linear, no coefficient, no offset",
        .coeffs   = {0.f},
        .nb_roots = 0,
    }, {
        .label    =  "linear, no coefficient",
        .coeffs   = {3.f},
        .nb_roots = 0,
    }, {
        .label    = "linear: 3x+2",
        .coeffs   = {3.f, 2.f},
        .nb_roots = 1,
        .roots    = {-1.5},
    }, {
        .label    = "quadratic negative discriminant",
        .coeffs   = {3.f, 1.f, 2.f},
        .nb_roots = 0,
    }, {
        .label    = "quadratic 2 roots",
        .coeffs   = {-2.f, 3.f, 5.f},
        .nb_roots = 2,
        .roots    = {-1.f, 2/5.f},
    }, {
        .label    = "quadratic: (x-1)^2",
        .coeffs   = {1.f, -2.f, 1.f},
        .nb_roots = 1,
        .roots    = {1.f},
    }, {
        .label    = "cubic 0: (x-1)^3",
        .coeffs   = {-1.f, 3.f, -3.f, 1.f},
        .nb_roots = 1,
        .roots    = {1.f},
    }, {
        .label    = "cubic 1: (x-1)(x-2)^2",
        .coeffs   = {-4.f, 8.f, -5.f, 1.f},
        .nb_roots = 3, // XXX, actually 2
        .roots    = {1.f, 2.f, 2.f},
    }, {
        .label    = "cubic 2: (x-1)(x-2)(x-3)",
        .coeffs   = {-6.f, 11.f, -6.f, 1.f},
        .nb_roots = 3,
        .roots    = {1.f, 2.f, 3.f},
    }, {
        .label    = "cubic 3: (x-1)(x^2 + 1)",
        .coeffs   = {-1.f, 1.f, -1.f, 1.f},
        .nb_roots = 1,
        .roots    = {1.f},
    }, {
        .label    = "quartic",
        .coeffs   = {7.f, -4.f, -1.f, -3.f, 2.f},
        .nb_roots = 2,
        .roots    = {1.15214f, 1.79394f},
    }, {
        .label    = "quintic",
        .coeffs   = {3.f, 0.f, 0.f, 0.f, 0.f, 1.f},
        .nb_roots = 1,
        .roots    = {-1.24573f},
    }, {
        .label    = "quintic (x-2)(x+3)(x-5)(x+7)(x-11)",
        .coeffs   = {-2310.f, 727.f, 382.f, -72.f, -8.f, 1.f},
        .roots    = {-7.f, -3.f, 2.f, 5.f, 11.f},
        .nb_roots = 5,
    }, {
        .label    = "quintic (x-1/2)(x+1/3)(x-4/3)(x-2/9)(x+5)",
        .coeffs   = {-20/81.f, 1.f, 349.f/162.f, -74.f/9.f, 59.f/18.f, 1.f},
        .roots    = {-5.f, -1.f/3.f, 2.f/9.f, 1.f/2.f, 4.f/3.f},
        .nb_roots = 5,
    }
};

static int cmp_root(const void *a, const void *b)
{
    const float f0 = *(const float *)a;
    const float f1 = *(const float *)b;
    return f0 < f1 ? -1 : (f0 > f1 ? 1 : 0);
}

int main(void)
{
    float roots[5];

    for (int i = 0; i < NGLI_ARRAY_NB(polytests); i++) {
        const struct polytest *t = &polytests[i];
        const float *c = t->coeffs;
        const int nroots = ngli_root_find5(roots, c[5], c[4], c[3], c[2], c[1], c[0]);
        qsort(roots, nroots, sizeof(*roots), cmp_root);
        printf("%s:\n"
               "  %gx^5 + %gx^4 + %gx^3 + %gx^2 + %gx + %g\n"
               "  %d roots (expected %d)\n",
               t->label,
               c[5], c[4], c[3], c[2], c[1], c[0],
               nroots, t->nb_roots);

        float max_err = 0.f;
        for (int n = 0; n < nroots; n++) {
            const float expected = t->roots[n];
            const float result = roots[n];
            const float err = fabsf(result - expected);
            printf("    r%d:%g expected:%g err:%g\n", n, result, expected, err);
            ngli_assert(!isnan(err));
            max_err = NGLI_MAX(err, max_err);
        }
        ngli_assert(nroots == t->nb_roots);
        ngli_assert(max_err < 0.001f);
    }
    return 0;
}
