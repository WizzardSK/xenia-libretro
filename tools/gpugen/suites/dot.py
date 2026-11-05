"""Dot products: dp4/dp3/dp2add over A and B. For the first 3072 vertices
B = 1 so the products are exact and the result shows how the terms are
summed: positive terms spread over 27 binades, mixed signs, close magnitudes
(cancellation) and wide ranges. The last 1024 use random B, to see whether the
products are rounded before summing. The products (mul) are exported too, for
reference."""

import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

VERTICES = 4096
f2w = gpugen.f2w


def term(rng, exponent, sign):
    return sign * math.ldexp(1 + rng.getrandbits(23) / 2.0 ** 23, exponent)


def inputs(rng):
    words = []
    for v in range(VERTICES):
        base = rng.randint(-20, 20)
        if v < 768:
            a = [term(rng, base - rng.randint(0, 26), 1) for _ in range(4)]
        elif v < 1536:
            a = [term(rng, base - rng.randint(0, 26), rng.choice((-1, 1)))
                 for _ in range(4)]
        elif v < 2304:
            a = [term(rng, base - rng.randint(0, 2), rng.choice((-1, 1)))
                 for _ in range(4)]
        elif v < 3072:
            a = [term(rng, rng.randint(-40, 40), rng.choice((-1, 1)))
                 for _ in range(4)]
        else:
            a = [term(rng, base - rng.randint(0, 20), rng.choice((-1, 1)))
                 for _ in range(4)]
        if v < 3072:
            b = [1.0] * 4
        else:
            b = [term(rng, rng.randint(-4, 4), rng.choice((-1, 1)))
                 for _ in range(4)]
        words += [f2w(t) for t in a] + [f2w(t) for t in b]
    return words


def add_tests(suite):
    """The tests, over 8-dword vertices of A then B."""
    a, b = gpugen.INPUT_REGISTER, gpugen.INPUT_REGISTER + 1

    def test(name, *op_sources):
        suite.test(name, gpugen.memexport_shader(
            8, [], [gpugen.export_vector(*op_sources)]))

    test('dp4', x.V_DP4, x.r(a), x.r(b))
    test('dp3', x.V_DP3, x.r(a), x.r(b))
    # dp2add: a.x*b.x + a.y*b.y + c.x, with c = A.zwzw.
    test('dp2add', x.V_DP2ADD, x.r(a), x.r(b), x.r(a, 'zwzw'))
    test('mul', x.V_MUL, x.r(a), x.r(b))


def build(suite):
    suite.set_inputs(inputs(random.Random(0xD07D)), 8)
    add_tests(suite)
