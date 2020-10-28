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
#include <stddef.h>
#include <string.h>

#include "log.h"
#include "nodegl.h"
#include "nodes.h"
#include "math_utils.h"
#include "transforms.h"

struct skew_priv {
    struct transform_priv trf;
    float factors[3];
    float axis[3];
    struct ngl_node *anim;
};

static void update_trf_matrix(struct ngl_node *node, const float *factors)
{
    struct skew_priv *s = node->priv_data;
    struct transform_priv *trf = &s->trf;
    ngli_mat4_skew(trf->matrix, factors, s->axis);
}

static int skew_init(struct ngl_node *node)
{
    struct skew_priv *s = node->priv_data;
    static const float zvec[3] = {0};
    if (!memcmp(s->axis, zvec, sizeof(s->axis))) {
        LOG(ERROR, "(0.0, 0.0, 0.0) is not a valid axis");
        return NGL_ERROR_INVALID_ARG;
    }
    if (!s->anim)
        update_trf_matrix(node, s->factors);
    return 0;
}

static int update_factors(struct ngl_node *node)
{
    struct skew_priv *s = node->priv_data;
    if (s->anim) {
        LOG(ERROR, "updating factors while the animation is set is unsupported");
        return NGL_ERROR_INVALID_USAGE;
    }
    update_trf_matrix(node, s->factors);
    return 0;
}

static int skew_update(struct ngl_node *node, double t)
{
    struct skew_priv *s = node->priv_data;
    struct transform_priv *trf = &s->trf;
    struct ngl_node *child = trf->child;
    if (s->anim) {
        struct ngl_node *anim_node = s->anim;
        struct variable_priv *anim = anim_node->priv_data;
        int ret = ngli_node_update(anim_node, t);
        if (ret < 0)
            return ret;
        update_trf_matrix(node, anim->vector);
    }
    return ngli_node_update(child, t);
}

#define OFFSET(x) offsetof(struct skew_priv, x)
static const struct node_param skew_params[] = {
    {"child",   PARAM_TYPE_NODE, OFFSET(trf.child),
                .flags=PARAM_FLAG_NON_NULL,
                .desc=NGLI_DOCSTRING("scene to skew")},
    {"factors", PARAM_TYPE_VEC3,  OFFSET(factors),
                .flags=PARAM_FLAG_ALLOW_LIVE_CHANGE,
                .update_func=update_factors,
                .desc=NGLI_DOCSTRING("skewing factors, only components forming a plane opposite to `axis` should be set")},
    {"axis",    PARAM_TYPE_VEC3, OFFSET(axis), {.vec={1.0, 0.0, 0.0}},
                .desc=NGLI_DOCSTRING("skew axis")},
    {"anim",    PARAM_TYPE_NODE, OFFSET(anim),
                .node_types=(const int[]){NGL_NODE_ANIMATEDVEC3, NGL_NODE_STREAMEDVEC3, -1},
                .desc=NGLI_DOCSTRING("`factors` animation")},
    {NULL}
};

NGLI_STATIC_ASSERT(trf_on_top_of_skew, OFFSET(trf) == 0);

const struct node_class ngli_skew_class = {
    .id        = NGL_NODE_SKEW,
    .name      = "Skew",
    .init      = skew_init,
    .update    = skew_update,
    .draw      = ngli_transform_draw,
    .priv_size = sizeof(struct skew_priv),
    .params    = skew_params,
    .file      = __FILE__,
};
