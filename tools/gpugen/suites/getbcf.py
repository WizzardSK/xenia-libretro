"""getBCF, the border color fraction, from vertex shaders. Each test exports
getBCF's whole destination (xyzw) and then a tfetch with the same fetch
constant at the same coordinates, which shows the border actually blended in.
Unless a test says otherwise, the texture is all zero bits with the white
border, so that tfetch returns the border's share in the format's precision.
Vertex v (1024) fetches its inputs:
- r1.xy normalized (u, 0.5), u = -2 + 5v/1024: point sampling in every clamp
  mode, the tfilter clamp coordinates.
- r1.zw unnormalized (-2 + v/128, 0.5): linear filtering of a 4-texel row
  across both edges in 1/128 texel steps, every clamp mode, border color,
  format and the fetch constant fields getBCF may or may not apply.
- r2.xy unnormalized (-1/2 + (v % 32)/32, -1/2 + (v // 32)/32): bilinear at
  the corner of a 4x4 texture, how the two axes' fractions combine.
- r3.xyz normalized (1/2, 1/2, -1/2 + v/512): a 4x4x4 3D texture through
  both z edges in 1/128 texel steps.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

SLOT = 1
VERTICES = 1024
POINT, LINEAR = (0, 0, 0), (1, 1, 0)
EDGE, BORDER = 2, 6
WHITE = 1
# Fetch constant swizzles.
SWIZZLE_WZYX = 0x053
SWIZZLE_0001 = 0xB24
DIMENSION_3D = 2 << 9
f2w = gpugen.f2w


def fetch_constant(fmt=6, width=4, height=1, clamp=(BORDER, EDGE, EDGE),
                   filters=LINEAR, border_color=WHITE, **keywords):
    return gpugen.texture_constant(fmt, width, height, clamp=clamp,
                                   filters=filters, border_color=border_color,
                                   **keywords)


def fetch_constant_3d(clamp, volume_filter):
    """A tiled 4x4x4 8888 3D texture, so its zeros need no linear 3D layout,
    filtered linearly in x and y and by `volume_filter` in z."""
    words = fetch_constant(clamp=clamp)
    words[0] |= 1 << 31
    words[2] = 3 | (3 << 11) | (3 << 22)
    words[4] |= volume_filter * 0b11
    words[5] = (words[5] & ~(3 << 9)) | DIMENSION_3D
    return words


def inputs():
    words = []
    for v in range(VERTICES):
        words += [f2w(c) for c in (
            -2 + 5 * v / 1024, 0.5, -2 + v / 128, 0.5,
            -0.5 + (v % 32) / 32, -0.5 + (v // 32) / 32, 0.5, 0.5,
            0.5, 0.5, -0.5 + v / 512, 0.5)]
    return words


def build(suite):
    suite.set_inputs(inputs(), 12)
    suite.add_texture('zero', [0] * 16384)
    suite.add_texture('four8888', gpugen.texture_words(
        bytes(range(0x10, 0x100, 0x10))[:15] + b'\xff', swap=True))
    input_fetches = gpugen.fetch_inputs(12)
    exports = [[gpugen.export_copy(5)], [gpugen.export_copy(6)]]

    def test(name, constant, source, swizzle, denormalized, texture='zero',
             dimension=1, dst_swizzle='xyzw', **fetch_keywords):
        fetches = [
            x.tfetch(5, source, swizzle, SLOT, dimension,
                     dst_swizzle=dst_swizzle, denormalized=denormalized,
                     opcode=x.FETCH_BORDER_COLOR_FRAC,
                     **fetch_keywords).serialized(),
            x.tfetch(6, source, swizzle, SLOT, dimension,
                     denormalized=denormalized, **fetch_keywords).serialized()]
        suite.test(name, gpugen.memexport_shader(
            12, [], exports, fetches=input_fetches + fetches),
            elements=2, fetch_constants={SLOT: (constant, texture)})

    def linear(name, constant, **keywords):
        test(name, constant, 1, 'zw', True, **keywords)

    for mode in range(8):
        test(f'point clamp {mode}',
             fetch_constant(clamp=(mode, EDGE, EDGE), filters=POINT), 1, 'xy',
             False)
    for mode in range(8):
        linear(f'linear clamp {mode}', fetch_constant(clamp=(mode, EDGE, EDGE)))
    # y = 0.5 in a 1-texel column: the second y tap is the border with weight
    # 0.
    linear('linear clamp 6 y border', fetch_constant(clamp=(BORDER,) * 3))
    for color in (0, 2, 3):
        linear(f'linear border color {color}',
               fetch_constant(border_color=color))
    linear('linear data', fetch_constant(), texture='four8888')
    linear('linear dst swizzle wzyx', fetch_constant(), dst_swizzle='wzyx')
    linear('linear instruction filter', fetch_constant(filters=POINT),
           mag_filter=1, min_filter=1)
    for name, fmt, sign, swizzle in (
            ('8', 2, 0, gpugen.SWIZZLE_XYZW),
            ('8_8_8_8 signed', 6, 1, gpugen.SWIZZLE_XYZW),
            ('8_8_8_8 gamma', 6, 3, gpugen.SWIZZLE_XYZW),
            ('8_8_8_8 swizzle wzyx', 6, 0, SWIZZLE_WZYX),
            ('8_8_8_8 swizzle 0001', 6, 0, SWIZZLE_0001),
            ('2_10_10_10', 7, 0, gpugen.SWIZZLE_XYZW),
            ('DXT1', 18, 0, gpugen.SWIZZLE_XYZW),
            ('16_16_FLOAT', 31, 0, gpugen.SWIZZLE_XYZW),
            ('32_FLOAT', 36, 0, gpugen.SWIZZLE_XYZW),
            ('32_32_32_32_FLOAT', 38, 0, gpugen.SWIZZLE_XYZW)):
        linear(f'linear format {name}',
               fetch_constant(fmt, sign=sign, swizzle=swizzle))
    for exp_adjust in (1, -1):
        linear(f'linear exp_adjust {exp_adjust}',
               fetch_constant(exp_adjust=exp_adjust))
    test('linear 1D', fetch_constant(), 1, 'z', True, dimension=0)
    for name, clamp in (('border', (BORDER, BORDER, EDGE)),
                        ('border x', (BORDER, EDGE, EDGE)),
                        ('halfway', (4, 4, EDGE)),
                        ('mirror border', (7, 7, EDGE))):
        test(f'bilinear corner {name}', fetch_constant(height=4, clamp=clamp),
             2, 'xy', True)
    for name, volume_filter in (('linear', 1), ('point', 0)):
        test(f'3D z border {name}',
             fetch_constant_3d((EDGE, EDGE, BORDER), volume_filter), 3, 'xyz',
             False, dimension=2)
