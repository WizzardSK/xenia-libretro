"""MSAA in the render backend: a 64x32 k_8_8_8_8 render target at 1x, 2x or 4x
(Direct3D's PA_SC_AA_CONFIG), a first draw over all of it, a second over part
of the samples, then a Resolve of one sample (raw) or of an average
(converted). The pixel shader is the rb suite's (oC0 = the texture's texel at
the pixel).

- sample k at 2x and 4x: the first draw writes 'zero', the second the msaa
  suite's probe triangles ((x0, y0), (x0 + 6/16, y0), (x0, y0 + 6/16) from the
  corner of sub-pixel cell (cx - 1, cy - 1), cx = (x + x // 16) % 16, cy
  likewise) with 'one': which sample positions EDRAM sample k holds.
- average: 'a' everywhere, then the probes with 'b' (random k/255 values),
  resolved averaging samples 01, 23 or 0123.
- alpha to mask at 1x, 2x and 4x: 'zero', then 'alpha ramp' over everything
  with RB_COLORCONTROL.alpha_to_mask_enable and offsets 0x00 (0,0,0,0), 0xAA
  (2,2,2,2) or Direct3D's 0x87 (3,1,0,2); alpha = (q + 0.5) / 512 for quad
  q = x // 2 + 32 (y // 2), the same in all four pixels of a quad. Each
  sample resolved raw.
- gamma average: the full averages (2x 01, 4x 0123) on a k_8_8_8_8_GAMMA
  render target.
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import msaa  # noqa: E402
import pm4  # noqa: E402
import rb  # noqa: E402

# RB_COLORCONTROL: alpha_func ALWAYS.
COLORCONTROL = 0x7
ALPHA_TO_MASK = 1 << 4
SELECT_01, SELECT_23, SELECT_0123 = 4, 5, 6
f2w = gpugen.f2w


def probes():
    return [c for y in range(rb.HEIGHT) for x in range(rb.WIDTH)
            for p in msaa.triangle(x, y) for c in p]


def random_texture(rng):
    return [f2w(rng.randrange(256) / 255)
            for _ in range(4 * rb.WIDTH * rb.HEIGHT)]


def alpha_ramp():
    words = []
    for y in range(rb.HEIGHT):
        for x in range(rb.WIDTH):
            q = x // 2 + 32 * (y // 2)
            words += [f2w(v) for v in (1.0, 1.0, 1.0, (q + 0.5) / 512)]
    return words


def raw(sample):
    """A Resolve of EDRAM sample `sample`, raw."""
    return gpugen.resolve_registers(rb.WIDTH, rb.HEIGHT, 6, 4, command=0,
                                    sample_select=sample)


def build(suite):
    rng = random.Random(0x4A4)
    suite.set_inputs(rb.RECT + [f2w(v) for v in probes()], 2)
    suite.add_texture('zero', rb.fill((0.0, 0.0, 0.0, 0.0)))
    suite.add_texture('one', rb.fill((1.0, 1.0, 1.0, 1.0)))
    suite.add_texture('a', random_texture(rng))
    suite.add_texture('b', random_texture(rng))
    suite.add_texture('alpha ramp', alpha_ramp())
    probe_draw = (pm4.PRIM_TRIANGLE_LIST, 3, 3 * rb.WIDTH * rb.HEIGHT)

    def test(name, samples, first, second, draw, resolve, rt_format=0,
             colorcontrol=COLORCONTROL):
        rb.test(suite, name, second,
                rb.registers(rt_format, samples, mode_cntl=gpugen.MODE_CNTL),
                resolve, draw=draw,
                passes=[([('RB_COLORCONTROL', [COLORCONTROL])],
                         rb.fetch(first), rb.RECT_DRAW)],
                after_passes=[('RB_COLORCONTROL', [colorcontrol])])

    def average(select):
        return gpugen.resolve_registers(rb.WIDTH, rb.HEIGHT, 6, 4,
                                        sample_select=select)

    for samples, count in ((1, 2), (2, 4)):
        for k in range(count):
            test(f'sample {k} {count}x', samples, 'zero', 'one', probe_draw,
                 raw(k))
    for rt_name, rt_format in (('', 0), ('gamma ', 1)):
        for samples, select, name in ((1, SELECT_01, '2x 01'),
                                      (2, SELECT_0123, '4x 0123'),
                                      (2, SELECT_01, '4x 01'),
                                      (2, SELECT_23, '4x 23')):
            full = SELECT_01 if samples == 1 else SELECT_0123
            if rt_name and select != full:
                continue
            test(f'{rt_name}average {name}', samples, 'a', 'b', probe_draw,
                 average(select), rt_format)
    for offsets in (0x00, 0xAA, 0x87):
        for samples, count in ((0, 1), (1, 2), (2, 4)):
            for k in range(count):
                test(f'alpha to mask {offsets:#04x} {count}x sample {k}',
                     samples, 'zero', 'alpha ramp', rb.RECT_DRAW, raw(k),
                     colorcontrol=(COLORCONTROL | ALPHA_TO_MASK |
                                   (offsets << 24)))
