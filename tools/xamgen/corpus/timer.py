"""Timer corpus: whether a periodic timer stays signaled once it fires, for a
notification and a synchronization timer, and the state NtCancelTimer reports.

Each timer is set to fire after 50 ms and then every 50 ms, and is polled with
zero timeouts around its first expiry and after a few more periods. The two
handle wait pairs it with an event that is never signaled, waiting for either.
"""

import struct

NOTIFICATION = 0
SYNCHRONIZATION = 1
USER_MODE = 1
WAIT_ANY = 1
PERIOD_MS = 50


def timeout(s, milliseconds):
    return s.alloc(8, f'{milliseconds} ms', struct.pack(
        '>q', -milliseconds * 10000))


def poll(s, timer, name):
    s.call('NtWaitForSingleObjectEx', timer, USER_MODE, 0, timeout(s, 0),
           name=name)


def build(corpus):
    for label, timer_type in (('notification', NOTIFICATION),
                              ('synchronization', SYNCHRONIZATION)):
        s = corpus.scenario(f'{label} timer')
        # The event comes first so a wait for either reports the timer as 1.
        # 16 bytes, since a store from an 8-byte range saves a doubleword.
        handles = s.alloc(16, 'handles')
        event = s.variable('event')
        timer = s.variable('timer')
        s.call('NtCreateEvent', handles, 0, NOTIFICATION, 0,
               store=[(event, handles)])
        s.call('NtCreateTimer', handles + 4, 0, timer_type,
               store=[(timer, handles + 4)])
        s.call('NtSetTimerEx', timer, timeout(s, PERIOD_MS), 0, USER_MODE, 0,
               0, PERIOD_MS, 0)
        poll(s, timer, 'poll before it fires')
        s.call('NtWaitForSingleObjectEx', timer, USER_MODE, 0,
               timeout(s, 10 * PERIOD_MS), name='wait for it to fire')
        poll(s, timer, 'poll after the wait')
        poll(s, timer, 'poll again')
        for i in (1, 2):
            s.call('NtWaitForMultipleObjectsEx', 2, handles, WAIT_ANY,
                   USER_MODE, 0, timeout(s, 0), name=f'poll either {i}')
        s.call('KeDelayExecutionThread', USER_MODE, 0,
               timeout(s, 3 * PERIOD_MS), name='sleep 3 periods')
        poll(s, timer, 'poll after 3 periods')
        poll(s, timer, 'poll after 3 periods again')
        state = s.alloc(4, 'state')
        s.call('NtCancelTimer', timer, state, capture=[state])
        poll(s, timer, 'poll after cancel')
        s.call('NtClose', timer)
        s.call('NtClose', event)

    s = corpus.scenario('cancel an invalid handle')
    state = s.alloc(4, 'state')
    s.call('NtCancelTimer', 0, state, capture=[state])
