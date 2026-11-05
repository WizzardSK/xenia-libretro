"""Texture filtering, what the tfilter suite left open. Vertex v (1024)
fetches its inputs:
- r1.xy normalized (u, 0.5), u = -2 + 5 (v // 4) / 256 + (0, 2^-12, -2^-12,
  2^-9)[v % 4]: the 4-texel 'four8888' texture filtered linearly in every
  clamp mode, also at negative coordinates.
- r1.zw unnormalized (t + 0.5 + j / 64 + s / 4096, 0.5), t = (20, 200)[v //
  512], j = v % 64, s = (v // 64) % 8: the 8-bit conversion of filtered
  values, on 'equal8888' (texel n = bytes n, n, n, n), as is, with the fetch
  constant swizzle WZYX and with the tfetch destination swizzle wzyx: X gets
  a coarser filter fraction, and these show whether that follows the data or
  the destination component.
- r2.xyz: normalized u for point sampling at widths 3, 5 and 255 (n / W +
  (-2^-24, 0, 2^-24, 2^-20)[v % 4], n = v // 4 % W), r2.w = 0.5.
- r3.xy unnormalized (0.5 + a / 64, 0.5 + b / 64), a = v % 32, b = v // 32:
  2D bilinear on a 2x2 texture. r3.zw normalized (v // 4) / 256 + (-2^-20,
  -2^-24, 0, 2^-24)[v % 4] on 'equal8888' filtered linearly.
- r4.xy unnormalized (0.5 + (v % 64) / 64 + ((v // 64) % 16) / 4096, 0.5):
  two-texel ramps (texel 0 all zero bits, texel 1 the format's 1.0 or all
  ones) filtered linearly in each format - bytewise or per field. r4.zw
  normalized (-(v % 64) 2^-16, 0.5) for width 256 in border mode.
- Border color 2 (ACBYCR) on other formats, point sampled at r1.xy, the
  clamp tests' coordinates.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

SLOT = 1
VERTICES = 1024
POINT, LINEAR = (0, 0, 0), (1, 1, 0)
SWIZZLE_WZYX = 0x053
BORDER = 6
# (name, TextureFormat, texel 0, texel 1) as little-endian bytes, read 8in32.
RAMPS = [('8', 2, b'\x00', b'\xff'), ('8_8', 10, b'\x00' * 2, b'\xff' * 2),
         ('5_6_5', 4, b'\x00' * 2, b'\xff' * 2),
         ('1_5_5_5', 3, b'\x00' * 2, b'\xff' * 2),
         ('4_4_4_4', 15, b'\x00' * 2, b'\xff' * 2),
         ('2_10_10_10', 7, b'\x00' * 4, b'\xff' * 4),
         ('10_11_11', 16, b'\x00' * 4, b'\xff' * 4),
         ('16', 24, b'\x00' * 2, b'\xff' * 2),
         ('16_16', 25, b'\x00' * 4, b'\xff' * 4),
         ('16_16_16_16', 26, b'\x00' * 8, b'\xff' * 8),
         ('16_FLOAT', 30, b'\x00\x00', b'\x00\x3c'),
         ('16_16_FLOAT', 31, b'\x00' * 4, b'\x00\x3c' * 2),
         ('16_16_16_16_FLOAT', 32, b'\x00' * 8, b'\x00\x3c' * 4),
         ('32_32_FLOAT', 37, b'\x00' * 8, struct.pack('<ff', 1.0, 1.0)),
         ('32_32_32_32_FLOAT', 38, b'\x00' * 16,
          struct.pack('<4f', *[1.0] * 4))]
# Signed and gamma 8_8_8_8 ramps reuse the '8_8_8_8' texture.
SIGNED_RAMPS = [('8_8_8_8 signed', 1), ('8_8_8_8 gamma', 3)]


f2w = gpugen.f2w
fetch_constant = gpugen.texture_constant


def rows(*texel_rows):
    """Texture words from rows of little-endian texel bytes, read 8in32."""
    return gpugen.texture_words(*texel_rows, swap=True)


def inputs():
    words = []
    for v in range(VERTICES):
        u_clamp = (-2 + 5 * (v // 4) / 256 +
                   (0, 2 ** -12, -2 ** -12, 2 ** -9)[v % 4])
        t = (20, 200)[v // 512] + 0.5 + (v % 64) / 64 + ((v // 64) % 8) / 4096
        n = v // 4
        edge = (-2 ** -24, 0, 2 ** -24, 2 ** -20)[v % 4]
        u3, u5, u255 = ((n % 3) / 3 + edge, (n % 5) / 5 + edge,
                        (n % 255) / 255 + edge)
        u256 = -(v % 64) * 2 ** -16
        bx, by = 0.5 + (v % 32) / 64, 0.5 + (v // 32) / 64
        u_edge = (v // 4) / 256 + (-2 ** -20, -2 ** -24, 0, 2 ** -24)[v % 4]
        ramp = 0.5 + (v % 64) / 64 + ((v // 64) % 16) / 4096
        words += [f2w(c) for c in (u_clamp, 0.5, t, 0.5, u3, u5, u255, 0.5,
                                   bx, by, u_edge, 0.5, ramp, 0.5, u256, 0.5)]
    return words


def build(suite):
    suite.set_inputs(inputs(), 16)
    suite.add_texture('four8888',
                      rows(bytes(range(0x10, 0x100, 0x10))[:15] + b'\xff'))
    suite.add_texture('equal8888',
                      rows(bytes(n for n in range(256) for _ in range(4))))
    for width in (3, 5, 255):
        suite.add_texture(f'index{width}', rows(bytes(range(width))))
    suite.add_texture('index256', rows(bytes(range(256))))
    suite.add_texture('2x2', rows(
        bytes([0x00, 0x40, 0x80, 0xFF, 0xFF, 0xC0, 0x80, 0x00]),
        bytes([0x10, 0xFF, 0x00, 0x80, 0xF0, 0x20, 0xFF, 0x40])))
    for name, _, t0, t1 in RAMPS:
        suite.add_texture(name, rows(t0 + t1))
    suite.add_texture('8_8_8_8', rows(b'\x00' * 4 + b'\xff' * 4))
    move = gpugen.export_copy(5)
    input_fetches = gpugen.fetch_inputs(16)

    def test(name, texture, constant, source, swizzle, denormalized,
             dst_swizzle='xyzw'):
        fetch = x.tfetch(5, source, swizzle, SLOT, denormalized=denormalized,
                         dst_swizzle=dst_swizzle).serialized()
        suite.test(name, gpugen.memexport_shader(
            16, [], [[move]], fetches=input_fetches + [fetch]),
            fetch_constants={SLOT: (constant, texture)})

    for mode in range(8):
        test(f'linear clamp {mode}', 'four8888',
             fetch_constant(6, 4, filters=LINEAR, clamp=mode), 1, 'xy', False)
    test('conversion', 'equal8888', fetch_constant(6, 256, filters=LINEAR),
         1, 'zw', True)
    test('conversion fetch swizzle wzyx', 'equal8888',
         fetch_constant(6, 256, filters=LINEAR, swizzle=SWIZZLE_WZYX),
         1, 'zw', True)
    test('conversion dst swizzle wzyx', 'equal8888',
         fetch_constant(6, 256, filters=LINEAR), 1, 'zw', True,
         dst_swizzle='wzyx')
    for width, component in ((3, 'x'), (5, 'y'), (255, 'z')):
        test(f'point width {width}', f'index{width}', fetch_constant(2, width),
             2, component + 'w', False)
    test('point border 256 below 0', 'index256',
         fetch_constant(2, 256, clamp=BORDER), 4, 'zw', False)
    # Rows 256 bytes apart, as rows() lays them out.
    test('bilinear 2x2', '2x2',
         fetch_constant(6, 2, 2, filters=LINEAR, pitch=64), 3, 'xy', True)
    test('linear edge 8888', 'equal8888',
         fetch_constant(6, 256, filters=LINEAR), 3, 'zw', False)
    for name, fmt, _, _ in RAMPS:
        test(f'ramp {name}', name, fetch_constant(fmt, 2, filters=LINEAR),
             4, 'xy', True)
    for name, sign in SIGNED_RAMPS:
        test(f'ramp {name}', '8_8_8_8',
             fetch_constant(6, 2, filters=LINEAR, sign=sign), 4, 'xy', True)
    for name, fmt, sign in (('16_16', 25, 0), ('32_FLOAT', 36, 0),
                            ('8_8_8_8 signed', 6, 1), ('8_8_8_8 gamma', 6, 3),
                            ('16_16_16_16_FLOAT', 32, 0)):
        test(f'border 2 {name}', '8_8_8_8',
             fetch_constant(fmt, 2, clamp=BORDER, sign=sign, border_color=2),
             1, 'xy', False)
