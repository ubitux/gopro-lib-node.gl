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

#include "animation.h"
#include "log.h" // XXX
#include "math_utils.h"
#include "nodes.h"
#include "type.h"

#define OFFSET(x) offsetof(struct variable_priv, x)
static const struct node_param motion2d_params[] = {
    {"animation", PARAM_TYPE_NODE, OFFSET(time_anim), .flags=PARAM_FLAG_NON_NULL,
                  .node_types=(const int[]){NGL_NODE_ANIMATEDVEC2, -1},
                  .desc=NGLI_DOCSTRING("2D animation to analyze the motion from")},
    {NULL}
};

static const struct node_param motion3d_params[] = {
    {"animation", PARAM_TYPE_NODE, OFFSET(time_anim), .flags=PARAM_FLAG_NON_NULL,
                  .node_types=(const int[]){NGL_NODE_ANIMATEDVEC3, -1},
                  .desc=NGLI_DOCSTRING("3D animation to analyze the motion from")},
    {NULL}
};

static void mix_motion(void *user_arg, void *dst,
                       const struct animkeyframe_priv *kf0,
                       const struct animkeyframe_priv *kf1,
                       double ratio, int len)
{
    float *dstf = dst;
    // XXX: vec2
    ngli_vec3_sub(dstf, kf1->value, kf0->value);
    ngli_vec3_norm(dstf, dstf);
    ngli_vec3_scale(dstf, dstf, ratio);
    //LOG(ERROR, "mix motion ((%g,%g,%g)-(%g,%g,%g))*%g=(%g,%g,%g)",
    //    kf0->value[0], kf0->value[1], kf0->value[2],
    //    kf1->value[0], kf1->value[1], kf1->value[2],
    //    ratio,
    //    dstf[0], dstf[1], dstf[2]);
}

#define DECLARE_MOTION_FUNCS(len)                                   \
static void mix_motion##len##d(void *user_arg, void *dst,           \
                               const struct animkeyframe_priv *kf0, \
                               const struct animkeyframe_priv *kf1, \
                               double ratio)                        \
{                                                                   \
    return mix_motion(user_arg, dst, kf0, kf1, ratio, len);         \
}                                                                   \
                                                                    \
static void cpy_motion##len##d(void *user_arg, void *dst,           \
                               const struct animkeyframe_priv *kf)  \
{                                                                   \
    memset(dst, 0, len * sizeof(*kf->value));                       \
}                                                                   \

DECLARE_MOTION_FUNCS(2)
DECLARE_MOTION_FUNCS(3)

static ngli_animation_mix_func_type get_mix_func(int node_class)
{
    switch (node_class) {
    case NGL_NODE_MOTION2D:      return mix_motion2d;
    case NGL_NODE_MOTION3D:      return mix_motion3d;
    }
    return NULL;
}

static ngli_animation_cpy_func_type get_cpy_func(int node_class)
{
    switch (node_class) {
    case NGL_NODE_MOTION2D:      return cpy_motion2d;
    case NGL_NODE_MOTION3D:      return cpy_motion3d;
    }
    return NULL;
}

static int animation_init(struct ngl_node *node)
{
    struct variable_priv *s = node->priv_data;
    struct variable_priv *anim = s->time_anim->priv_data;
    s->dynamic = 1;
    return ngli_animation_init(&s->anim, NULL,
                               anim->animkf, anim->nb_animkf,
                               get_mix_func(node->class->id),
                               get_cpy_func(node->class->id),
                               NGLI_ANIM_MODE_DERIVATIVE);
}

#define DECLARE_INIT_FUNC(suffix, class_data, class_data_size, class_data_type) \
static int motion##suffix##_init(struct ngl_node *node)                         \
{                                                                               \
    struct variable_priv *s = node->priv_data;                                  \
    s->data = class_data;                                                       \
    s->data_size = class_data_size;                                             \
    s->data_type = class_data_type;                                             \
    return animation_init(node);                                                \
}

DECLARE_INIT_FUNC(2d, s->vector, 2 * sizeof(*s->vector), NGLI_TYPE_VEC2)
DECLARE_INIT_FUNC(3d, s->vector, 3 * sizeof(*s->vector), NGLI_TYPE_VEC3)

static int motion_update(struct ngl_node *node, double t)
{
    struct variable_priv *s = node->priv_data;
    return ngli_animation_evaluate(&s->anim, s->data, t);
}

#define DEFINE_MOTION_CLASS(class_id, class_name, type)         \
const struct node_class ngli_motion##type##_class = {           \
    .id        = class_id,                                      \
    .category  = NGLI_NODE_CATEGORY_UNIFORM,                    \
    .name      = class_name,                                    \
    .init      = motion##type##_init,                           \
    .update    = motion_update,                                 \
    .priv_size = sizeof(struct variable_priv),                  \
    .params    = motion##type##_params,                         \
    .file      = __FILE__,                                      \
};

DEFINE_MOTION_CLASS(NGL_NODE_MOTION2D, "Motion2D", 2d)
DEFINE_MOTION_CLASS(NGL_NODE_MOTION3D, "Motion3D", 3d)
