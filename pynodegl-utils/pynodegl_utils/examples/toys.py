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

import pynodegl as ngl
from pynodegl_utils.misc import scene
from pynodegl_utils.toolbox.colors import COLORS


@scene(
    txt=scene.Text(),
    font_file=scene.File(),
    fg_color=scene.Color(),
    bg_color=scene.Color(),
    box_corner=scene.Vector(n=3, minv=(-1, -1, -1), maxv=(1, 1, 1)),
    box_width=scene.Vector(n=3, minv=(-10, -10, -10), maxv=(10, 10, 10)),
    box_height=scene.Vector(n=3, minv=(-10, -10, -10), maxv=(10, 10, 10)),
    padding=scene.Range(range=[0, 100]),
    font_scale=scene.Range(range=[0, 15], unit_base=100),
    valign=scene.List(choices=('top', 'center', 'bottom')),
    halign=scene.List(choices=('left', 'center', 'right')),
)
def text(cfg,
         #txt='the quick brown fox\njumps over the lazy dog',
         #txt='ABCD',
         #txt='abcd',
         #font_file='/home/ux/.fonts/FG_Virgil.otf',

         #txt="Ленивый рыжий кот",

         #txt="كسول الزنجبيل القط",
         #font_file='/usr/share/fonts/noto/NotoKufiArabic-Regular.ttf',

         #txt="懶惰的姜貓",
         #font_file='/usr/share/fonts/TTF/HAN NOM A.ttf',

         txt='ウィキペディア\nへようこそ',
         font_file='/usr/share/fonts/TTF/HanaMinA.ttf',
         fg_color=COLORS['cgreen'],
         bg_color=(0.3, 0.3, 0.3, 1.0),
         box_corner=(-1+.25, -1+.25, 0),
         box_width=(1.5, 0, 0),
         box_height=(0, 1.5, 0),
         padding=2,
         font_scale=1.3,
         valign='center',
         halign='center',
):
    return ngl.Text(txt,
                    font_file=font_file,
                    fg_color=fg_color,
                    bg_color=bg_color,
                    box_corner=box_corner,
                    box_width=box_width,
                    box_height=box_height,
                    padding=padding,
                    font_scale=font_scale,
                    valign=valign,
                    halign=halign,
                    aspect_ratio=cfg.aspect_ratio
    )
