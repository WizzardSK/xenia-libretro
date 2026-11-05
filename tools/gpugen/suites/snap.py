"""Vertex snapping, with the pixel suite's shaders and output layout (64x64
window, per pixel the interpolated attribute (X, Y, X + Y, 1) of the position
and PsParamGen, sentinel = not covered).

Row k (pixel centers y = k) holds two quads 1 pixel tall. Quad A spans x from
-0.5 to 8 + d_k, so pixel 8 is drawn iff the snapped right edge passes its
center; quad B spans 40 + d_k to 47.5, so pixel 40 is drawn iff the snapped
left edge reaches its center (the top-left rule includes it). Coarse d_k =
(k - 32) / 256, fine d_k = (k - 32) / 2048. Variants change PA_SU_VTX_CNTL's
round_mode and quant_mode from Direct3D's (2, 0).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pixel  # noqa: E402
import pm4  # noqa: E402


def quad(x0, y0, x1, y1):
    return pixel.positional([(x0, y0), (x1, y0), (x0, y1),
                             (x1, y0), (x1, y1), (x0, y1)])


def rows(step):
    vertices = []
    for k in range(64):
        d = (k - 32) / step
        vertices += quad(-0.5, k - 0.5, 8 + d, k + 0.5)
        vertices += quad(40 + d, k - 0.5, 47.5, k + 0.5)
    return vertices


BLOCKS = [('snap coarse', pm4.PRIM_TRIANGLE_LIST, rows(256)),
          ('snap fine', pm4.PRIM_TRIANGLE_LIST, rows(2048))]


def build(suite):
    draws = pixel.set_blocks(suite, BLOCKS)
    probe = pixel.Probe()
    for block in ('snap coarse', 'snap fine'):
        probe.test(suite, block, draws[block])
    # quant_mode 5 and 6 are past the 1/16 to 1 pixel modes; 2 and 3 (1/4,
    # 1/2) would snap nearly every row's edge alike.
    for round_mode in range(4):
        for quant_mode in (0, 1, 4, 5, 6):
            if (round_mode, quant_mode) == (2, 0):
                continue
            probe.test(
                suite, f'snap coarse round {round_mode} quant {quant_mode}',
                draws['snap coarse'],
                [('PA_SU_VTX_CNTL', [(round_mode << 1) | (quant_mode << 3)])])
