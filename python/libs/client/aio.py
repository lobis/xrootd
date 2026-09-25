# Copyright (c) 2026 by the XRootD developers
# This file is part of the XRootD software suite and is distributed under the
# terms of the GNU Lesser General Public License, version 3 or later.
"""Awaitable XRootD client operations backed by the native callback API.

The existing XRootD client performs the remote work asynchronously. This module
only transfers callback completion to the caller's asyncio event loop; it does
not run synchronous network calls in an executor.
"""

import asyncio

from XRootD import client
from XRootD.client.flags import AccessMode, OpenFlags
from XRootD.client.responses import raise_on_error


async def request(method, *args, **kwargs):
    """Await one callback-capable client operation and return its response.

    Cancellation stops waiting but does not cancel an already submitted XrdCl
    operation. The callback keeps the method, arguments, and buffers alive
    until the native operation completes.
    """
    if 'callback' in kwargs:
        raise TypeError('request() manages the callback argument')

    # get_running_loop was added in Python 3.7; XRootD also supports 3.6.
    running_loop = getattr(asyncio, 'get_running_loop', asyncio.get_event_loop)
    loop = running_loop()
    future = loop.create_future()
    keepalive = (method, args, kwargs)

    def complete(status, response):
        if future.done():
            return
        try:
            raise_on_error(status)
        except Exception as error:
            future.set_exception(error)
        else:
            future.set_result(response)

    def callback(status, response, hostlist):
        # The native callback runs on an XrdCl thread, not on the event loop.
        # Retain the bound method and buffers even if the waiter is cancelled.
        _ = keepalive
        try:
            loop.call_soon_threadsafe(complete, status, response)
        except RuntimeError:
            # The loop may have closed after cancellation. XrdCl still owns the
            # callback until completion, but there is no waiter to notify.
            pass

    submission = method(*args, callback=callback, **kwargs)
    try:
        raise_on_error(submission)
    except Exception:
        future.cancel()
        raise
    return await future


class File:
    """Awaitable facade over :class:`XRootD.client.File`."""

    def __init__(self, native=None):
        self.native = native if native is not None else client.File()

    async def open(self, url, flags=OpenFlags.READ, mode=AccessMode.NONE,
                   timeout=0):
        await request(self.native.open, url, flags, mode, timeout)
        return self

    async def close(self, timeout=0):
        await request(self.native.close, timeout)

    async def stat(self, force=False, timeout=0):
        return await request(self.native.stat, force, timeout)

    async def read(self, offset, size, timeout=0):
        if size <= 0 or size > 0xffffffff:
            raise ValueError('async reads require a bounded 32-bit size')
        return await request(self.native.read, offset, size, timeout)

    async def write(self, data, offset=0, timeout=0):
        # The native callback form receives a pointer to the input buffer.
        # Own immutable bytes until completion, including after cancellation.
        data = bytes(data)
        if len(data) > 0xffffffff:
            raise ValueError('async writes require a bounded 32-bit size')
        await request(self.native.write, data, offset, len(data), timeout)
        return len(data)

    async def vector_read(self, chunks, timeout=0):
        return await request(self.native.vector_read, chunks, timeout)

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc_value, traceback):
        if self.native.is_open():
            await self.close()


class FileSystem:
    """Awaitable facade over :class:`XRootD.client.FileSystem`."""

    def __init__(self, url, native=None):
        self.native = native if native is not None else client.FileSystem(url)

    async def stat(self, path, timeout=0):
        return await request(self.native.stat, path, timeout)

    async def dirlist(self, path, flags=0, timeout=0):
        return await request(self.native.dirlist, path, flags, timeout)

    async def locate(self, path, flags=0, timeout=0):
        return await request(self.native.locate, path, flags, timeout)

    async def mkdir(self, path, flags=0, mode=0, timeout=0):
        await request(self.native.mkdir, path, flags, mode, timeout)

    async def chmod(self, path, mode, timeout=0):
        await request(self.native.chmod, path, mode, timeout)

    async def truncate(self, path, size, timeout=0):
        await request(self.native.truncate, path, size, timeout)

    async def rmdir(self, path, timeout=0):
        await request(self.native.rmdir, path, timeout)

    async def rm(self, path, timeout=0):
        await request(self.native.rm, path, timeout)

    async def mv(self, source, dest, timeout=0):
        await request(self.native.mv, source, dest, timeout)

    async def query(self, code, arg, timeout=0):
        return await request(self.native.query, code, arg, timeout)
