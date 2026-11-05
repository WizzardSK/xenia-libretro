"""Corner cases of the dot product rule, one vertex each, in the dot suite's
layout (A then B, 8 words; dp4, dp3 and dp2add with c = A.zwzw): a carry into
a negative power of two, cancellation to zero mid-chain followed by a term
below the window, zero products beside huge partners, infinities, signed
zeros and underflow, then products that nearly cancel. Padded to 320
vertices with ones."""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dot  # noqa: E402
import gpugen  # noqa: E402

FLT_MAX = 0x7F7FFFFF
VERTICES = 320
RANDOM_VERTICES = 256
f2w = gpugen.f2w
ONE = [f2w(1.0)] * 4
INF, NINF, QNAN = 0x7F800000, 0xFF800000, 0x7FC00000


def probe_vertices():
    """[(label naming the case, A words, B words)]."""
    v = []
    m = 1 + 0x5A5A5 / 2.0 ** 23  # a significand with low bits set
    for k in (-20, 0, 20):
        s = 2.0 ** k
        # w + z = -2^(k+1) exactly: a carry into a negative power of two.
        v.append((f'negpow2 carry dp4 2^{k}',
                  [f2w(0.0), f2w(1.5 * s * 2.0 ** -24), f2w(-s), f2w(-s)],
                  ONE))
        v.append((f'negpow2 carry dp4 2^{k}, y=2^-24',
                  [f2w(0.0), f2w(s * 2.0 ** -24), f2w(-1.25 * s),
                   f2w(-0.75 * s)], ONE))
        # dp3: z + y = -2^(k+1), then x.
        v.append((f'negpow2 carry dp3 2^{k}',
                  [f2w(1.5 * s * 2.0 ** -24), f2w(-s), f2w(-s), f2w(0.0)],
                  ONE))
    for sign in (1, -1):
        for k in (-26, -30, -40):
            t = sign * m * 2.0 ** k
            # w + z cancels to exactly zero, then a term below the window.
            v.append((f'midzero dp4 y={sign:+d}*2^{k}',
                      [f2w(0.0), f2w(t), f2w(m), f2w(-m)], ONE))
            v.append((f'midzero dp3 x={sign:+d}*2^{k}',
                      [f2w(t), f2w(m), f2w(-m), f2w(0.0)], ONE))
    for zw, zb in ((0x00000000, FLT_MAX), (0x00000001, FLT_MAX),
                   (0x80000000, 0x7F000000), (0x00000000, INF)):
        # Zero product with a huge partner beside small full-mantissa terms.
        v.append((f'zero product {zw:08X}*{zb:08X} dp4',
                  [f2w(-m * 2.0 ** -10), f2w(-m * 2.0 ** -11),
                   f2w(-m * 2.0 ** -12), zw], [ONE[0]] * 3 + [zb]))
        v.append((f'zero product {zw:08X}*{zb:08X} in x, dp3',
                  [zw, f2w(-m * 2.0 ** -11), f2w(-m * 2.0 ** -12), f2w(0.0)],
                  [zb] + [ONE[0]] * 3))
    v.append(('inf once', ONE, [INF, ONE[0], ONE[0], ONE[0]]))
    v.append(('inf twice', ONE, [INF, INF, ONE[0], ONE[0]]))
    v.append(('-inf once', ONE, [NINF, ONE[0], ONE[0], ONE[0]]))
    v.append(('inf + overflow', ONE, [INF, FLT_MAX, FLT_MAX, ONE[0]]))
    v.append(('dp2add c=+inf', [ONE[0], ONE[0], INF, 0], ONE))
    v.append(('dp2add c=-inf, products finite',
              [ONE[0], ONE[0], NINF, 0], ONE))
    v.append(('dp2add c=NaN', [ONE[0], ONE[0], QNAN, 0], ONE))
    v.append(('dp2add c=-inf vs product +inf', [INF, ONE[0], NINF, 0], ONE))
    v.append(('all -0', [0x80000000] * 4, ONE))
    v.append(('dp2add c=-0, zero products', [0, 0, 0x80000000, 0], ONE))
    v.append(('dp2add c=-0, cancelling products',
              [ONE[0], f2w(-1.0), 0x80000000, 0], ONE))
    v.append(('negative underflow dp4',
              [0, 0, f2w(-1.5 * 2.0 ** -126), f2w(2.0 ** -126)], ONE))
    v.append(('positive underflow dp4',
              [0, 0, f2w(1.5 * 2.0 ** -126), f2w(-2.0 ** -126)], ONE))
    v.append(('negative tiny product dp4',
              [0, 0, 0, f2w(2.0 ** -70)], [ONE[0]] * 3 + [f2w(-2.0 ** -70)]))
    return v


def probe_random_vertices():
    """Inexact products that nearly cancel: B random, A chosen so the
    products land on close magnitudes with mixed signs."""
    rng = random.Random(0xD07E)
    v = []
    for i in range(RANDOM_VERTICES):
        base = rng.randint(-20, 20)
        spread = 2 if i < RANDOM_VERTICES // 2 else 12
        b = [rng.choice((-1, 1)) * (1 + rng.getrandbits(23) / 2.0 ** 23) *
             2.0 ** rng.randint(-4, 4) for _ in range(4)]
        target = [rng.choice((-1, 1)) * (1 + rng.getrandbits(23) / 2.0 ** 23) *
                  2.0 ** (base - rng.randint(0, spread)) for _ in range(4)]
        a = [target[j] / b[j] for j in range(4)]
        v.append((f'random cancel {i}', [f2w(value) for value in a],
                  [f2w(value) for value in b]))
    return v


def probe_words():
    words = []
    for _, a, b in probe_vertices() + probe_random_vertices():
        words += a + b
    return words + (ONE + ONE) * (VERTICES - len(words) // 8)


def build(suite):
    suite.set_inputs(probe_words(), 8)
    dot.add_tests(suite)
