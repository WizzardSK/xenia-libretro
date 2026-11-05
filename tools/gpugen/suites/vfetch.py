"""Vertex fetch format conversion: the same 16 bytes per vertex fetched as
every vertex format, unsigned/signed, normalized/integer and both signed
repeating fraction modes, plus exponent adjusts. Each test exports the fetched
register through max (which passes bits unchanged) so missing components show
their defaults."""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import xenos as x  # noqa: E402

VERTICES = 1024

INTEGER_FORMATS = [('8_8_8_8', 6), ('2_10_10_10', 7), ('10_11_11', 16),
                   ('11_11_10', 17), ('16_16', 25), ('16_16_16_16', 26),
                   ('32', 33), ('32_32', 34), ('32_32_32_32', 35)]
FLOAT_FORMATS = [('16_16_FLOAT', 31), ('16_16_16_16_FLOAT', 32),
                 ('32_FLOAT', 36), ('32_32_FLOAT', 37),
                 ('32_32_32_32_FLOAT', 38), ('32_32_32_FLOAT', 57)]
# (name, signed, normalized, rf_mode)
MODES = [('unorm', False, True, 0), ('snorm', True, True, 0),
         ('snorm nozero', True, True, 1), ('uint', False, False, 0),
         ('sint', True, False, 0)]
EXP_ADJUSTS = [('16_16', 25, 'unorm', 3), ('16_16', 25, 'unorm', -5),
               ('8_8_8_8', 6, 'sint', -2), ('16_16_FLOAT', 31, 'float', 4)]

PATTERNS = [
    0x00000000, 0xFFFFFFFF, 0x80008000, 0x7FFF7FFF, 0x80808080, 0x7F7F7F7F,
    0x01010101, 0x81818181, 0x00010001, 0xFFFEFFFE, 0x7C00FC00, 0x7E007C01,
    0x00018001, 0x3C00BC00, 0x7BFFFBFF, 0x03FF83FF, 0x20080200, 0xA0080200,
    0x60180600, 0x1FF7FDFF, 0x3FFFFFFF, 0xC0000000, 0x40000000, 0x00000400,
    0x000007FF, 0x003FF800, 0xFFC00000, 0x00200000, 0x00000200, 0x0007FC00,
    0x7F800000, 0xFF800000, 0x7FC00000, 0x00000001, 0x80000001, 0x7F7FFFFF,
    0x3F800000, 0xBF800000, 0x12345678, 0x9ABCDEF0,
]


def inputs(rng):
    """Vertex v < len(PATTERNS) starts with pattern v, so each pattern takes
    every dword of a vertex once; random words after."""
    words = []
    for v in range(VERTICES):
        if v < len(PATTERNS):
            words += [PATTERNS[(v + i) % len(PATTERNS)] for i in range(4)]
        else:
            words += [rng.getrandbits(32) for _ in range(4)]
    return words


def fetch_test(suite, name, fmt, signed, normalized, rf_mode, exp_adjust=0):
    fetch = x.vfetch(gpugen.INPUT_REGISTER, 0, 'x', 0, 0, fmt, 4,
                     signed=signed, normalized=normalized, rf_mode=rf_mode,
                     exp_adjust=exp_adjust, prefetch=4)
    move = gpugen.export_copy(gpugen.INPUT_REGISTER)
    suite.test(name, gpugen.memexport_shader(4, [], [[move]], fetches=[fetch]))


def build(suite):
    rng = random.Random(0xF3A7)
    suite.set_inputs(inputs(rng), 4)
    for fname, fmt in INTEGER_FORMATS:
        for mname, signed, normalized, rf_mode in MODES:
            fetch_test(suite, f'{fname} {mname}', fmt, signed, normalized,
                       rf_mode)
    for fname, fmt in FLOAT_FORMATS:
        # Direct3D's bits for float data: signed, not normalized.
        fetch_test(suite, fname, fmt, True, False, 0)
        fetch_test(suite, f'{fname} unsigned normalized', fmt, False, True, 0)
    modes = {m[0]: m[1:] for m in MODES}
    modes['float'] = (True, False, 0)
    for fname, fmt, mname, adjust in EXP_ADJUSTS:
        signed, normalized, rf_mode = modes[mname]
        fetch_test(suite, f'{fname} {mname} exp {adjust:+d}', fmt, signed,
                   normalized, rf_mode, adjust)
