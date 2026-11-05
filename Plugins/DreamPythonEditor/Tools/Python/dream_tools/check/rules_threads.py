# SPDX-License-Identifier: Apache-2.0
"""SUM201..SUM205: the freeze patterns of LRS-07 (TDD §7.3.3, U5, D24).

* Thread targets: the function given to ``threading.Thread(target=...)`` (or a Thread subclass's constructor), the
  ``run`` method of a ``threading.Thread`` subclass, and callables passed to a ``concurrent.futures`` executor's
  ``submit`` or ``map``; plus the same-module functions they call, down to depth 3 (proposed).
* SUM201 (Warning): ``unreal`` used inside a thread target. Property access is not thread-gated in PythonScriptPlugin
  (``PyUtil.cpp:606-616``), so every use is flagged.
* Everything else is "the game thread": the module and every function outside the thread targets.
* SUM202 (Warning): ``time.sleep`` (or an imported ``sleep``) in a loop on the game thread, outside
  ``@dream_editor.task`` generators. Fix: ``dream_editor.sleep`` plus the import (D24).
* SUM203 (Warning): ``asyncio.run``, ``run_until_complete`` or ``run_forever`` on the game thread.
* SUM204 (Warning): the builtin ``input()`` on the game thread.
* SUM205 (Information): ``.join()`` with no timeout on a value inferred as a ``threading.Thread``.
"""

import ast

from .analysis import INFORMATION, WARNING, Fix

MAX_DEPTH = 3


def _functions_by_name(ctx):
    """Module-level functions and each class's methods: name -> node, (class node, name) -> node."""
    module_functions = {}
    for node in ctx.tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            module_functions[node.name] = node
    methods = {}
    for klass in ctx.classes:
        for node in klass.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                methods[(id(klass), node.name)] = node
    return module_functions, methods


def _enclosing_class(ctx, node):
    for ancestor in ctx.ancestors(node):
        if isinstance(ancestor, ast.ClassDef):
            return ancestor
    return None


class ThreadRegions:
    """The thread-target functions (with their depth and a description) of one module."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.module_functions, self.methods = _functions_by_name(ctx)
        self.regions = {}           # id(function node) -> (node, depth, description)
        self._find_targets()
        self._close()

    def _resolve_callable(self, expr, site):
        ctx = self.ctx
        if isinstance(expr, ast.Lambda):
            return expr
        if isinstance(expr, ast.Call) and (ctx.qualname(expr.func) or "") == "functools.partial" and expr.args:
            return self._resolve_callable(expr.args[0], site)
        if isinstance(expr, ast.Name):
            # A def in the enclosing function first, then a module-level def.
            scope = ctx.scope_of(site)
            if not isinstance(scope, ast.Module):
                for node in getattr(scope, "body", ()):
                    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == expr.id:
                        return node
            return self.module_functions.get(expr.id)
        if isinstance(expr, ast.Attribute) and isinstance(expr.value, ast.Name) and expr.value.id in ("self", "cls"):
            klass = _enclosing_class(ctx, site)
            if klass is not None:
                return self.methods.get((id(klass), expr.attr))
        return None

    def _add(self, node, depth, description):
        if node is None:
            return
        known = self.regions.get(id(node))
        if known is None or known[1] > depth:
            self.regions[id(node)] = (node, depth, description)

    def _find_targets(self):
        ctx = self.ctx
        for call in ctx.calls:
            func = call.func
            is_thread = ctx.is_thread_class(func) or (isinstance(func, ast.Name) and func.id in ctx.local_thread_classes)
            if is_thread:
                target = next((k.value for k in call.keywords if k.arg == "target"), None)
                if target is None and len(call.args) >= 2:
                    target = call.args[1]
                if target is not None:
                    node = self._resolve_callable(target, call)
                    self._add(node, 0, "Thread target %s" % (ctx.segment(target) or "?"))
                continue
            if isinstance(func, ast.Attribute) and func.attr in ("submit", "map") and call.args:
                receiver = ctx.infer.value_type(func.value, ctx.scope_of(call))
                from .infer import EXECUTOR
                if receiver == EXECUTOR or ctx.is_executor_class(getattr(func.value, "func", None)):
                    node = self._resolve_callable(call.args[0], call)
                    self._add(node, 0, "executor %s %s" % (func.attr, ctx.segment(call.args[0]) or "?"))
        for klass in ctx.classes:
            if klass.name in ctx.local_thread_classes:
                run = self.methods.get((id(klass), "run"))
                self._add(run, 0, "%s.run" % klass.name)

    def _close(self):
        frontier = list(self.regions.values())
        while frontier:
            node, depth, description = frontier.pop()
            if depth >= MAX_DEPTH:
                continue
            for inner in _own_nodes(node):
                if isinstance(inner, ast.Call):
                    callee = self._resolve_callable(inner.func, inner)
                    if callee is not None and callee is not node:
                        before = self.regions.get(id(callee))
                        self._add(callee, depth + 1, description)
                        after = self.regions.get(id(callee))
                        if after is not before:
                            frontier.append(after)

    def region_of(self, node):
        """(function node, depth, description) when ``node`` runs in a thread target, else None."""
        for ancestor in self.ctx.ancestors(node):
            found = self.regions.get(id(ancestor))
            if found is not None:
                return found
        return None


def _own_nodes(function):
    stack = [function.body] if isinstance(function, ast.Lambda) else list(function.body)
    while stack:
        node = stack.pop()
        yield node
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)):
            continue
        stack.extend(ast.iter_child_nodes(node))


def _is_task_generator(ctx, node):
    """True when ``node`` is inside a function decorated with ``dream_editor.task``."""
    for ancestor in ctx.ancestors(node):
        if isinstance(ancestor, (ast.FunctionDef, ast.AsyncFunctionDef)):
            for decorator in ancestor.decorator_list:
                target = decorator.func if isinstance(decorator, ast.Call) else decorator
                if (ctx.qualname(target) or "") == "dream_editor.task":
                    return True
    return False


def _dream_editor_import_edit(ctx):
    """(alias to call, the import edit or None)."""
    for local, module in ctx.module_aliases.items():
        if module == "dream_editor":
            return local, None
    body = ctx.tree.body
    index = 0
    if body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant) \
            and isinstance(body[0].value.value, str):
        index = 1
    while index < len(body) and isinstance(body[index], ast.ImportFrom) and body[index].module == "__future__":
        index += 1
    if index < len(body):
        line = body[index].lineno - 1
        return "dream_editor", ((line, 0), (line, 0), "import dream_editor\n")
    if body:
        last = body[-1].end_lineno - 1
        end = (last, ctx.line_bytes(last))
        return "dream_editor", (end, end, "\nimport dream_editor")
    return "dream_editor", ((0, 0), (0, 0), "import dream_editor\n")


def run(ctx):
    out = []
    regions = ThreadRegions(ctx)

    # SUM201
    seen = set()
    for name in ctx.names:
        if not isinstance(name.ctx, ast.Load) or not ctx.is_unreal_name(name):
            continue
        found = regions.region_of(name)
        if found is None:
            continue
        top = ctx.chain_top(name)
        if id(top) in seen:
            continue
        seen.add(id(top))
        _, depth, description = found
        where = description if depth == 0 else "called from %s" % description
        out.append(ctx.diagnostic(
            "SUM201", WARNING,
            "unreal.* in a thread target (%s): the Unreal API is game-thread only; queue the work back to the game "
            "thread" % where, top))

    for call in ctx.calls:
        if regions.region_of(call) is not None:
            continue
        func = call.func
        qualname = ctx.qualname(func) or ""
        # SUM202
        if qualname == "time.sleep" and ctx.enclosing_loop(call) is not None and not _is_task_generator(ctx, call):
            alias, import_edit = _dream_editor_import_edit(ctx)
            edits = [(ctx.start(func), ctx.end(func), "%s.sleep" % alias)]
            if import_edit is not None:
                edits.insert(0, import_edit)
            out.append(ctx.diagnostic(
                "SUM202", WARNING,
                "time.sleep in a loop on the game thread freezes the editor; dream_editor.sleep keeps it responsive "
                "and lets Stop work", call, Fix("Use dream_editor.sleep", edits)))
            continue
        # SUM203
        if qualname == "asyncio.run" or (isinstance(func, ast.Attribute)
                                         and func.attr in ("run_until_complete", "run_forever")
                                         and qualname != "asyncio.run"):
            shown = qualname if qualname == "asyncio.run" else func.attr
            out.append(ctx.diagnostic(
                "SUM203", WARNING,
                "%s on the game thread blocks the editor until the event loop finishes" % shown, call))
            continue
        # SUM204
        if isinstance(func, ast.Name) and func.id == "input" and not ctx.infer.is_bound("input", ctx.scope_of(call)):
            out.append(ctx.diagnostic(
                "SUM204", WARNING,
                "input() on the game thread blocks the editor waiting for console input that never comes", call))
            continue
        # SUM205
        if isinstance(func, ast.Attribute) and func.attr == "join" and not call.args and not call.keywords:
            from .infer import THREAD
            if ctx.infer.value_type(func.value, ctx.scope_of(call)) == THREAD:
                out.append(ctx.diagnostic(
                    "SUM205", INFORMATION,
                    "Thread.join() with no timeout blocks the editor until the thread ends; pass a timeout or poll "
                    "is_alive()", call))
    return out
