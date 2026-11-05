"""The scalar unit's estimate and transcendental ops: rcp and rsq with their
clamping (c) and fixed-function (f) variants, sqrt, exp, log and logc, sin
and cos.

A holds positive inputs for the reciprocal, root and log ops: the first 2048
vertices sweep the mantissa of [1, 2) evenly (times a power of two per lane,
so any lookup table shows), the rest are random over 2^-60..2^60 plus special
values. B holds signed inputs for exp, sin and cos.
"""

import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

VERTICES = 4096
SWEEP = 2048

OPS = [('rcp', x.S_RCP, 'a'), ('rcpc', x.S_RCPC, 'a'),
       ('rcpf', x.S_RCPF, 'a'), ('rsq', x.S_RSQ, 'a'),
       ('rsqc', x.S_RSQC, 'a'), ('rsqf', x.S_RSQF, 'a'),
       ('sqrt', x.S_SQRT, 'a'), ('log', x.S_LOG, 'a'),
       ('logc', x.S_LOGC, 'a'), ('exp', x.S_EXP, 'b'),
       ('sin', x.S_SIN, 'b'), ('cos', x.S_COS, 'b')]

SPECIALS = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000,
            0xFF800000, 0x7FC00000, 0x00000001, 0x007FFFFF, 0x00800000,
            0x7F7FFFFF, 0x40000000, 0x3F000000, 0x80800000, 0xC0000000,
            0x4B000000]


f2w = gpugen.f2w


def inputs(rng):
    words = []
    for v in range(VERTICES):
        if v < SWEEP:
            # Lane i covers mantissa step v * 4 + i of 8192 over [1, 2),
            # scaled by a power of two that differs per lane.
            a = [f2w(math.ldexp(1.0 + (4 * v + i) / 8192.0, (-3, 0, 5, -9)[i]))
                 for i in range(4)]
        elif v < SWEEP + len(SPECIALS):
            special = SPECIALS[v - SWEEP]
            a = [special, special ^ 0x80000000, special, special]
        else:
            a = [f2w(math.ldexp(1.0 + rng.getrandbits(23) / 2.0 ** 23,
                                rng.randint(-60, 60))) for _ in range(4)]
        if SWEEP <= v < SWEEP + len(SPECIALS):
            b = list(a)
        elif v % 2:
            b = [f2w(rng.uniform(-24.0, 24.0)) for _ in range(4)]
        else:
            b = [f2w(rng.uniform(-4.0, 4.0)) for _ in range(4)]
        words += a + b
    return words


def build(suite):
    rng = random.Random(0x7A5C)
    suite.set_inputs(inputs(rng), 8)
    registers = {'a': gpugen.INPUT_REGISTER, 'b': gpugen.INPUT_REGISTER + 1}
    for name, op, source in OPS:
        lanes = gpugen.export_scalar_lanes(
            op, gpugen.lane_operands(registers[source]))
        suite.test(name, gpugen.memexport_shader(8, [], [lanes]))
