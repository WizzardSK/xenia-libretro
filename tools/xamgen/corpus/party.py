"""Party corpus: XamPartyGetUserList (XMsgInProcessCall 0xF4/0x500001) and
XamPartyGetUserListInternal (0x50000C) outside a party.

The public list is cleared to 0xF08 bytes first. The internal one is a copy of
xam's cached 0x1008-byte list with the count then set to 0 (Xam.Community
9124B858 in 2.0.16747). Guard bytes past each list show how far xam writes.
"""

PUBLIC_LIST_SIZE = 0xF08
INTERNAL_LIST_SIZE = 0x1008
GUARD = 0x10


def build(corpus):
    s = corpus.scenario('user list internal')
    users = s.alloc(INTERNAL_LIST_SIZE + GUARD, 'users')
    s.call('XamPartyGetUserListInternal', users, capture=[users])

    for caller in (1, 0):
        s = corpus.scenario(f'user list caller {caller}')
        users = s.alloc(PUBLIC_LIST_SIZE + GUARD, 'users')
        s.call('XamPartyGetUserList', caller, users, capture=[users])
