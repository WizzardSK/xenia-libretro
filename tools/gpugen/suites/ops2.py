"""More ALU ops over the specials suite's 32x32 operand pairs: cube, dst,
the scalar predicate setters' results, and the ops that take the previous
scalar result (ps): adds_prev, muls_prev, muls_prev2, subs_prev, with ps set
by the instruction before from B."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gpugen  # noqa: E402
import specials  # noqa: E402
import xenos as x  # noqa: E402

A, B, PAIRS = specials.A, specials.B, specials.PAIRS

SETP = [('setp_eq', x.S_SETP_EQ), ('setp_ne', x.S_SETP_NE),
        ('setp_gt', x.S_SETP_GT), ('setp_ge', x.S_SETP_GE),
        ('setp_inv', x.S_SETP_INV), ('setp_pop', x.S_SETP_POP),
        ('setp_clr', x.S_SETP_CLR), ('setp_rstr', x.S_SETP_RSTR)]
PREV = [('adds_prev', x.S_ADDS_PREV), ('muls_prev', x.S_MULS_PREV),
        ('muls_prev2', x.S_MULS_PREV2), ('subs_prev', x.S_SUBS_PREV)]


def build(suite):
    suite.set_inputs(specials.inputs(), 8)
    f = specials.fetches()

    def test(name, body, writes):
        suite.test(name, gpugen.memexport_shader(8, body, [writes], fetches=f))

    # cube r, A.zzxy, A.yxzz as Direct3D uses it; B gives the direction too.
    for name, r in (('cube A', A), ('cube B', B)):
        test(name, [], gpugen.export_vector(x.V_CUBE, x.r(r, 'zzxy'),
                                            x.r(r, 'yxzz')))
    test('dst', [], gpugen.export_vector(x.V_DST, x.r(A), x.r(B)))
    for name, op in SETP:
        test(name, [], gpugen.export_scalar_lanes(op, gpugen.lane_operands(A)))
    for name, op in PREV:
        # ps = B.lane (maxs B.lane, B.lane), then op on (a = A.lane,
        # b = B.lane).
        writes = []
        for i in range(4):
            writes.append(x.alu(scalar_op=x.S_MAXS, scalar_dest=7,
                                scalar_mask=1, src3=x.r(PAIRS + i, 'yyyy')))
            writes.append(x.alu(vector_dest=x.EXPORT_DATA0, export=True,
                                scalar_op=op, scalar_mask=1 << i,
                                src3=x.r(PAIRS + i, 'yxxx')))
        test(name, [], writes)
