# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import asyncio
import unittest

from pywatchman import pybser
from pywatchman_aio import AIOClient, AsyncBserCodec, AsyncUnixSocketTransport


class TestAIOClientLog(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.reader = asyncio.StreamReader()
        transport = AsyncUnixSocketTransport()
        transport.reader = self.reader
        self.client = AIOClient(AsyncBserCodec(transport))
        self.client.enable_receiving()
        self.receive_task = self.client.receive_task

    async def asyncTearDown(self):
        self.client.close()
        await asyncio.gather(self.receive_task, return_exceptions=True)

    def feed_log(self, message):
        self.reader.feed_data(pybser.dumps({"log": message}, version=2))

    async def test_pop_log_returns_message(self):
        self.feed_log("watching root")
        self.assertEqual(
            await asyncio.wait_for(self.client.pop_log(), 1), "watching root"
        )

    async def test_pop_log_returns_message_containing_error(self):
        self.feed_log("error while watching root")
        self.assertEqual(
            await asyncio.wait_for(self.client.pop_log(), 1),
            "error while watching root",
        )

    async def test_pop_log_waits_for_message(self):
        pending = asyncio.create_task(self.client.pop_log())
        try:
            await asyncio.sleep(0)
            self.assertFalse(pending.done())
            self.feed_log("watching root")
            self.assertEqual(await asyncio.wait_for(pending, 1), "watching root")
        finally:
            pending.cancel()
            await asyncio.gather(pending, return_exceptions=True)

    async def test_pop_log_raises_connection_error(self):
        error = ConnectionResetError("connection closed")
        pending = asyncio.create_task(self.client.pop_log())
        try:
            await asyncio.sleep(0)
            self.reader.set_exception(error)
            with self.assertRaises(ConnectionResetError) as raised:
                await asyncio.wait_for(pending, 1)
            self.assertIs(raised.exception, error)
        finally:
            pending.cancel()
            await asyncio.gather(pending, return_exceptions=True)
