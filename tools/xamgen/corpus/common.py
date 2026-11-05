"""Scenarios every corpus that compares profile values starts with."""


def identity(corpus):
    """The XUID and gamertag run.py replaces in comparisons."""
    s = corpus.scenario('identity')
    xuid = s.alloc(8, 'xuid')
    s.call('XamUserGetXUID', 0, 1, xuid, capture=[xuid])
    gamertag = s.alloc(16, 'gamertag')
    s.call('XamUserGetName', 0, gamertag, 16, capture=[gamertag])
