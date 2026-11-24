#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import asyncio
import os
import subprocess
import typing

from pywatchman import CommandError, encoding, WatchmanError


try:
    from pywatchman import bser
except ImportError:
    from pywatchman import pybser as bser


# 2 bytes marker, 1 byte int size, 8 bytes int64 value
SNIFF_LEN = 13


# TODO: Fix this when https://github.com/python/asyncio/issues/281 is resolved.
# tl;dr is that you cannot have different event loops running in different
# threads all fork subprocesses and listen for child events. The current
# workaround is to do the old fashioned blocking process communication using a
# ThreadPool.
def _resolve_sockname_helper():
    # if invoked via a trigger, watchman will set this env var; we
    # should use it unless explicitly set otherwise
    path = os.getenv("WATCHMAN_SOCK")
    if path:
        return path

    cmd = ["watchman", "--output-encoding=bser", "get-sockname"]

    try:
        p = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            close_fds=os.name != "nt",
        )
    except OSError as e:
        raise WatchmanError('"watchman" executable not in PATH (%s)', e)

    stdout, stderr = p.communicate()
    exitcode = p.poll()

    if exitcode:
        raise WatchmanError("watchman exited with code %d" % exitcode)

    result = bser.loads(stdout)

    if "error" in result:
        raise WatchmanError(str(result["error"]))

    return result["sockname"]


async def _resolve_sockname():
    """Find the Unix socket path to the global Watchman instance."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(None, _resolve_sockname_helper)


class AsyncTransport:
    """Communication transport to the Watchman Service."""

    async def activate(self, **kwargs):
        """Make the transport ready for use. Optional for subclasses."""
        pass

    async def read(self, size):
        """Read 'size' bytes from the transport."""
        raise NotImplementedError()

    async def write(self, buf):
        """Write 'buf' bytes to the transport."""
        raise NotImplementedError()

    def close(self):
        """Close the transport. Optional for subclasses."""
        pass


class AsyncUnixSocketTransport(AsyncTransport):
    """Local Unix domain socket transport supporting asyncio."""

    def __init__(self):
        self.sockname = None
        self.reader = None
        self.writer = None

    async def activate(self, **kwargs):
        # Requires keyword-argument 'sockname'
        reader, writer = await asyncio.open_unix_connection(kwargs["sockname"])
        self.reader = reader
        self.writer = writer

    # pyrefly: ignore [bad-override-param-name]
    async def write(self, data):
        self.writer.write(data)
        await self.writer.drain()

    async def read(self, size):
        res = await self.reader.read(size)
        if not len(res):
            raise ConnectionResetError("connection closed")
        return res

    def close(self):
        if self.writer:
            self.writer.close()


class AsyncCodec:
    """Communication encoding for the Watchman service."""

    def __init__(self, transport):
        self.transport = transport

    async def receive(self):
        """Read from the underlying transport, parse and return the message."""
        raise NotImplementedError()

    async def send(self, *args):
        """Send the given message via the underlying transport."""
        raise NotImplementedError()

    def close(self):
        """Close the underlying transport."""
        self.transport.close()


# This requires BSERv2 support of the server, but doesn't gracefully check
# for the requisite capability being present in older versions.
class AsyncBserCodec(AsyncCodec):
    """Use the BSER encoding."""

    async def receive(self):
        sniff = await self.transport.read(SNIFF_LEN)
        if not sniff:
            raise WatchmanError("empty watchman response")
        _1, _2, elen = bser.pdu_info(sniff)
        rlen = len(sniff)
        buf = bytearray(elen)
        buf[:rlen] = sniff
        while elen > rlen:
            b = await self.transport.read(elen - rlen)
            buf[rlen : rlen + len(b)] = b
            rlen += len(b)
        response = bytes(buf)
        try:
            res = self._loads(response)
            return res
        except (ValueError, RecursionError) as e:
            raise WatchmanError("watchman response decode error: %s" % e)

    async def send(self, *args):
        cmd = bser.dumps(*args, version=2, capabilities=0)
        await self.transport.write(cmd)

    def _loads(self, response):
        """Parse the BSER packet"""
        return bser.loads(
            response,
            True,
            value_encoding=encoding.get_local_encoding(),
            value_errors=encoding.default_local_errors,
        )


class ReceiveLoopError(Exception):
    pass


class AIOClient:
    """Create and manage an asyncio Watchman connection.

    Example usage:
     with await AIOClient.from_socket() as client:
         res = await client.query(...)
         # ... use res ...
         await client.query(
             'subscribe',
             root_dir,
             sub_name,
             {expression: ..., ...},
         )
         while True:
             sub_update = await client.get_subscription(sub_name, root_dir)
             # ... process sub_update ...
    """

    # Don't call this directly use ::from_socket() instead.
    def __init__(self, connection):
        self.connection = connection
        self.log_queue = asyncio.Queue()
        self.sub_by_root = {}
        self.bilateral_response_queue = asyncio.Queue()
        self.receive_task = None
        self.receive_task_exception = None
        self._closed = False

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.close()
        return False

    async def receive_bilateral_response(self):
        """Receive the response to a request made to the Watchman service."""

        self._check_receive_loop()
        resp = await self.bilateral_response_queue.get()
        self._check_error(resp)
        return resp

    async def query(self, *args):
        """Send a query to the Watchman service and return the response."""

        self._check_receive_loop()
        try:
            await self.connection.send(args)
            return await self.receive_bilateral_response()
        except CommandError as ex:
            ex.setCommand(args)
            raise ex

    async def capability_check(self, optional=None, required=None):
        """Perform a server capability check."""

        self._check_receive_loop()
        # If the returned response is an error, self.query will raise an error
        await self.query(
            "version", {"optional": optional or [], "required": required or []}
        )

    async def get_subscription(self, name, root):
        """Retrieve the data associated with a named subscription

        Returns None if there is no data associated with `name`

        If root is not None, then only return the subscription
        data that matches both root and name.  When used in this way,
        remove processing impacts both the unscoped and scoped stores
        for the subscription data.
        """
        self._check_receive_loop()
        self._ensure_subscription_queue_exists(name, root)
        res = await self.sub_by_root[root][name].get()
        self._check_error(res)
        return res

    async def pop_log(self):
        """Get one log from the log queue."""
        self._check_receive_loop()
        res = await self.log_queue.get()
        self._check_error(res)
        return res

    def close(self):
        """Close the underlying connection."""
        self._closed = True
        if self.receive_task:
            self.receive_task.cancel()
        if self.connection:
            self.connection.close()

    def enable_receiving(self, loop=None):
        """Schedules the receive loop to run on the given loop."""
        self.receive_task = asyncio.ensure_future(self._receive_loop(), loop=loop)

        def do_if_done(fut):
            try:
                fut.result()
            except asyncio.CancelledError:
                pass
            except Exception as ex:
                self.receive_task_exception = ex
            self.receive_task = None

        self.receive_task.add_done_callback(do_if_done)

    @classmethod
    async def from_socket(cls, sockname: typing.Optional[str] = None) -> "AIOClient":
        """Create a new AIOClient using Unix transport and BSER Codec
        connecting to the specified socket. If the specified socket is None,
        then resolve the socket path automatically.

        This method also schedules the receive loop to run on the event loop.

        This method is a coroutine."""
        if not sockname:
            sockname = await _resolve_sockname()
        transport = AsyncUnixSocketTransport()
        await transport.activate(sockname=sockname)
        connection = AsyncBserCodec(transport)
        obj = cls(connection)
        obj.enable_receiving()
        return obj

    async def _receive_loop(self):
        """Receive the response to a request made to the Watchman service.

        Note that when trying to receive a PDU from the Watchman service,
        we might get a unilateral response to a subscription or log, so these
        are processed and queued up for later retrieval. This function only
        returns when a non-unilateral response is received."""

        try:
            while True:
                response = await self.connection.receive()
                if self._is_unilateral(response):
                    await self._process_unilateral_response(response)
                else:
                    await self.bilateral_response_queue.put(response)
        except Exception as ex:
            await self._broadcast_exception(ex)
            # We may get a cancel exception on close, so don't close again.
            if not self._closed:
                self.close()

    async def _broadcast_exception(self, ex):
        await self.bilateral_response_queue.put(ex)
        await self.log_queue.put(ex)
        for root in self.sub_by_root.values():
            for sub_queue in root.values():
                await sub_queue.put(ex)

    def _check_error(self, res):
        if isinstance(res, Exception):
            raise res
        if isinstance(res, dict) and "error" in res:
            raise CommandError(res["error"])

    def _check_receive_loop(self):
        if self._closed:
            raise Exception("Connection has been closed, make a new one to reconnect.")
        if self.receive_task is None:
            raise ReceiveLoopError("Receive loop was not started.")

    def _is_unilateral(self, res):
        return res.get("unilateral") or "subscription" in res or "log" in res

    def _ensure_subscription_queue_exists(self, name, root):
        # Note this function must be called from an async function on only one
        # event loop.
        self.sub_by_root.setdefault(root, {}).setdefault(name, asyncio.Queue())

    async def _process_unilateral_response(self, response):
        if "log" in response:
            await self.log_queue.put(response["log"])

        elif "subscription" in response:
            sub = response["subscription"]
            root = os.path.normcase(response["root"])
            self._ensure_subscription_queue_exists(sub, root)
            await self.sub_by_root[root][sub].put(response)

        elif self._is_unilateral(response):
            raise WatchmanError("Unknown unilateral response: " + str(response))

        else:
            raise WatchmanError("Not a unilateral response: " + str(response))
