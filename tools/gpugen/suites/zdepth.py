"""Depth from interpolated Z (no oDepth) into D24S8 / D24FS8, resolved raw like
the depth suite, with polygon offset and out-of-range Z.

Vertices are 8 dwords (position, attribute); vertex 0's first 6 dwords are the
Resolve's rectangle. The geometry covers the 64x32 render target with two
triangles, Z = Z0 + A x + B y at screen (x, y):
- 'plane': Z0 0.1234567, A 0.0123456, B 0.00071234, clockwise (back facing)
  and counterclockwise (front facing).
- 'steep': Z0 0.25, A 0.01, B 0.0001 (slope-scaled offsets).
- 'out of range': Z0 -0.5, A 2 / 64, B 0 (Z -0.5 to 1.47 at pixel centers).
The pixel shader is the rb suite's, over a zero texture. PsParamGen goes to
r0, where the vertex shader's interpolator would, so that one goes unused.
Polygon offset: PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET} with the
PA_SU_SC_MODE_CNTL enables.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import depth  # noqa: E402
import gpugen  # noqa: E402
import pm4  # noqa: E402
import rb  # noqa: E402

OFFSET_FRONT, OFFSET_BACK = 1 << 11, 1 << 12
PLANES = {'plane': (0.1234567, 0.0123456, 0.00071234),
          'steep': (0.25, 0.01, 0.0001),
          'out of range': (-0.5, 2 / 64, 0.0)}
# The quads drawn: (plane, clockwise).
QUADS = [('plane', True), ('plane', False), ('steep', False),
         ('out of range', False)]
# 8 VS and 4 PS registers, vertex and pixel fetches, PsParamGen, one
# interpolator, Direct3D's PS export mode.
PROGRAM_CNTL = 7 | (3 << 8) | (1 << 16) | (1 << 17) | (1 << 18) | (2 << 27)
f2w = gpugen.f2w


def quad(plane, clockwise):
    z0, a, b = plane
    x0, y0, x1, y1 = -0.5, -0.5, rb.WIDTH - 0.5, rb.HEIGHT - 0.5
    corners = [(x0, y0), (x1, y0), (x0, y1), (x1, y0), (x1, y1), (x0, y1)]
    if not clockwise:
        corners = corners[2::-1] + corners[:2:-1]
    return [[f2w(v) for v in (x, y, z0 + a * x + b * y, 1.0, 0, 0, 0, 1.0)]
            for x, y in corners]


def build(suite):
    words, draws = rb.RECT + [0, 0], {}
    for key in QUADS:
        name, clockwise = key
        draws[key] = (pm4.PRIM_TRIANGLE_LIST, len(words) // 8, 6)
        words += [w for v in quad(PLANES[name], clockwise) for w in v]
    suite.set_inputs(words, 8)
    suite.add_texture('zero', rb.fill((0.0, 0.0, 0.0, 0.0)))
    vertex = gpugen.interpolating_vertex_shader(8)

    def test(name, fmt, key, mode=0, offsets=(0.0, 0.0, 0.0, 0.0)):
        state = rb.registers(depth_format=fmt,
                             mode_cntl=gpugen.MODE_CNTL | mode)
        state.append(('PA_SU_POLY_OFFSET_FRONT_SCALE',
                      [f2w(v) for v in offsets]))
        rb.test(suite, name, 'zero', state, depth.RESOLVE, draw=draws[key],
                shader=vertex, program_cntl=PROGRAM_CNTL)

    for fname, fmt in (('D24S8', depth.D24S8), ('D24FS8', depth.D24FS8)):
        test(f'{fname} plane cw', fmt, ('plane', True))
        test(f'{fname} plane ccw', fmt, ('plane', False))
        test(f'{fname} out of range', fmt, ('out of range', False))
        test(f'{fname} steep', fmt, ('steep', False))
        for offset in (1.0, 16.0, -1.0, 0.5, 1048576.0):
            test(f'{fname} front offset {offset:g}', fmt, ('plane', False),
                 OFFSET_FRONT, (0.0, offset, 0.0, 0.0))
        for scale in (1.0, 16.0):
            test(f'{fname} front scale {scale:g}', fmt, ('steep', False),
                 OFFSET_FRONT, (scale, 0.0, 0.0, 0.0))
        test(f'{fname} back offset 16', fmt, ('plane', True), OFFSET_BACK,
             (0.0, 0.0, 0.0, 16.0))
        test(f'{fname} front offset 16 on back face', fmt, ('plane', True),
             OFFSET_FRONT, (0.0, 16.0, 0.0, 0.0))
        # Where the offset stops having an effect, and which faces take which
        # offset.
        for offset in (2.0, 4.0, 7.99, 8.0, 8.01, 15.99, 32.0, -2.0, -8.0,
                       -16.0, 2.0 ** -24, 2.0 ** -20, 1 / 16, 0.75):
            test(f'{fname} front offset {offset:g}', fmt, ('plane', False),
                 OFFSET_FRONT, (0.0, offset, 0.0, 0.0))
        test(f'{fname} back offset 0.25', fmt, ('plane', True), OFFSET_BACK,
             (0.0, 0.0, 0.0, 0.25))
        test(f'{fname} front offset 0.25 on back face', fmt, ('plane', True),
             OFFSET_FRONT, (0.0, 0.25, 0.0, 0.0))
        test(f'{fname} back offset 0.25 on front face', fmt, ('plane', False),
             OFFSET_BACK, (0.0, 0.0, 0.0, 0.25))
        test(f'{fname} both offsets on back face', fmt, ('plane', True),
             OFFSET_FRONT | OFFSET_BACK, (0.0, 0.125, 0.0, 0.25))
