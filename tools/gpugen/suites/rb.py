"""Render target formats: a pixel shader writes chosen float4 values into each
color render target format, then Direct3D's Resolve copies the 64x32 render
target to a tiled texture in memory, converted to the matching texture format
and raw (copy command 0, the EDRAM bits).

The pixel shader fetches texel (x, y) of a 64x32 k_32_32_32_32_FLOAT texture
at its pixel position (PsParamGen.xy, unnormalized + 0.5) and outputs it as
oC0, so the render backend receives exact float4s. Texel i = x + 64y:
- 'ramp': R = -0.125 + i * 5 / 2^13, up to about 1.125;
  G = (i // 8 + (-0.5, -0.5 + 2^-12, -0.25, 0, 0.25, 0.5 - 2^-12, 0.5,
  0.5 + 2^-12)[i % 8]) / 255; B = (i // 2 + (0.5 - 2^-14, 0.5)[i % 2]) / 1023;
  A = SPECIALS, then random floats.
- 'random': random floats in every channel, exponents -30 to 20, both signs,
  the first texels SPECIALS.
The draw is one rectangle list over the render target from Direct3D's Resolve
vertex shader (the same vertex buffer serves the Resolve).

The other render backend suites draw through the same setup (registers(),
test()).
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gpugen  # noqa: E402
import pm4  # noqa: E402
import xenos as x  # noqa: E402

WIDTH, HEIGHT = 64, 32
SLOT = 1
K = gpugen.C_FIRST_FREE  # pixel c8: (0.5, ...)
F32_32_32_32 = 38
f2w = gpugen.f2w
CONSTANTS = {256 + K: [f2w(0.5), 0, 0, 0]}
# The rectangle as Direct3D's Resolve passes it: (L, T), (R, T), (R, B) less
# the half-pixel offset, the first input dwords.
RECT = [f2w(v) for v in (-0.5, -0.5, WIDTH - 0.5, -0.5, WIDTH - 0.5,
                         HEIGHT - 0.5)]
RECT_DRAW = (pm4.PRIM_RECTANGLE_LIST, 0, 3)
# The depth buffer, in the EDRAM tiles after the color buffer's:
# stencil_enable, z_enable, z_write_enable, zfunc ALWAYS, stencilfunc ALWAYS,
# stencilzpass REPLACE, reference 0x5A.
DEPTH_BASE = 0x10
DEPTHCONTROL = 0x1 | 0x2 | 0x4 | (7 << 4) | (7 << 8) | (2 << 14)
STENCILREFMASK = 0x00FFFF00 | 0x5A

SPECIALS = [0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000, 0x00000000,
            0x80000000, 0x3F800000, 0xBF800000, 0x3F000000, 0x40000000,
            0x41FF0000, 0x42000000, 0x42040000, 0x501502F9, 0xD01502F9,
            0x477FE000, 0x477FEF00, 0x477FF000, 0x47800000, 0x322BCC77,
            0x38800000, 0x38000000, 0x33800000, 0x33000000, 0x00800000,
            0x00000001, 0x80000001, 0x007FFFFF, 0x3E800000, 0x3F400000,
            0x3EAAAAAB, 0x3F2AAAAB, 0x3F800347, 0x3F7FF972, 0x3F7F7F80,
            0x3F808081, 0x3E000000, 0x3D800000, 0x3D000000, 0x3C800000,
            0x3C000000, 0x3B800000, 0x41FC0000, 0x41FE0000, 0x41FF8000,
            0x41FFC000, 0x3E010000, 0x3E008000, 0x3DFF0000, 0x3FC00000,
            0xBF000000, 0xC2000000, 0x7F7FFFFF, 0xFF7FFFFF, 0x3F7FFFFF,
            0x3F800001, 0x3B808081, 0x3B000000, 0x3A800000, 0x3C010204,
            0x3F7EFEFF, 0x3F7F0000, 0x3F7F7F7F, 0x3F7F8080]

# (name, ColorRenderTargetFormat, bytes per pixel, resolve texture format,
# number format)
FORMATS = [('8_8_8_8', 0, 4, 6, 0), ('8_8_8_8_GAMMA', 1, 4, 6, 0),
           ('2_10_10_10', 2, 4, 7, 0), ('2_10_10_10_FLOAT', 3, 4, 32, 7),
           ('16_16', 4, 4, 25, 0), ('16_16_16_16', 5, 8, 26, 0),
           ('16_16_FLOAT', 6, 4, 31, 7), ('16_16_16_16_FLOAT', 7, 8, 32, 7),
           ('2_10_10_10_AS_10_10_10_10', 10, 4, 7, 0),
           ('2_10_10_10_FLOAT_AS_16_16_16_16', 12, 4, 32, 7),
           ('32_FLOAT', 14, 4, 36, 7), ('32_32_FLOAT', 15, 8, 37, 7)]
# Bytes per texel of the resolve texture formats.
TEXEL_BYTES = {6: 4, 7: 4, 25: 4, 26: 8, 31: 4, 32: 8, 36: 4, 37: 8}


def random_float(rng):
    sign = rng.getrandbits(1) << 31
    exponent = rng.randint(127 - 30, 127 + 20) << 23
    return sign | exponent | rng.getrandbits(23)


def ramp_texture(rng):
    words = []
    offsets = (-0.5, -0.5 + 2 ** -12, -0.25, 0, 0.25, 0.5 - 2 ** -12, 0.5,
               0.5 + 2 ** -12)
    for i in range(WIDTH * HEIGHT):
        r = -0.125 + i * 5 / 2 ** 13
        g = (i // 8 + offsets[i % 8]) / 255
        b = (i // 2 + (0.5 - 2 ** -14, 0.5)[i % 2]) / 1023
        a = SPECIALS[i] if i < len(SPECIALS) else random_float(rng)
        words += [f2w(r), f2w(g), f2w(b), a]
    return words


def random_texture(rng):
    words = []
    for i in range(WIDTH * HEIGHT):
        if i < len(SPECIALS):
            words += [SPECIALS[(i + k * 16) % len(SPECIALS)] for k in range(4)]
        else:
            words += [random_float(rng) for _ in range(4)]
    return words


def fill(value):
    """A texture of the float4 `value` everywhere."""
    return [f2w(v) for _ in range(WIDTH * HEIGHT) for v in value]


def fetch(texture):
    """The pixel shader's fetch constant, for the named texture."""
    return {SLOT: (gpugen.texture_constant(F32_32_32_32, WIDTH, HEIGHT),
                   texture)}


def pixel_shader(depth=False):
    """oC0 = texel (|PsParamGen.xy| + 0.5), unnormalized, and with `depth`
    oDepth = its X."""
    s = x.Shader()
    s.exec_([x.alu(vector_op=x.V_ADD, vector_dest=1, vector_mask=0x3,
                   src1=x.r(0, absolute=True), src2=x.c(K, 'xxxx'))])
    s.exec_([x.tfetch(2, 1, 'xy', SLOT, denormalized=True).serialized()])
    s.alloc(x.ALLOC_COLORS, 0)
    exports = [x.alu(vector_op=x.V_MAX, vector_dest=0, vector_mask=0xF,
                     export=True, src1=x.r(2), src2=x.r(2))]
    if depth:
        exports.append(x.alu(vector_op=x.V_MAX, vector_dest=x.EXPORT_DEPTH,
                             vector_mask=0xF, export=True,
                             src1=x.r(2, 'xxxx'), src2=x.r(2, 'xxxx')))
    s.exec_(exports, end=True)
    return s


# 3 VS and 4 PS registers, vertex and pixel fetches, PsParamGen, Direct3D's
# PS export mode.
PROGRAM_CNTL = 2 | (3 << 8) | (1 << 16) | (1 << 17) | (1 << 18) | (2 << 27)


def registers(rt_format=0, samples=0, depth_format=None, mode_cntl=None):
    """The 64x32 render target's registers: `rt_format` at `samples` (0, 1, 2
    for 1x, 2x, 4x), a depth buffer of `depth_format` if given, and
    PA_SU_SC_MODE_CNTL `mode_cntl` if given, with msaa_enable when
    multisampled."""
    depth = depth_format is not None
    state = [('SQ_CONTEXT_MISC', [gpugen.SCREEN_XY]),
             ('RB_SURFACE_INFO', [80 | (samples << 16)]),
             ('RB_COLOR_INFO', [rt_format << 16])]
    if depth:
        state.append(('RB_DEPTH_INFO', [DEPTH_BASE | (depth_format << 16)]))
    state.append(('RB_COLOR_MASK', [0xF]))
    if depth:
        state += [('RB_DEPTHCONTROL', [DEPTHCONTROL]),
                  ('RB_STENCILREFMASK', [STENCILREFMASK])]
    state += [('PA_SC_WINDOW_SCISSOR_BR', [(HEIGHT << 16) | WIDTH]),
              ('PA_SC_AA_CONFIG', [gpugen.AA_CONFIG[samples]])]
    if mode_cntl is not None:
        state.append(('PA_SU_SC_MODE_CNTL', [
            mode_cntl | (gpugen.MSAA_ENABLE if samples else 0)]))
    return state


def test(suite, name, texture, state, resolve, draw=RECT_DRAW, passes=(),
         after_passes=(), shader=None, pixel=None,
         program_cntl=PROGRAM_CNTL):
    """A test drawing `texture` through `pixel` (pixel_shader() by default)
    into the render target `state` sets up, then resolving it; `shader` is
    the vertex shader, Direct3D's Resolve one by default."""
    suite.test(name, shader or gpugen.resolve_vertex_shader(),
               constants=CONSTANTS, pixel_shader=pixel or pixel_shader(),
               program_cntl=program_cntl, registers=state, passes=passes,
               after_passes=after_passes, fetch_constants=fetch(texture),
               draw=draw, resolve=resolve)


def build(suite):
    rng = random.Random(0x4B0)
    suite.set_inputs(RECT, 2)
    suite.add_texture('ramp', ramp_texture(rng))
    suite.add_texture('random', random_texture(rng))
    for name, rt_format, bpp, dest_format, number in FORMATS:
        state = registers(rt_format)
        for texture in ('ramp', 'random'):
            test(suite, f'{name} {texture}', texture, state,
                 gpugen.resolve_registers(WIDTH, HEIGHT, dest_format,
                                          TEXEL_BYTES[dest_format], number))
        # The EDRAM bits: copy command 0 to a texture of the pixel's size.
        test(suite, f'{name} raw', 'ramp', state, gpugen.resolve_registers(
            WIDTH, HEIGHT, 6 if bpp == 4 else 26, bpp, command=0))
