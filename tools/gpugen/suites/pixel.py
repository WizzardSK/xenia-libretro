"""Pixel shaders exporting what they receive, one element pair per pixel of a
64x64 window (screen-space positions, PA_CL_VTE_CNTL 0x300, pixel centers on
integers): element 2p = the interpolated attribute, 2p + 1 = PsParamGen, for
pixel p = x + 64y from floor(|PsParamGen.xy|), or p = the attribute's X in
the tests 'by attribute'. Pixels not drawn keep the sentinel, so the outputs
are a coverage map too. Interpolation is smooth
(SQ_INTERPOLATOR_CNTL.param_shade 1) unless a test says flat.

Vertices: position (r1), attribute (r2 -> interpolator 0).
- 256 one-pixel points, point i at pixel (i % 16, i // 16), attribute
  (i, x, y, 7).
- 16 sprites of 4x4 pixels at (4a + 1.5, 4b + 1.5), attribute (a, b, 0, 7).
- Two triangles over pixels 0-15 square, both windings: the quad (-0.5, -0.5)
  to (15.5, 15.5), the diagonal through the pixel centers x + y = 15,
  attribute (X, Y, X + Y, 1) from the position, so a pixel's attribute is its
  center.
- One triangle (-0.5, -0.5), (23.5, 3.5), (2.5, 19.5) with attribute
  (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0): barycentrics, to compare with
  PsParamGen.zw.
- A line from (-0.5, 7.25) to (15.5, 8.75), attribute as the triangles'.
- A rectangle (-0.5, -0.5), (15.5, -0.5), (-0.5, 15.5), attribute as the
  triangles'.
- Shapes: 64 triangles, one per 8x8 cell of the window, the first 64 of
  SHAPES in every rotation and winding, attribute one-hot by the vertex's
  place in the draw and W = the cell, to find which vertices PsParamGen.zw
  weigh.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import pm4  # noqa: E402
import xenos as x  # noqa: E402

WINDOW = 64
K = gpugen.C_FIRST_FREE  # pixel c8: (WINDOW, 0, 0, 0)
PARAM_GEN = 1  # PS register PsParamGen goes to, after interpolator 0
f2w = gpugen.f2w


def vertex(position, attribute):
    return [f2w(v) for v in (position[0], position[1], 0.0, 1.0)] + \
        [f2w(v) for v in attribute]


def positional(points):
    return [vertex(p, (p[0], p[1], p[0] + p[1], 1.0)) for p in points]


# In an 8x8 cell, pixel centers 0-7, corners off the centers.
SHAPES = [
    ((0.25, 0.25), (7.25, 0.25), (0.25, 7.25)),
    ((7.25, 0.25), (7.25, 7.25), (0.25, 7.25)),
    ((0.25, 0.25), (7.25, 3.25), (2.25, 7.25)),
    ((3.75, 0.25), (7.25, 7.25), (0.25, 7.25)),  # two longest edges equal
    ((0.25, 0.25), (7.25, 0.25), (3.75, 7.25)),  # two longest edges equal
    ((0.25, 3.75), (7.25, 0.25), (7.25, 7.25)),
    ((0.25, 0.25), (7.25, 1.25), (1.25, 7.25)),
    ((0.25, 0.25), (7.25, 7.25), (0.25, 4.25)),
    ((0.25, 1.25), (7.25, 0.25), (4.25, 7.25)),
    ((0.25, 6.25), (7.25, 0.25), (6.25, 7.25)),
    ((-0.25, -0.25), (7.25, 7.25), (7.25, -0.25)),
]


def shapes():
    orders = []
    for shape in SHAPES:
        for reverse in (False, True):
            ordered = shape[::-1] if reverse else shape
            for r in range(3):
                orders.append(ordered[r:] + ordered[:r])
    vertices = []
    for cell, corners in enumerate(orders[:64]):
        ox, oy = 8 * (cell % 8), 8 * (cell // 8)
        for k, (px, py) in enumerate(corners):
            attribute = [0.0, 0.0, 0.0, float(cell)]
            attribute[k] = 1.0
            vertices.append(vertex((ox + px, oy + py), attribute))
    return vertices


QUAD = [(-0.5, -0.5), (15.5, -0.5), (-0.5, 15.5),
        (15.5, -0.5), (15.5, 15.5), (-0.5, 15.5)]
TRIANGLE = ((-0.5, -0.5), (23.5, 3.5), (2.5, 19.5))
BLOCKS = [
    ('points', pm4.PRIM_POINT_LIST,
     [vertex((i % 16, i // 16), (i, i % 16, i // 16, 7.0))
      for i in range(256)]),
    ('sprites', pm4.PRIM_POINT_LIST,
     [vertex((4 * a + 1.5, 4 * b + 1.5), (a, b, 0.0, 7.0))
      for b in range(4) for a in range(4)]),
    ('triangles', pm4.PRIM_TRIANGLE_LIST, positional(QUAD)),
    ('triangles reversed', pm4.PRIM_TRIANGLE_LIST,
     positional(QUAD[2::-1] + QUAD[:2:-1])),
    ('triangle', pm4.PRIM_TRIANGLE_LIST,
     [vertex(p, a) for p, a in
      zip(TRIANGLE, ((1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0)))]),
    ('line', pm4.PRIM_LINE_LIST, positional([(-0.5, 7.25), (15.5, 8.75)])),
    ('rectangle', pm4.PRIM_RECTANGLE_LIST, positional(QUAD[:3])),
    ('shapes', pm4.PRIM_TRIANGLE_LIST, shapes()),
]

# 8 VS and 4 PS registers, vertex fetch, PsParamGen, one interpolator,
# Direct3D's PS export mode.
PROGRAM_CNTL = 7 | (3 << 8) | (1 << 16) | (1 << 18) | (2 << 27)
SMOOTH = 0xFFFF
# PsParamGen's register, and sc_output_screen_xy: without it PsParamGen.xy is
# not the pixel position on the console.
REGISTERS = [('SQ_CONTEXT_MISC', [(PARAM_GEN << 8) | gpugen.SCREEN_XY]),
             ('SQ_INTERPOLATOR_CNTL', [SMOOTH]),
             ('PA_SC_WINDOW_SCISSOR_BR', [(WINDOW << 16) | WINDOW]),
             ('RB_COLOR_MASK', [0xF])]
NO_SCREEN_XY = [('SQ_CONTEXT_MISC', [PARAM_GEN << 8])]
PROVOKING_LAST = 1 << 19  # PA_SU_SC_MODE_CNTL.provoking_vtx_last


def copy(register):
    return [gpugen.export_copy(register)]


class Probe:
    """The shaders exporting interpolator 0 and PsParamGen per pixel."""

    def __init__(self):
        self.shader = gpugen.interpolating_vertex_shader(8)
        exports = [copy(0), copy(PARAM_GEN)]
        # r2.x = floor(|PsParamGen.y|) * WINDOW + floor(|PsParamGen.x|).
        by_position = [
            x.alu(vector_op=x.V_FLOOR, vector_dest=2, vector_mask=0x3,
                  src1=x.r(PARAM_GEN, absolute=True),
                  src2=x.r(PARAM_GEN, absolute=True)),
            x.alu(vector_op=x.V_MAD, vector_dest=2, vector_mask=0x1,
                  src1=x.r(2, 'y'), src2=x.c(K, 'x'), src3=x.r(2, 'x'))]
        self.by_pixel = gpugen.memexport_pixel_shader(by_position, exports,
                                                      x.r(2, 'x'))
        # Indexed by the attribute's X instead, independent of PsParamGen.
        self.by_attribute = gpugen.memexport_pixel_shader([], exports,
                                                          x.r(0, 'x'))
        self.constants = {256 + K: [f2w(float(WINDOW)), 0, 0, 0]}

    def test(self, suite, name, draw, registers=(), pixel_shader=None,
             constants=None):
        suite.test(name, self.shader, constants=constants or self.constants,
                   elements=2, pixel_shader=pixel_shader or self.by_pixel,
                   program_cntl=PROGRAM_CNTL,
                   registers=REGISTERS + list(registers), draw=draw)


def set_blocks(suite, blocks):
    """The suite's inputs from (name, primitive, vertices) blocks, and slots
    for the window; returns each block's draw by name."""
    words, draws = [], {}
    for name, primitive, vertices in blocks:
        draws[name] = (primitive, len(words) // 8, len(vertices))
        words += [w for v in vertices for w in v]
    suite.set_inputs(words, 8)
    suite.export_slots = WINDOW * WINDOW
    return draws


def build(suite):
    draws = set_blocks(suite, BLOCKS)
    probe = Probe()

    def test(name, block, registers=(), pixel_shader=None):
        probe.test(suite, name, draws[block], registers, pixel_shader)

    test('points by attribute', 'points', pixel_shader=probe.by_attribute)
    test('points by attribute no screen xy', 'points', NO_SCREEN_XY,
         probe.by_attribute)
    test('points', 'points')
    test('sprites', 'sprites', [('PA_SU_POINT_SIZE', [0x00200020])])
    for block in ('triangles', 'triangles reversed', 'triangle', 'line',
                  'rectangle', 'shapes'):
        test(block, block)
    test('triangles flat', 'triangles', [('SQ_INTERPOLATOR_CNTL', [0])])
    test('triangles flat last', 'triangles',
         [('SQ_INTERPOLATOR_CNTL', [0]),
          ('PA_SU_SC_MODE_CNTL', [gpugen.MODE_CNTL | PROVOKING_LAST])])
