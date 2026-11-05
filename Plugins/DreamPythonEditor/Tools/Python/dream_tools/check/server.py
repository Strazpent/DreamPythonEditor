# SPDX-License-Identifier: Apache-2.0
"""The dream-check server loop (TDD §7.3.3, §7.1, §7.2; INT-07, INT-11).

``python.exe -I <plugin>/Tools/Python/dream_launch.py check --data <snapshot dir> --parent-pid <pid> [--key <key>]``.
A reader thread decodes Content-Length frames from ``sys.stdin.buffer`` into a queue; the main thread handles them,
analyses each document 300 ms after its last change (INT-07's debounce) and publishes diagnostics. A watch thread
exits the process once a second check finds the editor gone (``--parent-pid`` and ``initialize``'s ``processId``).
stdout carries only frames; logs go to stderr, which the editor writes to ``<root>/Logs/LSP/dream-check.log``.

Server-to-client: ``workspace/configuration`` (section ``dream``) after ``didOpen`` and after
``workspace/didChangeConfiguration``. Dream Python Editor notifications: ``dream/skipped {uri, version, bytes}`` (a buffer over 5 MB:
SUM902 alone, the text never arrives) and ``dream/snapshotChanged {path, key}`` (reload the snapshot index).
"""

import argparse
import os
import queue
import sys
import threading
import time

from . import VERSION
from .analysis import DEFAULT_CONFIG, INFORMATION, Diagnostic, analyze
from .document import UTF8, UTF16, Document
from .fixes import code_actions, lsp_diagnostic
from .framing import FrameError, FrameWriter, read_frame
from .snapshot import SnapshotIndex

DEBOUNCE_SECONDS = 0.3
WATCH_SECONDS = 1.0

METHOD_NOT_FOUND = -32601
INVALID_REQUEST = -32600
SERVER_NOT_INITIALIZED = -32002

_EOF = object()


def log(message):
    try:
        sys.stderr.write("dream-check: %s\n" % message)
        sys.stderr.flush()
    except (OSError, ValueError):
        pass


# ---- the parent watch ---------------------------------------------------------------------------------------------

class _ProcessProbe:
    """Whether one process is alive. Windows keeps a SYNCHRONIZE handle, so a reused PID is never mistaken."""

    def __init__(self, pid):
        self.pid = pid
        self.handle = None
        self.gone = False
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            self._kernel32.OpenProcess.restype = wintypes.HANDLE
            self._kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
            self._kernel32.WaitForSingleObject.restype = wintypes.DWORD
            self._kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            synchronize = 0x00100000
            handle = self._kernel32.OpenProcess(synchronize, False, pid)
            if handle:
                self.handle = handle
            elif ctypes.get_last_error() == 87:     # ERROR_INVALID_PARAMETER: no such process
                self.gone = True

    def alive(self):
        if self.gone:
            return False
        if os.name == "nt":
            if self.handle is None:
                return True                         # cannot watch it (access denied): assume alive
            return self._kernel32.WaitForSingleObject(self.handle, 0) != 0    # WAIT_OBJECT_0: it exited
        try:
            os.kill(self.pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True


class ParentWatch:
    def __init__(self):
        self._lock = threading.Lock()
        self._probes = {}
        self._thread = threading.Thread(target=self._loop, name="dream-check-parent-watch", daemon=True)

    def watch(self, pid):
        if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0 or pid == os.getpid():
            return
        with self._lock:
            if pid not in self._probes:
                self._probes[pid] = _ProcessProbe(pid)
        if not self._thread.is_alive():
            try:
                self._thread.start()
            except RuntimeError:
                pass

    def _loop(self):
        while True:
            with self._lock:
                probes = list(self._probes.values())
            for probe in probes:
                if not probe.alive():
                    log("parent process %d is gone; exiting" % probe.pid)
                    os._exit(0)
            time.sleep(WATCH_SECONDS)


# ---- the server ---------------------------------------------------------------------------------------------------

class Server:
    def __init__(self, data_path, key, startup_modules=None, stdin=None, stdout=None):
        self.stdin = stdin if stdin is not None else sys.stdin.buffer
        self.writer = FrameWriter(stdout if stdout is not None else sys.stdout.buffer)
        self.inbox = queue.Queue()
        self.index = SnapshotIndex(data_path, key)
        self.docs = {}
        self.encoding = UTF16
        self.initialized = False
        self.shutdown_requested = False
        self.next_id = 1
        self.pending = {}               # our request id -> uri (workspace/configuration)
        self.watch = ParentWatch()
        self.startup_modules = list(startup_modules or ())

    # ---- io -----------------------------------------------------------------------------------------------------

    def _reader(self):
        try:
            while True:
                message = read_frame(self.stdin)
                if message is None:
                    self.inbox.put(_EOF)
                    return
                self.inbox.put(message)
        except FrameError as error:
            self.inbox.put(("frame-error", str(error)))
        except (OSError, ValueError) as error:
            log("stdin failed: %s" % error)
            self.inbox.put(_EOF)

    def send(self, message):
        message.setdefault("jsonrpc", "2.0")
        try:
            self.writer.write(message)
        except (OSError, ValueError) as error:
            log("stdout failed: %s; exiting" % error)
            os._exit(1)

    def reply(self, request_id, result=None, error=None):
        message = {"jsonrpc": "2.0", "id": request_id}
        if error is not None:
            message["error"] = {"code": error[0], "message": error[1]}
        else:
            message["result"] = result
        self.send(message)

    def request(self, method, params, uri):
        request_id = self.next_id
        self.next_id += 1
        self.pending[request_id] = uri
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})

    # ---- loop ---------------------------------------------------------------------------------------------------

    def run(self, parent_pid=None):
        if parent_pid:
            self.watch.watch(parent_pid)
        threading.Thread(target=self._reader, name="dream-check-reader", daemon=True).start()
        while True:
            timeout = self._next_timeout()
            try:
                message = self.inbox.get(timeout=timeout)
            except queue.Empty:
                message = None
            if message is _EOF:
                log("end of input; exiting")
                return 0 if self.shutdown_requested else 1
            if isinstance(message, tuple) and message and message[0] == "frame-error":
                log("protocol error: %s; exiting" % message[1])
                return 1
            if message is not None:
                result = self.handle(message)
                if result is not None:
                    return result
            self._run_due()

    def _next_timeout(self):
        due = [doc.due for doc in self.docs.values() if doc.due is not None]
        if not due:
            return None
        return max(0.0, min(due) - time.monotonic())

    def _run_due(self):
        now = time.monotonic()
        for doc in list(self.docs.values()):
            if doc.due is not None and doc.due <= now:
                self.analyse(doc)

    def analyse(self, doc):
        doc.due = None
        if doc.skipped_bytes is not None:
            return
        began = time.perf_counter()
        diagnostics = analyze(doc.text, doc.config, self.index)
        elapsed = (time.perf_counter() - began) * 1000.0
        doc.result = diagnostics
        doc.analysed_version = doc.version
        log("analysed %s v%s: %d diagnostics, %d lines in %.1f ms"
            % (doc.uri, doc.version, len(diagnostics), len(doc.lines), elapsed))
        self.publish(doc, diagnostics)

    def publish(self, doc, diagnostics):
        self.send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics", "params": {
            "uri": doc.uri,
            "version": doc.version,
            "diagnostics": [lsp_diagnostic(doc, d, self.encoding) for d in diagnostics],
        }})

    # ---- dispatch -----------------------------------------------------------------------------------------------

    def handle(self, message):
        if not isinstance(message, dict):
            return None
        method = message.get("method")
        if "id" in message and method is None:
            self._on_response(message)
            return None
        params = message.get("params")
        params = params if isinstance(params, dict) else {}
        if "id" in message:
            request_id = message["id"]
            if method == "initialize":
                self.reply(request_id, self._initialize(params))
            elif not self.initialized:
                self.reply(request_id, error=(SERVER_NOT_INITIALIZED, "initialize first"))
            elif self.shutdown_requested:
                self.reply(request_id, error=(INVALID_REQUEST, "the server is shutting down"))
            elif method == "shutdown":
                self.shutdown_requested = True
                self.reply(request_id, None)
            elif method == "textDocument/codeAction":
                self.reply(request_id, self._code_action(params))
            elif method == "dream/debug/modules":
                self.reply(request_id, {"startup": self.startup_modules, "modules": sorted(sys.modules)})
            else:
                self.reply(request_id, error=(METHOD_NOT_FOUND, "method not found: %s" % method))
            return None
        if method == "exit":
            log("exit")
            return 0 if self.shutdown_requested else 1
        handler = self._notifications.get(method)
        if handler is not None and self.initialized:
            handler(self, params)
        return None

    def _initialize(self, params):
        self.initialized = True
        offered = (((params.get("capabilities") or {}).get("general") or {}).get("positionEncodings")) or ()
        self.encoding = UTF8 if UTF8 in offered else UTF16
        self.watch.watch(params.get("processId"))
        return {
            "capabilities": {
                "positionEncoding": self.encoding,
                "textDocumentSync": {"openClose": True, "change": 2},
                "codeActionProvider": {"codeActionKinds": ["quickfix"]},
            },
            "serverInfo": {"name": "dream-check", "version": VERSION},
        }

    def _on_response(self, message):
        uri = self.pending.pop(message.get("id"), None)
        if uri is None:
            return
        doc = self.docs.get(uri)
        if doc is None:
            return
        result = message.get("result")
        if "error" in message or not isinstance(result, list) or not result or not isinstance(result[0], dict):
            return
        config = dict(DEFAULT_CONFIG)
        config.update({k: v for k, v in result[0].items() if k in DEFAULT_CONFIG})
        if config != doc.config:
            changed = doc.config is not None or config != DEFAULT_CONFIG
            doc.config = config
            if changed and doc.skipped_bytes is None:
                doc.due = time.monotonic() if doc.due is None else min(doc.due, time.monotonic())

    def _ask_configuration(self, doc):
        self.request("workspace/configuration", {"items": [{"scopeUri": doc.uri, "section": "dream"}]}, doc.uri)

    # ---- notifications ------------------------------------------------------------------------------------------

    def _did_open(self, params):
        item = params.get("textDocument") or {}
        uri = item.get("uri")
        if not isinstance(uri, str):
            return
        doc = Document(uri, item.get("version", 0), item.get("text") if isinstance(item.get("text"), str) else "")
        previous = self.docs.get(uri)
        if previous is not None:
            doc.config = previous.config
        self.docs[uri] = doc
        doc.due = time.monotonic() + DEBOUNCE_SECONDS
        self._ask_configuration(doc)

    def _did_change(self, params):
        item = params.get("textDocument") or {}
        doc = self.docs.get(item.get("uri"))
        if doc is None or doc.skipped_bytes is not None:
            return
        doc.apply_changes(params.get("contentChanges"), self.encoding)
        if item.get("version") is not None:
            doc.version = item.get("version")
        doc.due = time.monotonic() + DEBOUNCE_SECONDS

    def _did_close(self, params):
        uri = (params.get("textDocument") or {}).get("uri")
        doc = self.docs.pop(uri, None)
        if doc is not None:
            self.publish(doc, [])

    def _skipped(self, params):
        uri = params.get("uri")
        if not isinstance(uri, str):
            return
        doc = self.docs.get(uri)
        if doc is None:
            doc = Document(uri, params.get("version", 0), "")
            self.docs[uri] = doc
        doc.text = ""
        doc.version = params.get("version", doc.version)
        doc.skipped_bytes = params.get("bytes", 0)
        doc.due = None
        size = doc.skipped_bytes if isinstance(doc.skipped_bytes, int) else 0
        diagnostic = Diagnostic("SUM902", INFORMATION,
                                "Not analysed: the buffer is over 5 MB (%s bytes)." % format(size, ","), (0, 0), (0, 0))
        doc.result = [diagnostic]
        doc.analysed_version = doc.version
        self.publish(doc, [diagnostic])

    def _snapshot_changed(self, params):
        path = params.get("path") if isinstance(params.get("path"), str) else self.index.path
        key = params.get("key") if isinstance(params.get("key"), str) else self.index.key
        self.index.reset(path, key)
        log("snapshot changed: %s (%s)" % (path, key))
        now = time.monotonic()
        for doc in self.docs.values():
            if doc.skipped_bytes is None:
                doc.due = now

    def _did_change_configuration(self, params):
        for doc in self.docs.values():
            self._ask_configuration(doc)

    _notifications = {
        "textDocument/didOpen": _did_open,
        "textDocument/didChange": _did_change,
        "textDocument/didClose": _did_close,
        "dream/skipped": _skipped,
        "dream/snapshotChanged": _snapshot_changed,
        "workspace/didChangeConfiguration": _did_change_configuration,
    }

    def _code_action(self, params):
        uri = (params.get("textDocument") or {}).get("uri")
        doc = self.docs.get(uri)
        if doc is None or doc.skipped_bytes is not None:
            return []
        if doc.analysed_version != doc.version or doc.result is None:
            self.analyse(doc)
        requested = params.get("range") or {}
        start = doc.from_lsp(requested.get("start") or {}, self.encoding)
        end = doc.from_lsp(requested.get("end") or {}, self.encoding)
        return code_actions(doc, doc.result or [], start, end, self.encoding)


def main(argv=None, startup_modules=None):
    parser = argparse.ArgumentParser(prog="dream_launch.py check", description="dream-check, Dream's lint server")
    parser.add_argument("--data", default="", help="the Stage 1 snapshot folder (<cache>/<key>/snapshot)")
    parser.add_argument("--parent-pid", type=int, default=0, help="exit when this process is gone")
    parser.add_argument("--key", default="", help="the expected snapshot key")
    args = parser.parse_args(argv)
    server = Server(args.data or None, args.key or None, startup_modules)
    log("started (python %s, data %s)" % (sys.version.split()[0], args.data or "none"))
    code = server.run(args.parent_pid)
    try:
        sys.stdout.flush()
    except (OSError, ValueError):
        pass
    return code
