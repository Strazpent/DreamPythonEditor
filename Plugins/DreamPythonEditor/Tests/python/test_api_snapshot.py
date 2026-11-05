# SPDX-License-Identifier: Apache-2.0
"""T-API-006: the public API snapshot (TDD §12.2 "Snapshot test", D29, API-01, OI-31).

``dream_editor`` is imported with a fake ``unreal`` module; each public name (``__all__`` and ``__version__``) is
recorded with its kind, ``inspect.signature``, first doc line and ``since`` (the plugin version it appeared in), and the
result must equal ``Tests/python/api/dream_editor_api.json``. A difference fails unless the snapshot is updated and the
plugin's ``CHANGELOG.md`` has a line starting ``API:`` that names the snapshot's digest (the first 12 hex digits of the
SHA-256 over its canonical ``names`` section), so an API change cannot land without a documented CHANGELOG entry.

Update after an intended change (engine python, repository root)::

    python.exe -I -B Plugins/DreamPythonEditor/Tests/python/test_api_snapshot.py --update

It rewrites the JSON (``since`` of existing names is kept, new names get the current ``__version__``) and prints the
``API:`` line to add to CHANGELOG.md.
"""

import hashlib
import inspect
import json
import os
import re
import sys
import types
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_PLUGIN = os.path.normpath(os.path.join(_HERE, "..", ".."))
CONTENT_PYTHON = os.path.join(_PLUGIN, "Content", "Python")
SNAPSHOT = os.path.join(_HERE, "api", "dream_editor_api.json")
CHANGELOG = os.path.join(_PLUGIN, "CHANGELOG.md")
SCHEMA = 1


def _fake_unreal():
    module = types.ModuleType("unreal")
    module.log = module.log_warning = module.log_error = lambda text: None
    return module


def _import_package():
    if CONTENT_PYTHON not in sys.path:
        sys.path.insert(0, CONTENT_PYTHON)
    installed = "unreal" not in sys.modules
    if installed:
        sys.modules["unreal"] = _fake_unreal()
    try:
        import dream_editor
    finally:
        if installed:
            sys.modules.pop("unreal", None)
    return dream_editor


def _first_doc_line(obj):
    doc = inspect.getdoc(obj) or ""
    return doc.strip().splitlines()[0] if doc.strip() else ""


def _signature(obj):
    try:
        return str(inspect.signature(obj))
    except (TypeError, ValueError):
        return None


def _describe(name, obj):
    if inspect.isclass(obj):
        entry = {
            "kind": "class",
            "bases": [f"{base.__module__}.{base.__qualname__}" if base.__module__ != "builtins" else base.__qualname__
                      for base in obj.__bases__],
            "doc": _first_doc_line(obj),
            "attributes": {key: {"type": type(value).__name__, "default": value}
                           for key, value in sorted(vars(obj).items())
                           if not key.startswith("_") and isinstance(value, (int, str, float, bool, type(None)))},
        }
    elif callable(obj):
        entry = {"kind": "function", "signature": _signature(obj), "doc": _first_doc_line(obj)}
    else:
        entry = {"kind": type(obj).__name__}
        if name == "__version__":
            entry["kind"] = "version"
    return entry


def build_names(package):
    """{name: description} of the public surface (``__all__`` plus ``__version__``), without ``since``."""
    names = {"__version__": _describe("__version__", package.__version__)}
    for name in package.__all__:
        names[name] = _describe(name, getattr(package, name))
    return names


def digest(names):
    canonical = json.dumps(names, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return hashlib.sha256(canonical.encode("ascii")).hexdigest()[:12]


def load_snapshot():
    with open(SNAPSHOT, encoding="utf-8") as handle:
        return json.load(handle)


def _changelog_api_lines():
    if not os.path.exists(CHANGELOG):
        return []
    with open(CHANGELOG, encoding="utf-8") as handle:
        return [line.strip() for line in handle if line.strip().startswith("API:")]


def _version_tuple(text):
    return tuple(int(part) for part in re.findall(r"\d+", text)[:3])


class ApiSnapshot(unittest.TestCase):
    """T-API-006: matches dream_editor_api.json; an undocumented diff fails."""

    @classmethod
    def setUpClass(cls):
        cls.package = _import_package()
        cls.snapshot = load_snapshot()

    def test_schema(self):
        self.assertEqual(self.snapshot.get("schema"), SCHEMA)
        self.assertIsInstance(self.snapshot.get("names"), dict)

    def test_the_public_api_matches_the_snapshot(self):
        recorded = {name: {key: value for key, value in entry.items() if key != "since"}
                    for name, entry in self.snapshot["names"].items()}
        live = build_names(self.package)
        if live != recorded:
            added = sorted(set(live) - set(recorded))
            removed = sorted(set(recorded) - set(live))
            changed = sorted(name for name in set(live) & set(recorded) if live[name] != recorded[name])
            self.fail("the public dream_editor API differs from Tests/python/api/dream_editor_api.json "
                      f"(added {added}, removed {removed}, changed {changed}): update the snapshot with "
                      "test_api_snapshot.py --update and add the CHANGELOG.md line starting 'API:' that it prints")

    def test_every_name_has_a_since_version(self):
        current = _version_tuple(self.package.__version__)
        for name, entry in self.snapshot["names"].items():
            with self.subTest(name=name):
                since = entry.get("since")
                self.assertIsInstance(since, str)
                self.assertLessEqual(_version_tuple(since), current)

    def test_the_snapshot_digest_is_documented_in_the_changelog(self):
        names = {name: {key: value for key, value in entry.items() if key != "since"}
                 for name, entry in self.snapshot["names"].items()}
        expected = digest(names)
        self.assertEqual(self.snapshot.get("digest"), expected,
                         "dream_editor_api.json was edited by hand: its digest does not match its names")
        lines = _changelog_api_lines()
        self.assertTrue(any(expected in line for line in lines),
                        f"CHANGELOG.md has no line starting 'API:' that names the snapshot digest {expected}")


def update():
    package = _import_package()
    live = build_names(package)
    previous = {}
    if os.path.exists(SNAPSHOT):
        previous = load_snapshot().get("names", {})
    names = {}
    for name, entry in live.items():
        since = previous.get(name, {}).get("since") or package.__version__
        names[name] = dict(entry, since=since)
    snapshot = {"schema": SCHEMA, "package": "dream_editor", "version": package.__version__,
                "digest": digest(live), "names": names}
    with open(SNAPSHOT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(snapshot, handle, indent=2, sort_keys=True, ensure_ascii=False)
        handle.write("\n")
    print(f"wrote {SNAPSHOT}")
    print(f"add to CHANGELOG.md: API: {package.__version__} {snapshot['digest']} <what changed>")


if __name__ == "__main__":
    if "--update" in sys.argv[1:]:
        update()
    else:
        unittest.main()
