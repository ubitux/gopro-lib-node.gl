#
# Copyright 2020 GoPro Inc.
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

import os.path as op
import pynodegl as ngl
from pynodegl_utils.misc import scene, Media
from pynodegl_utils.toolbox.colors import COLORS
from pynodegl_utils.toolbox.shapes import equilateral_triangle_coords

def _get_bg(cfg, m0):
    q = ngl.Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))
    m = ngl.Media(m0.filename)
    t = ngl.Texture2D(data_src=m, mag_filter='linear', min_filter='linear')
    p = ngl.Program(vertex=cfg.get_vert('texture'), fragment=cfg.get_frag('texture'))
    p.update_vert_out_vars(var_tex0_coord=ngl.IOVec2(), var_uvcoord=ngl.IOVec2())
    bg = ngl.Render(q, p, label='background')
    bg.update_frag_resources(tex0=t)
    return bg

def _get_filtered_bg(cfg, m0, color):
    frag = '''
void main()
{
    ngl_out_color = mix(ngl_texvideo(tex0, var_tex0_coord), color, color.a);
}
'''

    q = ngl.Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))
    m = ngl.Media(m0.filename)
    t = ngl.Texture2D(data_src=m, mag_filter='linear', min_filter='linear')
    p = ngl.Program(vertex=cfg.get_vert('texture'), fragment=frag)
    p.update_vert_out_vars(var_tex0_coord=ngl.IOVec2(), var_uvcoord=ngl.IOVec2())
    bg = ngl.Render(q, p, label='background')
    bg.update_frag_resources(tex0=t, color=color)
    return bg

@scene()
def jp(cfg):
    m0 = Media(op.join(op.dirname(__file__), 'data', 'japan-gate.webp'))
    cfg.duration = 9.0
    cfg.aspect_ratio = (m0.width, m0.height)

    bgalpha_animkf = [
        ngl.AnimKeyFrameVec4(0,                (0, 0, 0, 0.0)),
        ngl.AnimKeyFrameVec4(1,                (0, 0, 0, 0.4)),
        ngl.AnimKeyFrameVec4(cfg.duration - 1, (0, 0, 0, 0.4)),
        ngl.AnimKeyFrameVec4(cfg.duration,     (0, 0, 0, 0.0)),
    ]
    bg = _get_filtered_bg(cfg, m0, ngl.AnimatedVec4(bgalpha_animkf))

    text = ngl.Text(
        text='減る記憶、\nそれでも増える、\nパスワード',
        font_file='/usr/share/fonts/TTF/HanaMinA.ttf',
        bg_color=(0.0, 0.0, 0.0, 0.0),
        font_scale=1/2.,
        box_height=(0,1.8,0),
        writing_mode='vertical-rl',
        aspect_ratio=cfg.aspect_ratio,
        effects=[
            ngl.TextEffect(
                target='text',
                start=0.0,
                end=cfg.duration,
                color=ngl.UniformVec4(value=(1.0, 0.8, 0.6, 1.0)),
            ),
            ngl.TextEffect(
                start=1.0,
                end=cfg.duration - 3.0,
                target='char',
                overlap=ngl.UniformFloat(value=0.7),
                alpha=ngl.AnimatedFloat([
                    ngl.AnimKeyFrameFloat(0, 0),
                    ngl.AnimKeyFrameFloat(1, 1),
                ]),
            ),
            ngl.TextEffect(
                target='text',
                start=cfg.duration - 2.0,
                end=cfg.duration - 1.0,
                alpha=ngl.AnimatedFloat([
                    ngl.AnimKeyFrameFloat(0, 1),
                    ngl.AnimKeyFrameFloat(1, 0),
                ]),
            ),
        ]
    )

    text_ranges = [
        ngl.TimeRangeModeNoop(0),
        ngl.TimeRangeModeCont(1),
        ngl.TimeRangeModeNoop(cfg.duration - 1.0)
    ]
    text = ngl.TimeRangeFilter(text, ranges=text_ranges)

    group = ngl.Group(children=(bg, text))
    return ngl.GraphicConfig(group, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


@scene()
def motion(cfg):
    cfg.duration = 3.
    cfg.aspect_ratio = (1, 1)

    coords = list(equilateral_triangle_coords())
    coords.append(coords[0])
    pos_kf = [ngl.AnimKeyFrameVec3(cfg.duration * i/3., pos, 'exp_out') for i, pos in enumerate(coords)]
    anim = ngl.AnimatedVec3(pos_kf)
    motion = ngl.Motion3D(anim)

    vert = '''
void main()
{
    ngl_out_pos = ngl_projection_matrix * ngl_modelview_matrix * ngl_position;
}
'''
    frag = '''
void main()
{
    ngl_out_color = vec4(color.rgb, length(motion));
}
'''

    geom = ngl.Circle(radius=0.2, npoints=128)
    prog = ngl.Program(vertex=vert, fragment=frag)
    shape = ngl.Render(geom, prog)
    shape.update_frag_resources(color=ngl.UniformVec4([1.0] * 4), motion=motion)
    shape.update_vert_resources(motion=motion)
    shape = ngl.Translate(shape, anim=anim)
    return ngl.GraphicConfig(shape, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


@scene()
def motion2(cfg):
    cfg.duration = 4.
    cfg.aspect_ratio = (1, 1)

    coords = list(equilateral_triangle_coords())
    coords.append(coords[0])
    pos_kf = [ngl.AnimKeyFrameVec3(cfg.duration * i/3., pos, 'exp_in_out') for i, pos in enumerate(coords)]
    anim = ngl.AnimatedVec3(pos_kf)
    motion = ngl.Motion3D(anim)

    vert = '''
void main()
{
    float distort_max = 0.15; // % of the distance between 2 keyframes, converging toward this value when acceleration is +∞
    float motion_l = length(motion);
    float direction_l = length(ngl_position.xyz);
    vec3 normed_motion = motion_l == 0.0 ? vec3(0.0) : -motion / motion_l;
    vec3 normed_direction = direction_l == 0.0 ? vec3(0.0) : ngl_position.xyz / direction_l;
    float distort = clamp(dot(normed_motion, normed_direction) / 3.0 * distort_max, 0.0, 1.0);
    vec4 pos = ngl_position + vec4(-distort * motion, 0.0);
    ngl_out_pos = ngl_projection_matrix * ngl_modelview_matrix * pos;
}
'''
    frag = '''
void main()
{
    ngl_out_color = color;
}
'''

    geom = ngl.Circle(radius=0.2, npoints=128)
    prog = ngl.Program(vertex=vert, fragment=frag)
    shape = ngl.Render(geom, prog)
    shape.update_frag_resources(color=ngl.UniformVec4([1.0] * 4), motion=motion)
    shape.update_vert_resources(motion=motion)
    return ngl.Translate(shape, anim=anim)


@scene()
def title(cfg):
    m0 = Media(op.join(op.dirname(__file__), 'data', 'city2.webp'))
    cfg.aspect_ratio = (m0.width, m0.height)

    delay = 2.0
    pause = 1.5
    text_effect_duration = 4.0

    in_start  = 0
    in_end    = in_start + text_effect_duration
    out_start = in_end + pause
    out_end   = out_start + text_effect_duration
    cfg.duration = out_end + 2.0  # leave a delay before looping

    alphain_animkf  = [ngl.AnimKeyFrameFloat(0, 0), ngl.AnimKeyFrameFloat(1, 1)]
    alphaout_animkf = [ngl.AnimKeyFrameFloat(0, 1), ngl.AnimKeyFrameFloat(1, 0)]
    blurin_animkf   = [ngl.AnimKeyFrameFloat(0, 1), ngl.AnimKeyFrameFloat(1, 0)]
    blurout_animkf  = [ngl.AnimKeyFrameFloat(0, 0), ngl.AnimKeyFrameFloat(1, 1)]

    text_effect_settings = dict(
        target='char',
        random=True,
        overlap=ngl.UniformFloat(value=0.5),
    )

    text = ngl.Text(
        text='Prototype',
        bg_color=(0.0, 0.0, 0.0, 0.0),
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDN_2.woff'),
        aspect_ratio=cfg.aspect_ratio,
        effects=[
            ngl.TextEffect(
                start=in_start,
                end=in_end,
                random_seed=6,
                alpha=ngl.AnimatedFloat(alphain_animkf),
                blur=ngl.AnimatedFloat(blurin_animkf),
                **text_effect_settings),
            ngl.TextEffect(
                start=out_start,
                end=out_end,
                random_seed=50,
                alpha=ngl.AnimatedFloat(alphaout_animkf),
                blur=ngl.AnimatedFloat(blurout_animkf),
                **text_effect_settings),
        ]
    )

    text_animkf = [
        ngl.AnimKeyFrameVec3(0.0, (0.5, 0.5, 0.5)),
        ngl.AnimKeyFrameVec3(cfg.duration, (0.9, 0.9, 0.9)),
    ]
    text = ngl.Scale(text, anim=ngl.AnimatedVec3(text_animkf))
    text_ranges = [
        ngl.TimeRangeModeCont(0),
        ngl.TimeRangeModeNoop(out_end)
    ]
    text = ngl.TimeRangeFilter(text, ranges=text_ranges)

    bg = _get_bg(cfg, m0)
    bg_animkf = [
        ngl.AnimKeyFrameVec3(0.0, (1.0, 1.0, 1.0)),
        ngl.AnimKeyFrameVec3(cfg.duration, (1.2, 1.2, 1.2)),
    ]
    bg = ngl.Scale(bg, anim=ngl.AnimatedVec3(bg_animkf))

    group = ngl.Group(children=(bg, text))
    return ngl.GraphicConfig(group, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


@scene()
def simple(cfg):
    return ngl.Text(
        text='the quick brown fox\njumps over the lazy dog',
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDN_2.woff'),
        bg_color=(0.0, 0.0, 0.0, 0.0),
        aspect_ratio=cfg.aspect_ratio,
    )


@scene()
def pwet(cfg):
    cfg.duration = 5

    animkf = [
        ngl.AnimKeyFrameVec3(0, (0.0, 2.0, 0.0)),
        ngl.AnimKeyFrameVec3(1, (0.0, 0.0, 0.0), 'bounce_out'),
    ]
    bounce_down = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(animkf))

    return ngl.Text(
        text='Pwet pwet!',
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDD_2.woff'),
        bg_color=(0.0, 0.0, 0.0, 0.0),
        aspect_ratio=cfg.aspect_ratio,
        font_scale=0.8,
        effects=[
            ngl.TextEffect(
                start=0,
                end=cfg.duration - 2.0,
                target='char',
                overlap=ngl.UniformFloat(value=0.9),
                transform=bounce_down,
                random=True,
            ),
        ]
    )


@scene()
def glow(cfg):
    m0 = Media(op.join(op.dirname(__file__), 'data', 'dark-street.webp'))
    cfg.aspect_ratio = (m0.width, m0.height)

    text = ngl.Text(
        text='CyberRetro',
        font_file=op.join(op.dirname(__file__), 'data', 'Quicksand-Medium.ttf'),
        bg_color=(0.0, 0.0, 0.0, 0.0),
        aspect_ratio=cfg.aspect_ratio,
        font_scale=.8,
        effects=[
            ngl.TextEffect(
                start=0,
                end=cfg.duration,
                target='text',
                color=ngl.UniformVec4(value=(0.2, 0.2, 0.2, 1.0)),
                glow_color=ngl.UniformVec4(value=(1, 0, 1, 1)),
                glow_width=ngl.UniformFloat(value=0.5),
            ),
            ngl.TextEffect(
                start=0,
                end=cfg.duration,
                start_pos=ngl.UniformFloat(0.5),
                target='text',
                glow_width=ngl.Noise(lacunarity=15., gain=2.),
            ),
        ]
    )

    bg = _get_bg(cfg, m0)
    group = ngl.Group(children=(bg, text))
    return ngl.GraphicConfig(group, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


_rtt_dim = 1024, 1024

def _mask(cfg, target_scene, mask_geom, inverse=False):
    # mask is the alpha scene using the shape
    alpha = 0 if inverse else 1
    mask_prog = ngl.Program(vertex=cfg.get_vert('color'), fragment=cfg.get_frag('color'))
    mask_scene = ngl.Render(mask_geom, mask_prog, label='mask scene')
    mask_scene.update_frag_resources(color=ngl.UniformVec4(value=(0, 0, 0, alpha)))
    # XXX: generate a texture with 1 comp

    # make a texture where only the alpha is set
    # the blending is here to make sure we preserve the alpha of the mask
    mask_gc = ngl.GraphicConfig(mask_scene,
        blend=True,
        blend_src_factor='zero',
        blend_dst_factor='zero',
        blend_src_factor_a='src_alpha',
        blend_dst_factor_a='one_minus_src_alpha',
    )
    mask_texture = ngl.Texture2D(width=_rtt_dim[0], height=_rtt_dim[1], min_filter='linear', mag_filter='linear', mipmap_filter='linear', label='mask')
    mask_rtt = ngl.RenderToTexture(mask_gc, color_textures=[mask_texture], clear_color=(0, 0, 0, 1 - alpha))

    # rendering the mask texture fullscreen
    screen_geometry = ngl.Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))
    mask_prog = ngl.Program(vertex=cfg.get_vert('texture'), fragment=cfg.get_frag('texture'))
    mask_prog.update_vert_out_vars(var_tex0_coord=ngl.IOVec2(), var_uvcoord=ngl.IOVec2())
    mask_render = ngl.Render(screen_geometry, mask_prog, label='mask tex render')
    mask_render.update_frag_resources(tex0=mask_texture)

    target_scene = ngl.GraphicConfig(target_scene, blend=True,
                             blend_src_factor='one',
                             blend_dst_factor='zero',
                             blend_src_factor_a='one',
                             blend_dst_factor_a='zero',
                             label='target scene')
    scene_texture = ngl.Texture2D(width=_rtt_dim[0], height=_rtt_dim[1], min_filter='linear', mag_filter='linear', mipmap_filter='linear', label='scene texture')
    scene_rtt = ngl.RenderToTexture(target_scene, color_textures=[scene_texture])
    scene_prog = ngl.Program(vertex=cfg.get_vert('texture'), fragment=cfg.get_frag('texture'))
    scene_prog.update_vert_out_vars(var_tex0_coord=ngl.IOVec2(), var_uvcoord=ngl.IOVec2())
    scene = ngl.Render(screen_geometry, scene_prog, label='scene tex render')
    scene.update_frag_resources(tex0=scene_texture)
    #return ngl.Group(children=(scene_rtt, scene))

    gc = ngl.GraphicConfig(
        ngl.Group(children=(mask_render, scene)),
        blend=True,
        blend_src_factor='dst_alpha',
        blend_dst_factor='zero',
        blend_src_factor_a='one',
        blend_dst_factor_a='zero',
    )

    g = ngl.Group(children=(mask_rtt, scene_rtt, gc))
    return g




@scene()
def masking(cfg):
    cfg.duration = 5

    m0 = Media(op.join(op.dirname(__file__), 'data', 'gloomy-staircase.webp'))
    cfg.aspect_ratio = (m0.width, m0.height)
    bg = _get_bg(cfg, m0)

    animkf = [
        ngl.AnimKeyFrameVec3(0, (0.0, -1, 0.0)),
        ngl.AnimKeyFrameVec3(1, (0.0,  0.0, 0.0), 'exp_out'),
    ]
    transform = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(animkf))
    #transform = ngl.Skew(transform, factors=(0, -0.1, 0))

    text = ngl.Text(
        text='Staircase',
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDD_2.woff'),
        bg_color=(1.0, 0.0, 0.0, 0.2),
        aspect_ratio=cfg.aspect_ratio,
        box_corner=(-1,0,0),
        box_width=(2,0,0),
        box_height=(0,1,0),
        font_scale=0.8,
        effects=[
            ngl.TextEffect(
                start=0,
                end=cfg.duration - 2.0,
                target='char',
                overlap=ngl.UniformFloat(value=0.9),
                #blur=ngl.UniformFloat(value=0.1),
                transform=transform,
            ),
        ]
    )
    #return text

    #mask_geom = ngl.Quad(corner=(-1, -1, 0), width=(2, 0, 0), height=(0, 1, 0))
    mask_geom = ngl.Triangle()
    text = _mask(cfg, text, mask_geom)

    #return text
    group = ngl.Group(children=(bg, text))
    return ngl.GraphicConfig(group, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')
