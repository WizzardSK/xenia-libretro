"""Texture fetch, the cases the tfetch suite left open. Vertex v point-samples
texel (v % 256, v // 256) at its center, unnormalized, and exports the float4.

- Block formats (DXT1, DXT2_3, DXT4_5, DXN, DXT3A, DXT5A, CTX1,
  DXT3A_AS_1_1_1_1 and the DXT AS_16_16_16_16 variants): 256x4 textures, 64
  blocks of standard little-endian block data read with endian none; 1024
  vertices. Blocks cover both endpoint orders, equal endpoints and extremes;
  'DXT1 8in16' reads the same blocks byte-swapped in 16-bit units.
- Everything else is 256x1, 256 vertices:
  - endian none / 8in16 / 16in32 for 8, 16, 16_16, 8_8_8_8 over random data;
  - 2_10_10_10, 10_11_11, 11_11_10 texels holding the field values where
    exact and float32-rounded conversion differ (959, 1535) and neighbours,
    unsigned and signed;
  - 16_16_16_16 gamma with the top 10 bits within 6 of 256, 384 and 768;
  - integer num_format with biased and gamma signs;
  - exp_adjust;
  - 32_FLOAT and 16_FLOAT zeros, infinities, NaNs, denormals;
  - the other formats over one shared random 4 KiB buffer (16 bytes per
    texel's worth, so any texel size can be decoded from it).
"""

import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import pm4  # noqa: E402
import xenos as x  # noqa: E402

WIDTH = 256
BLOCK_HEIGHT = 4
SLOT = 1
# TextureSign: unsigned, signed, biased, gamma.
UNSIGNED, SIGNED, BIASED, GAMMA = 0, 1, 2, 3
f2w = gpugen.f2w


def constant(fmt, sign=UNSIGNED, num_format=0, **keywords):
    return gpugen.texture_constant(fmt, WIDTH, sign=sign,
                                   num_format=num_format, **keywords)


def swap16(data):
    return b''.join(data[i:i + 2][::-1] for i in range(0, len(data), 2))


def color_block(rng, kind):
    """A DXT1-style 8-byte color block: endpoints, then 2-bit indices."""
    c0, c1 = rng.getrandbits(16), rng.getrandbits(16)
    if kind == 'four' and c0 <= c1:
        c0, c1 = max(c0, c1) | 1, min(c0, c1) & ~1
    elif kind == 'three' and c0 > c1:
        c0, c1 = c1, c0
    elif kind == 'equal':
        c1 = c0
    elif kind == 'extreme':
        c0, c1 = rng.choice([(0xFFFF, 0), (0, 0xFFFF), (0xF800, 0x07E0),
                             (0x001F, 0xF800), (0x07E0, 0x001F)])
    indices = rng.choice([0xE4E4E4E4, 0x1B1B1B1B, rng.getrandbits(32)])
    return struct.pack('<HHI', c0 & 0xFFFF, c1 & 0xFFFF, indices)


def alpha_block(rng, kind):
    """A DXT5-style 8-byte alpha block: endpoints, then 3-bit indices."""
    a0, a1 = rng.getrandbits(8), rng.getrandbits(8)
    if kind == 'eight' and a0 <= a1:
        a0, a1 = (a1, a0) if a1 != a0 else (min(a0 + 1, 255), max(a0 - 1, 0))
    elif kind == 'six' and a0 > a1:
        a0, a1 = a1, a0
    elif kind == 'equal':
        a1 = a0
    elif kind == 'extreme':
        a0, a1 = rng.choice([(255, 0), (0, 255), (1, 0), (0, 1)])
    # Each index exactly twice, shuffled over the 16 texels.
    codes = list(range(8)) * 2
    rng.shuffle(codes)
    bits = sum(c << (3 * i) for i, c in enumerate(codes))
    return bytes([a0, a1]) + bits.to_bytes(6, 'little')


KINDS_COLOR = ['four'] * 28 + ['three'] * 24 + ['equal'] * 4 + ['extreme'] * 8
KINDS_ALPHA = ['eight'] * 28 + ['six'] * 24 + ['equal'] * 4 + ['extreme'] * 8


def explicit_alpha_block(rng, index):
    if index < 4:
        return bytes([0x10 * index + 0x0F, 0xF0, 0x00, 0xFF] * 2)
    return bytes(rng.getrandbits(8) for _ in range(8))


def block_texture(rng, fmt):
    blocks = []
    for i in range(WIDTH // 4):
        if fmt == 'DXT1':
            blocks.append(color_block(rng, KINDS_COLOR[i]))
        elif fmt == 'DXT2_3':
            blocks.append(explicit_alpha_block(rng, i) +
                          color_block(rng, KINDS_COLOR[i]))
        elif fmt == 'DXT4_5':
            blocks.append(alpha_block(rng, KINDS_ALPHA[i]) +
                          color_block(rng, KINDS_COLOR[i]))
        elif fmt == 'DXN':
            blocks.append(alpha_block(rng, KINDS_ALPHA[i]) +
                          alpha_block(rng, KINDS_ALPHA[(i * 7 + 3) % 64]))
        elif fmt == 'DXT3A':
            blocks.append(explicit_alpha_block(rng, i))
        elif fmt == 'DXT5A':
            blocks.append(alpha_block(rng, KINDS_ALPHA[i]))
        elif fmt == 'CTX1':
            # Two 8:8 endpoints, then 2-bit indices.
            e = bytes(rng.getrandbits(8) for _ in range(4))
            if KINDS_COLOR[i] == 'equal':
                e = e[:2] * 2
            elif KINDS_COLOR[i] == 'extreme':
                e = rng.choice([b'\x00\x00\xff\xff', b'\xff\xff\x00\x00',
                                b'\xff\x00\x00\xff'])
            blocks.append(e + struct.pack('<I', rng.choice(
                [0xE4E4E4E4, rng.getrandbits(32)])))
    return b''.join(blocks)


# (name, TextureFormat, texture)
BLOCK_TESTS = [('DXT1', 18, 'DXT1'), ('DXT2_3', 19, 'DXT2_3'),
               ('DXT4_5', 20, 'DXT4_5'), ('DXN', 49, 'DXN'),
               ('DXT3A', 58, 'DXT3A'), ('DXT5A', 59, 'DXT5A'),
               ('CTX1', 60, 'CTX1'), ('DXT3A_AS_1_1_1_1', 61, 'DXT3A')]
# Run last with the other formats likeliest to misbehave.
BLOCK_AS_16_TESTS = [('DXT1_AS_16_16_16_16', 51, 'DXT1'),
                     ('DXT2_3_AS_16_16_16_16', 52, 'DXT2_3'),
                     ('DXT4_5_AS_16_16_16_16', 53, 'DXT4_5')]


def packed_10_11(rng, fmt):
    """Texels whose fields hold the values that tell exact from 32-bit
    rounding (10-bit 959, 11-bit 1535, magnitude 959) and neighbours, the
    rest random."""
    widths = {'2_10_10_10': (10, 10, 10, 2), '10_11_11': (11, 11, 10),
              '11_11_10': (10, 11, 11)}[fmt]
    special = {10: [959, 958, 960, 64 + 959, 1023 - 959, 512 + 447, 512, 511,
                    1023, 0, 1, 513],
               11: [1535, 1534, 1536, 959, 1024 + 959, 2047 - 1535, 1024,
                    1023, 2047, 0, 1, 1025],
               2: [0, 1, 2, 3]}
    texels = []
    for i in range(WIDTH):
        value, shift = 0, 0
        for c, width in enumerate(widths):
            values = special[width]
            if i < 64:
                field = values[(i + 5 * c) % len(values)]
            else:
                field = rng.getrandbits(width)
            value |= field << shift
            shift += width
        texels.append(struct.pack('<I', value))
    return b''.join(texels)


def gamma16():
    """16_16_16_16 texels with the top 10 bits within 6 of 256, 384 and 768
    (where the gamma curve's segments start), low 6 bits varied."""
    tops = (list(range(250, 262)) + list(range(378, 390)) +
            list(range(760, 772)))
    texels = []
    for i in range(WIDTH):
        fields = [(tops[(i + 9 * c) % len(tops)] << 6) |
                  ((i * 13 + c * 21) & 0x3F)
                  for c in range(4)]
        texels.append(struct.pack('<4H', *fields))
    return b''.join(texels)


FLOAT32_SPECIALS = [0x00000000, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000,
                    0xFFC00000, 0x7F800001, 0x7FFFFFFF, 0x00000001, 0x807FFFFF,
                    0x00800000, 0x80800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F800000,
                    0xBF800000]
FLOAT16_SPECIALS = [0x0000, 0x8000, 0x7C00, 0xFC00, 0x7E00, 0xFE00, 0x7C01,
                    0x7FFF, 0x0001, 0x83FF, 0x0400, 0x8400, 0x7BFF, 0xFBFF,
                    0x3C00, 0xBC00]


def build(suite):
    rng = random.Random(0x7E7C2)
    words = []
    for v in range(WIDTH * BLOCK_HEIGHT):
        words += [f2w(v % WIDTH + 0.5), f2w(v // WIDTH + 0.5), 0, 0]
    suite.set_inputs(words, 4)
    move = gpugen.export_copy(3)
    fetch = x.tfetch(3, gpugen.INPUT_REGISTER, 'xy', SLOT,
                     denormalized=True).serialized()
    shader = gpugen.memexport_shader(
        4, [], [[move]], fetches=gpugen.fetch_inputs(4) + [fetch])
    row = (pm4.PRIM_POINT_LIST, 0, WIDTH)

    def test(name, texture_name, fetch_constant, rows=False):
        suite.test(name, shader,
                   fetch_constants={SLOT: (fetch_constant, texture_name)},
                   draw=None if rows else row)

    def texture(name, data):
        suite.add_texture(name, gpugen.texture_words(data))

    def block_test(name, fmt, data, endian=pm4.ENDIAN_NONE):
        test(name, data, constant(fmt, height=BLOCK_HEIGHT, endian=endian),
             rows=True)

    # Blocks.
    blocks = {}
    for fmt in ('DXT1', 'DXT2_3', 'DXT4_5', 'DXN', 'DXT3A', 'DXT5A', 'CTX1'):
        blocks[fmt] = block_texture(rng, fmt)
        texture(fmt, blocks[fmt])
    texture('DXT1 swapped', swap16(blocks['DXT1']))
    for name, fmt, data in BLOCK_TESTS:
        block_test(name, fmt, data)
    block_test('DXT1 8in16', 18, 'DXT1 swapped', pm4.ENDIAN_8IN16)

    # Endian modes over random texels.
    random_data = bytes([0x00, 0xFF, 0x80, 0x7F, 0x01, 0x81, 0xFE, 0x40] * 4)
    random_data += bytes(rng.getrandbits(8) for _ in range(16 * WIDTH - 32))
    texture('random', random_data)
    for fname, fmt in (('8', 2), ('16', 24), ('16_16', 25), ('8_8_8_8', 6)):
        for ename, endian in (('none', pm4.ENDIAN_NONE),
                              ('8in16', pm4.ENDIAN_8IN16),
                              ('16in32', pm4.ENDIAN_16IN32)):
            test(f'{fname} {ename}', 'random', constant(fmt, endian=endian))

    # 10/11-bit fields.
    for fname, fmt in (('2_10_10_10', 7), ('10_11_11', 16), ('11_11_10', 17)):
        texture(fname, packed_10_11(rng, fname))
        for mname, sign in (('unsigned', UNSIGNED), ('signed', SIGNED)):
            test(f'{fname} edges {mname}', fname,
                 constant(fmt, sign=sign, endian=pm4.ENDIAN_NONE))
    texture('gamma16', gamma16())
    test('16_16_16_16 gamma edges', 'gamma16',
         constant(26, sign=GAMMA, endian=pm4.ENDIAN_NONE))

    # Integer with biased and gamma signs; exp_adjust.
    test('8_8_8_8 uint biased', 'random', constant(6, BIASED, 1))
    test('8_8_8_8 uint gamma', 'random', constant(6, GAMMA, 1))
    test('16_16 uint biased', 'random', constant(25, BIASED, 1))
    for adjust in (-3, 2, 31, -32):
        test(f'8_8_8_8 exp_adjust {adjust}', 'random',
             constant(6, exp_adjust=adjust))
    test('8_8_8_8 sint exp_adjust 2', 'random',
         constant(6, SIGNED, 1, exp_adjust=2))
    test('16_16_FLOAT exp_adjust 1', 'random', constant(31, exp_adjust=1))
    test('32_FLOAT exp_adjust -1', 'random', constant(36, exp_adjust=-1))

    # Float specials, little-endian texels read with endian none.
    texture('float32', b''.join(struct.pack('<I', FLOAT32_SPECIALS[i % 16])
                                for i in range(WIDTH)))
    texture('float16', b''.join(struct.pack('<H', FLOAT16_SPECIALS[i % 16])
                                for i in range(WIDTH)))
    test('32_FLOAT specials', 'float32',
         constant(36, endian=pm4.ENDIAN_NONE))
    test('16_FLOAT specials', 'float16',
         constant(30, endian=pm4.ENDIAN_NONE))
    test('16_FLOAT specials signed', 'float16',
         constant(30, SIGNED, endian=pm4.ENDIAN_NONE))

    # The rest over the random buffer (8in32); likeliest to misbehave last.
    for name, fmt in (('8_A', 8), ('8_B', 9), ('8_8_8_8_A', 14),
                      ('8_8_8_8_AS_16_16_16_16', 50),
                      ('2_10_10_10_AS_16_16_16_16', 54),
                      ('10_11_11_AS_16_16_16_16', 55),
                      ('11_11_10_AS_16_16_16_16', 56),
                      ('16_EXPAND', 27), ('16_16_EXPAND', 28),
                      ('16_16_16_16_EXPAND', 29), ('24_8', 22),
                      ('24_8_FLOAT', 23), ('32', 33), ('32_32', 34),
                      ('32_32_32_32', 35), ('32_AS_8', 39), ('32_AS_8_8', 40),
                      ('16_MPEG', 41), ('16_16_MPEG', 42),
                      ('Cr_Y1_Cb_Y0_REP', 11), ('Y1_Cr_Y0_Cb_REP', 12),
                      ('1', 1), ('1_REVERSE', 0), ('16_16_EDRAM', 13),
                      ('16_16_16_16_EDRAM', 21), ('8_8_8_8_GAMMA_EDRAM', 62),
                      ('2_10_10_10_FLOAT_EDRAM', 63), ('32_32_32_FLOAT', 57)):
        test(name, 'random', constant(fmt))
    for name, fmt, data in BLOCK_AS_16_TESTS:
        block_test(name, fmt, data)
