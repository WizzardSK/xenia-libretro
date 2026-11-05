"""Smoke suite for the gpugen executor: a copy of the fetched inputs, vector
multiplies and the same products through the scalar constant multiply."""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

VERTICES = 64
f2w = gpugen.f2w


def build(suite):
    rng = random.Random(0x6E05)
    words = []
    for _ in range(VERTICES):
        for _ in range(8):
            words.append(f2w(rng.uniform(-4.0, 4.0)))
    suite.set_inputs(words, 8)
    a = gpugen.INPUT_REGISTER
    b = gpugen.INPUT_REGISTER + 1

    copy = gpugen.export_copy(a)
    suite.test('copy', gpugen.memexport_shader(8, [], [[copy]]))

    mul = x.alu(vector_op=x.V_MUL, vector_dest=x.EXPORT_DATA0, vector_mask=0xF,
                export=True, src1=x.r(a), src2=x.r(b))
    suite.test('mul', gpugen.memexport_shader(8, [], [[mul]]))

    # mulsc c8.<k> * a.x into each component of the first element (a.x
    # broadcast into r3 and read from another component than k: a console
    # reads both operands from the temporary when they share one), and the
    # same products with the vector multiply into the second.
    k = gpugen.C_FIRST_FREE
    constants = {k: [f2w(rng.uniform(-4.0, 4.0)) for _ in range(4)]}
    body = [x.alu(vector_op=x.V_MAX, vector_dest=3, vector_mask=0xF,
                  src1=x.r(a, 'x'), src2=x.r(a, 'x'))]
    mulsc = [x.alu(vector_dest=x.EXPORT_DATA0, export=True,
                   scalar_op=x.S_MULSC0, scalar_mask=1 << i,
                   src3=x.c(k, 'xxx' + 'xyzw'[i]),
                   scalar_const_temp=(3, 'xyzw'[(i + 1) % 4]))
             for i in range(4)]
    vector = x.alu(vector_op=x.V_MUL, vector_dest=x.EXPORT_DATA0,
                   vector_mask=0xF, export=True, src1=x.r(a, 'x'),
                   src2=x.c(k))
    suite.test('mulsc', gpugen.memexport_shader(8, body, [mulsc, [vector]]),
               constants=constants, elements=2)
