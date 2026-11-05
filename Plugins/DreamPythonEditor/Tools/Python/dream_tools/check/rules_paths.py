# SPDX-License-Identifier: Apache-2.0
"""SUM301: ``os.rename`` / ``shutil.move`` on a Content path (TDD §7.3.3, U5, INT-08).

The first or second argument is a literal, f-string, ``os.path.join`` or ``pathlib`` expression containing
``/Content/``, ``\\Content\\`` or a leading ``/Game/``, or is built from ``unreal.Paths.project_content_dir()`` or
``unreal.SystemLibrary.get_project_content_directory()``. A name is followed to the values it is assigned in its scope
(flow-insensitive, three levels). Moving asset files on disk breaks every reference to them; the message names
``EditorAssetLibrary.rename_asset``.
"""

import ast

from .analysis import WARNING

_MOVERS = {"os.rename": "os.rename", "os.replace": None, "shutil.move": "shutil.move"}
_CONTENT_CALLS = ("unreal.Paths.project_content_dir", "unreal.SystemLibrary.get_project_content_directory")
_JOINS = ("os.path.join", "posixpath.join", "ntpath.join")
_PATH_TYPES = ("pathlib.Path", "pathlib.PurePath", "pathlib.WindowsPath", "pathlib.PureWindowsPath",
               "pathlib.PosixPath", "pathlib.PurePosixPath")
_MAX_NAME_DEPTH = 3
_HOLE = "\x00"


def _content_text(text):
    normalized = text.replace("\\", "/")
    return "/Content/" in normalized or text.startswith("/Game/") or normalized.startswith("/Game/")


class _PathCheck:
    def __init__(self, ctx, scope):
        self.ctx = ctx
        self.scope = scope

    def flat(self, node, depth):
        """The path text an expression builds (unknown parts as a hole), or None when it is not a path expression."""
        ctx = self.ctx
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return node.value
        if isinstance(node, ast.JoinedStr):
            return "".join(v.value if isinstance(v, ast.Constant) and isinstance(v.value, str) else _HOLE
                           for v in node.values)
        if isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Div)):
            left = self.flat(node.left, depth)
            right = self.flat(node.right, depth)
            if left is None and right is None:
                return None
            joiner = "/" if isinstance(node.op, ast.Div) else ""
            return (left if left is not None else _HOLE) + joiner + (right if right is not None else _HOLE)
        if isinstance(node, ast.Call):
            name = ctx.qualname(node.func) or ""
            if name in _JOINS or name in _PATH_TYPES:
                parts = [self.flat(arg, depth) for arg in node.args]
                return "/".join(p if p is not None else _HOLE for p in parts)
        if isinstance(node, ast.Name) and depth < _MAX_NAME_DEPTH:
            values = self._values_of(node.id)
            for value in values:
                text = self.flat(value, depth + 1)
                if text is not None:
                    return text
        return None

    def _values_of(self, name):
        bindings = self.ctx.infer.bindings(self.scope)
        values = [node for kind, node in bindings.assigned.get(name, ()) if kind == "value"]
        if not values and self.scope is not self.ctx.tree:
            module = self.ctx.infer.bindings(self.ctx.tree)
            values = [node for kind, node in module.assigned.get(name, ()) if kind == "value"]
        return values

    def built_from_content(self, node, depth):
        """True when a project-content-dir call is part of the expression (or of a name's assigned values)."""
        cache = self.ctx.cache.setdefault("sum301", {})
        key = (id(node), depth)
        if key not in cache:
            cache[key] = False              # a cycle through names reads as "no"
            cache[key] = self._built_from_content(node, depth)
        return cache[key]

    def _built_from_content(self, node, depth):
        ctx = self.ctx
        for inner in ast.walk(node):
            if isinstance(inner, ast.Call) and (ctx.qualname(inner.func) or "") in _CONTENT_CALLS:
                return True
            if isinstance(inner, ast.Name) and depth < _MAX_NAME_DEPTH and isinstance(inner.ctx, ast.Load):
                for value in self._values_of(inner.id):
                    if self.built_from_content(value, depth + 1):
                        return True
        return False

    def is_content(self, node):
        if self.built_from_content(node, 0):
            return True
        text = self.flat(node, 0)
        if text is None:
            return False
        if _content_text(text):
            return True
        # A join of segments: "Content" as a whole segment between two others.
        segments = text.replace("\\", "/").split("/")
        return "Content" in segments[1:-1] if len(segments) > 2 else False


def run(ctx):
    out = []
    for call in ctx.calls:
        name = ctx.qualname(call.func) or ""
        shown = _MOVERS.get(name)
        if not shown:
            continue
        check = _PathCheck(ctx, ctx.scope_of(call))
        arguments = list(call.args[:2]) + [k.value for k in call.keywords if k.arg in ("src", "dst")]
        if any(check.is_content(argument) for argument in arguments):
            out.append(ctx.diagnostic(
                "SUM301", WARNING,
                "%s on a Content path moves asset files behind the editor's back and breaks references to them; use "
                "unreal.EditorAssetLibrary.rename_asset" % shown, call))
    return out
