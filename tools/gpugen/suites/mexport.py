"""Memory export conversions: each vertex's float4 exported through packed
formats (unorm, snorm, uint, sint), 16-bit float and 32-bit float formats,
endian 8in32, the raw element words recorded."""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402

# (name, ColorFormat, element bytes)
FORMATS = [('8_8_8_8', 6, 4), ('2_10_10_10', 7, 4), ('10_11_11', 16, 4),
           ('11_11_10', 17, 4), ('16_16', 25, 4), ('16_16_16_16', 26, 8)]
FLOAT_FORMATS = [('16_16_FLOAT', 31, 4), ('16_16_16_16_FLOAT', 32, 8),
                 ('32_FLOAT', 36, 4), ('32_32_FLOAT', 37, 8)]
NUMBER_FORMATS = [('unorm', 0), ('snorm', 1), ('uint', 2), ('sint', 3)]

SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F000000,
           0x7F800000, 0xFF800000, 0x7FC00000, 0x00000001, 0x80000001,
           0x477FE000, 0x47800000, 0x47FFE000, 0x48000000, 0x387FC000,
           0x33800000, 0x33000000, 0x4B800000, 0x4F800000, 0xCF000000,
           0x3B808081, 0x3B000000, 0x3F7FFFFF, 0x3F800001, 0x437F0000,
           0x437F8000, 0x42FE0000, 0xC3000000, 0x3A802008, 0x3A800000]


f2w = gpugen.f2w


def build(suite):
    rng = random.Random(0xE4F0)
    words = []
    for v in range(1024):
        for lane in range(4):
            i = 4 * v + lane
            if i < len(SPECIAL):
                words.append(SPECIAL[i])
            elif v < 512:
                # [-2, 2] in 16-bit unorm steps and halfway between them.
                words.append(f2w(rng.randint(-2 * 65535, 2 * 65535) / 65535.0
                                 + rng.choice((0.0, 0.5 / 65535))))
            else:
                words.append(f2w(rng.uniform(-70000.0, 70000.0) if lane % 2
                                 else rng.uniform(-300.0, 300.0)))
    suite.set_inputs(words, 4)
    a = gpugen.INPUT_REGISTER
    move = [gpugen.export_copy(a)]
    for fname, color, size in FORMATS:
        for nname, number in NUMBER_FORMATS:
            suite.test(f'{fname} {nname}',
                       gpugen.memexport_shader(4, [], [move]),
                       export=(color, number, size))
    for fname, color, size in FLOAT_FORMATS:
        suite.test(fname, gpugen.memexport_shader(4, [], [move]),
                   export=(color, 7, size))
