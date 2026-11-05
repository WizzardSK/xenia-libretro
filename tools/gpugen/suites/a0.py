"""The address register: maxa (vector), maxas and maxasf (scalar) load a0
from each vertex's value, then the export reads c[a0 + 128] from a constant
file where c[k] = k for k >= 8 (to c511, the vertex and pixel halves; c0-c7
hold the harness's constants). The first element is that read, the second
the op's own result."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

BASE = 128
VALUES = [-1e10, -300.0, -257.0, -256.5, -256.0, -255.5, -200.0, -128.5,
          -128.0, -127.5, -10.5, -2.5, -1.5, -0.75, -0.5, -0.49, -0.25, 0.0,
          0.25, 0.49, 0.5, 0.51, 0.75, 1.0, 1.49, 1.5, 2.5, 3.5, 10.5, 100.5,
          126.5, 127.0, 127.5, 128.0, 200.0, 254.5, 255.0, 255.5, 256.0, 300.0,
          1e10]
SPECIAL_WORDS = [0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x80000000,
                 0x00000001, 0x80000001]
f2w = gpugen.f2w


def build(suite):
    words = [f2w(v) for v in VALUES] + SPECIAL_WORDS
    suite.set_inputs([w for w in words for _ in range(4)], 4)
    a = gpugen.INPUT_REGISTER
    constants = {k: [f2w(float(k))] * 4
                 for k in range(gpugen.C_FIRST_FREE, 512)}
    # a0 is read an instruction after the one loading it, as compilers do.
    nop = x.alu(vector_op=x.V_MAX)

    def read(dest, export):
        return x.alu(vector_op=x.V_MAX, vector_dest=dest, vector_mask=0xF,
                     export=export, src1=x.c(BASE), src2=x.c(BASE),
                     a0_relative=True)

    move = gpugen.export_copy
    loads = {
        'maxa': x.alu(vector_op=x.V_MAXA, vector_dest=2, vector_mask=0xF,
                      src1=x.r(a), src2=x.r(a)),
        'maxas': x.alu(scalar_op=x.S_MAXAS, scalar_dest=2, scalar_mask=0xF,
                       src3=x.r(a, 'xxxx')),
        'maxasf': x.alu(scalar_op=x.S_MAXASF, scalar_dest=2, scalar_mask=0xF,
                        src3=x.r(a, 'xxxx')),
    }
    for name, load in loads.items():
        # a0 loaded and read into r3 in one clause, r3 exported.
        suite.test(f'{name} temp', gpugen.memexport_shader(
            4, [load, nop, read(3, False)], [[move(3)], [move(2)]]),
            constants=constants, elements=2)
    # a0 loaded inside the export clause, read straight into eM0.
    suite.test('maxa export clause', gpugen.memexport_shader(
        4, [], [[loads['maxa'], nop, read(x.EXPORT_DATA0, True)], [move(2)]]),
        constants=constants, elements=2)
    # a0 loaded in the clause before, read into eM0.
    suite.test('maxa earlier clause', gpugen.memexport_shader(
        4, [loads['maxa'], nop], [[read(x.EXPORT_DATA0, True)], [move(2)]]),
        constants=constants, elements=2)
