"""Depth formats: the rb suite's pixel shader also exporting oDepth = the
texel's X, into a D24S8 or D24FS8 depth buffer (depth test ALWAYS with writes,
stencil ALWAYS / REPLACE with reference 0x5A), then a depth Resolve as
Direct3D's (source 4, copy command 0 raw, to k_8_8_8_8) copies the depth
buffer's bits. Each format runs with SQ_PROGRAM_CNTL.ps_export_mode 2
(Direct3D's for color only) and 3, to show which one oDepth needs.

Texture 'depth' X (texel i = x + 64y): 64 specials, then 992 D24 probes
(k + (-0.5, -0.25, 0, 0.25, 0.5, 0.5 + 2^-10, -2^-10, 1/3)[i % 8]) / (2^24 - 1)
for random k, then 992 random floats in [2^-24, 2) of every exponent.
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import rb  # noqa: E402

D24S8, D24FS8 = 0, 1
OFFSETS = (-0.5, -0.25, 0, 0.25, 0.5, 0.5 + 2 ** -10, -2 ** -10, 1 / 3)
RESOLVE = gpugen.resolve_registers(rb.WIDTH, rb.HEIGHT, 6, 4, command=0,
                                   source=4)
f2w = gpugen.f2w


def depth_texture(rng):
    words = []
    for i in range(rb.WIDTH * rb.HEIGHT):
        if i < 64:
            depth = rb.SPECIALS[i]
        elif i < 1056:
            k = rng.randrange(0, 1 << 24)
            depth = f2w((k + OFFSETS[i % 8]) / 16777215)
        else:
            depth = f2w(rng.uniform(1, 2) * 2.0 ** rng.randint(-24, 0))
        words += [depth, f2w(0.25), f2w(0.5), f2w(0.75)]
    return words


def build(suite):
    rng = random.Random(0xD24)
    suite.set_inputs(rb.RECT, 2)
    suite.add_texture('depth', depth_texture(rng))
    pixel = rb.pixel_shader(depth=True)
    for fname, fmt in (('D24S8', D24S8), ('D24FS8', D24FS8)):
        for export_mode in (2, 3):
            program_cntl = ((rb.PROGRAM_CNTL & ~(0xF << 27)) |
                            (export_mode << 27))
            rb.test(suite, f'{fname} export mode {export_mode}', 'depth',
                    rb.registers(depth_format=fmt), RESOLVE, pixel=pixel,
                    program_cntl=program_cntl)
