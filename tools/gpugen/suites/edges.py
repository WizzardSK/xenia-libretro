"""Edge cases of vertex fetch indexing and memory export addressing.

Vertex buffer: 64 vertices, vertex i = (i, i + 0.5, -i, 1000 + i).
- vfetch index *: r1.x = vertex index * c8.x + c8.y, then vfetch from r1.x
  with and without is_index_rounded: fractions, negative and past-the-end
  indices (the fetch constant holds 64 vertices).
- export *: the stream constant changed per test: half the element count,
  count 0, index exponent 24 and 22, a negative index, an index past the
  count; and byte and halfword formats, where neighbouring vertices share a
  dword.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

VERTICES = 64
f2w = gpugen.f2w


def build(suite):
    words = []
    for i in range(VERTICES):
        words += [f2w(float(i)), f2w(i + 0.5), f2w(-float(i)), f2w(1000.0 + i)]
    suite.set_inputs(words, 4)
    k = gpugen.C_FIRST_FREE
    move = [gpugen.export_copy(2)]
    index = x.alu(vector_op=x.V_MAD, vector_dest=1, vector_mask=0xF,
                  src1=x.r(0, 'x'), src2=x.c(k, 'xxxx'), src3=x.c(k, 'yyyy'))
    cases = [('plus 0.25', 1.0, 0.25), ('plus 0.5', 1.0, 0.5),
             ('plus 0.75', 1.0, 0.75), ('minus 0.5', 1.0, -0.5),
             ('minus 0.75', 1.0, -0.75), ('minus 1', 1.0, -1.0),
             ('minus 40', 1.0, -40.0), ('plus 40', 1.0, 40.0),
             ('plus 1000', 1.0, 1000.0), ('times 0.5', 0.5, 0.0),
             ('times -1', -1.0, 0.0)]
    for name, scale, bias in cases:
        for rounded in (False, True):
            fetch = x.vfetch(2, 1, 'x', 0, 0, x.VF_32_32_32_32_FLOAT, 4,
                             prefetch=4, index_rounded=rounded)
            suite.test(f'vfetch {name}{" rounded" if rounded else ""}',
                       gpugen.memexport_shader(4, [], [move], fetches=[fetch],
                                               prologue=[index]),
                       constants={k: [f2w(scale), f2w(bias), 0, 0]})

    a = gpugen.INPUT_REGISTER
    copy = [gpugen.export_copy(a)]

    def stream_test(name, change, export=gpugen.EXPORT_FLOAT4):
        suite.test(f'export {name}', gpugen.memexport_shader(4, [], [copy]),
                   stream=change, export=export)

    stream_test('half count', lambda w: w[:3] + [0x4B000000 | (VERTICES // 2)])
    stream_test('count 0', lambda w: w[:3] + [0x4B000000])
    stream_test('exponent 24', lambda w: [w[0], 0x4B800000] + w[2:])
    stream_test('exponent 22', lambda w: [w[0], 0x4A800000] + w[2:])
    stream_test('negative', lambda w: [w[0], 0xCB000000] + w[2:])
    stream_test('offset 32', lambda w: [w[0], 0x4B000020] + w[2:])
    # Sub-dword formats: k_8 unorm, k_16 unorm, k_16_FLOAT.
    stream_test('8 unorm', None, export=(2, 0, 1))
    stream_test('16 unorm', None, export=(24, 0, 2))
    stream_test('16 float', None, export=(30, 7, 2))
