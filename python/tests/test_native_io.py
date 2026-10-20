"""Native range batching, buffer ownership and copy lifecycle integration."""

import asyncio
import gc
import threading
import uuid
import weakref

import pytest

from XRootD import client
from XRootD.client import aio
from XRootD.client.flags import OpenFlags
from XRootD.client.responses import XRootDError
from env import SERVER_URL


@pytest.fixture
def remote():
    path = SERVER_URL + '/tmp/native-io-' + uuid.uuid4().hex
    yield path
    fs = client.FileSystem(SERVER_URL)
    fs.rm(client.URL(path).path)


async def seed(path, data):
    file = aio.File()
    await file.open(path, OpenFlags.DELETE)
    try:
        await file.write(data)
    finally:
        await file.close()


def test_native_ranges_split_sizes_counts_and_preserve_order(remote):
    async def run():
        # Larger than one server readv segment.
        data = bytes(range(251)) * 20000
        await seed(remote, data)
        file = aio.File()
        await file.open(remote, OpenFlags.READ)
        try:
            chunks = [(300, 3000000), (90, 0), (3, 100)]
            chunks += [(i, 1) for i in range(1030)]
            for parallel in (1, 4):
                result = await file.read_ranges(chunks, parallel=parallel)
                assert result == [data[start:start + size]
                                  for start, size in chunks]
            vector = await file.vector_read([(2, 4)])
            assert vector.chunks[0].buffer == data[2:6]
            assert await file.read_ranges([]) == []
            assert await file.read_ranges([(len(data), 0)]) == [b'']
            with pytest.raises(XRootDError):
                await file.read_ranges([(len(data) - 1, 5)])
            # A failed batch still drains all submitted native requests.
            await file.drain()
            assert await file.read_ranges([(1, 3)]) == [data[1:4]]
        finally:
            await file.close()

    asyncio.run(run())


def test_readinto_and_write_slices(remote):
    async def run():
        file = aio.File()
        await file.open(remote, OpenFlags.DELETE)
        try:
            assert await file.write(b'prefix-data-suffix', buffer_offset=7,
                                    size=4) == 4
            mutable = bytearray(b'1234')
            task = asyncio.create_task(file.write(mutable, 4))
            await asyncio.sleep(0)
            mutable[:] = b'xxxx'
            assert await task == 4
            target = bytearray(b'?' * 12)
            assert await file.readinto(target) == 8
            assert target == b'data1234????'
            assert await file.readinto(bytearray()) == 0
            with pytest.raises(BufferError):
                await file.readinto(b'readonly')
            with pytest.raises(BufferError):
                await file.readinto(memoryview(target)[::2])
            with pytest.raises(ValueError):
                await file.write(b'a', buffer_offset=2)
        finally:
            await file.close()
        async with aio.open(remote) as stream:
            assert await stream.read(1) == b'd'
            target = bytearray(10)
            assert await stream.readinto(target) == 7
            assert target[:7] == b'ata1234'
            assert stream.tell() == 8

    asyncio.run(run())


def test_native_callback_owns_file_export_and_drain(remote):
    asyncio.run(seed(remote, b'owned'))
    file = client.File()
    assert file.open(remote, OpenFlags.READ)[0].ok
    target = bytearray(5)
    entered, release, finished = (threading.Event() for _ in range(3))
    drained = threading.Event()
    reference = weakref.ref(file)

    def callback(status, count, hosts):
        assert status.ok and count == 5
        entered.set()
        assert release.wait(5)
        finished.set()

    assert file.readinto(target, callback=callback).ok
    try:
        assert entered.wait(5)
        with pytest.raises(BufferError):
            target.extend(b'x')
        assert file.drain(lambda *args: drained.set()).ok
        assert not drained.is_set()
        del file
        gc.collect()
        # The extension owns the internal file even if its Python facade dies.
        assert reference() is None
    finally:
        release.set()
        assert finished.wait(5)
        assert drained.wait(5)
    target.extend(b'x')
    assert target == b'ownedx'


def test_empty_write_and_range_validation(remote):
    async def run():
        file = aio.File()
        await file.open(remote, OpenFlags.DELETE)
        try:
            assert await file.write(b'') == 0
            for chunks, parallel, error in [
                    ([(0, -1)], 4, OverflowError),
                    ([(2**64 - 1, 1)], 4, OverflowError),
                    ([(0, 1)], 0, ValueError),
                    ([(0, 1)], -1, OverflowError),
                    ([(0, 1)], 65536, ValueError),
                    ([1], 4, TypeError),
                    ('invalid', 4, TypeError)]:
                with pytest.raises(error):
                    await file.read_ranges(chunks, parallel=parallel)
        finally:
            await file.close()

    asyncio.run(run())


def test_rejected_native_operation_releases_callback(remote):
    file = client.File()
    assert file.open(remote, OpenFlags.DELETE)[0].ok
    done = threading.Event()

    class Callback:
        def __call__(self, *args):
            done.set()

    callback = Callback()
    reference = weakref.ref(callback)
    # Opening an already-open native file is rejected during submission.
    assert not file.open(remote, OpenFlags.READ, callback=callback).ok
    del callback
    gc.collect()
    assert reference() is None
    assert not done.is_set()
    assert file.close()[0].ok
