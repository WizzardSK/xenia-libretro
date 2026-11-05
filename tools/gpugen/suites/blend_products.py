"""Blend products on fixed point render targets: one product at a time, with
each operand either swept finely or held, to separate how the value is
quantized, how each kind of factor is widened and how products are rounded and
summed.

Like the blend suite, a first draw writes the 'dst' texture unblended and a
second the 'src' texture with RB_BLENDCONTROL0 set, then the 64x32 render
target is resolved to its own format (the bits unchanged). Texel i = x + 64y.
Sweeps use exact binary values, i / 2048, rather than k / 255.

- table: (DST_COLOR, ZERO), every 8-bit source k / 255 against every
  destination byte, 32 destinations per test.
- fine: (DST_COLOR, ZERO), the source ramp against a destination byte held per
  channel.
- swapped: (ZERO, SRC_COLOR) over the same data, the destination as the value
  and the source as the factor.
- float factor / float value: (SRC_ALPHA, ZERO), the alpha or the color swept
  with the other held; the destination plays no part.
- constant: (CONSTANT_COLOR, ZERO) against the source ramp.
- sum: (SRC_ALPHA, INV_SRC_ALPHA) with the alpha swept.
- 10-bit: fine on 2_10_10_10.
- min / max: MIN and MAX with factors other than ONE, whether they apply.
- constant alpha: (CONSTANT_ALPHA, ZERO) and (ZERO, INV_CONSTANT_ALPHA).
- saturate alpha: SRC_ALPHA_SATURATE as the alpha's source factor.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import blend as b  # noqa: E402
import gpugen  # noqa: E402
import rb  # noqa: E402

SRC_COLOR = 4
CONSTANT_ALPHA, INV_CONSTANT_ALPHA = 14, 15
PIXELS = rb.WIDTH * rb.HEIGHT
f2w = gpugen.f2w

# Destination bytes the fine and swapped tests hold, four per test.
FINE_BYTES = [0, 1, 2, 3, 63, 64, 85, 127, 128, 129, 170, 191, 192, 253, 254,
              255, 5, 17, 31, 42, 51, 77, 99, 111, 143, 155, 167, 200, 213,
              222, 233, 247]
FLOAT_FACTOR_COLORS = [(1.0, 0.5, 129 / 255), (0.75, 2 / 255, 128 / 255)]
FLOAT_VALUE_ALPHAS = [1.0, 0.5, 128 / 255, 1 / 255]
CONSTANTS = [(0.5, 128 / 255, 1 / 3, 0.75), (1 / 255, 254 / 255, 0.25, 1.0)]
# (destination bytes, source color) per sum test.
SUMS = [((0, 128, 255, 77), (1.0, 0.5, 0.0)),
        ((1, 254, 85, 170), (0.25, 129 / 255, 0.75))]
TEN_BIT = [(1, 2, 341), (511, 512, 513), (1022, 1023, 682)]
ALPHA_CONSTANT = (0.5, 0.25, 0.75, 1 / 3)

PRODUCT = b.blend(b.DST_COLOR, b.ADD, b.ZERO, (b.DST_ALPHA, b.ADD, b.ZERO))
SWAPPED = b.blend(b.ZERO, b.ADD, SRC_COLOR, (b.ZERO, b.ADD, b.SRC_ALPHA))
FLOAT_PRODUCT = b.blend(b.SRC_ALPHA, b.ADD, b.ZERO, (b.ONE, b.ADD, b.ZERO))
CONSTANT_PRODUCT = b.blend(b.CONSTANT_COLOR, b.ADD, b.ZERO)
SUM = b.blend(b.SRC_ALPHA, b.ADD, b.INV_SRC_ALPHA)


def ramp(i):
    return i / 2048


def texture(texel):
    """A texture of texel(i) -> float4 for every texel."""
    return [f2w(v) for i in range(PIXELS) for v in texel(i)]


def build(suite):
    suite.set_inputs(rb.RECT, 2)
    suite.add_texture('ramp', texture(lambda i: (ramp(i),) * 4))
    suite.add_texture('zero', rb.fill((0.0, 0.0, 0.0, 0.0)))
    suite.add_texture('table src', texture(
        lambda i: tuple(((4 * i + c) & 0xFF) / 255 for c in range(4))))
    for t in range(8):
        suite.add_texture(f'table dst {t}', texture(
            lambda i, t=t: tuple((32 * t + ((4 * i + c) >> 8)) / 255
                                 for c in range(4))))
    for t in range(len(FINE_BYTES) // 4):
        suite.add_texture(f'fine dst {t}', rb.fill(
            [d / 255 for d in FINE_BYTES[4 * t:4 * t + 4]]))
    for k, color in enumerate(FLOAT_FACTOR_COLORS):
        suite.add_texture(f'float factor {k}',
                          texture(lambda i, color=color: color + (ramp(i),)))
    for a in FLOAT_VALUE_ALPHAS:
        suite.add_texture(f'float value {a}', texture(
            lambda i, a=a: tuple(ramp(i + c / 4) for c in range(3)) + (a,)))
    for k, (dst, color) in enumerate(SUMS):
        suite.add_texture(f'sum dst {k}', rb.fill([d / 255 for d in dst]))
        suite.add_texture(f'sum src {k}',
                          texture(lambda i, color=color: color + (ramp(i),)))
    for k, dst in enumerate(TEN_BIT):
        suite.add_texture(f'10-bit dst {k}',
                          rb.fill([d / 1023 for d in dst] + [1.0]))

    formats = {f[0]: f for f in rb.FORMATS}

    def test(name, fmt, src, dst, control, constant=None):
        _, rt_format, _, dest_format, number = formats[fmt]
        state = rb.registers(rt_format)
        if constant:
            state.append(('RB_BLEND_RED', [f2w(v) for v in constant]))
        resolve = gpugen.resolve_registers(
            rb.WIDTH, rb.HEIGHT, dest_format, rb.TEXEL_BYTES[dest_format],
            number)
        rb.test(suite, name, src, state, resolve,
                passes=[([('RB_BLENDCONTROL0', [b.NO_BLEND])], rb.fetch(dst))],
                after_passes=[('RB_BLENDCONTROL0', [control])])

    for t in range(8):
        test(f'table {t}', '8_8_8_8', 'table src', f'table dst {t}', PRODUCT)
    for name, control in (('fine', PRODUCT), ('swapped', SWAPPED)):
        for t in range(len(FINE_BYTES) // 4):
            test(f'{name} {t}', '8_8_8_8', 'ramp', f'fine dst {t}', control)
    for k in range(len(FLOAT_FACTOR_COLORS)):
        test(f'float factor {k}', '8_8_8_8', f'float factor {k}', 'zero',
             FLOAT_PRODUCT)
    for a in FLOAT_VALUE_ALPHAS:
        test(f'float value {a}', '8_8_8_8', f'float value {a}', 'zero',
             FLOAT_PRODUCT)
    for k, constant in enumerate(CONSTANTS):
        test(f'constant {k}', '8_8_8_8', 'ramp', 'zero', CONSTANT_PRODUCT,
             constant)
    for k in range(len(SUMS)):
        test(f'sum {k}', '8_8_8_8', f'sum src {k}', f'sum dst {k}', SUM)
    for k in range(len(TEN_BIT)):
        test(f'10-bit {k}', '2_10_10_10', 'ramp', f'10-bit dst {k}', PRODUCT)
    # The source ramps with alpha 0.5 against destinations 63, 64, 85, 127.
    test('min factors', '8_8_8_8', 'float value 0.5', 'fine dst 1',
         b.blend(b.SRC_ALPHA, b.MIN, b.ONE))
    test('max factors', '8_8_8_8', 'ramp', 'fine dst 2',
         b.blend(b.ONE, b.MAX, b.DST_COLOR))
    test('constant alpha', '8_8_8_8', 'ramp', 'fine dst 1',
         b.blend(CONSTANT_ALPHA, b.ADD, b.ZERO), ALPHA_CONSTANT)
    test('inv constant alpha', '8_8_8_8', 'ramp', 'fine dst 1',
         b.blend(b.ZERO, b.ADD, INV_CONSTANT_ALPHA), ALPHA_CONSTANT)
    # Destination alpha 191, so 1 - alpha is 64.
    test('saturate alpha', '8_8_8_8', 'ramp', 'fine dst 2',
         b.blend(b.ONE, b.ADD, b.ZERO, (b.SRC_ALPHA_SATURATE, b.ADD, b.ZERO)))
