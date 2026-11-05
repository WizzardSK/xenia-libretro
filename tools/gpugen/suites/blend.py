"""Blending: per render target format and blend mode, a first draw writes the
'dst' texture unblended, a second the 'src' texture with RB_BLENDCONTROL0 set,
then the 64x32 render target is resolved converted as in the rb suite.

Textures (64x32 float4, texel i = x + 64y): 'src'/'dst' channels in [0, 1],
over the first 100 texels every pair of the edge values 0, 1, 0.5 and k/255
in each channel, the rest random 24-bit fractions; 'src wide'/'dst wide' in
[-4, 40) for the float formats.
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import rb  # noqa: E402

ZERO, ONE, SRC_ALPHA, INV_SRC_ALPHA = 0, 1, 6, 7
DST_COLOR, DST_ALPHA = 8, 10
CONSTANT_COLOR, INV_CONSTANT_COLOR, SRC_ALPHA_SATURATE = 12, 13, 16
ADD, SUBTRACT, MIN, MAX, REV_SUBTRACT = 0, 1, 2, 3, 4
NO_BLEND = 0x00010001
CONSTANT = (0.3, 0.6, 0.9, 0.1)
f2w = gpugen.f2w


def blend(src, op, dst, alpha=None):
    a_src, a_op, a_dst = alpha or (src, op, dst)
    return (src | (op << 5) | (dst << 8) | (a_src << 16) | (a_op << 21) |
            (a_dst << 24))


MODES = [('alpha', blend(SRC_ALPHA, ADD, INV_SRC_ALPHA)),
         ('add', blend(ONE, ADD, ONE)),
         ('multiply', blend(DST_COLOR, ADD, ZERO, (DST_ALPHA, ADD, ZERO))),
         ('subtract', blend(ONE, SUBTRACT, ONE)),
         ('reverse subtract', blend(ONE, REV_SUBTRACT, ONE)),
         ('min', blend(ONE, MIN, ONE)), ('max', blend(ONE, MAX, ONE)),
         ('constant', blend(CONSTANT_COLOR, ADD, INV_CONSTANT_COLOR)),
         ('saturate', blend(SRC_ALPHA_SATURATE, ADD, ONE, (ONE, ADD, ONE)))]
# rb.FORMATS entries blending 'src' over 'dst', and the float ones blending
# the wide textures.
UNIT_FORMATS = ['8_8_8_8', '8_8_8_8_GAMMA', '2_10_10_10', '16_16_16_16',
                '2_10_10_10_AS_10_10_10_10']
WIDE_FORMATS = ['2_10_10_10_FLOAT', '16_16_16_16_FLOAT',
                '2_10_10_10_FLOAT_AS_16_16_16_16', '32_FLOAT']
EDGES = [0.0, 1.0, 0.5] + [k / 255 for k in (1, 2, 127, 128, 129, 253, 254)]


def unit_texture(rng, dst):
    """Texel i < len(EDGES)^2 takes edge i % len(EDGES) in 'src' and
    i // len(EDGES) in 'dst', rotated per channel."""
    n = len(EDGES)
    words = []
    for i in range(rb.WIDTH * rb.HEIGHT):
        for c in range(4):
            if i < n * n:
                v = EDGES[((i // n if dst else i) + 3 * c) % n]
            else:
                v = rng.getrandbits(24) / 2 ** 24
            words.append(f2w(v))
    return words


def wide_texture(rng):
    return [f2w(rng.uniform(-4, 40)) for _ in range(4 * rb.WIDTH * rb.HEIGHT)]


def build(suite):
    rng = random.Random(0xB1E)
    suite.set_inputs(rb.RECT, 2)
    suite.add_texture('src', unit_texture(rng, False))
    suite.add_texture('dst', unit_texture(rng, True))
    for name in ('src wide', 'dst wide'):
        suite.add_texture(name, wide_texture(rng))
    formats = {f[0]: f for f in rb.FORMATS}
    for names, src, dst in ((UNIT_FORMATS, 'src', 'dst'),
                            (WIDE_FORMATS, 'src wide', 'dst wide')):
        for fname in names:
            _, rt_format, _, dest_format, number = formats[fname]
            state = rb.registers(rt_format) + [
                ('RB_BLEND_RED', [f2w(v) for v in CONSTANT])]
            resolve = gpugen.resolve_registers(
                rb.WIDTH, rb.HEIGHT, dest_format, rb.TEXEL_BYTES[dest_format],
                number)
            for mode, control in MODES:
                rb.test(suite, f'{fname} {mode}', src, state, resolve,
                        passes=[([('RB_BLENDCONTROL0', [NO_BLEND])],
                                 rb.fetch(dst))],
                        after_passes=[('RB_BLENDCONTROL0', [control])])
