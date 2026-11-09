from __future__ import absolute_import, division, print_function, unicode_literals

import json
from queue import Empty, Queue
from threading import Thread
from typing import BinaryIO, Optional

from hphp.hack.test.integration.utils import Json


class JsonRpcStreamReader:
    def __init__(self, stream: BinaryIO) -> None:
        self.stream = stream
        self.queue: Queue[Json] = Queue()
        # daemon ensures the reading thread will get cleaned up when
        # the main program exits.  no need to explicitly manage it.
        self.read_thread = Thread(target=self._async_read_loop, daemon=True)
        self.read_thread.start()

    def read(self, timeout_seconds: float) -> Optional[Json]:
        try:
            return self.queue.get(block=True, timeout=timeout_seconds)
        except Empty:
            return None

    def _async_read_loop(self) -> None:
        while True:
            try:
                self.queue.put(json.loads(self._read_payload()))
            except ValueError:
                break
            except IndexError:
                break

    def _read_content_length(self) -> int:
        # read the 'Content-Length:' line and absorb the newline
        # after it
        length_line = self.stream.readline().decode()
        self._read_content(len("\r\n"))

        # get the content length as an integer for the
        # rest of the package
        parts = length_line.split(":", 1)
        return int(parts[1].strip())

    def _read_content(self, length: int) -> bytes:
        content = bytearray()
        while len(content) < length:
            chunk = self.stream.read(length - len(content))
            if not chunk:
                raise ValueError("Unexpected EOF in JSON-RPC message")
            content.extend(chunk)
        return bytes(content)

    def _read_payload(self) -> bytes:
        length = self._read_content_length()
        return self._read_content(length)


class JsonRpcStreamWriter:
    def __init__(self, stream: BinaryIO) -> None:
        self.stream = stream

    def write(self, json_data: Json) -> None:
        serialized = json.dumps(json_data)
        content_length = len(serialized)
        payload = "Content-Length: {c}\n\n{s}".format(c=content_length, s=serialized)
        self._write_string(payload)

    def _write_string(self, s: str) -> None:
        self.stream.write(s.encode())
        self.stream.flush()
