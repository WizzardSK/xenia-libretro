"""Object Manager corpus: handle and pointer counts of an object of a type the
title defines, what its close, delete and free procedures are called with,
and when.

The type's procedures are recorders. An object either has a notification event
body laid out in the arena, the way a title type embedding a dispatcher object
looks, or comes from ObCreateObject, whose allocate procedure hands back an
arena block. Steps capture the object header (pointer count, handle count,
type, flags) rather than handle values, which differ between runs.

Created objects also cover a name (opened and referenced by it), an event body
waited on through the inserted handle and one opened by pointer, and a zeroed
body opened by pointer, whose bytes must stay as they are.

Every scenario closes its handles and drops the object's last reference, since
the next scenario lays its arena over the object and the type.

It also records MmIsAddressValid over reserved, committed and decommitted
pages, a double NtClose of a native object, the previous state the event
functions report, a named event whose first handle closes before it's looked up
again, and whether an XAM overlapped signals its event when it has a completion
routine.
"""

import struct

NOTIFICATION = 0
USER_MODE = 1
DUPLICATE_CLOSE_SOURCE = 1
POOL_TAG = 0x58674F62  # XgOb
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_DECOMMIT = 0x4000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 4
REGION_SIZE = 0x10000
OBJ_CASE_INSENSITIVE = 0x40
OBJ_OPENIF = 0x80
NAME = '\\XgenObNamed'
EVENT_NAME = '\\XgenEvNamed'


def object_type(s, allocate=None):
    """A title X_OBJECT_TYPE whose free, close and delete procedures are
    recorders, and the recorders."""
    type_ = s.alloc(0x1C, 'type', struct.pack('>24xI', POOL_TAG), fill=0)
    procedures = {}
    if allocate:
        s.write(type_, allocate)
    for offset, name, args, calls in ((4, 'free', 1, 2), (8, 'close', 2, 4),
                                      (0xC, 'delete', 1, 2)):
        procedures[name] = s.recorder(f'{name} procedure', args, calls)
        s.write(type_ + offset, procedures[name])
    return type_, procedures


def event_object(s, type_):
    """An object of `type_` with a notification event body: its header and
    body."""
    obj = s.alloc(0x20, 'object', fill=0)
    body = obj + 0x10
    wait_list = body.address + 8
    s.write(obj, struct.pack('>iiIh2x', 1, 0, type_.address, 0) +
            struct.pack('>BBBBiII', NOTIFICATION, 0, 4, 0, 0, wait_list,
                        wait_list))
    return obj.prefix(0x10), body


def handle(s, name):
    """A slot an export writes a handle to, and the variable it's stored in.
    Its own 4 bytes, since a store from 8 saves a doubleword and passing that
    would pass the next slot's fill as the handle. A handle to the title's type
    left open hangs a console when the title exits."""
    return s.alloc(4, name), s.variable(name)


def timeout(s, milliseconds):
    return s.alloc(8, f'{milliseconds} ms', struct.pack(
        '>q', -milliseconds * 10000))


def build(corpus):
    s = corpus.scenario('open by pointer')
    type_, procedures = object_type(s)
    other_type = s.alloc(0x1C, 'other type', fill=0)
    header, body = event_object(s, type_)
    close = procedures['close']
    h1_slot, h1 = handle(s, 'h1')
    h2_slot, h2 = handle(s, 'h2')
    out = s.alloc(4, 'object out')
    s.call('ObOpenObjectByPointer', body, h1_slot, name='open',
           capture=[header], store=[(h1, h1_slot)])
    s.call('ObOpenObjectByPointer', body, h2_slot, name='open again',
           capture=[header], store=[(h2, h2_slot)])
    s.call('ObReferenceObjectByHandle', h1, type_, out, name='reference',
           capture=[header, out])
    s.call('ObReferenceObjectByHandle', h1, other_type, out,
           name='reference as another type', capture=[header, out])
    s.call('ObReferenceObjectByHandle', h1, 0, out,
           name='reference as any type', capture=[header, out])
    for i in (1, 2):
        s.call('ObDereferenceObject', body, name=f'dereference {i}',
               capture=[header], void=True)
    s.call('NtClose', h1, name='close the first',
           capture=[header, close.log])
    s.call('NtClose', h1, name='close the first again',
           capture=[header, close.log])
    s.call('NtClose', h2, name='close the second',
           capture=[header, close.log, procedures['delete'].log,
                    procedures['free'].log])
    s.call('ObDereferenceObject', body, name='drop the last reference',
           capture=[header, procedures['delete'].log,
                    procedures['free'].log], void=True)

    s = corpus.scenario('duplicate')
    type_, procedures = object_type(s)
    header, body = event_object(s, type_)
    close = procedures['close']
    source_slot, source = handle(s, 'source')
    copy_slot, copy = handle(s, 'copy')
    moved_slot, moved = handle(s, 'moved')
    s.call('ObOpenObjectByPointer', body, source_slot, name='open',
           capture=[header], store=[(source, source_slot)])
    s.call('NtDuplicateObject', source, copy_slot, 0, name='duplicate',
           capture=[header, close.log], store=[(copy, copy_slot)])
    s.call('NtClose', copy, name='close the copy',
           capture=[header, close.log])
    s.call('NtDuplicateObject', source, moved_slot, DUPLICATE_CLOSE_SOURCE,
           name='duplicate closing the source',
           capture=[header, close.log], store=[(moved, moved_slot)])
    # Succeeds only if the new handle took the source's value.
    s.call('NtClose', source, name='close the source',
           capture=[header, close.log])
    s.call('NtClose', moved, name='close the moved handle',
           capture=[header, close.log])
    s.call('ObDereferenceObject', body, name='drop the last reference',
           capture=[header, procedures['delete'].log,
                    procedures['free'].log], void=True)
    invalid = s.alloc(4, 'invalid out')
    s.call('NtDuplicateObject', 0, invalid, 0, name='duplicate an invalid '
           'handle', capture=[invalid])

    s = corpus.scenario('created object')
    block = s.alloc(0x30, 'block')
    allocate = s.recorder('allocate procedure', 3, result=block)
    type_, procedures = object_type(s, allocate)
    close = procedures['close']
    header = block.prefix(0x10)
    created = s.alloc(4, 'created')
    inserted_slot, inserted = handle(s, 'inserted')
    opened_slot, opened = handle(s, 'opened')
    out = s.alloc(4, 'object out')
    obj = s.variable('created object')
    s.call('ObCreateObject', type_, 0, 0x20, created, name='create',
           capture=[allocate.log, header, created], store=[(obj, created)])
    s.call('ObInsertObject', obj, 0, 0, inserted_slot, name='insert',
           capture=[header], store=[(inserted, inserted_slot)])
    s.call('ObOpenObjectByPointer', obj, opened_slot, name='open',
           capture=[header], store=[(opened, opened_slot)])
    s.call('ObReferenceObjectByHandle', inserted, type_, out,
           name='reference', capture=[header, out])
    s.call('ObDereferenceObject', obj, name='dereference', capture=[header],
           void=True)
    s.call('NtClose', opened, name='close the opened handle',
           capture=[header, close.log])
    s.call('NtClose', inserted, name='close the inserted handle',
           capture=[header, close.log, procedures['delete'].log,
                    procedures['free'].log])

    s = corpus.scenario('named object')
    block = s.alloc(0x80, 'block')
    allocate = s.recorder('allocate procedure', 3, result=block)
    type_, procedures = object_type(s, allocate)
    close = procedures['close']
    # A named object's allocation starts with its name info.
    name_info = block.prefix(0x10)
    header = (block + 0x10).prefix(0x10)
    name = s.string(NAME, 'name')
    name_string = s.alloc(8, 'name string', struct.pack(
        '>HHI', len(NAME), len(NAME) + 1, name.address))
    attributes = s.alloc(12, 'attributes', struct.pack(
        '>III', 0, name_string.address, OBJ_CASE_INSENSITIVE))
    created = s.alloc(4, 'created')
    obj = s.variable('named object')
    inserted_slot, inserted = handle(s, 'inserted')
    opened_slot, opened = handle(s, 'opened')
    out = s.alloc(4, 'object out')
    s.call('ObCreateObject', type_, attributes, 0x20, created, name='create',
           capture=[allocate.log, name_info, header, created],
           store=[(obj, created)])
    s.call('ObInsertObject', obj, attributes, 0, inserted_slot, name='insert',
           capture=[header], store=[(inserted, inserted_slot)])
    s.call('ObOpenObjectByName', attributes, type_, 0, opened_slot,
           name='open by name', capture=[header],
           store=[(opened, opened_slot)])
    s.call('ObReferenceObjectByName', name_string, OBJ_CASE_INSENSITIVE, type_,
           0, out, name='reference by name', capture=[header, out])
    s.call('ObDereferenceObject', obj, name='dereference', capture=[header],
           void=True)
    s.call('NtClose', opened, name='close the opened handle',
           capture=[header, close.log])
    s.call('NtClose', opened, name='close the opened handle again',
           capture=[header, close.log])
    s.call('ObReferenceObjectByName', name_string, OBJ_CASE_INSENSITIVE, type_,
           0, out, name='reference by name with one handle left',
           capture=[header, out])
    s.call('ObDereferenceObject', obj, name='dereference again',
           capture=[header], void=True)
    s.call('NtClose', inserted, name='close the inserted handle',
           capture=[header, close.log, procedures['delete'].log,
                    procedures['free'].log])
    s.call('ObReferenceObjectByName', name_string, OBJ_CASE_INSENSITIVE, type_,
           0, out, name='reference by name after the last close',
           capture=[out])

    s = corpus.scenario('inserted event opened by pointer')
    block = s.alloc(0x30, 'block', fill=0)
    allocate = s.recorder('allocate procedure', 3, result=block)
    type_, procedures = object_type(s, allocate)
    close = procedures['close']
    header = block.prefix(0x10)
    created = s.alloc(4, 'created')
    obj = s.variable('event object')
    inserted_slot, inserted = handle(s, 'inserted')
    opened_slot, opened = handle(s, 'opened')
    poll = timeout(s, 0)
    s.call('ObCreateObject', type_, 0, 0x10, created, name='create',
           capture=[header], store=[(obj, created)])
    s.call('KeInitializeEvent', obj, NOTIFICATION, 0, void=True)
    s.call('ObInsertObject', obj, 0, 0, inserted_slot, name='insert',
           capture=[header], store=[(inserted, inserted_slot)])
    s.call('ObOpenObjectByPointer', obj, opened_slot, name='open',
           capture=[header], store=[(opened, opened_slot)])
    for state in ('unset', 'set'):
        if state == 'set':
            s.call('KeSetEvent', obj, 0, 0, name='set')
        for which, event in (('inserted', inserted), ('opened', opened)):
            s.call('NtWaitForSingleObjectEx', event, USER_MODE, 0, poll,
                   name=f'poll the {which} handle, {state}')
    s.call('NtClose', opened, name='close the opened handle',
           capture=[header, close.log])
    s.call('NtClose', inserted, name='close the inserted handle',
           capture=[header, close.log, procedures['delete'].log,
                    procedures['free'].log])

    s = corpus.scenario('zeroed body opened by pointer')
    block = s.alloc(0x30, 'block', fill=0)
    allocate = s.recorder('allocate procedure', 3, result=block)
    type_, procedures = object_type(s, allocate)
    header = block.prefix(0x10)
    body = (block + 0x10).prefix(0x20)
    created = s.alloc(4, 'created')
    obj = s.variable('zeroed object')
    opened_slot, opened = handle(s, 'opened')
    s.call('ObCreateObject', type_, 0, 0x20, created, name='create',
           capture=[header, body], store=[(obj, created)])
    s.call('ObOpenObjectByPointer', obj, opened_slot, name='open',
           capture=[header, body], store=[(opened, opened_slot)])
    s.call('NtClose', opened, name='close',
           capture=[header, body, procedures['close'].log])
    s.call('ObDereferenceObject', obj, name='drop the last reference',
           capture=[header, body, procedures['delete'].log,
                    procedures['free'].log], void=True)

    s = corpus.scenario('close a native object twice')
    event_handle = s.alloc(4, 'event')
    event = s.variable('event')
    out = s.alloc(4, 'object out')
    s.call('NtCreateEvent', event_handle, 0, NOTIFICATION, 0,
           store=[(event, event_handle)])
    s.call('NtClose', event, name='close')
    s.call('NtClose', event, name='close again')
    s.call('ObReferenceObjectByHandle', event, 0, out,
           name='reference after close', capture=[out])

    s = corpus.scenario('event previous state')
    event_slot, event = handle(s, 'event')
    previous = s.alloc(4, 'previous')
    s.call('NtCreateEvent', event_slot, 0, NOTIFICATION, 0,
           store=[(event, event_slot)])
    for export, name in (('NtSetEvent', 'set'), ('NtSetEvent', 'set again'),
                         ('NtPulseEvent', 'pulse while set'),
                         ('NtPulseEvent', 'pulse while clear')):
        s.write(previous, b'\xcd' * 4)
        s.call(export, event, previous, name=name, capture=[previous])
    s.call('NtClose', event)
    kevent = s.alloc(0x10, 'kevent', fill=0)
    s.call('KeInitializeEvent', kevent, NOTIFICATION, 0, void=True)
    for export, name in (('KeSetEvent', 'KeSetEvent'),
                         ('KeSetEvent', 'KeSetEvent again'),
                         ('KeResetEvent', 'KeResetEvent'),
                         ('KeResetEvent', 'KeResetEvent again'),
                         ('KeSetEvent', 'KeSetEvent after reset'),
                         ('KePulseEvent', 'KePulseEvent while set'),
                         ('KePulseEvent', 'KePulseEvent while clear')):
        args = (kevent,) if export == 'KeResetEvent' else (kevent, 0, 0)
        s.call(export, *args, name=name)

    s = corpus.scenario('named event')
    name = s.string(EVENT_NAME, 'name')
    name_string = s.alloc(8, 'name string', struct.pack(
        '>HHI', len(EVENT_NAME), len(EVENT_NAME) + 1, name.address))
    attributes = s.alloc(12, 'attributes', struct.pack(
        '>III', 0, name_string.address, OBJ_CASE_INSENSITIVE))
    open_if = s.alloc(12, 'attributes open if', struct.pack(
        '>III', 0, name_string.address, OBJ_CASE_INSENSITIVE | OBJ_OPENIF))
    first_slot, first = handle(s, 'first')
    copy_slot, copy = handle(s, 'copy')
    opened_slot, opened = handle(s, 'opened')
    again_slot, again = handle(s, 'created again')
    open_if_slot, opened_if = handle(s, 'created again open if')
    semaphore_slot, semaphore = handle(s, 'semaphore')
    semaphore_if_slot, semaphore_if = handle(s, 'semaphore open if')
    after_slot, after = handle(s, 'opened after')
    poll = timeout(s, 0)
    s.call('NtCreateEvent', first_slot, attributes, NOTIFICATION, 0,
           name='create', store=[(first, first_slot)])
    s.call('NtDuplicateObject', first, copy_slot, 0, name='duplicate',
           store=[(copy, copy_slot)])
    s.call('NtClose', first, name='close the first handle')
    s.call('ObOpenObjectByName', attributes, 0, 0, opened_slot,
           name='open by name', store=[(opened, opened_slot)])
    s.call('NtCreateEvent', again_slot, attributes, NOTIFICATION, 0,
           name='create again', capture=[again_slot],
           store=[(again, again_slot)])
    s.call('NtCreateEvent', open_if_slot, open_if, NOTIFICATION, 0,
           name='create again with OBJ_OPENIF',
           store=[(opened_if, open_if_slot)])
    s.call('NtCreateSemaphore', semaphore_slot, attributes, 0, 1,
           name='create a semaphore with the name', capture=[semaphore_slot],
           store=[(semaphore, semaphore_slot)])
    s.call('NtCreateSemaphore', semaphore_if_slot, open_if, 0, 1,
           name='create a semaphore with the name and OBJ_OPENIF',
           capture=[semaphore_if_slot],
           store=[(semaphore_if, semaphore_if_slot)])
    s.call('NtSetEvent', opened, 0, name='set through the opened handle')
    for which, h in (('copy', copy), ('created again', again),
                     ('created again open if', opened_if)):
        s.call('NtWaitForSingleObjectEx', h, USER_MODE, 0, poll,
               name=f'poll the {which} handle')
    for which, h in (('copy', copy), ('opened', opened),
                     ('created again', again),
                     ('created again open if', opened_if),
                     ('semaphore', semaphore),
                     ('semaphore open if', semaphore_if)):
        s.call('NtClose', h, name=f'close the {which} handle')
    s.call('ObOpenObjectByName', attributes, 0, 0, after_slot,
           name='open by name after the last close',
           store=[(after, after_slot)])
    s.call('NtClose', after, name='close what that opened')

    s = corpus.scenario('address validity')
    base = s.alloc(4, 'base', fill=0)
    size = s.alloc(4, 'size', struct.pack('>I', REGION_SIZE))
    address = s.variable('region')
    s.call('NtAllocateVirtualMemory', base, size, MEM_RESERVE, PAGE_READWRITE,
           0, name='reserve', capture=[size], store=[(address, base)])
    s.call('MmIsAddressValid', address, name='valid when reserved')
    s.write(size, struct.pack('>I', REGION_SIZE))
    s.call('NtAllocateVirtualMemory', base, size, MEM_COMMIT, PAGE_READWRITE,
           0, name='commit', capture=[size])
    s.call('MmIsAddressValid', address, name='valid when committed')
    s.write(size, struct.pack('>I', REGION_SIZE))
    s.call('NtFreeVirtualMemory', base, size, MEM_DECOMMIT, 0,
           name='decommit', capture=[size])
    s.call('MmIsAddressValid', address, name='valid when decommitted')
    s.write(size, struct.pack('>I', 0))
    s.call('NtFreeVirtualMemory', base, size, MEM_RELEASE, 0, name='release')
    s.call('MmIsAddressValid', address, name='valid when released')

    s = corpus.scenario('overlapped event with a completion routine')
    overlapped = s.alloc(0x1C, 'overlapped', fill=0)
    routine = s.recorder('completion routine', 3)
    s.write(overlapped + 0x10, routine)
    event = s.variable('event')
    root = s.string('xgob', 'root')
    # The event handle goes straight into the XOVERLAPPED.
    s.call('NtCreateEvent', overlapped + 0xC, 0, NOTIFICATION, 0,
           store=[(event, (overlapped + 0xC).prefix(4))])
    s.call('XamContentClose', root, overlapped, name='close nothing open',
           wait=overlapped, capture=[routine.log])
    s.call('KeDelayExecutionThread', USER_MODE, 1, timeout(s, 10),
           name='alertable wait', capture=[routine.log])
    s.call('NtWaitForSingleObjectEx', event, USER_MODE, 0, timeout(s, 0),
           name='poll the event')
    s.call('NtClose', event)
