"""Smoke corpus for the xamgen executor: plain returns, outputs into
sentinel-filled buffers, guard bytes around a buffer and an overlapped wait."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import identity  # noqa: E402

USER_INDICES = (0, 1, 2, 3, 4, 0xFD, 0xFE, 0xFF)
HDD = 1


def build(corpus):
    identity(corpus)
    s = corpus.scenario('signin state')
    for index in USER_INDICES:
        s.call('XamUserGetSigninState', index, name=f'user {index:#x}')

    for index in (0, 1, 4, 0xFF):
        for mask in (1, 2, 7):
            s = corpus.scenario(f'xuid user {index:#x} mask {mask}')
            xuid = s.alloc(8, 'xuid')
            s.call('XamUserGetXUID', index, mask, xuid, capture=[xuid])
    # xam stores through the pointer unchecked (816D8580).
    s = corpus.scenario('xuid null')
    s.call('XamUserGetXUID', 0, 7, 0, faults=True)

    for index, size in ((0, 16), (0, 4), (0, 0), (1, 16)):
        s = corpus.scenario(f'name user {index:#x} size {size}')
        buffer = s.alloc(0x20, 'buffer')
        s.call('XamUserGetName', index, buffer + 8, size, capture=[buffer])

    for capacity in (0, 1, 4, 64):
        s = corpus.scenario(f'device name capacity {capacity}')
        buffer = s.alloc(8 + 2 * 64 + 8, 'buffer')
        s.call('XamContentGetDeviceName', HDD, buffer + 8, capacity,
               capture=[buffer])
    s = corpus.scenario('device name unknown device')
    buffer = s.alloc(8 + 2 * 64 + 8, 'buffer')
    s.call('XamContentGetDeviceName', 0x55, buffer + 8, 64, capture=[buffer])

    for device in (HDD, 0x55):
        s = corpus.scenario(f'device state {device:#x}')
        s.call('XamContentGetDeviceState', device, 0, name='sync')
        overlapped = s.alloc(0x1C, 'overlapped', fill=0)
        s.call('XamContentGetDeviceState', device, overlapped,
               name='overlapped', wait=overlapped, capture=[overlapped])
