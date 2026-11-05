# SPDX-License-Identifier: Apache-2.0
"""SUM101, SUM102 and SUM103: the ``unreal`` API against the Stage 1 snapshot (TDD §7.3.3, U5, INT-08, N2).

Runs only with a usable snapshot (analysis skips this family otherwise, and SUM900 says why).

* SUM101 unknown ``unreal.*`` name. An Attribute chain rooted at a name bound by ``import unreal [as X]``, and
  ``from unreal import N``. Level 1 against the top-level names; level 2 against the class members (bases included)
  when level 1 is a known class. ``getattr(unreal, ...)`` and names assigned into ``unreal`` in the same file are
  skipped. Fix: ``Replace with unreal.<closest>`` (``difflib.get_close_matches``, cutoff 0.8).
* SUM102 attribute assignment where ``set_editor_property`` is needed. ``obj.p = v`` / ``obj.p += v`` where ``obj``'s
  class C is inferred and ``p`` is one of C's editor properties (own and inherited): edit-only (no getset, so the
  assignment raises), or a ``[Read-Only]`` getset the editor list calls ``[Read-Write]`` (BlueprintReadOnly but
  editable, N2). Unknown C: Information when ``p`` is edit-only on some class, an attribute on none, and ``obj`` is
  not ``self`` or ``cls``. The off-by-default variant (``sum102NotifyVariant``) also flags ``[Read-Write]``
  attributes. Fix: ``obj.set_editor_property("p", v)`` (AugAssign reads ``get_editor_property`` first); none for
  chained or tuple targets.
* SUM103 deprecated API: a member, type or module-level function whose doc says ``deprecated: 'old' was renamed to
  'new'.`` (fix: rename) or that is otherwise marked deprecated.
"""

import ast
import difflib

from .analysis import INFORMATION, WARNING, Fix

_EDIT_ONLY = ("`{p}` is not a Python attribute of `{c}` (editor-only property); this assignment raises "
              "AttributeError. Use set_editor_property.")
_READ_ONLY = "`{p}` is read-only as an attribute; use set_editor_property."
_NOTIFY = "Attribute writes skip edit-change notifications; set_editor_property sends them."

_AUG_OPS = {
    ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/", ast.FloorDiv: "//", ast.Mod: "%", ast.Pow: "**",
    ast.LShift: "<<", ast.RShift: ">>", ast.BitOr: "|", ast.BitXor: "^", ast.BitAnd: "&", ast.MatMult: "@",
}
_ATOMS = (ast.Name, ast.Constant, ast.Attribute, ast.Call, ast.Subscript, ast.List, ast.Tuple, ast.Dict, ast.Set,
          ast.ListComp, ast.SetComp, ast.DictComp, ast.GeneratorExp, ast.JoinedStr)


def _closest(name, candidates):
    found = difflib.get_close_matches(name, list(candidates), n=1, cutoff=0.8)
    return found[0] if found else None


def _assigned_into_unreal(ctx):
    """Names a file assigns into the unreal module (``unreal.X = ...``, ``setattr(unreal, "X", ...)``)."""
    names = set()
    for node in ctx.attributes:
        if isinstance(node.ctx, (ast.Store, ast.Del)) and isinstance(node.value, ast.Name) \
                and node.value.id in ctx.unreal_aliases:
            names.add(node.attr)
    for call in ctx.calls:
        if isinstance(call.func, ast.Name) and call.func.id == "setattr" and len(call.args) >= 2 \
                and isinstance(call.args[0], ast.Name) and call.args[0].id in ctx.unreal_aliases \
                and isinstance(call.args[1], ast.Constant) and isinstance(call.args[1].value, str):
            names.add(call.args[1].value)
    return names


def _sum101_and_103(ctx, out):
    index = ctx.index
    skipped = _assigned_into_unreal(ctx)
    reported = set()

    def level1(node, name, alias_text, name_range, rename_range):
        if name.startswith("__") or name in skipped:
            return False
        if not index.has_name(name):
            closest = _closest(name, index.top_names())
            message = "Unknown Unreal name '%s.%s'" % (alias_text, name)
            fix = None
            if closest:
                message += "; did you mean '%s.%s'?" % (alias_text, closest)
                fix = Fix("Replace with %s.%s" % (alias_text, closest), [(rename_range[0], rename_range[1], closest)])
            out.append(ctx.diagnostic("SUM101", WARNING, message, name_range, fix))
            return False
        deprecated = index.deprecated_type(name) or index.deprecated_func(name)
        if deprecated:
            _deprecated(ctx, out, "%s.%s" % (alias_text, name), deprecated, rename_range)
        return True

    for node in ctx.attributes:
        if id(node) in reported or not isinstance(node.ctx, ast.Load):
            continue
        root = node.value
        if isinstance(root, ast.Name) and root.id in ctx.unreal_aliases:
            alias_text = root.id
            if not level1(node, node.attr, alias_text, (ctx.start(node), ctx.end(node)), ctx.attr_range(node)):
                continue
            klass = node.attr
        elif isinstance(root, ast.Name) and root.id in ctx.unreal_from:
            klass = ctx.unreal_from[root.id]
            alias_text = root.id
            if not index.is_class(klass):
                continue
            _level2(ctx, out, node, klass, alias_text, reported)     # the from-imported class is level 1
            continue
        else:
            continue
        if not index.is_class(klass):
            continue
        parent = ctx.parent(node)
        if isinstance(parent, ast.Attribute) and parent.value is node and isinstance(parent.ctx, ast.Load):
            _level2(ctx, out, parent, klass, "%s.%s" % (alias_text, klass), reported)

    # from unreal import N
    for node in ctx.import_froms:
        if node.module == "unreal" and node.level == 0:
            for alias in node.names:
                if alias.name == "*" or alias.name.startswith("__"):
                    continue
                start = (alias.lineno - 1, alias.col_offset)
                name_end = (alias.lineno - 1, alias.col_offset + len(alias.name.encode("utf-8")))
                if not index.has_name(alias.name):
                    closest = _closest(alias.name, index.top_names())
                    message = "Unknown Unreal name 'unreal.%s'" % alias.name
                    fix = None
                    if closest:
                        message += "; did you mean 'unreal.%s'?" % closest
                        fix = Fix("Replace with unreal.%s" % closest, [(start, name_end, closest)])
                    out.append(ctx.diagnostic("SUM101", WARNING, message, (start, name_end), fix))
                else:
                    deprecated = index.deprecated_type(alias.name) or index.deprecated_func(alias.name)
                    if deprecated:
                        _deprecated(ctx, out, "unreal.%s" % alias.name, deprecated, (start, name_end))


def _level2(ctx, out, node, klass, owner_text, reported):
    """``node`` is ``<class expr>.member`` in a load context."""
    index = ctx.index
    member = node.attr
    reported.add(id(node))
    if member.startswith("_"):
        return
    present = index.has_member(klass, member)
    if present is False:
        closest = _closest(member, index.member_names(klass))
        message = "'%s' has no member '%s'" % (owner_text, member)
        fix = None
        attr_range = ctx.attr_range(node)
        if closest:
            message += "; did you mean '%s.%s'?" % (owner_text, closest)
            fix = Fix("Replace with %s.%s" % (owner_text, closest), [(attr_range[0], attr_range[1], closest)])
        out.append(ctx.diagnostic("SUM101", WARNING, message, attr_range, fix))
        return
    deprecated = index.deprecated_member(klass, member)
    if deprecated:
        _deprecated(ctx, out, "%s.%s" % (owner_text, member), deprecated, ctx.attr_range(node))


def _deprecated(ctx, out, shown, deprecated, rename_range):
    message, new_name = deprecated
    fix = Fix("Rename to %s" % new_name, [(rename_range[0], rename_range[1], new_name)]) if new_name else None
    out.append(ctx.diagnostic("SUM103", WARNING, "'%s' is deprecated: %s" % (shown, message), rename_range, fix))


def _instance_members(ctx, out, seen):
    """SUM103 on members read from a value whose class is inferred (``mesh.old_name``)."""
    index = ctx.index
    deprecated_names = index.deprecated_member_names()
    if not deprecated_names:
        return
    for node in ctx.attributes:
        if node.attr not in deprecated_names or id(node) in seen or not isinstance(node.ctx, ast.Load):
            continue
        value = node.value
        if isinstance(value, ast.Name) and (value.id in ctx.unreal_aliases or value.id in ctx.unreal_from):
            continue
        if isinstance(value, ast.Attribute) and isinstance(value.value, ast.Name) \
                and value.value.id in ctx.unreal_aliases:
            continue                                    # unreal.C.m: level 2 handled it
        klass = ctx.infer.class_of(value, ctx.scope_of(node))
        if not klass:
            continue
        deprecated = index.deprecated_member(klass, node.attr)
        if deprecated:
            _deprecated(ctx, out, node.attr, deprecated, ctx.attr_range(node))


def _sum102(ctx, out):
    index = ctx.index
    variant = bool(ctx.config.get("sum102NotifyVariant"))
    for node in ctx.assignments:
        if isinstance(node, ast.Assign):
            targets = node.targets
            value = node.value
        elif isinstance(node, ast.AugAssign):
            targets = [node.target]
            value = node.value
        elif isinstance(node, ast.AnnAssign) and node.value is not None:
            targets = [node.target]
            value = node.value
        else:
            continue
        flat = []
        for target in targets:
            if isinstance(target, (ast.Tuple, ast.List)):
                flat.extend((element, False) for element in target.elts)
            else:
                flat.append((target, isinstance(node, (ast.Assign, ast.AugAssign)) and len(targets) == 1))
        for target, fixable in flat:
            if not isinstance(target, ast.Attribute):
                continue
            obj = target.value
            if isinstance(obj, ast.Name) and (obj.id in ctx.unreal_aliases or obj.id in ctx.unreal_from):
                continue
            prop = target.attr
            klass = ctx.infer.class_of(obj, ctx.scope_of(node))
            message = None
            severity = WARNING
            if klass:
                eprops = index.eprops(klass)
                entry = eprops.get(prop)
                if entry is None:
                    continue
                member = index.member(klass, prop)
                if member is None or member.kind != "getset":
                    message = _EDIT_ONLY.format(p=prop, c=klass)
                else:
                    attribute = member.property_doc
                    if attribute is not None and attribute.read_only and not entry[1]:
                        message = _READ_ONLY.format(p=prop)
                    elif variant and attribute is not None and not attribute.read_only:
                        message = _NOTIFY
                if message is None:
                    continue
            else:
                if isinstance(obj, ast.Name) and obj.id in ("self", "cls"):
                    continue
                classes = index.edit_only_classes(prop)
                if not classes or index.is_attribute_anywhere(prop):
                    continue
                shown = ", ".join("`%s`" % c for c in classes[:3]) + (", …" if len(classes) > 3 else "")
                message = _EDIT_ONLY.format(p=prop, c=shown.strip("`")) if len(classes) == 1 else \
                    _EDIT_ONLY.replace("`{c}`", "{c}").format(p=prop, c=shown)
                severity = INFORMATION
            fix = _sum102_fix(ctx, node, target, value, prop) if fixable else None
            out.append(ctx.diagnostic("SUM102", severity, message, target, fix))


def _sum102_fix(ctx, node, target, value, prop):
    obj_text = ctx.segment(target.value)
    value_text = ctx.segment(value)
    if obj_text is None or value_text is None:
        return None
    quoted = '"%s"' % prop
    if isinstance(node, ast.AugAssign):
        op = _AUG_OPS.get(type(node.op))
        if op is None:
            return None
        operand = value_text if isinstance(value, _ATOMS) else "(%s)" % value_text
        new_value = "%s.get_editor_property(%s) %s %s" % (obj_text, quoted, op, operand)
    else:
        new_value = value_text
    replacement = "%s.set_editor_property(%s, %s)" % (obj_text, quoted, new_value)
    return Fix("Use set_editor_property", [(ctx.start(node), ctx.end(node), replacement)], preferred=True)


def run(ctx):
    out = []
    _sum101_and_103(ctx, out)
    _instance_members(ctx, out, set())
    _sum102(ctx, out)
    # One SUM103 per position (a level-2 and an inferred read of the same node).
    unique = {}
    for diagnostic in out:
        unique.setdefault((diagnostic.code, diagnostic.start, diagnostic.end), diagnostic)
    return list(unique.values())
