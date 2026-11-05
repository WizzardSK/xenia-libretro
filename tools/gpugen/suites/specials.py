"""The shader ALU on special operands: every ordered pair of 32 values (zeros,
infinities, NaNs, denormals, ones, extremes) through the vector and scalar
ops. Pair p = 4 * vertex + lane holds A = SPECIALS[p // 32] and
B = SPECIALS[p % 32]. Operands come straight from vfetch (destination
swizzles place them), so no ALU move touches them on the way in."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

SPECIALS = [
    0x00000000, 0x80000000, 0x3F800000, 0xBF800000,  # +0 -0 +1 -1
    0x40000000, 0xBF000000, 0x3FC00000, 0xC0400000,  # 2 -0.5 1.5 -3
    0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,  # +inf -inf qNaN -qNaN
    0x7F800001, 0xFF800001, 0x7FC12345, 0x7FFFFFFF,  # sNaN -sNaN payloads
    0x00000001, 0x80000001, 0x007FFFFF, 0x807FFFFF,  # denormals
    0x00800000, 0x80800000, 0x00C00000, 0x7F7FFFFF,  # min normal, max
    0xFF7FFFFF, 0x7F000000, 0x01000000, 0x3F7FFFFF,  # -max 2^127 2^-125 1-ulp
    0x3F800001, 0x4B000000, 0x40490FDB, 0xC0490FDB,  # 1+ulp 2^23 pi -pi
]

UNARY_SCALAR = [('frcs', x.S_FRCS), ('truncs', x.S_TRUNCS),
                ('floors', x.S_FLOORS), ('exp', x.S_EXP), ('logc', x.S_LOGC),
                ('log', x.S_LOG), ('rcpc', x.S_RCPC), ('rcpf', x.S_RCPF),
                ('rcp', x.S_RCP), ('rsqc', x.S_RSQC), ('rsqf', x.S_RSQF),
                ('rsq', x.S_RSQ), ('sqrt', x.S_SQRT), ('sin', x.S_SIN),
                ('cos', x.S_COS), ('seqs', x.S_SEQS), ('sgts', x.S_SGTS),
                ('sges', x.S_SGES), ('snes', x.S_SNES)]
BINARY_SCALAR = [('adds', x.S_ADDS), ('muls', x.S_MULS), ('maxs', x.S_MAXS),
                 ('mins', x.S_MINS), ('subs', x.S_SUBS)]
BINARY_VECTOR = [('add', x.V_ADD), ('mul', x.V_MUL), ('max', x.V_MAX),
                 ('min', x.V_MIN), ('seq', x.V_SEQ), ('sgt', x.V_SGT),
                 ('sge', x.V_SGE), ('sne', x.V_SNE)]
UNARY_VECTOR = [('frc', x.V_FRC), ('trunc', x.V_TRUNC), ('floor', x.V_FLOOR)]

A = gpugen.INPUT_REGISTER       # r1
B = gpugen.INPUT_REGISTER + 1   # r2
PAIRS = 3                       # r3-r6: (A.lane, B.lane) for scalar ops


def inputs():
    words = []
    n = len(SPECIALS)
    for v in range(n * n // 4):
        pairs = [divmod(4 * v + lane, n) for lane in range(4)]
        words += ([SPECIALS[i] for i, _ in pairs] +
                  [SPECIALS[j] for _, j in pairs])
    return words


def fetches():
    """A into r1, B into r2, and r3 + lane = (A.lane, B.lane, -, -)."""
    out = [x.vfetch(A, 0, 'x', 0, 0, x.VF_32_32_32_32_FLOAT, 8),
           x.vfetch(B, 0, 'x', 0, 0, x.VF_32_32_32_32_FLOAT, 0, offset=4,
                    mini=True)]
    for lane in range(4):
        c = 'xyzw'[lane]
        # The second exec of fetches (from lane 2) opens with a full fetch, as
        # mini fetches reuse the address of the full fetch before them.
        full = lane == 2
        out.append(x.vfetch(PAIRS + lane, 0, 'x', 0, 0, x.VF_32_32_32_32_FLOAT,
                            8 if full else 0, dst_swizzle=c + '___',
                            mini=not full))
        out.append(x.vfetch(PAIRS + lane, 0, 'x', 0, 0,
                            x.VF_32_32_32_32_FLOAT, 0, offset=4,
                            dst_swizzle='_' + c + '__', mini=True))
    return out


def build(suite):
    suite.set_inputs(inputs(), 8)
    f = fetches()
    one = gpugen.C_FIRST_FREE

    def test(name, writes, constants=None):
        suite.test(name, gpugen.memexport_shader(8, [], [writes], fetches=f),
                   constants=constants)

    vector = gpugen.export_vector
    # How each "move" carries the operand, the baseline for everything else.
    test('mov max', vector(x.V_MAX, x.r(A), x.r(A)))
    test('mov min', vector(x.V_MIN, x.r(A), x.r(A)))
    test('mov mul 1', vector(x.V_MUL, x.r(A), x.c(one)),
         {one: [gpugen.f2w(1.0)] * 4})
    # The harness keeps zeros in C_FLUSH.
    test('mov add 0', vector(x.V_ADD, x.r(A), x.c(gpugen.C_FLUSH)))
    for name, op in BINARY_VECTOR:
        test(name, vector(op, x.r(A), x.r(B)))
    for name, op in UNARY_VECTOR:
        test(name, vector(op, x.r(A)))
    # Three operands: C is B rotated.
    test('mad', vector(x.V_MAD, x.r(A), x.r(B), x.r(B, 'yzwx')))
    for name, op in (('cndeq', x.V_CNDEQ), ('cndge', x.V_CNDGE),
                     ('cndgt', x.V_CNDGT)):
        test(name, vector(op, x.r(A), x.r(B), x.r(B, 'yzwx')))
    test('dp4', vector(x.V_DP4, x.r(A), x.r(B)))
    test('max4', vector(x.V_MAX4, x.r(A)))
    for name, op in UNARY_SCALAR:
        test(name, gpugen.export_scalar_lanes(op, gpugen.lane_operands(A)))
    for name, op in BINARY_SCALAR:
        # a from the W slot (A.lane), b from the X slot (B.lane).
        test(name, gpugen.export_scalar_lanes(
            op, lambda i: x.r(PAIRS + i, 'yxxx')))
