# SPDX-License-Identifier: Apache-2.0
"""SUM104: actor edits in a loop with no transaction around them (TDD §7.3.3, U3, U5).

Only when **One undo step per run is off** for the buffer (``oneUndoStepPerRun`` from ``workspace/configuration``):
with it on, the run's own transaction already makes the loop one undo step. A loop body (not a nested ``def``) that
calls a mutator (``set_*``, ``add_*``, ``destroy_*``, ``attach_*``, ``modify``, ``set_editor_property``) on an
``Actor`` or ``ActorComponent`` subclass or on ``EditorActorSubsystem``, outside ``with
unreal.ScopedEditorTransaction(...)``. The outermost such loop is reported once. Fix: wrap the loop in that block.
Without the snapshot the class test falls back to the name (``...Actor``, ``...Component``).
"""

import ast

from .analysis import INFORMATION, Fix

_MUTATOR_PREFIXES = ("set_", "add_", "destroy_", "attach_")
_MUTATOR_NAMES = ("modify", "set_editor_property")
_ACTOR_BASES = ("Actor", "ActorComponent", "EditorActorSubsystem")
MESSAGE = ("Actor edits in a loop without a transaction: with One undo step per run off, each edit is its own undo "
           "step. Wrap the loop in unreal.ScopedEditorTransaction.")


def _is_mutator(name):
    return name in _MUTATOR_NAMES or name.startswith(_MUTATOR_PREFIXES)


def _is_actorish(ctx, klass):
    if ctx.index_usable() and ctx.index.is_class(klass):
        return any(ctx.index.is_subclass(klass, base) for base in _ACTOR_BASES)
    return klass in _ACTOR_BASES or klass.endswith(("Actor", "Component"))


def _in_transaction(ctx, node):
    for ancestor in ctx.ancestors(node):
        if isinstance(ancestor, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            return False
        if isinstance(ancestor, (ast.With, ast.AsyncWith)):
            for item in ancestor.items:
                expr = item.context_expr
                func = expr.func if isinstance(expr, ast.Call) else expr
                if (ctx.qualname(func) or "") == "unreal.ScopedEditorTransaction":
                    return True
    return False


def _body_nodes(loop):
    stack = list(loop.body)
    while stack:
        node = stack.pop()
        yield node
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)):
            continue
        stack.extend(ast.iter_child_nodes(node))


def _mutates(ctx, loop):
    scope = ctx.scope_of(loop)
    for node in _body_nodes(loop):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute) and _is_mutator(node.func.attr):
            klass = ctx.infer.class_of(node.func.value, scope)
            if klass is None:
                klass = ctx.infer.class_ref(node.func.value)
            if klass and _is_actorish(ctx, klass):
                return True
    return False


def _wrap_fix(ctx, loop):
    alias = next((a for a in sorted(ctx.unreal_aliases)), None)
    if alias is None:
        return None
    first, last = loop.lineno - 1, loop.end_lineno - 1
    header = ctx.lines[first]
    prefix = header[: len(header.encode("utf-8")[: loop.col_offset].decode("utf-8", "ignore"))]
    if prefix.strip():
        return None                                     # something precedes the loop on its line
    tail = ctx.lines[last].encode("utf-8")[loop.end_col_offset:].decode("utf-8", "ignore").strip()
    if tail and not tail.startswith("#"):
        return None                                     # something follows the loop on its last line
    # Lines inside a multi-line string must not be re-indented.
    if any(span_first <= last and span_last >= first for span_first, span_last in ctx.multiline_strings()):
        return None
    unit = "\t" if prefix.startswith("\t") else "    "
    lines = [prefix + "with %s.ScopedEditorTransaction(\"Edit actors\"):" % alias]
    for number in range(first, last + 1):
        text = ctx.lines[number]
        lines.append(unit + text if text.strip() else text)
    return Fix("Wrap the loop in %s.ScopedEditorTransaction" % alias,
               [((first, 0), (last, ctx.line_bytes(last)), "\n".join(lines))])


def run(ctx):
    if ctx.config.get("oneUndoStepPerRun", True):
        return []
    out = []
    reported = []
    for loop in sorted(ctx.loops, key=lambda n: (n.lineno, n.col_offset)):
        if reported and _contains(reported[-1], loop):
            continue                                    # inside the loop reported last (loops are in source order)
        if _in_transaction(ctx, loop) or not _mutates(ctx, loop):
            continue
        reported.append(loop)
        header_end = (loop.lineno - 1, ctx.line_bytes(loop.lineno - 1))
        end = header_end if loop.end_lineno > loop.lineno else ctx.end(loop)
        out.append(ctx.diagnostic("SUM104", INFORMATION, MESSAGE, (ctx.start(loop), end), _wrap_fix(ctx, loop)))
    return out


def _contains(outer, node):
    return (outer.lineno, outer.col_offset) <= (node.lineno, node.col_offset) and \
        (node.end_lineno, node.end_col_offset) <= (outer.end_lineno, outer.end_col_offset)
