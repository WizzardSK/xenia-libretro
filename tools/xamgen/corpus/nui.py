"""NUI corpus: XNuiTitleInitialize (XMsgInProcessCall 0xFE/0x2B003) and
XNuiTitleShutdown (0x2B004), read back through XamNuiHudGetInitializeFlags and
XamNuiHudGetVersions.

Every scenario shuts NUI down at the end, so each starts from the cleared
state whatever ran before it. The flags always carry 0x40000000. Without it
xam checks the device status and can open the Kinect troubleshooter
(816F9D7C in 17559).
"""

import struct

XAM_APP = 0xFE
TITLE_INITIALIZE = 0x2B003
TITLE_SHUTDOWN = 0x2B004

XAM_VERSION = 5
# What Forza Motorsport 4 (4D5309C9) passes.
XTL_VERSION = 0x511C
FM4_FLAGS = 0xC0000830

INITIALIZES = (
    ('fm4 flags', XAM_VERSION, FM4_FLAGS),
    ('bit 31 clear', XAM_VERSION, FM4_FLAGS & ~0x80000000),
    ('version 4', 4, FM4_FLAGS),
    ('version 4 bit 31 clear', 4, FM4_FLAGS & ~0x80000000),
)


def read_state(s, when):
    s.call('XamNuiHudGetInitializeFlags', name=f'flags {when}')
    xam_version = s.alloc(8, 'xam version')
    xtl_version = s.alloc(8, 'xtl version')
    s.call('XamNuiHudGetVersions', xam_version, xtl_version,
           name=f'versions {when}', capture=[xam_version, xtl_version])


def build(corpus):
    s = corpus.scenario('nui initial state')
    read_state(s, 'at start')

    for label, xam_version, flags in INITIALIZES:
        s = corpus.scenario(f'title initialize {label}')
        data = struct.pack('>QQI', xam_version, XTL_VERSION, flags)
        # Guard bytes past the 0x18 bytes show whether xam writes back.
        buffer = s.alloc(0x20, 'buffer', data)
        s.call('XMsgInProcessCall', XAM_APP, TITLE_INITIALIZE, buffer, 0,
               name='initialize', capture=[buffer])
        read_state(s, 'after initialize')
        s.call('XMsgInProcessCall', XAM_APP, TITLE_SHUTDOWN, 0, 0,
               name='shutdown')
        read_state(s, 'after shutdown')
