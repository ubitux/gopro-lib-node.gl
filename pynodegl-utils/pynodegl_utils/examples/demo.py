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

import array
import os.path as op
import pynodegl as ngl
from pynodegl_utils.misc import scene, Media
from pynodegl_utils.tests.debug import get_debug_points
from pynodegl_utils.toolbox.colors import COLORS
from pynodegl_utils.toolbox.shapes import equilateral_triangle_coords


HIDE_SCENE_NAMES = False


def _get_bg(cfg, m0):
    q = ngl.Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))
    m = ngl.Media(m0.filename)
    t = ngl.Texture2D(data_src=m, mag_filter='linear', min_filter='linear')
    p = ngl.Program(vertex=cfg.get_vert('texture'), fragment=cfg.get_frag('texture'))
    p.update_vert_out_vars(var_tex0_coord=ngl.IOVec2(), var_uvcoord=ngl.IOVec2())
    bg = ngl.Render(q, p, label='background')
    bg.update_frag_resources(tex0=t)
    return bg


def _get_colored_bg(cfg, color):
    q = ngl.Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))
    p = ngl.Program(vertex=cfg.get_vert('color'), fragment=cfg.get_frag('color'))
    bg = ngl.Render(q, p, label='colored background')
    bg.update_frag_resources(color=color)
    return bg


def _get_filtered_bg(cfg, m0, color):
    frag = 'void main() { ngl_out_color = mix(ngl_texvideo(tex0, var_tex0_coord), color, color.a); }\n'
    q = ngl.Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))
    m = ngl.Media(m0.filename)
    t = ngl.Texture2D(data_src=m, mag_filter='linear', min_filter='linear')
    p = ngl.Program(vertex=cfg.get_vert('texture'), fragment=frag)
    p.update_vert_out_vars(var_tex0_coord=ngl.IOVec2(), var_uvcoord=ngl.IOVec2())
    bg = ngl.Render(q, p, label='filtered background')
    bg.update_frag_resources(tex0=t, color=color)
    return bg


def _render_shape(cfg, geometry, color=COLORS['white']):
    prog = ngl.Program(vertex=cfg.get_vert('color'), fragment=cfg.get_frag('color'))
    render = ngl.Render(geometry, prog)
    render.update_frag_resources(color=ngl.UniformVec4(value=color))
    return render


@scene()
def skew(cfg):
    d = cfg.duration = 3
    geom = ngl.Quad()
    shape = _render_shape(cfg, geom)
    anim_kf = [
        ngl.AnimKeyFrameVec3(0,     (0, 0, 0)),
        ngl.AnimKeyFrameVec3(d/3,   (0, 1, 0), 'exp_in_out'),
        ngl.AnimKeyFrameVec3(2*d/3, (0,-1, 0), 'exp_in_out'),
        ngl.AnimKeyFrameVec3(d,     (0, 0, 0), 'exp_in_out'),
    ]
    return ngl.Skew(shape, anim=ngl.AnimatedVec3(anim_kf))


@scene()
def wiggle(cfg):
    vert = '''
void main()
{
    ngl_out_pos = ngl_projection_matrix * ngl_modelview_matrix * ngl_position;
    ngl_out_pos += vec4(wiggle_x, wiggle_y, 0, 0);
}
'''
    frag = '''
void main()
{
    ngl_out_color = color;
}
'''

    geom = ngl.Circle(radius=0.1, npoints=128)
    prog = ngl.Program(vertex=vert, fragment=frag)
    render = ngl.Render(geom, prog)
    render.update_vert_resources(wiggle_x=ngl.Noise(seed=1), wiggle_y=ngl.Noise(seed=2))
    render.update_frag_resources(color=ngl.UniformVec4(value=COLORS['white']))
    return render


@scene()
def motion_circles(cfg):
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
    shape.update_frag_resources(color=ngl.UniformVec4(COLORS['white']), motion=motion)
    shape = ngl.Translate(shape, anim=anim)
    return ngl.GraphicConfig(shape, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


@scene()
def motion_circles_distort(cfg):
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
    shape.update_frag_resources(color=ngl.UniformVec4(COLORS['white']))
    shape.update_vert_resources(motion=motion)
    return ngl.Translate(shape, anim=anim)


@scene()
def text_prototype(cfg):
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
def text_japanese(cfg):
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
def text_quick_brown_fox(cfg):
    return ngl.Text(
        text='The quick brown fox\njumps over the lazy dog',
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDN_2.woff'),
        bg_color=(0.0, 0.0, 0.0, 0.0),
        aspect_ratio=cfg.aspect_ratio,
        font_scale=0.8,
    )


@scene(step=scene.Range(range=[0,11]))
def text_demo_pwetpwet(cfg, step=0):
    cfg.duration = 5

    string = 'Pwet pwet!'
    font_file = None
    font_scale = None
    bounce_down = None
    target = None
    overlap = None
    effects = None
    random = None
    easing = None
    global_transform = None
    aspect_ratio = None
    bg_color = (0, .2, .3, 1)

    if step > 8:
        aspect_ratio = cfg.aspect_ratio = (16, 10)

    if step > 9:
        m0 = Media(op.join(op.dirname(__file__), 'data', 'ryan-steptoe-XnOD9308hV4-unsplash.jpg'))
        aspect_ratio = cfg.aspect_ratio = (m0.width, m0.height)
        bg_color = (0, 0, 0, 0)

    if step > 7:
        easing = 'bounce_out'

    if step > 6:
        random = True

    if step > 5:
        overlap = ngl.UniformFloat(value=0.9)

    if step > 4:
        target = 'char'

    if step > 3:
        animkf = [
            ngl.AnimKeyFrameVec3(0, (0.0, 2.0, 0.0)),
            ngl.AnimKeyFrameVec3(1, (0.0, 0.0, 0.0), easing),
        ]
        bounce_down = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(animkf))
        effects = [
            ngl.TextEffect(
                start=0,
                end=cfg.duration - 2.0,
                target=target,
                overlap=overlap,
                transform=bounce_down,
                random=random,
            ),
        ]

    if step > 1:
        font_scale = 0.8

    if step > 0:
        font_file = op.join(op.dirname(__file__), 'data', 'AVGARDD_2.woff')

    if step > 10:
        string = 'كسول الزنجبيل القط'
        font_file = '/usr/share/fonts/noto/NotoKufiArabic-Regular.ttf'

    text = ngl.Text(
        text=string,
        font_file=font_file,
        bg_color=bg_color,
        aspect_ratio=cfg.aspect_ratio,
        font_scale=font_scale,
        effects=effects
    )

    if step == 3:
        animkf = [
            ngl.AnimKeyFrameVec3(0, (0.0, 2.0, 0.0)),
            ngl.AnimKeyFrameVec3(cfg.duration - 2.0, (0.0, 0.0, 0.0), easing),
        ]
        return ngl.Translate(text, anim=ngl.AnimatedVec3(animkf))


    if step >= 10:
        bg = _get_filtered_bg(cfg, m0, color=ngl.UniformVec4(value=(0, 0, 0, .2)))
        group = ngl.Group(children=(bg, text))
        return ngl.GraphicConfig(group, blend=True,
                                 blend_src_factor='src_alpha',
                                 blend_dst_factor='one_minus_src_alpha',
                                 blend_src_factor_a='zero',
                                 blend_dst_factor_a='one')

    return text


@scene()
def text_noise_cyberretro(cfg):
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
            ),
            ngl.TextEffect(
                start=0,
                end=cfg.duration,
                start_pos=ngl.UniformFloat(0.5),
                target='text',
                glow_width=ngl.Noise(lacunarity=15., gain=2., seed=100),
            ),
            ngl.TextEffect(
                start=0,
                end=cfg.duration,
                end_pos=ngl.UniformFloat(0.5),
                target='text',
                glow_width=ngl.Noise(lacunarity=15., gain=2., seed=200),
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


_rtt_dim = 2048, 2048


_RENDER_WITH_MASK_VERTEX = '''
void main()
{
    ngl_out_pos = ngl_projection_matrix * ngl_modelview_matrix * ngl_position;
    var_uvcoord = ngl_uvcoord;
    var_tex0_coord = (tex0_coord_matrix * vec4(ngl_uvcoord, 0.0, 1.0)).xy;
    var_tex1_coord = (tex1_coord_matrix * vec4(ngl_uvcoord, 0.0, 1.0)).xy;
}
'''


_RENDER_WITH_MASK_POST_MULT_FRAGMENT = '''
void main()
{
    mediump vec4 color = ngl_texvideo(tex0, var_tex0_coord);
    mediump vec4 mask = ngl_texvideo(tex1, var_tex1_coord);
    highp float alpha = inverse ? (1.0 - mask.a) * color.a : color.a * mask.a;
    color = vec4(color.rgb / color.a, alpha);
    ngl_out_color = color;
}
'''


# XXX: inverse looks swapped
def _mask(cfg, scene, mask, inverse=False):
    mask_config = ngl.GraphicConfig(
        mask,
        blend=False,  # XXX
        blend_src_factor='one',
        blend_dst_factor='zero',
        blend_src_factor_a='one',
        blend_dst_factor_a='zero',
    )

    mask_camera = ngl.Camera(mask_config, eye=(0, 0, 1), orthographic=(-1, 1, -1, 1), clipping=(0.1, 10000.0))
    mask_texture = ngl.Texture2D(width=_rtt_dim[0], height=_rtt_dim[1], min_filter='linear', mag_filter='linear', mipmap_filter='linear', label='mask')
    mask_rtt = ngl.RenderToTexture(mask_camera, color_textures=(mask_texture,))

    scene_texture = ngl.Texture2D(width=_rtt_dim[0], height=_rtt_dim[1], min_filter='linear', mag_filter='linear', mipmap_filter='linear', label='scene')
    scene_rtt = ngl.RenderToTexture(scene, color_textures=(scene_texture,))

    quad = ngl.Quad((-1, -1, 0), (2, 0, 0), (0, 2, 0))
    program = ngl.Program(vertex=_RENDER_WITH_MASK_VERTEX, fragment=_RENDER_WITH_MASK_POST_MULT_FRAGMENT)
    program.update_vert_out_vars(var_uvcoord=ngl.IOVec2(), var_tex0_coord=ngl.IOVec2(), var_tex1_coord=ngl.IOVec2())
    render = ngl.Render(quad, program, label='render with mask')
    render.update_frag_resources(tex0=scene_texture)
    render.update_frag_resources(tex1=mask_texture)
    render.update_frag_resources(inverse=ngl.UniformBool(value=inverse))

    config = ngl.GraphicConfig(
        render,
        blend=True,
        # Post mult
        blend_src_factor='src_alpha',
        blend_dst_factor='one_minus_src_alpha',
        blend_src_factor_a='one',
        blend_dst_factor_a='one_minus_src_alpha',
    )
    camera = ngl.Camera(config, eye=(0, 0, 1), orthographic=(-1, 1, -1, 1), clipping=(0.1, 10000.0))

    return ngl.Group(children=(scene_rtt, mask_rtt, camera))


def _mask_with_geometry(cfg, scene, geometry, inverse=False):
    program = ngl.Program(vertex=cfg.get_vert('color'), fragment=cfg.get_frag('color'))
    render = ngl.Render(geometry, program)
    render.update_frag_resources(color=ngl.UniformVec4(value=(0, 0, 0, 1)))
    return _mask(cfg, scene, render, inverse)


@scene()
def text_masking(cfg):
    d = cfg.duration = 5
    easing = 'exp_out'
    ratio = 2/3

    m0 = Media(op.join(op.dirname(__file__), 'data', 'gloomy-staircase.webp'))
    cfg.aspect_ratio = (m0.width, m0.height)
    bg = _get_bg(cfg, m0)

    move_up_animkf = [
        ngl.AnimKeyFrameVec3(0, (0, -ratio, 0)),
        ngl.AnimKeyFrameVec3(1, (0, 0, 0), easing),
    ]
    move_up = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(move_up_animkf))

    move_down_animkf = [
        ngl.AnimKeyFrameVec3(0, (0, ratio, 0)),
        ngl.AnimKeyFrameVec3(1, (0, 0, 0), easing),
    ]
    move_down = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(move_down_animkf))

    text_params = dict(
        text='Stair\ncase',
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDD_2.woff'),
        bg_color=(0, 0, 0, 0),
        aspect_ratio=cfg.aspect_ratio,
        box_corner=(-.5, -ratio, 0),
        box_width=(1, 0, 0),
        box_height=(0, 2*ratio, 0),
        font_scale=0.8,
    )

    text_up   = ngl.Text(**text_params, effects=[ngl.TextEffect(start=0, end=d - 2, transform=move_up)])
    text_down = ngl.Text(**text_params, effects=[ngl.TextEffect(start=0, end=d - 2, transform=move_down)])

    mask_up_geom   = ngl.Quad(corner=(-1, 0, 0), width=(2, 0, 0), height=(0, 1, 0))
    mask_down_geom = ngl.Quad(corner=(-1,-1, 0), width=(2, 0, 0), height=(0, 1, 0))

    text_up   = _mask_with_geometry(cfg, text_up, mask_down_geom, inverse=True)
    text_down = _mask_with_geometry(cfg, text_down, mask_up_geom, inverse=True)

    line_height = .03
    animkf = [
        ngl.AnimKeyFrameVec3(0, (0, 1, 1)),
        ngl.AnimKeyFrameVec3(cfg.duration - 2, (1, 1, 1), easing),
    ]
    geom = ngl.Quad(corner=(-.5, -line_height/2, 0), width=(1, 0, 0), height=(0, line_height, 0))
    shape = _render_shape(cfg, geom)
    shape = ngl.Scale(shape, anim=ngl.AnimatedVec3(animkf))

    return ngl.Group(children=(bg, text_up, text_down, shape))


@scene(easing=scene.List(choices=['linear', 'back_in_out']))
def path_simple_bezier(cfg, easing='linear'):
    cfg.aspect_ratio = (1, 1)
    cfg.duration = 2

    coords = dict(
        A=[-0.7, 0.0],
        B=[-0.2,-0.3],
        C=[ 0.2, 0.8],
        D=[ 0.8, 0.0],
    )

    points = array.array('f')
    points.extend(coords['A'] + [0])
    points.extend(coords['D'] + [0])
    points_buffer = ngl.BufferVec3(data=points)

    controls = array.array('f')
    controls.extend(coords['B'] + [0])
    controls.extend(coords['C'] + [0])
    controls_buffer = ngl.BufferVec3(data=controls)

    path = ngl.Path(points_buffer, controls_buffer, mode='bezier3')

    anim_kf = [
        ngl.AnimKeyFrameFloat(0, 0),
        ngl.AnimKeyFrameFloat(cfg.duration, 1, easing),
    ]

    geom = ngl.Circle(radius=0.03, npoints=64)
    shape = _render_shape(cfg, geom, COLORS['orange'])
    moving_shape = ngl.Translate(shape, anim=ngl.AnimatedPath(anim_kf, path))

    return ngl.Group(children=(
        ngl.PathDraw(path),
        get_debug_points(cfg, coords),
        moving_shape,
    ))


@scene(tension=scene.Range(range=[0.01, 2], unit_base=1000))
def path_catmull(cfg, tension=0.5):
    cfg.aspect_ratio = (1, 1)
    cfg.duration = 3

    conv = lambda x: x/255 * 2 - 1
    v2 = lambda x, y: [conv(x), conv(y)]
    v3 = lambda x, y: [conv(x), conv(y), 0]

    coords = dict(
        A=v2( 48,  89),
        B=v2( 82, 178),
        C=v2(132,  93),
        D=v2(174, 163),
        E=v2(210, 122),
    )

    points = array.array('f')
    for coord in coords.values():
        points.extend(coord + [0])
    points_buffer = ngl.BufferVec3(data=points)

    controls = array.array('f', [
        *v3( 21, 136),
        *v3(234, 132),
    ])
    controls_buffer = ngl.BufferVec3(data=controls)

    path = ngl.Path(points_buffer, controls_buffer, mode='catmull', tension=tension)

    anim_kf = [
        ngl.AnimKeyFrameFloat(0, 0),
        ngl.AnimKeyFrameFloat(cfg.duration, 1, 'exp_in_out'),
    ]

    geom = ngl.Circle(radius=0.03, npoints=64)
    shape = _render_shape(cfg, geom, COLORS['orange'])
    moving_shape = ngl.Translate(shape, anim=ngl.AnimatedPath(anim_kf, path))

    return ngl.Group(children=(
        ngl.PathDraw(path),
        get_debug_points(cfg, coords),
        moving_shape,
    ))


@scene()
def text_path_follow(cfg):
    cfg.aspect_ratio = (1, 1)

    coords = dict(
        A=[-0.8, 0.0],
        B=[-0.2,-0.3],
        C=[ 0.2, 0.8],
        D=[ 0.8, 0.0],
    )

    points = array.array('f')
    points.extend(coords['A'] + [0])
    points.extend(coords['D'] + [0])
    points_buffer = ngl.BufferVec3(data=points)

    controls = array.array('f')
    controls.extend(coords['B'] + [0])
    controls.extend(coords['C'] + [0])
    controls_buffer = ngl.BufferVec3(data=controls)

    path = ngl.Path(points_buffer, controls_buffer, mode='bezier3')

    text = ngl.Text(
        text='Geronimo',
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDN_2.woff'),
        bg_color=(1.0, 0.0, 0.0, 0.5),
        aspect_ratio=cfg.aspect_ratio,
        box_corner=(-.5,-.5,0),
        box_width=(1,0,0),
        box_height=(0,1,0),
        font_scale=0.8,
        path=path,
    )

    group = ngl.Group(children=(
        ngl.PathDraw(path),
        get_debug_points(cfg, dict(p0=(-0.800000,0.000000), p=(-0.659694,-0.046800))),
        text,
    ))
    return ngl.GraphicConfig(group, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


@scene()
def text_skew_zomgspeed(cfg):
    cfg.duration = 4

    m0 = Media(op.join(op.dirname(__file__), 'data', 'tim-foster-CoSJhdxIiik-unsplash.jpg'))
    cfg.aspect_ratio = (m0.width, m0.height)
    bg = _get_bg(cfg, m0)

    skew_amount = -1

    skew_animkf = [
        ngl.AnimKeyFrameVec3(0,   (0, skew_amount, 0)),
        ngl.AnimKeyFrameVec3(0.2, (0, skew_amount, 0), 'exp_in'),
        ngl.AnimKeyFrameVec3(1,   (0, 0, 0), 'elastic_out'),
    ]
    move_animkf = [
        ngl.AnimKeyFrameVec3(0,   (-3, 0, 0)),
        ngl.AnimKeyFrameVec3(0.2, (skew_amount, 0, 0), 'exp_in'),
        ngl.AnimKeyFrameVec3(1.0, (0, 0, 0), 'elastic_out'),
    ]

    trf = ngl.Skew(ngl.Identity(), anim=ngl.AnimatedVec3(skew_animkf))
    trf = ngl.Translate(trf, anim=ngl.AnimatedVec3(move_animkf))

    text = ngl.Text(
        text='ZOMG\nSPEED',
        font_file=op.join(op.dirname(__file__), 'data', 'AVGARDD_2.woff'),
        bg_color=(0.0, 0.0, 0.0, 0.0),
        aspect_ratio=cfg.aspect_ratio,
        font_scale=0.8,
        effects=[ngl.TextEffect(start=0, end=cfg.duration - 2.0, transform=trf)]
    )

    group = ngl.Group(children=(bg, text))
    return ngl.GraphicConfig(group, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


# derived from https://www.youtube.com/watch?v=Y2D-k-8syoE
@scene()
def text_motion(cfg):
    cfg.duration = 4
    cfg.aspect_ratio = (16, 9)
    bg = _get_colored_bg(cfg, ngl.UniformVec4(value=(.1, .1, .1, 1)))

    easing = 'back_out'
    vert_scale_down_animkf = [
        ngl.AnimKeyFrameVec3(0, (1, 5, 1)),
        ngl.AnimKeyFrameVec3(1, (1, 1, 1), easing),
    ]
    scale_down_animkf = [
        ngl.AnimKeyFrameVec3(0, (15, 15, 15)),
        ngl.AnimKeyFrameVec3(1, (1, 1, 1), easing),
    ]
    drop_down_animkf = [
        ngl.AnimKeyFrameVec3(0, (0, 2, 0)),
        ngl.AnimKeyFrameVec3(1, (0, 0, 0), easing),
    ]
    jump_up_animkf = [
        ngl.AnimKeyFrameVec3(0, (0,-2, 0)),
        ngl.AnimKeyFrameVec3(1, (0, 0, 0), easing),
    ]
    slide_r2l_animkf = [
        ngl.AnimKeyFrameVec3(0, (2, 0, 0)),
        ngl.AnimKeyFrameVec3(1, (0, 0, 0), easing),
    ]

    vert_scale_down = ngl.Scale(ngl.Identity(),     anim=ngl.AnimatedVec3(vert_scale_down_animkf), label='vertical scale down')
    scale_down      = ngl.Scale(ngl.Identity(),     anim=ngl.AnimatedVec3(scale_down_animkf),      label='scale down')
    drop_down       = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(drop_down_animkf),       label='drop down')
    jump_up         = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(jump_up_animkf),         label='jump up')
    scale_down2     = ngl.Scale(ngl.Identity(),     anim=ngl.AnimatedVec3(scale_down_animkf),      label='scale down')  # XXX worth perf wise?
    slide_r2l       = ngl.Translate(ngl.Identity(), anim=ngl.AnimatedVec3(slide_r2l_animkf),       label='slide right to left')

    string = 'MOTION'
    chr_effects = [
        dict(transform=vert_scale_down),  # M
        dict(transform=scale_down),       # O
        dict(transform=drop_down),        # T
        dict(transform=jump_up),          # I
        dict(transform=scale_down2),      # O
        dict(transform=slide_r2l),        # N
    ]
    assert len(string) == len(chr_effects)
    n = len(string)

    texts = [
        ('shadow', 0.15, ngl.UniformVec4(value=(0,.6,.9, 1))),
        ('main',   0,    ngl.UniformVec4(value=(1, 1, 1, 1))),
    ]

    children = [bg]
    for label, lag, color in texts:
        text = ngl.Text(
            label=label,
            text=string,
            font_file=op.join(op.dirname(__file__), 'data', 'AVGARDD_2.woff'),
            bg_color=(0.0, 0.0, 0.0, 0.0),
            aspect_ratio=cfg.aspect_ratio,
            font_scale=0.6,
            effects=[
                ngl.TextEffect(
                    color=color,
                    start=0,
                    end=cfg.duration + lag - 2.0,
                    target='char',
                    start_pos=ngl.UniformFloat(i / n),
                    end_pos=ngl.UniformFloat((i + 1) / n),
                    overlap=ngl.UniformFloat(value=0.2),
                    **chr_effect)
                for i, chr_effect in enumerate(chr_effects)
            ]
        )
        children.append(text)

    group = ngl.Group(children)
    return ngl.GraphicConfig(group, blend=True,
                             blend_src_factor='src_alpha',
                             blend_dst_factor='one_minus_src_alpha',
                             blend_src_factor_a='zero',
                             blend_dst_factor_a='one')


if HIDE_SCENE_NAMES:
    demos = (
        'skew',
        'wiggle',
        'motion_circles',
        'motion_circles_distort',
        'path_simple_bezier',
        'path_catmull',
        'text_quick_brown_fox',
        'text_demo_pwetpwet',
        'text_skew_zomgspeed',
        'text_noise_cyberretro',
        'text_prototype',
        'text_japanese',
        'text_masking',
        'text_motion',
        'text_path_follow',  # WIP
    )

    for i, demo_name in enumerate(demos):
        globals()[f's{i:02d}'] = globals().pop(demo_name)
