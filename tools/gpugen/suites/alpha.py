"""Alpha precision in the render backend, with the msaarb suite's setup.

- alpha to mask at 4x with offsets 0x00 and 0xAA (thresholds k/8): each 2x2
  quad q (512) has alpha k/8 + d, k = q // 64 + 1, d = (q % 64 - 32) / 4096
  ('coarse') or (q % 64 - 32) * 2^-20 ('fine'); every sample resolved raw.
- alpha test (RB_COLORCONTROL alpha_test_enable, every function) against
  RB_ALPHA_REF 0.5 at 1x: pixel i has alpha 0.5 + (i % 64 - 32) * 2^-(10 +
  i // 64 % 16), the last 64 pixels specials; resolved raw.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import msaarb  # noqa: E402
import rb  # noqa: E402

ALPHA_TEST = 1 << 3
SPECIALS = [0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000, 0x00000000,
            0x80000000, 0x3F000000, 0x3F000001, 0x3EFFFFFF, 0x3F800000,
            0xBF000000, 0x00000001]
f2w = gpugen.f2w


def a2m_texture(step):
    words = []
    for y in range(rb.HEIGHT):
        for x in range(rb.WIDTH):
            q = x // 2 + 32 * (y // 2)
            alpha = (q // 64 + 1) / 8 + (q % 64 - 32) * step
            words += [f2w(1.0)] * 3 + [f2w(alpha)]
    return words


def alpha_test_texture():
    words = []
    for i in range(rb.WIDTH * rb.HEIGHT):
        if i >= rb.WIDTH * rb.HEIGHT - 64:
            alpha = SPECIALS[i % len(SPECIALS)]
        else:
            alpha = f2w(0.5 + (i % 64 - 32) * 2.0 ** -(10 + i // 64 % 16))
        words += [f2w(1.0)] * 3 + [alpha]
    return words


def build(suite):
    suite.set_inputs(rb.RECT, 2)
    suite.add_texture('zero', rb.fill((0.0, 0.0, 0.0, 0.0)))
    suite.add_texture('coarse', a2m_texture(1 / 4096))
    suite.add_texture('fine', a2m_texture(2 ** -20))
    suite.add_texture('alpha test', alpha_test_texture())

    def test(name, samples, texture, colorcontrol, sample=0):
        state = rb.registers(samples=samples, mode_cntl=gpugen.MODE_CNTL)
        state.append(('RB_ALPHA_REF', [f2w(0.5)]))
        rb.test(suite, name, texture, state, msaarb.raw(sample),
                passes=[([('RB_COLORCONTROL', [msaarb.COLORCONTROL])],
                         rb.fetch('zero'))],
                after_passes=[('RB_COLORCONTROL', [colorcontrol])])

    for texture in ('coarse', 'fine'):
        for offsets in (0x00, 0xAA):
            for k in range(4):
                test(f'alpha to mask {texture} {offsets:#04x} sample {k}', 2,
                     texture, msaarb.COLORCONTROL | msaarb.ALPHA_TO_MASK |
                     (offsets << 24), k)
    for function in range(8):
        test(f'alpha test {function}', 0, 'alpha test',
             function | ALPHA_TEST)
