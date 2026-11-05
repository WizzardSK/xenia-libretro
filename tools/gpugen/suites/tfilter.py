"""Texture filtering and addressing from vertex shaders.

Per vertex v (1024): r1 = (0.5 + v/1024, 0.5, u_edge, 0.5), r2 = (u_wrap, 0.5).
- bilinear *: two-texel textures (0 and full scale) sampled linearly at
  unnormalized x = 0.5 + v/1024, so the result is the filter weight.
- point/linear edge: 256 texels of k_8, bytes 0-255 read 8in32 so texel n
  holds n ^ 3, at normalized u_edge = (v // 4) / 256 + (-2^-20, -2^-24, 0,
  2^-24)[v % 4].
- clamp m / border c: 4 texels with distinct values at normalized
  u_wrap = -2 + 5v/1024, every ClampMode and every BorderColor.
The texture fetch is serialized after the vertex fetches of its coordinates;
'point edge unserialized' leaves the bit off: on a console the fetch then
reads another vertex's coordinates.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

SLOT = 1
VERTICES = 1024
EDGE_OFFSETS = (-2.0 ** -20, -2.0 ** -24, 0.0, 2.0 ** -24)
f2w = gpugen.f2w
row = gpugen.texture_words


def fetch_constant(fmt, width, filters, **keywords):
    return gpugen.texture_constant(fmt, width, filters=filters, **keywords)


def build(suite):
    words = []
    for v in range(VERTICES):
        edge = (v // 4) / 256 + EDGE_OFFSETS[v % 4]
        words += [f2w(0.5 + v / 1024), f2w(0.5), f2w(edge), f2w(0.5),
                  f2w(-2.0 + 5.0 * v / 1024), f2w(0.5), 0, 0]
    suite.set_inputs(words, 8)
    suite.add_texture('ramp8888', row(bytes([0x00, 0xFF, 0x80, 0x40,
                                             0xFF, 0x00, 0x80, 0xC0])))
    suite.add_texture('ramp1616', row(bytes([0x00, 0x00, 0xFF, 0xFF,
                                             0xFF, 0xFF, 0x00, 0x00])))
    suite.add_texture('ramp32f', row(struct.pack('>ff', 0.0, 1.0)))
    suite.add_texture('index8', row(bytes(range(256))))
    suite.add_texture('four8888', row(bytes([0x10, 0x20, 0x30, 0x40,
                                             0x50, 0x60, 0x70, 0x80,
                                             0x90, 0xA0, 0xB0, 0xC0,
                                             0xD0, 0xE0, 0xF0, 0xFF])))
    move = gpugen.export_copy(3)
    fetch_inputs = gpugen.fetch_inputs(8)

    def test(name, texture, constant, source, swizzle, denormalized,
             serialize=True):
        fetch = x.tfetch(3, source, swizzle, SLOT, denormalized=denormalized)
        if serialize:
            fetch = fetch.serialized()
        suite.test(name, gpugen.memexport_shader(
            8, [], [[move]], fetches=fetch_inputs + [fetch]),
            fetch_constants={SLOT: (constant, texture)})

    linear, point = (1, 1, 0), (0, 0, 0)
    for name, texture, fmt in (('bilinear 8888', 'ramp8888', 6),
                               ('bilinear 1616', 'ramp1616', 25),
                               ('bilinear 32f', 'ramp32f', 36)):
        test(name, texture, fetch_constant(fmt, 2, linear), 1, 'xy', True)
    test('point edge', 'index8', fetch_constant(2, 256, point), 1, 'zw', False)
    test('linear edge', 'index8', fetch_constant(2, 256, linear), 1, 'zw',
         False)
    test('point edge unserialized', 'index8', fetch_constant(2, 256, point), 1,
         'zw', False, serialize=False)
    for mode in range(8):
        test(f'clamp {mode}', 'four8888',
             fetch_constant(6, 4, point, clamp=(mode, mode, mode)), 2, 'xy',
             False)
    for color in range(4):
        test(f'border {color}', 'four8888',
             fetch_constant(6, 4, point, clamp=(6, 6, 6), border_color=color),
             2, 'xy', False)
