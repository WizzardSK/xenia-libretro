"""Shader ALU arithmetic: the products, sums and multiply-adds the vector and
scalar units compute over inputs chosen so rounding modes disagree.

Every vertex carries A and B (two float4s). Inputs mix random floats over
several exponent ranges, k * N paired with 1/N (dividing by N through its
reciprocal) and inexact products (full 24-bit times odd 12-bit mantissas).
"""

import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

VERTICES = 4096
# Divisors without an exact reciprocal.
DIVISORS = [n for n in range(3, 257) if n & (n - 1)]
f2w = gpugen.f2w


def random_float(rng):
    kind = rng.random()
    if kind < 0.6:
        return rng.uniform(-8.0, 8.0)
    if kind < 0.8:
        return math.ldexp(rng.uniform(1.0, 2.0), rng.randint(-20, 20)) * \
            rng.choice((-1, 1))
    return float(rng.randint(-1000, 1000))


def inputs(rng):
    """A and B for every vertex, as words."""
    words = []
    for v in range(VERTICES):
        if v < 1024:
            # k * N times 1/N: A = (k * N, ...), B = (1/N, ...) per lane.
            a, b = [], []
            for _ in range(4):
                n = rng.choice(DIVISORS)
                k = rng.randint(1, 4096)
                a.append(float(k * n))
                b.append(1.0 / n)
        elif v < 1536:
            # Inexact products: mantissas with many low bits.
            a = [math.ldexp(1 + rng.getrandbits(23) / 2.0 ** 23,
                            rng.randint(-4, 4)) for _ in range(4)]
            b = [math.ldexp(1 + (rng.getrandbits(11) | 1) / 2.0 ** 11,
                            rng.randint(-4, 4)) for _ in range(4)]
        else:
            a = [random_float(rng) for _ in range(4)]
            b = [random_float(rng) for _ in range(4)]
        words += [f2w(f) for f in a] + [f2w(f) for f in b]
    return words


def build(suite):
    rng = random.Random(0xA1A1)
    suite.set_inputs(inputs(rng), 8)
    a = gpugen.INPUT_REGISTER
    b = gpugen.INPUT_REGISTER + 1

    def vector_test(name, op, *sources):
        suite.test(name, gpugen.memexport_shader(
            8, [], [gpugen.export_vector(op, *sources)]))

    vector_test('copy a', x.V_MAX, x.r(a), x.r(a))
    vector_test('copy b', x.V_MAX, x.r(b), x.r(b))
    vector_test('mul', x.V_MUL, x.r(a), x.r(b))
    vector_test('add', x.V_ADD, x.r(a), x.r(b))
    # a * b + a.yzwx.
    vector_test('mad a*b+a', x.V_MAD, x.r(a), x.r(b), x.r(a, 'yzwx'))
    vector_test('dp4', x.V_DP4, x.r(a), x.r(b))
    vector_test('dp3', x.V_DP3, x.r(a), x.r(b))

    # muls and adds read `a` from src3's W slot and `b` from its X slot (the
    # vector op here takes two sources), so each lane first gathers
    # r3 = (A.i, B.i), then computes r3.x op r3.y.
    for op, name in ((x.S_MULS, 'muls'), (x.S_ADDS, 'adds')):
        body = []
        exports = []
        for i in range(4):
            lane = 'xyzw'[i]
            # r3 = (A.lane, B.lane, -, -)
            body.append(x.alu(vector_op=x.V_MAX, vector_dest=3,
                              vector_mask=0b0001, src1=x.r(a, lane),
                              src2=x.r(a, lane)))
            body.append(x.alu(vector_op=x.V_MAX, vector_dest=3,
                              vector_mask=0b0010, src1=x.r(b, lane),
                              src2=x.r(b, lane)))
            body.append(x.alu(scalar_op=op, scalar_dest=4, scalar_mask=1 << i,
                              src3=x.r(3, 'yxxx')))
        exports.append(gpugen.export_copy(4))
        suite.test(name, gpugen.memexport_shader(8, body, [exports]))

    # mulsc: constant lane k times A.<lane>, against the vector multiply of
    # the same pairs. Five draws of four constants each. Compilers never put
    # the constant and the temporary operand on the same component (a console
    # then reads both from the temporary), so A.<lane> is broadcast into r3
    # and read from the component after k.
    k = gpugen.C_FIRST_FREE
    for draw in range(5):
        constants = []
        for _ in range(4):
            n = rng.choice(DIVISORS)
            constants.append(1.0 / n if rng.random() < 0.5
                             else random_float(rng))
        for lane in 'xyzw':
            body = [x.alu(vector_op=x.V_MAX, vector_dest=3, vector_mask=0xF,
                          src1=x.r(a, lane), src2=x.r(a, lane))]
            mulsc = gpugen.export_scalar_lanes(x.S_MULSC0, lambda i: {
                'src3': x.c(k, 'xxx' + 'xyzw'[i]),
                'scalar_const_temp': (3, 'xyzw'[(i + 1) % 4])})
            vector = gpugen.export_vector(x.V_MUL, x.r(a, lane), x.c(k))
            suite.test(f'mulsc {draw} a.{lane}', gpugen.memexport_shader(
                8, body, [mulsc, vector]),
                constants={k: [f2w(c) for c in constants]}, elements=2)
