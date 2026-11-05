"""User corpus: argument checks and empty-slot results of the user exports,
content restrictions, profile setting reads by XUID, gamer tile keys and the
online language and country.

Slot 0 is the signed-in profile and slot 1 is empty. XamWriteGamerTile awards
a picture to the profile, so it's only called with an index past the last
user and tile ids that don't exist.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import identity  # noqa: E402

# XPROFILE_GAMER_YAXIS_INVERSION, an int32.
SETTING = 0x10040002
SETTING_BUFFER_SIZE = 0x300

WSTRING = 4
INT32 = 1

# InternalLow, InternalHigh and dwExtendedError hold sentinels to show what's
# written, the event and completion routine are null.
OVERLAPPED = struct.pack('>7I', 0xCDCDCDCD, 0xCDCDCDCD, 0, 0, 0, 0,
                         0xCDCDCDCD)


def overlapped(s, name='overlapped'):
    return s.alloc(len(OVERLAPPED), name, OVERLAPPED)


def wide(text):
    return text.encode('utf-16-be') + b'\0\0'


def build(corpus):
    identity(corpus)

    s = corpus.scenario('signin info')
    for index in (1, 4):
        info = s.alloc(0x28, 'info')
        s.call('XamUserGetSigninInfo', index, 0, info, capture=[info],
               name=f'user {index:#x}')

    s = corpus.scenario('content restriction flags')
    for index in (0, 1, 4, 0xFF):
        flags = s.alloc(4, 'flags')
        s.call('XamUserContentRestrictionGetFlags', index, flags,
               capture=[flags], name=f'user {index:#x}')

    # Without parental controls only type 3 (explicit video) has a rating.
    s = corpus.scenario('content restriction rating')
    for index in (0, 1, 4, 0xFF):
        for content_type in (0, 1, 2, 3, 4, 5):
            rating = s.alloc(4, 'rating')
            descriptors = s.alloc(4, 'descriptors')
            s.call('XamUserContentRestrictionGetRating', index, content_type,
                   rating, descriptors, capture=[rating, descriptors],
                   name=f'user {index:#x} type {content_type}')

    s = corpus.scenario('content restriction check access')
    for content_type in (0, 1):
        access = s.alloc(4, 'access')
        s.call('XamUserContentRestrictionCheckAccess', 0, 0, content_type, 0,
               0, access, 0, capture=[access], name=f'type {content_type}')
        access = s.alloc(4, 'access')
        ov = overlapped(s)
        s.call('XamUserContentRestrictionCheckAccess', 0, 0, content_type, 0,
               0, access, ov, wait=ov, capture=[access, ov],
               name=f'type {content_type} overlapped')
    # Modes 1 and 2 ask for the passcode when access is denied, which can't
    # happen here as the title has no rating.
    for mode in (1, 2, 3, 4):
        access = s.alloc(4, 'access')
        ov = overlapped(s)
        s.call('XamUserContentRestrictionCheckAccess', 0, mode, 1, 0, 0,
               access, ov, wait=ov, capture=[access, ov],
               name=f'mode {mode} overlapped')
    for index in (4, 0xFF):
        access = s.alloc(4, 'access')
        s.call('XamUserContentRestrictionCheckAccess', index, 0, 1, 0, 0,
               access, 0, capture=[access], name=f'user {index:#x}')
    for index in (0, 1):
        access = s.alloc(4, 'access')
        s.call('XamUserContentRestrictionCheckAccess', index, 0, 3, 7, 1,
               access, 0, capture=[access], name=f'user {index:#x} type 3')
    s.call('XamUserContentRestrictionCheckAccess', 0, 0, 1, 0, 0, 0, 0,
           name='null access')

    # The XUID array holds the profile's offline XUID and zeros. The calls
    # are synchronous.
    s = corpus.scenario('read profile settings')
    xuids = s.alloc(17 * 8, 'xuids', fill=0)
    s.call('XamUserGetXUID', 0, 1, xuids, name='own xuid')
    setting_ids = s.alloc(4, 'setting ids', struct.pack('>I', SETTING))
    size = s.alloc(4, 'buffer size')
    buffer = s.alloc(SETTING_BUFFER_SIZE, 'buffer')
    for xuid_count, buffer_size in ((1, SETTING_BUFFER_SIZE),
                                    (0, SETTING_BUFFER_SIZE),
                                    (17, SETTING_BUFFER_SIZE), (0, 0),
                                    (17, 0)):
        s.write(size, struct.pack('>I', buffer_size))
        s.write(buffer, bytes([0xCD]) * SETTING_BUFFER_SIZE)
        s.call('XamUserReadProfileSettings', 0, 0, xuid_count, xuids, 1,
               setting_ids, size, buffer, 0, capture=[size, buffer],
               name=f'{xuid_count} xuids buffer {buffer_size:#x}')

    s = corpus.scenario('subscription type')
    for index, null in ((4, False), (1, False), (0, True), (0, False)):
        length = s.alloc(4, 'length')
        payment = s.alloc(4, 'payment')
        s.call('XamUserGetSubscriptionType', index, 0 if null else length,
               0 if null else payment, capture=[length, payment],
               name=f'user {index:#x}{" null outputs" if null else ""}')

    s = corpus.scenario('user tenure')
    for index in (1, 4, 0xFF):
        level = s.alloc(4, 'level')
        milestone = s.alloc(4, 'milestone')
        date = s.alloc(8, 'milestone date')
        s.call('XamUserGetUserTenure', index, level, milestone, date, 0,
               capture=[level, milestone, date], name=f'user {index:#x}')

    s = corpus.scenario('titles played enumerator')
    for index, count in ((1, 4), (4, 4), (0, 0)):
        size = s.alloc(4, 'buffer size')
        handle = s.alloc(4, 'handle')
        s.call('XamUserCreateTitlesPlayedEnumerator', 0, index, 0, 0, count,
               size, handle, capture=[size, handle],
               name=f'user {index:#x} count {count}')

    # The key is captured too, xam rewrites its size from the string.
    s = corpus.scenario('gamer tile key')
    keys = (('hex', WSTRING, 'fffe07d10001000100020001'),
            ('not hex', WSTRING, 'zzzzzzzzzzzzzzzzzzzzzzzz'),
            ('partly hex', WSTRING, '1234zz780001000100020001'),
            ('short', WSTRING, 'fffe07d1'),
            ('long', WSTRING, '4d5307e6' * 7),
            ('avatar', WSTRING, 'fffe08540001000100020001'),
            ('high half 2', WSTRING, '000200050001000100020001'),
            ('int32', INT32, None),
            ('null string', WSTRING, ''))
    for name, data_type, text in keys:
        if text is None:
            data = struct.pack('>B7xi4x', data_type, 5)
        elif not text:
            data = struct.pack('>B7xII', data_type, 0x32, 0)
        else:
            string = s.alloc(len(wide(text)), f'{name} string', wide(text))
            data = struct.pack('>B7xII', data_type, 0x32, string.address)
        key = s.alloc(16, f'{name} key', data)
        outputs = [s.alloc(4, field) for field in ('title', 'big', 'small')]
        s.call('XamParseGamerTileKey', key, *outputs,
               capture=[key] + outputs, name=name)

    s = corpus.scenario('write gamer tile')
    s.call('XamWriteGamerTile', 4, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0, 0,
           name='user 0x4')

    s = corpus.scenario('online language and country')
    offline = s.variable('offline xuid')
    online = s.variable('online xuid')
    xuid = s.alloc(8, 'offline xuid')
    s.call('XamUserGetXUID', 0, 1, xuid, store=[(offline, xuid)],
           name='own offline xuid')
    xuid = s.alloc(8, 'online xuid')
    s.call('XamUserGetXUID', 0, 2, xuid, store=[(online, xuid)],
           name='own online xuid')
    for name, value in (('offline', offline), ('online', online), ('zero', 0),
                        ('unknown', 0x0009FFFFFFFFFFFF)):
        s.call('XamUserGetOnlineLanguageFromXUID', value,
               name=f'language {name}')
        s.call('XamUserGetOnlineCountryFromXUID', value,
               name=f'country {name}')
