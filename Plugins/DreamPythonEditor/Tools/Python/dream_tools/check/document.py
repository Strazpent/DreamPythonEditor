# SPDX-License-Identifier: Apache-2.0
"""Open documents, LSP positions and URIs (TDD §7.2.3).

Positions inside dream-check are ``(line, byte column)``: 0-based lines and UTF-8 byte columns, which is what ``ast``
reports (``col_offset``). They are converted to the negotiated position encoding only on the wire: ``utf-8`` (the
client's preference, §7.2.3) counts bytes, ``utf-16`` counts code units.
"""

import os
import urllib.parse

UTF8 = "utf-8"
UTF16 = "utf-16"


def utf16_len(text):
    """UTF-16 code units in ``text``."""
    return len(text) + sum(1 for char in text if ord(char) > 0xFFFF)


def byte_col_to_char(line, byte_col):
    """The ``str`` index of a UTF-8 byte column in ``line`` (clamped; a column inside a character rounds down)."""
    if byte_col <= 0:
        return 0
    data = line.encode("utf-8", "surrogatepass")
    if byte_col >= len(data):
        return len(line)
    return len(data[:byte_col].decode("utf-8", "ignore"))


def char_to_byte_col(line, index):
    """The UTF-8 byte column of ``str`` index ``index`` in ``line``."""
    return len(line[: max(0, index)].encode("utf-8", "surrogatepass"))


def unit_to_char(line, units, encoding):
    """The ``str`` index of an LSP ``character`` in ``line`` (clamped to the line)."""
    if units <= 0:
        return 0
    if encoding == UTF8:
        return byte_col_to_char(line, units)
    count = 0
    for index, char in enumerate(line):
        if count >= units:
            return index
        count += 2 if ord(char) > 0xFFFF else 1
    return len(line)


def char_to_unit(line, index, encoding):
    """The LSP ``character`` of ``str`` index ``index`` in ``line``."""
    prefix = line[: max(0, index)]
    if encoding == UTF8:
        return len(prefix.encode("utf-8", "surrogatepass"))
    return utf16_len(prefix)


def uri_to_path(uri):
    """A ``file:`` URI as a local path (``file:///C:/x%20y.py`` -> ``C:/x y.py``); other URIs unchanged."""
    parsed = urllib.parse.urlparse(uri)
    if parsed.scheme != "file":
        return uri
    path = urllib.parse.unquote(parsed.path)
    if os.name == "nt" and len(path) >= 3 and path[0] == "/" and path[2] == ":":
        path = path[1:]
    return path


class Document:
    """One open document: its text, version, configuration and analysis state (server thread only)."""

    def __init__(self, uri, version, text):
        self.uri = uri
        self.version = version
        self._text = text
        self._lines = None
        self.skipped_bytes = None       # set by dream/skipped: the text is dropped and never parsed
        self.config = None              # the workspace/configuration answer, None until it arrives
        self.due = None                 # monotonic time of the next analysis (the 300 ms debounce)
        self.result = None              # the last analysis: (version, [Diagnostic])
        self.analysed_version = None

    @property
    def text(self):
        return self._text

    @text.setter
    def text(self, value):
        self._text = value
        self._lines = None

    @property
    def lines(self):
        """The text split at ``\\n`` (a trailing ``\\n`` gives a last empty line, as LSP counts lines)."""
        if self._lines is None:
            self._lines = self._text.split("\n")
        return self._lines

    def line(self, number):
        lines = self.lines
        return lines[number] if 0 <= number < len(lines) else ""

    def offset_of(self, position, encoding):
        """The ``str`` offset of an LSP position (clamped)."""
        lines = self.lines
        line_no = position.get("line", 0) if isinstance(position, dict) else 0
        units = position.get("character", 0) if isinstance(position, dict) else 0
        if line_no < 0:
            return 0
        if line_no >= len(lines):
            return len(self._text)
        offset = self._line_start(line_no)
        return offset + unit_to_char(lines[line_no], units, encoding)

    def _line_start(self, line_no):
        index = -1
        for _ in range(line_no):
            index = self._text.find("\n", index + 1)
            if index < 0:
                return len(self._text)
        return index + 1

    def apply_changes(self, changes, encoding):
        """``textDocument/didChange`` content changes, in order: ranged (incremental) or whole-text."""
        for change in changes or ():
            if not isinstance(change, dict):
                continue
            new_text = change.get("text", "")
            if not isinstance(new_text, str):
                continue
            change_range = change.get("range")
            if not isinstance(change_range, dict):
                self.text = new_text
                continue
            start = self.offset_of(change_range.get("start") or {}, encoding)
            end = self.offset_of(change_range.get("end") or {}, encoding)
            if end < start:
                start, end = end, start
            self.text = self._text[:start] + new_text + self._text[end:]

    def to_lsp(self, position, encoding):
        """An internal ``(line, byte column)`` as an LSP position dict."""
        line_no, byte_col = position
        line = self.line(line_no)
        return {"line": max(0, line_no), "character": char_to_unit(line, byte_col_to_char(line, byte_col), encoding)}

    def range_to_lsp(self, start, end, encoding):
        return {"start": self.to_lsp(start, encoding), "end": self.to_lsp(end, encoding)}

    def from_lsp(self, position, encoding):
        """An LSP position dict as an internal ``(line, byte column)``."""
        line_no = position.get("line", 0) if isinstance(position, dict) else 0
        units = position.get("character", 0) if isinstance(position, dict) else 0
        line = self.line(line_no)
        return line_no, char_to_byte_col(line, unit_to_char(line, units, encoding))
