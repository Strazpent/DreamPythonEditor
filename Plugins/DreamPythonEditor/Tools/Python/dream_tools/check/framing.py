# SPDX-License-Identifier: Apache-2.0
"""LSP base-protocol framing (TDD §7.2.2): ``Content-Length: N\r\n\r\n`` then N bytes of UTF-8 JSON."""

import json
import threading

MAX_HEADER_BYTES = 8 * 1024
MAX_FRAME_BYTES = 64 * 1024 * 1024


class FrameError(Exception):
    """A malformed header, a missing or bad length, an oversized frame or a body that is not JSON."""


def read_frame(stream):
    """The next message from a binary stream, or None at a clean end of input. Raises FrameError."""
    header = b""
    while True:
        line = stream.readline(MAX_HEADER_BYTES + 1)
        if not line:
            if header.strip():
                raise FrameError("end of input inside a header")
            return None
        header += line
        if len(header) > MAX_HEADER_BYTES:
            raise FrameError("header over 8 KB")
        if line in (b"\r\n", b"\n"):
            if header.strip():
                break
            header = b""
    length = None
    for raw in header.split(b"\n"):
        name, sep, value = raw.strip().partition(b":")
        if not sep:
            continue
        if name.strip().lower() == b"content-length":
            try:
                length = int(value.strip())
            except ValueError:
                raise FrameError("bad Content-Length") from None
    if length is None:
        raise FrameError("missing Content-Length")
    if length < 0:
        raise FrameError("bad Content-Length")
    if length > MAX_FRAME_BYTES:
        raise FrameError("frame over 64 MB")
    body = b""
    while len(body) < length:
        chunk = stream.read(length - len(body))
        if not chunk:
            raise FrameError("end of input inside a body")
        body += chunk
    try:
        return json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as error:
        raise FrameError("bad JSON: %s" % error) from None


def encode_frame(message):
    """One frame as bytes: the header and the compact UTF-8 JSON body, no trailing newline."""
    body = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


class FrameWriter:
    """Writes whole frames to a binary stream; one lock, so frames never interleave."""

    def __init__(self, stream):
        self._stream = stream
        self._lock = threading.Lock()

    def write(self, message):
        data = encode_frame(message)
        with self._lock:
            self._stream.write(data)
            self._stream.flush()
