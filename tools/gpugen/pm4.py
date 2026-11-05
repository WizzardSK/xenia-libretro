#!/usr/bin/env python3
"""Builds PM4 command streams for the Xenos command processor.

Register indices come from Xenia's src/xenia/gpu/register_table.inc, packet
layouts from src/xenia/gpu/pm4_command_processor_implement.h.

    s = Stream()
    s.reg('SQ_PROGRAM_CNTL', 0x...)
    s.im_load_immediate(VERTEX, shader_words)
    s.draw_auto(PRIM_POINT_LIST, 64)
    words = s.words
"""

import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))

# Type 3 opcodes.
ME_INIT = 0x48
WAIT_REG_MEM = 0x3C
EVENT_WRITE = 0x46
EVENT_WRITE_SHD = 0x58
DRAW_INDX_2 = 0x36
IM_LOAD_IMMEDIATE = 0x2B
INVALIDATE_STATE = 0x3B
REG_RMW = 0x21
SET_BIN_MASK_LO = 0x60

VERTEX = 0
PIXEL = 1

# VGT_DRAW_INITIATOR.
PRIM_POINT_LIST = 1
PRIM_LINE_LIST = 2
PRIM_TRIANGLE_LIST = 4
PRIM_RECTANGLE_LIST = 8
SOURCE_AUTO_INDEX = 2

ENDIAN_NONE = 0
ENDIAN_8IN16 = 1
ENDIAN_8IN32 = 2
ENDIAN_16IN32 = 3


def _load_registers():
    names = {}
    pattern = re.compile(
        r'^XE_GPU_REGISTER\((0x[0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)', re.M)
    with open(os.path.join(REPO, 'src', 'xenia', 'gpu',
                           'register_table.inc')) as f:
        for match in pattern.finditer(f.read()):
            names.setdefault(match.group(2), int(match.group(1), 16))
    return names


REGISTERS = _load_registers()


def register(name):
    return REGISTERS[name] if isinstance(name, str) else name


def type0(index, count):
    return ((count - 1) << 16) | index


def type3(opcode, count):
    return (3 << 30) | ((count - 1) << 16) | (opcode << 8)


class Stream:
    """A command stream. Addresses the GPU uses are physical; a relocation
    (dword index, shift) records a dword that the executor adds the block's
    physical address, shifted right by `shift`, to at run time."""

    def __init__(self):
        self.words = []
        self.relocations = []  # (dword index, shift)
        self.segments = []     # end of each submission, in dwords

    def packet3(self, opcode, payload):
        self.words.append(type3(opcode, len(payload)))
        self.words += [w & 0xFFFFFFFF for w in payload]

    def reg(self, name, *values):
        index = register(name)
        self.words.append(type0(index, len(values)))
        self.words += [v & 0xFFFFFFFF for v in values]

    def im_load_immediate(self, shader_type, words):
        self.packet3(IM_LOAD_IMMEDIATE, [shader_type, len(words), *words])

    def draw_auto(self, primitive, count):
        initiator = primitive | (SOURCE_AUTO_INDEX << 6) | (count << 16)
        self.packet3(DRAW_INDX_2, [initiator])
