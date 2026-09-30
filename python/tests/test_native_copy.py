"""Native copy worker lifecycle and awaitable progress integration."""

import asyncio
import threading
import uuid

import pytest

from XRootD import client
from XRootD.client import aio
from XRootD.client.responses import XRootDError
from env import SERVER_URL


@pytest.fixture
def remote():
    path = SERVER_URL + '/tmp/native-copy-' + uuid.uuid4().hex
    yield path
    client.FileSystem(SERVER_URL).rm(client.URL(path).path)


def test_copy_uses_native_worker_progress_and_error_mapping(remote, tmp_path):
    async def run():
        source = tmp_path / 'source'
        target = tmp_path / 'target'
        data = bytes(range(251)) * 5000
        source.write_bytes(data)
        thread = threading.get_ident()
        progress = []

        class Progress:
            def set_size(self, size):
                assert size == len(data)

            def absolute_update(self, done):
                assert threading.get_ident() == thread
                progress.append(done)

        await aio.copy(str(source), remote, force=True, callback=Progress())
        await asyncio.sleep(0)
        assert progress and progress[-1] == len(data)
        await aio.copy(remote, str(target), force=True)
        assert target.read_bytes() == data
        with pytest.raises(XRootDError):
            await aio.copy(str(source) + '-missing', remote, force=True)

    asyncio.run(run())


def test_copy_cancellation_drains_and_rejects_concurrent_mutation(
        remote, tmp_path):
    source = tmp_path / 'source'
    source.write_bytes(b'x' * (4 * 1024 * 1024))
    process = client.CopyProcess()
    assert process.add_job(str(source), remote, force=True, chunksize=65536).ok
    started, release, completed = (threading.Event() for _ in range(3))

    class Progress(client.utils.CopyProgressHandler):
        def begin(self, *args):
            started.set()
            assert release.wait(5)

    result = []
    process.run_async(lambda status, response, hosts:
                      (result.append(status), completed.set()), Progress())
    try:
        assert started.wait(5)
        for mutate in (lambda: process.parallel(2), process.prepare,
                       process.run, lambda: process.add_job('a', 'b'),
                       lambda: process.run_async(lambda *args: None)):
            with pytest.raises(RuntimeError, match='already running'):
                mutate()
        process.cancel()
    finally:
        release.set()
    assert completed.wait(10)
    assert result and not result[0].ok


def test_awaitable_copy_cancellation_waits_for_native_completion(monkeypatch):
    async def run():
        started, release = asyncio.Event(), asyncio.Event()
        cancelled = []

        class Process:
            def add_job(self, *args, **kwargs):
                return {'ok': True, 'code': 0, 'errno': 0, 'message': 'ok'}

            def run_async(self, callback, handler):
                started.set()

                async def finish():
                    await release.wait()
                    callback(self.add_job(), [], [])

                asyncio.create_task(finish())
                return self.add_job()

            def cancel(self):
                cancelled.append(True)

        monkeypatch.setattr(client, 'CopyProcess', Process)
        task = asyncio.create_task(aio.copy('source', 'target'))
        await started.wait()
        task.cancel()
        await asyncio.sleep(0)
        task.cancel()
        await asyncio.sleep(0)
        assert cancelled and not task.done()
        release.set()
        with pytest.raises(asyncio.CancelledError):
            await task

    asyncio.run(run())


def test_copy_progress_bridge_defers_notifications_to_loop(monkeypatch):
    async def run():
        notifications = []

        class Progress:
            def relative_update(self, amount):
                notifications.append(amount)

        class Process:
            def add_job(self, *args, **kwargs):
                return {'ok': True, 'code': 0, 'errno': 0, 'message': 'ok'}

            def run_async(self, callback, handler):
                handler.begin(1, 1, 'source', 'target')
                handler.update(1, 2, 4)
                handler.update(1, 4, 4)
                handler.end(1, {})
                assert not handler.should_cancel(1)
                assert not notifications
                callback(self.add_job(), [], [])
                return self.add_job()

        monkeypatch.setattr(client, 'CopyProcess', Process)
        assert await aio.copy('source', 'target', callback=Progress()) == {}
        assert notifications == [2, 2]
        notifications.clear()
        await aio.copy('source', 'target')
        assert not notifications

    asyncio.run(run())


@pytest.mark.parametrize('completion_ok', [True, False])
def test_progress_failure_requests_cancel_and_waits_for_completion(
        monkeypatch, completion_ok):
    async def run():
        release = asyncio.Event()
        cancelled = []

        class Progress:
            def relative_update(self, amount):
                raise ValueError('progress failed')

        class Process:
            def add_job(self, *args, **kwargs):
                return {'ok': True, 'code': 0, 'errno': 0, 'message': 'ok'}

            def run_async(self, callback, handler):
                handler.update(1, 1, 2)
                handler.update(1, 2, 2)

                async def finish():
                    await release.wait()
                    status = self.add_job()
                    if not completion_ok:
                        status.update(ok=False, code=207)
                    callback(status, [{}], [])

                asyncio.create_task(finish())
                return self.add_job()

            def cancel(self):
                cancelled.append(True)

        monkeypatch.setattr(client, 'CopyProcess', Process)
        task = asyncio.create_task(aio.copy('a', 'b', callback=Progress()))
        for _ in range(4):
            await asyncio.sleep(0)
        assert cancelled == [True] and not task.done()
        release.set()
        with pytest.raises(ValueError, match='progress failed'):
            await task

    asyncio.run(run())


def test_interpreter_shutdown_joins_native_copy_worker(remote, tmp_path):
    import os
    import subprocess
    import sys
    import textwrap

    source = tmp_path / 'shutdown-source'
    source.write_bytes(b'x' * (8 * 1024 * 1024))
    code = textwrap.dedent('''
        import threading
        import time
        from XRootD import client
        import sys
        started = threading.Event()
        class Progress(client.utils.CopyProgressHandler):
            def begin(self, *args):
                started.set()
                time.sleep(0.05)
        process = client.CopyProcess()
        process.add_job(sys.argv[1], sys.argv[2], force=True, chunksize=65536)
        process.run_async(lambda *args: print('completed', flush=True),
                          Progress())
        assert started.wait(5)
        # atexit cancels and joins while the worker still owns Python objects.
    ''')
    result = subprocess.run([sys.executable, '-c', code, str(source), remote],
                            env=dict(os.environ), capture_output=True,
                            text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert 'completed' in result.stdout


def test_copy_options_forward_rate_and_retry_in_native_order(monkeypatch):
    from XRootD.client import copyprocess
    received = []

    class Native:
        def add_job(self, *args):
            received.extend(args)
            return {'ok': True, 'code': 0, 'errno': 0, 'message': 'ok'}

    monkeypatch.setattr(copyprocess.client, 'CopyProcess', Native)
    process = copyprocess.CopyProcess()
    assert process.add_job('source', 'target', cptimeout=17,
                           xrateThreshold=23, xrate=29, retry=31).ok
    assert received[17:21] == [17, 23, 29, 31]
