"""Multisample coverage, with the pixel suite's shaders over a 64x64 window,
exporting per probe triangle and pixel it covers samples of rather than per
PsParamGen pixel: triangle i = X + 64Y drawn into the pixel (dx, dy) from
(X, Y) exports to slot 9i + 3 (dy + 1) + dx + 1, so a triangle reaching
samples of two pixels records both.

Pixel (X, Y)'s triangle is (x0, y0), (x0 + 6/16, y0), (x0, y0 + 6/16) from
the corner x0 = X - 0.5 + (cx - 1) / 16, y0 likewise, of sub-pixel cell
(cx, cy) = ((X + X // 16) % 16, (Y + Y // 16) % 16): each 16x16 tile shifts
the cells by a pixel, so every 1/16 offset is probed at every pixel parity.
The attribute is (X + 64Y, x, y, 1) of the position; with centroid sampling a
triangle covering one sample interpolates it at that sample.

MSAA takes RB_SURFACE_INFO.msaa_samples, PA_SU_SC_MODE_CNTL.msaa_enable and
PA_SC_AA_CONFIG: MSAA_NUM_SAMPLES (samples - 1) and MAX_SAMPLE_DIST, 0x8001 at
2x and 0xC003 at 4x as Direct3D writes them. With MAX_SAMPLE_DIST 0 the
rasterizer drops triangles that cover a sample without reaching toward the
pixel's center; without PA_SC_AA_CONFIG only the center is sampled.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import pixel  # noqa: E402
import pm4  # noqa: E402
import xenos as x  # noqa: E402

CENTERS_ONLY = 1 << 2  # SQ_CONTEXT_MISC.sc_sample_cntl
# Slots per triangle: its pixel and the 8 around it.
NEIGHBOURHOOD = 9
K = pixel.K
f2w = gpugen.f2w
CONSTANTS = {256 + K: [f2w(v) for v in (pixel.WINDOW, 1 / 64, 0.5, 4.0)],
             257 + K: [f2w(9.0), f2w(-61.0), 0, 0]}
# (name, RB_SURFACE_INFO.msaa_samples, PA_SC_AA_CONFIG, centers only)
CONFIGS = [('1x', 0, 0, False), ('2x', 1, 0x8001, False),
           ('4x', 2, 0xC003, False), ('2x centers', 1, 0x8001, True),
           ('4x centers', 2, 0xC003, True),
           ('2x max sample dist 0', 1, 1, False),
           ('4x max sample dist 0', 2, 3, False),
           ('4x 3 samples', 2, 0xC002, False),
           ('2x without aa config', 1, 0, False),
           ('4x without aa config', 2, 0, False)]


def triangle(px, py):
    """Pixel (px, py)'s probe triangle."""
    cx, cy = (px + px // 16) % 16, (py + py // 16) % 16
    x0, y0 = px - 0.5 + (cx - 1) / 16, py - 0.5 + (cy - 1) / 16
    return [(x0, y0), (x0 + 6 / 16, y0), (x0, y0 + 6 / 16)]


def probes():
    vertices = []
    for py in range(pixel.WINDOW):
        for px in range(pixel.WINDOW):
            index = px + pixel.WINDOW * py
            vertices += [pixel.vertex(p, (index, p[0], p[1], 1.0))
                         for p in triangle(px, py)]
    return vertices


def slot_shader():
    """The pixel suite's exports to slot 9i + 3 (dy + 1) + dx + 1, computed as
    9i + d + 4 - 61 dy from d = the pixel's index - i (the attribute's X) and
    dy = floor(d / 64 + 0.5)."""
    param_gen = x.r(pixel.PARAM_GEN, absolute=True)
    body = [
        # r2.x = the pixel's index, as the pixel suite's.
        x.alu(vector_op=x.V_FLOOR, vector_dest=2, vector_mask=0x3,
              src1=param_gen, src2=param_gen),
        x.alu(vector_op=x.V_MAD, vector_dest=2, vector_mask=0x1,
              src1=x.r(2, 'y'), src2=x.c(K, 'x'), src3=x.r(2, 'x')),
        # r2.y = d, r2.z = dy.
        x.alu(vector_op=x.V_ADD, vector_dest=2, vector_mask=0x2,
              src1=x.r(2, 'x'), src2=x.r(0, 'x', negate=True)),
        x.alu(vector_op=x.V_MUL, vector_dest=2, vector_mask=0x4,
              src1=x.r(2, 'y'), src2=x.c(K, 'y')),
        x.alu(vector_op=x.V_ADD, vector_dest=2, vector_mask=0x4,
              src1=x.r(2, 'z'), src2=x.c(K, 'z')),
        x.alu(vector_op=x.V_FLOOR, vector_dest=2, vector_mask=0x4,
              src1=x.r(2, 'z'), src2=x.r(2, 'z')),
        # r2.w = the slot.
        x.alu(vector_op=x.V_MAD, vector_dest=2, vector_mask=0x8,
              src1=x.r(0, 'x'), src2=x.c(K + 1, 'x'), src3=x.r(2, 'y')),
        x.alu(vector_op=x.V_MAD, vector_dest=2, vector_mask=0x8,
              src1=x.r(2, 'z'), src2=x.c(K + 1, 'y'), src3=x.r(2, 'w')),
        x.alu(vector_op=x.V_ADD, vector_dest=2, vector_mask=0x8,
              src1=x.r(2, 'w'), src2=x.c(K, 'w'))]
    return gpugen.memexport_pixel_shader(
        body, [pixel.copy(0), pixel.copy(pixel.PARAM_GEN)], x.r(2, 'w'))


def build(suite):
    draws = pixel.set_blocks(suite, [('probes', pm4.PRIM_TRIANGLE_LIST,
                                      probes())])
    suite.export_slots *= NEIGHBOURHOOD
    probe = pixel.Probe()
    shader = slot_shader()
    misc = (pixel.PARAM_GEN << 8) | gpugen.SCREEN_XY
    for name, samples, aa_config, centers in CONFIGS:
        probe.test(suite, name, draws['probes'], [
            ('RB_SURFACE_INFO', [1280 | (samples << 16)]),
            ('PA_SU_SC_MODE_CNTL',
             [gpugen.MODE_CNTL | (gpugen.MSAA_ENABLE if samples else 0)]),
            ('PA_SC_AA_CONFIG', [aa_config]),
            ('SQ_CONTEXT_MISC', [misc | (CENTERS_ONLY if centers else 0)])],
            shader, CONSTANTS)
