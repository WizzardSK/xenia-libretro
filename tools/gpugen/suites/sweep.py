"""Every mantissa of one scalar op: GPUGEN_SWEEP picks the op and the range,
one of SWEEPS or LINEAR. A SWEEPS binade is [scale, 2 scale), (-2, -1] for
-1: input scale * (1 + k * 2^-23). A LINEAR sweep's input is k * step. Vertex
v computes inputs k = 4v + lane exactly from its index and exports the op of
each. Results are raw, 2^23 words per run; GPUGEN_VERTICES shortens it."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

SWEEPS = {'rcp': (x.S_RCP, 1.0), 'rsq': (x.S_RSQ, 1.0),
          'rsq2': (x.S_RSQ, 2.0), 'sqrt': (x.S_SQRT, 1.0),
          'sqrt2': (x.S_SQRT, 2.0), 'log': (x.S_LOG, 1.0),
          'log2': (x.S_LOG, 2.0), 'logh': (x.S_LOG, 0.5),
          'exp': (x.S_EXP, 1.0), 'expn': (x.S_EXP, -1.0),
          'exp64': (x.S_EXP, 64.0), 'expn64': (x.S_EXP, -64.0),
          'expntiny': (x.S_EXP, -2.0 ** -24)}
LINEAR = {'sin': (x.S_SIN, 2.0 ** -21), 'cos': (x.S_COS, 2.0 ** -21),
          'expint': (x.S_EXP, 1.0), 'expnint': (x.S_EXP, -1.0)}
VERTICES = 1 << 21
f2w = gpugen.f2w


def build(suite):
    name = os.environ.get('GPUGEN_SWEEP', 'rcp')
    if name not in SWEEPS and name not in LINEAR:
        raise ValueError(f'GPUGEN_SWEEP is one of '
                         f'{", ".join(list(SWEEPS) + list(LINEAR))}')
    suite.set_generated(int(os.environ.get('GPUGEN_VERTICES', VERTICES)))
    suite.raw = True
    k = gpugen.C_FIRST_FREE
    if name in LINEAR:
        op, step = LINEAR[name]
        base = 0.0
    else:
        op, scale = SWEEPS[name]
        step = scale * 2.0 ** -23
        base = scale
    # r1 = r0.x * 4 step + (base + lane step), exact.
    body = [x.alu(vector_op=x.V_MAD, vector_dest=1, vector_mask=0xF,
                  src1=x.r(0, 'x'), src2=x.c(k), src3=x.c(k + 1))]
    lanes = gpugen.export_scalar_lanes(op, gpugen.lane_operands(1))
    suite.test(name, gpugen.memexport_shader(0, body, [lanes]),
               constants={k: [f2w(4 * step)] * 4 +
                          [f2w(base + i * step) for i in range(4)]})
