"""Texture fetch format conversion from vertex shaders: a 256x1 linear texture
per format, vertex v point-samples texel v at its center (unnormalized
coordinates) and exports the float4, under each sign/number mode (unsigned,
signed, unsigned biased and gamma fractions, unsigned and signed integers).
Texel data is random apart from leading edge patterns; endian 8in32."""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

WIDTH = 256
SLOT = 1  # texture fetch constant slot (slot 0 holds vertex fetch constants)

# (name, TextureFormat, bytes per texel)
INTEGER_FORMATS = [('8', 2, 1), ('1_5_5_5', 3, 2), ('5_6_5', 4, 2),
                   ('6_5_5', 5, 2), ('8_8_8_8', 6, 4), ('2_10_10_10', 7, 4),
                   ('8_8', 10, 2), ('4_4_4_4', 15, 2), ('10_11_11', 16, 4),
                   ('11_11_10', 17, 4), ('16', 24, 2), ('16_16', 25, 4),
                   ('16_16_16_16', 26, 8)]
FLOAT_FORMATS = [('16_FLOAT', 30, 2), ('16_16_FLOAT', 31, 4),
                 ('16_16_16_16_FLOAT', 32, 8), ('32_FLOAT', 36, 4),
                 ('32_32_FLOAT', 37, 8), ('32_32_32_32_FLOAT', 38, 16)]
# (name, per-component TextureSign, num_format)
MODES = [('unsigned', 0, 0), ('signed', 1, 0), ('biased', 2, 0),
         ('gamma', 3, 0), ('uint', 0, 1), ('sint', 1, 1)]
EDGE_BYTES = [0x00, 0xFF, 0x80, 0x7F, 0x01, 0x81, 0xFE, 0x40, 0xC0, 0x3F,
              0x7C, 0x00, 0xFF, 0x7B, 0x3C, 0x7E]


def texel_bytes(rng, size, index):
    if index < 16:
        return bytes([EDGE_BYTES[(index + i) % len(EDGE_BYTES)]
                      for i in range(size)])
    return bytes(rng.getrandbits(8) for _ in range(size))


def texture(rng, size):
    return gpugen.texture_words(b''.join(texel_bytes(rng, size, i)
                                         for i in range(WIDTH)))


f2w = gpugen.f2w


def build(suite):
    rng = random.Random(0x7E7C)
    suite.set_generated(WIDTH)
    k = gpugen.C_FIRST_FREE
    # r1 = (index + 0.5, 0.5): the texel center, unnormalized.
    coords = x.alu(vector_op=x.V_MAD, vector_dest=1, vector_mask=0xF,
                   src1=x.r(0, 'x'), src2=x.c(k), src3=x.c(k + 1))
    constants = {k: [f2w(1.0), 0, 0, 0], k + 1: [f2w(0.5), f2w(0.5), 0, 0]}
    fetch = x.tfetch(2, 1, 'xy', SLOT, denormalized=True)
    move = gpugen.export_copy(2)

    def test(name, texture_name, fmt, sign, num_format):
        suite.test(name, gpugen.memexport_shader(
            0, [], [[move]], fetches=[fetch], prologue=[coords]),
            constants=constants,
            fetch_constants={SLOT: (gpugen.texture_constant(
                fmt, WIDTH, sign=sign, num_format=num_format), texture_name)})

    for fname, fmt, size in INTEGER_FORMATS:
        suite.add_texture(fname, texture(rng, size))
        for mname, sign, num_format in MODES:
            test(f'{fname} {mname}', fname, fmt, sign, num_format)
    for fname, fmt, size in FLOAT_FORMATS:
        suite.add_texture(fname, texture(rng, size))
        test(fname, fname, fmt, 0, 0)
