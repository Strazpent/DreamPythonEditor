# SPDX-License-Identifier: Apache-2.0
"""dream-check as a process (TDD §7.3.3, §7.12): the LSP loop over framed stdin/stdout, the debounce (T-LSP-031), the
parent watch (T-LSP-009), the import audit (T-LSP-032), ``dream/skipped`` (T-DIAG-027), the snapshot appearing while
the server runs (T-STUB-025), configuration, code actions, position encodings and the exit codes."""

import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

import check_support as support

CONFIG = {"oneUndoStepPerRun": True, "bufferKind": "scratch", "namespaceMode": "session", "sum102NotifyVariant": False}


def codes(params):
    return sorted(d["code"] for d in params["diagnostics"])


class ServerTestCase(unittest.TestCase):
    def start(self, **kwargs):
        kwargs.setdefault("data", support.fixture_snapshot())
        kwargs.setdefault("key", support.SNAPSHOT_KEY)
        kwargs.setdefault("config", dict(CONFIG))
        server = support.ServerProcess(**kwargs)
        self.addCleanup(server.kill)
        return server


class Lifecycle(ServerTestCase):
    def test_initialize_capabilities(self):
        server = self.start()
        result = server.initialize()
        self.assertEqual(result["capabilities"]["positionEncoding"], "utf-8")
        self.assertEqual(result["capabilities"]["textDocumentSync"], {"openClose": True, "change": 2})
        self.assertEqual(result["capabilities"]["codeActionProvider"], {"codeActionKinds": ["quickfix"]})
        self.assertEqual(result["serverInfo"]["name"], "dream-check")
        self.assertEqual(server.close(), 0)

    def test_utf16_when_utf8_is_not_offered(self):
        server = self.start()
        self.assertEqual(server.initialize(encodings=("utf-16",))["capabilities"]["positionEncoding"], "utf-16")
        uri = support.file_uri("astral.py")
        server.open(uri, 'import unreal\nx = ("\U0001F600", unreal.EditorAssetLibary)\n')
        diagnostic = next(d for d in server.wait_diagnostics(uri)["diagnostics"] if d["code"] == "SUM101")
        # 'x = ("' is 6 units, the emoji 2 in UTF-16, then '", ' 3: the name starts at 11.
        self.assertEqual(diagnostic["range"]["start"], {"line": 1, "character": 11})
        self.assertEqual(server.close(), 0)

    def test_utf8_columns(self):
        server = self.start()
        server.initialize()
        uri = support.file_uri("astral8.py")
        server.open(uri, 'import unreal\nx = ("\U0001F600", unreal.EditorAssetLibary)\n')
        diagnostic = next(d for d in server.wait_diagnostics(uri)["diagnostics"] if d["code"] == "SUM101")
        self.assertEqual(diagnostic["range"]["start"], {"line": 1, "character": 13})   # the emoji is 4 bytes
        self.assertEqual(server.close(), 0)

    def test_requests_before_initialize_and_unknown_methods(self):
        server = self.start()
        early = server.request("textDocument/codeAction", {})
        self.assertEqual(early["error"]["code"], -32002)
        server.initialize()
        unknown = server.request("textDocument/hover", {})
        self.assertEqual(unknown["error"]["code"], -32601)
        server.notify("$/cancelRequest", {"id": 1})
        server.notify("some/unknownNotification", {})
        self.assertEqual(server.close(), 0)

    def test_exit_without_shutdown_is_1(self):
        server = self.start()
        server.initialize()
        server.notify("exit", None)
        self.assertEqual(server.process.wait(10), 1)

    def test_malformed_frame_exits(self):
        server = self.start()
        server.initialize()
        server.process.stdin.write(b"Content-Length: 5\r\n\r\n{nope")
        server.process.stdin.flush()
        self.assertEqual(server.process.wait(10), 1)

    def test_stdin_eof_exits(self):
        server = self.start()
        server.initialize()
        server.process.stdin.close()
        self.assertIn(server.process.wait(10), (0, 1))


class Documents(ServerTestCase):
    def test_publish_change_close(self):
        server = self.start()
        server.initialize()
        uri = support.file_uri("doc.py")
        server.open(uri, "import unreal\nx = unreal.StaticMesh()\n")
        self.assertEqual(server.wait_diagnostics(uri, 1)["diagnostics"], [])
        server.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [
            {"range": {"start": {"line": 1, "character": 11}, "end": {"line": 1, "character": 21}},
             "text": "StaticMesj"}]})
        params = server.wait_diagnostics(uri, 2)
        self.assertEqual(codes(params), ["SUM101"])
        self.assertIn("did you mean 'unreal.StaticMesh'", params["diagnostics"][0]["message"])
        server.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 3},
                                                 "contentChanges": [{"text": "import os\n"}]})
        self.assertEqual(server.wait_diagnostics(uri, 3)["diagnostics"], [])
        server.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
        self.assertEqual(server.wait_diagnostics(uri)["diagnostics"], [])
        self.assertEqual(server.close(), 0)

    def test_syntax_error_is_one_item(self):
        server = self.start()
        server.initialize()
        uri = support.file_uri("broken.py")
        server.open(uri, "import time\nfor i in range(3):\n    time.sleep(1)\nx = (\n")
        params = server.wait_diagnostics(uri)
        self.assertEqual(codes(params), ["syntax-error"])
        self.assertEqual(params["diagnostics"][0]["severity"], 1)
        self.assertTrue(params["diagnostics"][0]["message"].startswith("SyntaxError: "))
        self.assertEqual(server.close(), 0)

    def test_configuration_round_trip(self):
        # workspace/configuration (section dream) after didOpen and after didChangeConfiguration (§7.2.4).
        server = self.start(config=dict(CONFIG, oneUndoStepPerRun=False))
        server.initialize()
        uri = support.file_uri("loop.py")
        server.open(uri, support.read_fixture("sum104_pos.py"))
        params = server.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                             and "SUM104" in codes(m["params"]))["params"]
        self.assertEqual(codes(params).count("SUM104"), 3)
        server.config = dict(CONFIG)
        server.notify("workspace/didChangeConfiguration", {"settings": {}})
        params = server.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                             and "SUM104" not in codes(m["params"]))["params"]
        self.assertEqual(codes(params), [])
        self.assertEqual(server.close(), 0)

    def test_null_configuration_keeps_defaults(self):
        server = self.start(config=None)
        server.initialize()
        uri = support.file_uri("defaults.py")
        server.open(uri, support.read_fixture("sum104_pos.py"))
        self.assertNotIn("SUM104", codes(server.wait_diagnostics(uri)))
        self.assertEqual(server.close(), 0)

    def test_code_actions(self):
        # §7.8.3, INT-09: the SUM102 rewrite (preferred) and the noqa suppression, versioned.
        server = self.start()
        server.initialize()
        uri = support.file_uri("fix.py")
        text = "import unreal\nmesh = unreal.StaticMesh()\nmesh.lod_group = 'LargeProp'\n"
        server.open(uri, text, version=7)
        server.wait_diagnostics(uri, 7)
        reply = server.request("textDocument/codeAction", {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 2, "character": 3}, "end": {"line": 2, "character": 3}},
            "context": {"diagnostics": []}})
        actions = reply["result"]
        self.assertEqual([a["title"] for a in actions], ["Use set_editor_property", "Suppress SUM102 on this line"])
        self.assertTrue(actions[0]["isPreferred"])
        change = actions[0]["edit"]["documentChanges"][0]
        self.assertEqual(change["textDocument"], {"uri": uri, "version": 7})
        self.assertEqual(change["edits"], [{"range": {"start": {"line": 2, "character": 0},
                                                      "end": {"line": 2, "character": 28}},
                                            "newText": "mesh.set_editor_property(\"lod_group\", 'LargeProp')"}])
        self.assertEqual(actions[0]["kind"], "quickfix")
        self.assertEqual(actions[0]["diagnostics"][0]["code"], "SUM102")
        noqa = actions[1]["edit"]["documentChanges"][0]["edits"][0]
        self.assertEqual(noqa["newText"], "  # noqa: SUM102")
        nothing = server.request("textDocument/codeAction", {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 0}},
            "context": {"diagnostics": []}})
        self.assertEqual(nothing["result"], [])
        self.assertEqual(server.close(), 0)

    def test_code_action_after_an_unanalysed_change(self):
        server = self.start()
        server.initialize()
        uri = support.file_uri("fresh.py")
        server.open(uri, "import unreal\n")
        server.wait_diagnostics(uri, 1)
        server.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [
            {"range": {"start": {"line": 1, "character": 0}, "end": {"line": 1, "character": 0}},
             "text": "x = unreal.EditorAssetLibary\n"}]})
        reply = server.request("textDocument/codeAction", {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 1, "character": 6}, "end": {"line": 1, "character": 6}},
            "context": {"diagnostics": []}})
        self.assertEqual(reply["result"][0]["title"], "Replace with unreal.EditorAssetLibrary")
        self.assertEqual(reply["result"][0]["edit"]["documentChanges"][0]["textDocument"]["version"], 2)
        self.assertEqual(server.close(), 0)

    def test_skipped_buffer(self):
        # T-DIAG-027: dream/skipped for a 6 MB buffer gives SUM902 only; no text received or parsed.
        server = self.start()
        server.initialize()
        uri = support.file_uri("huge.py")
        server.notify("dream/skipped", {"uri": uri, "version": 4, "bytes": 6 * 1024 * 1024})
        params = server.wait_diagnostics(uri, 4)
        self.assertEqual(codes(params), ["SUM902"])
        self.assertEqual(params["diagnostics"][0]["severity"], 3)
        # A buffer that crosses 5 MB after it was opened: the skip replaces its text and diagnostics.
        small = support.file_uri("grows.py")
        server.open(small, "import unreal\nx = unreal.EditorAssetLibary\n")
        self.assertEqual(codes(server.wait_diagnostics(small, 1)), ["SUM101"])
        server.notify("dream/skipped", {"uri": small, "version": 2, "bytes": 5 * 1024 * 1024 + 1})
        self.assertEqual(codes(server.wait_diagnostics(small, 2)), ["SUM902"])
        self.assertEqual(server.close(), 0)
        self.assertFalse(any("analysed %s" % uri in line for line in server.stderr_lines))


class Debounce(ServerTestCase):
    def test_no_analysis_while_typing(self):
        # T-LSP-031: a 5,000-line file, edits 100 ms apart for 5 s: no analysis while edits arrive < 300 ms apart;
        # diagnostics <= 1 s after the last change.
        import test_check_rules
        server = self.start()
        server.initialize()
        uri = support.file_uri("big.py")
        text = test_check_rules.five_thousand_lines()
        server.open(uri, text, version=1)
        server.wait_diagnostics(uri, 1, timeout=30)
        last_version = 1
        sent_at = {}
        began = time.monotonic()
        while time.monotonic() - began < 5.0:
            last_version += 1
            sent_at[last_version] = time.monotonic()
            server.notify("textDocument/didChange", {"textDocument": {"uri": uri, "version": last_version},
                                                     "contentChanges": [{"range": {
                                                         "start": {"line": 3, "character": 0},
                                                         "end": {"line": 3, "character": 0}}, "text": "# edit\n"}]})
            time.sleep(0.1)
        last_change = time.monotonic()
        params = server.wait_diagnostics(uri, last_version, timeout=5)
        latency = time.monotonic() - last_change
        print("\nT-LSP-031: %d edits; diagnostics %.0f ms after the last change" % (last_version - 1, latency * 1000))
        self.assertLessEqual(latency, 1.0)

        def debounced(version):
            # An analysis of `version` breaks the debounce only when the next edit followed it by less than 300 ms. Under
            # build load this loop can stall past 300 ms between two sends; the debounce then rightly ran out (review r1).
            # 50 ms of slack covers the pipe and the server's own clock.
            following = sent_at.get(version + 1)
            return following is not None and version in sent_at and following - sent_at[version] < 0.3 - 0.05

        early = [m for m in server.notifications if m.get("method") == "textDocument/publishDiagnostics"
                 and m["params"]["uri"] == uri and m["params"]["version"] != last_version
                 and debounced(m["params"]["version"])]
        self.assertEqual(early, [], "an analysis ran while edits were arriving")
        self.assertTrue(params["diagnostics"])
        during = []
        for line in server.stderr_lines:
            if "analysed %s v" % uri in line:
                version = int(line.split("analysed %s v" % uri)[1].split(":")[0])
                if 1 < version < last_version and debounced(version):
                    during.append(line)
        self.assertEqual(during, [])
        stalls = sum(1 for v in sent_at if v + 1 in sent_at and sent_at[v + 1] - sent_at[v] >= 0.25)
        if stalls:
            print("T-LSP-031: %d send gaps of 250 ms or more (load); analyses after them are allowed" % stalls)
        self.assertEqual(server.close(), 0)


class ParentWatch(ServerTestCase):
    def dummy_parent(self):
        process = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(60)"],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        return process

    def assert_exits_within(self, server, seconds):
        began = time.monotonic()
        try:
            server.process.wait(seconds + 1.0)
        except subprocess.TimeoutExpired:
            self.fail("dream-check outlived its parent")
        elapsed = time.monotonic() - began
        self.assertLessEqual(elapsed, seconds, "exited after %.2f s" % elapsed)

    def test_parent_pid_argument(self):
        # T-LSP-009: exits within 2 s of the parent's death.
        parent = self.dummy_parent()
        server = self.start(parent_pid=parent.pid)
        server.initialize()
        parent.kill()
        parent.wait(5)
        self.assert_exits_within(server, 2.0)
        self.assertTrue(any("parent process %d is gone" % parent.pid in line for line in server.stderr_lines)
                        or server.process.returncode == 0)

    def test_initialize_process_id(self):
        # T-LSP-009: initialize's processId is watched as well.
        parent = self.dummy_parent()
        server = self.start(parent_pid=os.getpid())
        server.initialize(process_id=parent.pid)
        parent.kill()
        parent.wait(5)
        self.assert_exits_within(server, 2.0)


class ImportAudit(ServerTestCase):
    def test_only_stdlib_dream_tools_and_docstrings(self):
        # T-LSP-032: under -I, dream-check imports only the standard library, dream_tools and dream_docstrings.
        server = self.start()
        server.initialize()
        uri = support.file_uri("audit.py")
        server.open(uri, support.read_fixture("sum201_pos.py") + support.read_fixture("sum102_pos.py"))
        server.wait_diagnostics(uri)
        reply = server.request("textDocument/codeAction", {
            "textDocument": {"uri": uri},
            "range": {"start": {"line": 0, "character": 0}, "end": {"line": 200, "character": 0}},
            "context": {"diagnostics": []}})
        self.assertTrue(reply["result"])
        modules = server.request("dream/debug/modules", {})["result"]
        added = set(modules["modules"]) - set(modules["startup"])
        foreign = sorted(name for name in added if not (
            name.split(".")[0] in sys.stdlib_module_names or name == "dream_docstrings" or name == "dream_tools"
            or name.startswith("dream_tools.") or name == "__main__"))
        self.assertEqual(foreign, [])
        self.assertIn("dream_docstrings", added)
        self.assertFalse(any(name.startswith("dream_editor") for name in modules["modules"]))
        self.assertEqual(server.close(), 0)

    def test_unknown_command_and_usage(self):
        result = subprocess.run([sys.executable, "-I", support.LAUNCHER, "frobnicate"], stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 2)
        result = subprocess.run([sys.executable, "-I", support.LAUNCHER], stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 2)


class SnapshotLifecycle(ServerTestCase):
    def test_building_then_changed(self):
        # T-STUB-025 end to end: SUM900 while the walk is interrupted; dream/snapshotChanged once it completes.
        folder = os.path.join(tempfile.mkdtemp(prefix="dream-resume-"), "snapshot")
        self.addCleanup(shutil.rmtree, os.path.dirname(folder), True)
        modules = support.build_fake_unreal()
        writer = support.SnapshotWriter(folder)
        self.assertFalse(writer.run(modules=modules, budget_ns=600_000, stop_after_modules=2))
        self.assertFalse(os.path.exists(os.path.join(folder, ".complete")))

        server = self.start(data=folder)
        server.initialize()
        uri = support.file_uri("snap.py")
        server.open(uri, "import unreal\nx = unreal.EditorAssetLibary\n")
        params = server.wait_diagnostics(uri)
        self.assertEqual(codes(params), ["SUM900"])
        self.assertIn("being built", params["diagnostics"][0]["message"])

        import json
        with open(os.path.join(folder, "progress.json"), encoding="utf-8") as handle:
            done = json.load(handle)
        resumed = support.SnapshotWriter(folder)
        resumed.entries = list(done)
        self.assertTrue(resumed.run(modules=modules, done=done))
        server.notify("dream/snapshotChanged", {"path": folder, "key": support.SNAPSHOT_KEY})
        params = server.wait(lambda m: m.get("method") == "textDocument/publishDiagnostics"
                             and "SUM900" not in codes(m["params"]))["params"]
        self.assertEqual(codes(params), ["SUM101"])
        self.assertEqual(server.close(), 0)


if __name__ == "__main__":
    unittest.main()
