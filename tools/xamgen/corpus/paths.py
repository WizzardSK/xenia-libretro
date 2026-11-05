"""Path corpus: content file names that are paths or parts of one on a host.

xam builds a package's path from its file name as given and deletes it with
NtDeleteFile, which removes one file or one empty folder. Each name is tried
while the package xgp exists, so a name that reaches it or its folder shows.
The corpus has a title of its own, so a name that reaches past its package on a
Xenia without names checked can only remove this title's test packages, except
the ones that name a host root.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import identity  # noqa: E402
from content import CREATE_NEW, OPEN_ALWAYS, ROOT, Content  # noqa: E402

NAMES = (b'.', b'..', b'...', b'\\', b'/', b'C:', b'xgp.', b'xgp\\x',
         b'..\\00000001\\xgp', b'x"g', b'x+g', b'x,g', b'x;g', b'x=g',
         b'x<g', b'x>g', b'x|g', b'x*g', b'x?g', b'x\x01g', b'x\x7fg')
# Names an unchecked Xenia resolves to a host drive's root or another drive's
# current folder.
HOST_ROOTS = (b'\\', b'/', b'C:')


def deletes(c, names):
    package = c.data('xgp')
    c.create(0, ROOT, package, CREATE_NEW, name='create xgp')
    c.close(ROOT)
    for file_name in names:
        c.delete(0, c.data(file_name), name=f'delete {file_name!r}')
        c.create(0, ROOT, package, OPEN_ALWAYS,
                 name=f'xgp after {file_name!r}')
        c.close(ROOT, name=f'close after {file_name!r}')
    c.delete(0, package, name='delete xgp')


def build(corpus):
    corpus.title_id = 0xFFFF7E5C
    identity(corpus)

    # Calls that read or create, never delete.
    c = Content(corpus.scenario('path names'))
    package = c.data('xgp')
    c.delete(0, package, name='delete xgp first')
    c.create(0, ROOT, package, CREATE_NEW, name='create xgp')
    c.close(ROOT)
    for file_name in NAMES:
        data = c.data(file_name)
        c.create(0, ROOT, data, CREATE_NEW, name=f'create new {file_name!r}')
        c.close(ROOT, name=f'close after {file_name!r}')
        c.creator(0, data, name=f'creator {file_name!r}')
        c.thumbnail(0, data, name=f'thumbnail size {file_name!r}')
    c.delete(0, package, name='delete xgp')

    # These also delete what the names above made.
    deletes(Content(corpus.scenario('path deletes')),
            [n for n in NAMES if n not in HOST_ROOTS])
    deletes(Content(corpus.scenario(
        'host root deletes', console_only='a Xenia without file names '
        'checked deletes a host drive root for these')), HOST_ROOTS)
