#!/usr/bin/env python3
"""Encodes Xenos shader microcode for test titles.

The layouts follow src/xenia/gpu/ucode.h. A shader is control flow
instructions, 48 bits each and packed in pairs into three dwords, followed by
fetch and ALU instructions of three dwords each, which exec instructions
address in units of three dwords from the start of the shader.

    s = Shader()
    s.exec_([vfetch(...), alu(...)])
    s.alloc(ALLOC_MEMORY, 1)
    s.exec_([alu(...)], end=True)
    words = s.words()
"""

# Control flow opcodes.
CF_NOP = 0
CF_EXEC = 1
CF_EXEC_END = 2
CF_ALLOC = 12

# Alloc types.
ALLOC_POSITION = 1
ALLOC_INTERPOLATORS = 2
ALLOC_COLORS = 2
ALLOC_MEMORY = 3

# Fetch opcodes.
FETCH_VERTEX = 0
FETCH_TEXTURE = 1
FETCH_BORDER_COLOR_FRAC = 16

# Vertex formats used here (xenos::VertexFormat).
VF_32_32_FLOAT = 37
VF_32_32_32_32_FLOAT = 38
VF_32_32_32_FLOAT = 57

# Vector ALU opcodes (ucode.h AluVectorOpcode).
V_ADD = 0
V_MUL = 1
V_MAX = 2
V_MIN = 3
V_SEQ = 4
V_SGT = 5
V_SGE = 6
V_SNE = 7
V_FRC = 8
V_TRUNC = 9
V_FLOOR = 10
V_MAD = 11
V_CNDEQ = 12
V_CNDGE = 13
V_CNDGT = 14
V_DP4 = 15
V_DP3 = 16
V_DP2ADD = 17
V_CUBE = 18
V_MAX4 = 19
V_DST = 28
V_MAXA = 29

# Scalar ALU opcodes (ucode.h AluScalarOpcode).
S_ADDS = 0
S_ADDS_PREV = 1
S_MULS = 2
S_MULS_PREV = 3
S_MULS_PREV2 = 4
S_MAXS = 5
S_MINS = 6
S_SEQS = 7
S_SGTS = 8
S_SGES = 9
S_SNES = 10
S_FRCS = 11
S_TRUNCS = 12
S_FLOORS = 13
S_EXP = 14
S_LOGC = 15
S_LOG = 16
S_RCPC = 17
S_RCPF = 18
S_RCP = 19
S_RSQC = 20
S_RSQF = 21
S_RSQ = 22
S_MAXAS = 23
S_MAXASF = 24
S_SUBS = 25
S_SUBS_PREV = 26
S_SETP_EQ = 27
S_SETP_NE = 28
S_SETP_GT = 29
S_SETP_GE = 30
S_SETP_INV = 31
S_SETP_POP = 32
S_SETP_CLR = 33
S_SETP_RSTR = 34
S_SQRT = 40
S_MULSC0 = 42
S_SIN = 48
S_COS = 49
S_RETAIN_PREV = 50

# Export registers.
EXPORT_ADDRESS = 32
EXPORT_DATA0 = 33
EXPORT_DEPTH = 61
EXPORT_POSITION = 62


def swizzle(components):
    """ALU source swizzle from absolute components, e.g. 'xyzw' or 'wwww'.
    Each 2-bit field is relative to the component it feeds."""
    if len(components) == 1:
        components *= 4
    value = 0
    for i, c in enumerate(components):
        absolute = 'xyzw'.index(c)
        value |= ((absolute - i) & 3) << (2 * i)
    return value


def fetch_swizzle(components):
    """Vertex fetch destination swizzle, absolute 3-bit fields: xyzw, 0, 1 or
    _ to keep."""
    codes = {'x': 0, 'y': 1, 'z': 2, 'w': 3, '0': 4, '1': 5, '_': 7}
    value = 0
    for i, c in enumerate(components):
        value |= codes[c] << (3 * i)
    return value


class Instruction:
    """A fetch or ALU instruction, three dwords."""

    def __init__(self, words, is_fetch, serialize=False,
                 is_vertex_fetch=False):
        self.words = tuple(w & 0xFFFFFFFF for w in words)
        self.is_fetch = is_fetch
        self.serialize = serialize
        self.is_vertex_fetch = is_vertex_fetch

    def serialized(self):
        """A copy that waits for the instructions before it."""
        return Instruction(self.words, self.is_fetch, True,
                           self.is_vertex_fetch)


def vfetch(dst, src, src_component, const_index, const_sel, fmt, stride,
           offset=0, dst_swizzle='xyzw', signed=True, normalized=False,
           rf_mode=0, exp_adjust=0, mini=False, prefetch=None,
           index_rounded=False):
    """vfetch_full (or mini) of `fmt` into r`dst`, indexed by r`src`'s
    `src_component`, from vertex fetch constant (const_index, const_sel),
    `stride` and `offset` in dwords. `signed`, `normalized` and `rf_mode`
    (signed repeating fraction: 0 clamps to -1, 1 has no zero) are the
    format_comp_all, num_format_all (0 = normalized) and signed_rf_mode_all
    bits; the defaults are Direct3D's for float data. Full fetches default to
    the prefetch count Direct3D shaders use. `index_rounded` rounds the index
    instead of truncating it."""
    if prefetch is None:
        prefetch = 1 if mini else 8
    w0 = (FETCH_VERTEX | (src << 5) | (dst << 12) | (1 << 19) |
          (const_index << 20) | (const_sel << 25) |
          (((prefetch - 1) & 7) << 27) | ('xyzw'.index(src_component) << 30))
    w1 = (fetch_swizzle(dst_swizzle) |
          (int(signed) << 12) |
          ((0 if normalized else 1) << 13) |
          (rf_mode << 14) |
          (int(index_rounded) << 15) |
          (fmt << 16) |
          ((exp_adjust & 0x3F) << 24) |
          ((1 if mini else 0) << 30))
    w2 = (stride & 0xFF) | ((offset & 0x7FFFFF) << 8)
    return Instruction((w0, w1, w2), True, is_vertex_fetch=True)


def tfetch(dst, src, src_swizzle, const_index, dimension=1, dst_swizzle='xyzw',
           mag_filter=3, min_filter=3, mip_filter=3, aniso_filter=7,
           vol_mag_filter=3, vol_min_filter=3,
           denormalized=False, fetch_valid_only=True, use_reg_lod=False,
           use_comp_lod=True, lod_bias=0, offsets=(0, 0, 0),
           sample_location=0, unk=0, opcode=FETCH_TEXTURE):
    """tfetch (1D/2D/3D/cube by `dimension`) into r`dst` from r`src`'s
    `src_swizzle` (absolute, up to three components) with texture fetch
    constant `const_index`. Filters default to the fetch constant's (3, aniso
    7), as Direct3D's shaders do. `opcode` gives the other texture fetches
    the same fields, such as getBCF."""
    swz = 0
    for i, comp in enumerate(src_swizzle.ljust(3, 'x')):
        swz |= 'xyzw'.index(comp) << (2 * i)
    w0 = (opcode | (src << 5) | (dst << 12) |
          (int(fetch_valid_only) << 19) | (const_index << 20) |
          (int(denormalized) << 25) | (swz << 26))
    w1 = (fetch_swizzle(dst_swizzle) | (mag_filter << 12) |
          (min_filter << 14) | (mip_filter << 16) | (aniso_filter << 18) |
          (vol_mag_filter << 24) | (vol_min_filter << 26) |
          (int(use_comp_lod) << 28) | (int(use_reg_lod) << 29) | (unk << 30))
    w2 = (sample_location << 1) | ((lod_bias & 0x7F) << 2) | (dimension << 14)
    for i, offset in enumerate(offsets):
        w2 |= (offset & 0x1F) << (16 + 5 * i)
    return Instruction((w0, w1, w2), True)


class Source:
    """An ALU source: a temporary register or a float constant."""

    def __init__(self, index, is_temp, components='xyzw', negate=False,
                 absolute=False):
        self.index = index
        self.is_temp = is_temp
        self.swizzle = swizzle(components)
        self.negate = negate
        self.absolute = absolute

    def reg_byte(self):
        if not self.is_temp:
            return self.index & 0xFF
        return (self.index & 0x3F) | (0x80 if self.absolute else 0)


def r(index, components='xyzw', negate=False, absolute=False):
    return Source(index, True, components, negate, absolute)


def c(index, components='xyzw', negate=False):
    return Source(index, False, components, negate)


def alu(vector_op=V_ADD, vector_dest=0, vector_mask=0, scalar_op=S_RETAIN_PREV,
        scalar_dest=0, scalar_mask=0, src1=None, src2=None, src3=None,
        export=False, abs_constants=False, serialize=False,
        scalar_const_temp=None, a0_relative=False):
    """An ALU instruction. Exports write both results to `vector_dest`.
    `scalar_const_temp` is (temp register, component) for mulsc/addsc/subsc,
    whose constant operand is src3 (its W-relative swizzle picks the
    component). `a0_relative` makes the constant sources read c[a0 + index]
    (const_address_register_relative with both constant addressing bits)."""
    # Unused sources read c0, like the assembler leaves them.
    sources = [src1 or c(0), src2 or c(0), src3 or c(0)]
    w0 = ((vector_dest & 0x3F) | ((1 if abs_constants else 0) << 7) |
          ((scalar_dest & 0x3F) << 8) | ((1 if export else 0) << 15) |
          (vector_mask << 16) | (scalar_mask << 20) | (scalar_op << 26))
    src3_swizzle = sources[2].swizzle
    src3_sel = int(sources[2].is_temp)
    if scalar_const_temp is not None:
        temp, component = scalar_const_temp
        if scalar_op & 1:
            raise ValueError('pass the even (0) form of the scalar opcode')
        scalar_op_bit = temp & 1
        src3_sel = (temp >> 1) & 1
        # Bits 2-5 hold the rest of the temp index, bits 0-1 its component
        # (relative to X), bits 6-7 the constant's component (relative to W).
        src3_swizzle = ((temp & 0x3C) | 'xyzw'.index(component) |
                        (((sources[2].swizzle >> 6) & 3) << 6))
        w0 = (w0 & ~(0x3F << 26)) | ((scalar_op | scalar_op_bit) << 26)
    w1 = (src3_swizzle | (sources[1].swizzle << 8) |
          (sources[0].swizzle << 16) | (int(sources[2].negate) << 24) |
          (int(sources[1].negate) << 25) | (int(sources[0].negate) << 26))
    if a0_relative:
        w1 |= (1 << 29) | (1 << 30) | (1 << 31)
    w2 = (sources[2].reg_byte() | (sources[1].reg_byte() << 8) |
          (sources[0].reg_byte() << 16) | (vector_op << 24) |
          (src3_sel << 29) | (int(sources[1].is_temp) << 30) |
          (int(sources[0].is_temp) << 31))
    return Instruction((w0, w1, w2), False, serialize)


class Shader:
    def __init__(self):
        self.cf = []   # (kind, args)
        self.instructions = []

    def exec_(self, instructions, end=False):
        """One exec per up to six instructions. An empty one with `end`
        only ends the shader, as Direct3D's do after the last alloc."""
        if not instructions:
            self.cf.append(('exec', (0, [], end)))
            return
        for start in range(0, len(instructions), 6):
            chunk = instructions[start:start + 6]
            last = start + 6 >= len(instructions)
            self.cf.append(('exec', (len(self.instructions), chunk,
                                     end and last)))
            self.instructions += chunk

    def alloc(self, kind, size):
        self.cf.append(('alloc', (kind, size)))

    def words(self):
        cf_count = len(self.cf) + (len(self.cf) & 1)
        base = cf_count // 2  # each pair takes one instruction slot
        cf48 = []
        for kind, args in self.cf:
            if kind == 'exec':
                index, chunk, end = args
                sequence = 0
                # One bit per vertex fetch, as in Direct3D's shaders.
                vertex_fetches = 0
                for i, ins in enumerate(chunk):
                    bits = int(ins.is_fetch) | (int(ins.serialize) << 1)
                    sequence |= bits << (2 * i)
                    vertex_fetches |= int(ins.is_vertex_fetch) << i
                lo = (((base + index) & 0xFFF) | (len(chunk) << 12) |
                      (sequence << 16) | ((vertex_fetches & 0xF) << 28))
                hi = ((vertex_fetches >> 4) | (1 << 9) |
                      ((CF_EXEC_END if end else CF_EXEC) << 12))
            else:
                alloc_kind, size = args
                lo = size & 7
                hi = (alloc_kind << 9) | (CF_ALLOC << 12)
            cf48.append((lo, hi))
        if len(cf48) & 1:
            cf48.append((0, CF_NOP << 12))
        out = []
        for i in range(0, len(cf48), 2):
            (a_lo, a_hi), (b_lo, b_hi) = cf48[i], cf48[i + 1]
            out += [a_lo, a_hi | ((b_lo & 0xFFFF) << 16),
                    (b_lo >> 16) | (b_hi << 16)]
        for ins in self.instructions:
            out += ins.words
        # Direct3D loads one more, zero instruction slot after the program.
        return out + [0, 0, 0]
