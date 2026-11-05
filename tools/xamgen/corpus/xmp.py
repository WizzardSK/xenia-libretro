"""XMP corpus: playback behavior and volume per client, their argument checks
and which clients may control playback, through XMsgInProcessCall to the XMP
app (0xFA).

Only the title's own settings (client 2) are changed and the last scenario
puts back the values the first one expects. Other clients are only read, or
called where they're refused, so the console's music player settings stay as
they were. Nothing plays during the run.
"""

import struct

XMP_APP = 0xFA

CONTINUE = 0x70003
STOP = 0x70004
PAUSE = 0x70005
NEXT = 0x70006
PREVIOUS = 0x70007
SET_PLAYBACK_BEHAVIOR = 0x70008
GET_VOLUME = 0x7000B
SET_VOLUME = 0x7000C
GET_PLAYBACK_CONTROLLER = 0x7001B
GET_PLAYBACK_BEHAVIOR = 0x70029

DASH = 0
GAME = 2
MCE = 6
CLIENTS = (('game', GAME), ('dash', DASH), ('mce', MCE))

SHUFFLE = 1
NO_REPEAT = 1
AUTO_PAUSE = 1
DEFAULT_VOLUME = 10 ** (-12 / 20)


def message(s, msg, data, name, capture=()):
    buffer = s.alloc(len(data), f'{name} args', data)
    s.call('XMsgInProcessCall', XMP_APP, msg, buffer, 0, name=name,
           capture=list(capture))


def pointers(s, msg, client, targets, name):
    """Sends {client, pointers...} with each target an output range or 0 and
    records the outputs."""
    data = struct.pack('>I', client) + b''.join(
        struct.pack('>I', t.address if t else 0) for t in targets)
    message(s, msg, data, name, capture=[t for t in targets if t])


def read_volume(s, client, name):
    pointers(s, GET_VOLUME, client, (s.alloc(4, f'{name} out'),), name)


def read_behavior(s, client, name):
    pointers(s, GET_PLAYBACK_BEHAVIOR, client,
             tuple(s.alloc(4, f'{name} {field}')
                   for field in ('mode', 'repeat', 'flags')), name)


def build(corpus):
    s = corpus.scenario('xmp initial state')
    for label, client in CLIENTS:
        read_volume(s, client, f'volume {label}')
        read_behavior(s, client, f'behavior {label}')
    pointers(s, GET_PLAYBACK_CONTROLLER, GAME,
             (s.alloc(4, 'controller'), s.alloc(4, 'locked')),
             'playback controller')

    s = corpus.scenario('xmp argument checks')
    pointers(s, GET_VOLUME, GAME, (0,), 'get volume null')
    pointers(s, GET_PLAYBACK_BEHAVIOR, GAME, (0, 0, 0),
             'get behavior all null')
    pointers(s, GET_PLAYBACK_BEHAVIOR, GAME,
             (0, s.alloc(4, 'repeat only'), 0), 'get behavior repeat only')
    for value in (1.5, -0.5):
        message(s, SET_VOLUME, struct.pack('>If', GAME, value),
                f'set volume {value}')
    message(s, SET_VOLUME, struct.pack('>If', MCE, 0.5), 'set volume mce')
    message(s, SET_PLAYBACK_BEHAVIOR, struct.pack('>IIII', MCE, 0, 0, 0),
            'set behavior mce')

    # The dash client is refused while the title has control.
    s = corpus.scenario('xmp transport access')
    for msg, label in ((CONTINUE, 'continue'), (PAUSE, 'pause'),
                       (NEXT, 'next'), (PREVIOUS, 'previous')):
        message(s, msg, struct.pack('>I', DASH), f'{label} dash')
    message(s, STOP, struct.pack('>II', DASH, 0), 'stop dash')
    message(s, PAUSE, struct.pack('>I', GAME), 'pause game')
    message(s, CONTINUE, struct.pack('>I', GAME), 'continue game')

    s = corpus.scenario('xmp game settings')
    message(s, SET_VOLUME, struct.pack('>If', GAME, 0.5), 'set volume game')
    read_volume(s, GAME, 'volume game after set')
    read_volume(s, DASH, 'volume dash after set')
    message(s, SET_PLAYBACK_BEHAVIOR,
            struct.pack('>IIII', GAME, SHUFFLE, NO_REPEAT, AUTO_PAUSE),
            'set behavior game')
    read_behavior(s, GAME, 'behavior game after set')
    read_behavior(s, DASH, 'behavior dash after set')
    message(s, SET_VOLUME, struct.pack('>If', GAME, DEFAULT_VOLUME),
            'restore volume game')
    message(s, SET_PLAYBACK_BEHAVIOR, struct.pack('>IIII', GAME, 0, 0, 0),
            'restore behavior game')
