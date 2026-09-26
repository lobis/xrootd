"""Python stream contracts and native-request ownership during cancellation."""

import asyncio
import io
import uuid

import pytest

from XRootD import client
from XRootD.client import aio
from XRootD.client.responses import XRootDStatus
from env import SERVER_URL


def run_async(coroutine):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coroutine)
    finally:
        loop.close()


def test_async_stream_round_trip(monkeypatch):
    path = SERVER_URL + '/tmp/async-stream-' + uuid.uuid4().hex

    async def run():
        def no_executor(*args, **kwargs):
            raise AssertionError('remote I/O must use native callbacks')

        monkeypatch.setattr(asyncio.get_event_loop(), 'run_in_executor',
                            no_executor)
        try:
            async with aio.open(path, 'w+b') as file:
                assert file.readable() and file.writable()
                assert file.seekable()
                data = memoryview(b'first\nsecond\nlast')
                assert await file.write(data) == 17
                await file.flush()
                assert await file.seek(0) == 0
                assert await file.readline(3) == b'fir'
                assert file.tell() == 3
                assert await file.readline() == b'st\n'
                # Writing after read-ahead must use the logical cursor.
                assert await file.write(b'S') == 1
                assert await file.seek(6) == 6
                assert [line async for line in file] == [b'Second\n', b'last']
                assert await file.read(0) == b''
                assert await file.read() == b''
                assert await file.seek(-4, io.SEEK_END) == 13
                target = bytearray(8)
                assert await file.readinto(target) == 4
                assert target[:4] == b'last'
                assert await file.truncate(6) == 6
                assert file.tell() == 17
                await file.seek(0)
                assert await file.read() == b'first\n'
            assert file.closed
            await file.aclose()
            with pytest.raises(ValueError, match='closed'):
                await file.read()

            file = await aio.open(path)
            async with file:
                assert await file.read() == b'first\n'
        finally:
            await aio.FileSystem(SERVER_URL).rm(client.URL(path).path)

    run_async(run())


def test_async_stream_lines_across_buffers():
    path = SERVER_URL + '/tmp/async-stream-' + uuid.uuid4().hex

    async def run():
        lines = [b'\xff' * 65537 + b'\n', b'\n', b'tail']
        try:
            async with aio.open(path, 'wb') as file:
                await file.write(b''.join(lines))
            async with aio.open(path) as file:
                assert [line async for line in file] == lines
        finally:
            await aio.FileSystem(SERVER_URL).rm(client.URL(path).path)

    run_async(run())


def test_cancelled_close_waits_for_a_busy_stream(monkeypatch):
    async def run():
        native = DelayedNative('read')
        wrapper = aio.File(native=native)
        monkeypatch.setattr(aio, 'File', lambda: wrapper)
        file = await aio.open('root://example//data')
        read = asyncio.ensure_future(file.read(4))
        await native.started.wait()
        close = asyncio.ensure_future(file.close())
        await asyncio.sleep(0)
        close.cancel()
        await asyncio.sleep(0)
        assert not close.done()
        assert 'close-submitted' not in native.events
        native.release()
        assert await read == b'data'
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(close, 2)
        assert file.closed

    run_async(run())


def test_async_stream_append_exclusive_and_shared_cursor():
    path = SERVER_URL + '/tmp/async-stream-' + uuid.uuid4().hex

    async def run():
        try:
            async with aio.open(path, 'ab') as file:
                await file.write(b'ab')
                await file.seek(0)
                await file.write(b'cd')
            with pytest.raises(FileExistsError):
                async with aio.open(path, 'xb'):
                    pass
            async with aio.open(path) as file:
                assert await asyncio.gather(file.read(2), file.read(2)) == \
                    [b'ab', b'cd']
                with pytest.raises(io.UnsupportedOperation):
                    await file.write(b'x')
                with pytest.raises(TypeError):
                    await file.seek(1.5)
                with pytest.raises(TypeError):
                    await file.readinto(b'immutable')
                assert file.tell() == 4
        finally:
            await aio.FileSystem(SERVER_URL).rm(client.URL(path).path)

    run_async(run())


def test_async_stream_errors_and_context_reuse():
    path = SERVER_URL + '/tmp/async-stream-' + uuid.uuid4().hex

    async def run():
        with pytest.raises(ValueError, match='binary'):
            await aio.open(path, 'w')
        with pytest.raises(FileNotFoundError) as error:
            await aio.open(path)
        assert error.value.filename == path
        assert error.value.xrootd_status is not None
        context = aio.open(path)
        with pytest.raises(FileNotFoundError):
            await context
        with pytest.raises(RuntimeError, match='only be used once'):
            await context

    run_async(run())


class DelayedNative:
    """Callback-driven fake with one manually released native operation."""

    def __init__(self, delayed):
        self.delayed = delayed
        self.started = asyncio.Event()
        self.release = None
        self.opened = False
        self.events = []

    def is_open(self):
        return self.opened

    def submit(self, name, callback, response=None):
        status = XRootDStatus({'ok': True, 'code': 0, 'errno': 0,
                              'message': 'ok'})
        self.events.append(name + '-submitted')

        def complete():
            if name == 'open':
                self.opened = True
            elif name == 'close':
                self.opened = False
            self.events.append(name + '-completed')
            callback(status, response, [])

        if name == self.delayed:
            self.release = complete
            self.started.set()
        else:
            complete()
        return status

    def open(self, *args, **kwargs):
        return self.submit('open', kwargs['callback'])

    def read(self, *args, **kwargs):
        return self.submit('read', kwargs['callback'], b'data')

    def write(self, *args, **kwargs):
        return self.submit('write', kwargs['callback'])

    def sync(self, *args, **kwargs):
        return self.submit('sync', kwargs['callback'])

    def close(self, *args, **kwargs):
        return self.submit('close', kwargs['callback'])


@pytest.mark.parametrize('operation', ['open', 'read', 'write', 'close'])
def test_cancellation_drains_request_before_closing(monkeypatch, operation):
    async def run():
        native = DelayedNative(operation)
        wrapper = aio.File(native=native)
        monkeypatch.setattr(aio, 'File', lambda: wrapper)
        streams = []

        async def work():
            async with aio.open('root://example//data', 'r+b') as file:
                streams.append(file)
                if operation == 'read':
                    await file.read(4)
                elif operation == 'write':
                    await file.write(b'data')

        task = asyncio.ensure_future(work())
        await asyncio.wait_for(native.started.wait(), 2)
        task.cancel()
        await asyncio.sleep(0)
        task.cancel()
        await asyncio.sleep(0)
        assert not task.done()
        if operation != 'close':
            assert 'close-submitted' not in native.events
        native.release()
        with pytest.raises(asyncio.CancelledError):
            await asyncio.wait_for(task, 2)
        assert not native.opened
        assert native.events.count('close-completed') == 1
        if streams:
            assert streams[0].closed

    run_async(run())
