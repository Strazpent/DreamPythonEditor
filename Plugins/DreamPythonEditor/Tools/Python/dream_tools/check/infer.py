# SPDX-License-Identifier: Apache-2.0
"""Local type inference for SUM102, SUM103, SUM104 and SUM205 (TDD §7.3.3 "Type inference").

Flow-insensitive, one scope at a time; user code is not followed. A value's type is a *type text* in the docstring
grammar (``StaticMeshActor``, ``Array[Actor]``) or one of the markers below. These patterns give a known class:

* ``x = unreal.C(...)``, ``x = unreal.C.cast(...)``, ``x = unreal.C.get_default_object()``;
* annotations, on parameters too (``unreal.C``, ``Optional[unreal.C]``, ``list[unreal.C]``, ``"unreal.C"``);
* ``x = <call>`` when the snapshot's parsed return type is a class (``unreal.C.m()``, ``obj.m()``, ``unreal.f()``),
  and ``unreal.get_editor_subsystem(unreal.C)`` (any ``get_*_subsystem`` with a class argument);
* ``for x in <call>`` when the call returns ``Array[C]``;
* ``isinstance(x, unreal.C)`` guards.

Priority within a scope: annotations, then isinstance guards, then assignments; candidates of one kind must agree, an
assignment of ``None`` is ignored and any other assignment of an unknown value makes the name unknown.
"""

import ast

from .docs import docstrings

THREAD = "<threading.Thread>"
EXECUTOR = "<concurrent.futures.Executor>"
NONE = "<None>"

class Bindings:
    """The binding sites of one scope; types are resolved lazily (cycles resolve to unknown)."""

    def __init__(self, scope):
        self.scope = scope
        self.annotated = {}         # name -> [annotation node]
        self.guards = {}            # name -> [class node of isinstance(name, C)]
        self.assigned = {}          # name -> [("value", node) | ("iter", node) | ("unknown", None)]
        self.params = set()
        self.bound = set()          # every name bound here, typed or not
        self.resolved = {}
        self.resolving = set()

    def add(self, table, name, value):
        table.setdefault(name, []).append(value)


class Inference:
    """Inference over one module; ``ctx`` is the analysis Context (aliases, the snapshot index)."""

    def __init__(self, ctx):
        self.ctx = ctx
        self._bindings = {}

    # ---- scopes -------------------------------------------------------------------------------------------------

    def bindings(self, scope):
        found = self._bindings.get(id(scope))
        if found is None:
            found = self._collect(scope)
            self._bindings[id(scope)] = found
        return found

    def _collect(self, scope):
        result = Bindings(scope)
        if isinstance(scope, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)):
            args = scope.args
            for arg in args.posonlyargs + args.args + args.kwonlyargs:
                result.params.add(arg.arg)
                result.bound.add(arg.arg)
                if arg.annotation is not None:
                    result.add(result.annotated, arg.arg, arg.annotation)
            for arg in (args.vararg, args.kwarg):
                if arg is not None:
                    result.params.add(arg.arg)
                    result.bound.add(arg.arg)
                    result.add(result.assigned, arg.arg, ("unknown", None))
        for node in self.ctx.sites.get(id(scope), ()):
            if isinstance(node, ast.Assign):
                for target in node.targets:
                    if isinstance(target, ast.Name):
                        result.bound.add(target.id)
                        result.add(result.assigned, target.id, ("value", node.value))
                    else:
                        for name in _names_in_target(target):
                            result.bound.add(name)
                            result.add(result.assigned, name, ("unknown", None))
            elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
                result.bound.add(node.target.id)
                result.add(result.annotated, node.target.id, node.annotation)
                if node.value is not None:
                    result.add(result.assigned, node.target.id, ("value", node.value))
            elif isinstance(node, ast.NamedExpr) and isinstance(node.target, ast.Name):
                result.bound.add(node.target.id)
                result.add(result.assigned, node.target.id, ("value", node.value))
            elif isinstance(node, (ast.For, ast.AsyncFor)):
                if isinstance(node.target, ast.Name):
                    result.bound.add(node.target.id)
                    result.add(result.assigned, node.target.id, ("iter", node.iter))
                else:
                    for name in _names_in_target(node.target):
                        result.bound.add(name)
                        result.add(result.assigned, name, ("unknown", None))
            elif isinstance(node, (ast.With, ast.AsyncWith)):
                for item in node.items:
                    if isinstance(item.optional_vars, ast.Name):
                        result.bound.add(item.optional_vars.id)
                        result.add(result.assigned, item.optional_vars.id, ("value", item.context_expr))
            elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                result.bound.add(node.name)
                result.add(result.assigned, node.name, ("unknown", None))
            elif isinstance(node, (ast.Import, ast.ImportFrom)):
                for alias in node.names:
                    name = alias.asname or alias.name.split(".")[0]
                    result.bound.add(name)
                    result.add(result.assigned, name, ("unknown", None))
            elif isinstance(node, ast.Call) and _is_isinstance_call(node):
                target, klass = node.args[0], node.args[1]
                if isinstance(target, ast.Name):
                    result.add(result.guards, target.id, klass)
            elif isinstance(node, (ast.Global, ast.Nonlocal)):
                for name in node.names:
                    result.bound.discard(name)
                    result.assigned.pop(name, None)
            elif isinstance(node, ast.ExceptHandler) and node.name:
                result.bound.add(node.name)
                result.add(result.assigned, node.name, ("unknown", None))
        return result

    def _resolve(self, bindings, name):
        if name in bindings.resolved:
            return bindings.resolved[name]
        if name in bindings.resolving:
            return None
        bindings.resolving.add(name)
        try:
            scope = bindings.scope
            typed = None
            annotated = [self.annotation_type(node, scope) for node in bindings.annotated.get(name, ())]
            annotated = [t for t in annotated if t]
            guards = [self.class_ref(node) for node in bindings.guards.get(name, ())]
            guards = [t for t in guards if t]
            if annotated and all(t == annotated[0] for t in annotated):
                typed = annotated[0]
            elif guards and all(t == guards[0] for t in guards):
                typed = guards[0]
            else:
                values = []
                for kind, node in bindings.assigned.get(name, ()):
                    if kind == "value":
                        values.append(self.value_type(node, scope))
                    elif kind == "iter":
                        iterated = self.value_type(node, scope)
                        values.append(docstrings.element_class(iterated)
                                      if iterated and not iterated.startswith("<") else None)
                    else:
                        values.append(None)
                values = [v for v in values if v != NONE]
                if values and all(v is not None and v == values[0] for v in values):
                    typed = values[0]
            bindings.resolved[name] = typed
            return typed
        finally:
            bindings.resolving.discard(name)

    # ---- names --------------------------------------------------------------------------------------------------

    def name_type(self, name, scope):
        """The inferred type text of ``name`` read in ``scope``: local bindings, else the module's."""
        local = self.bindings(scope)
        if name in local.bound or name in local.guards:
            return self._resolve(local, name)
        module = self.ctx.tree
        if scope is not module:
            outer = self.bindings(module)
            if name in outer.bound or name in outer.guards:
                return self._resolve(outer, name)
        return None

    def is_bound(self, name, scope):
        """True when ``name`` is bound in ``scope`` or at module level (a builtin is shadowed)."""
        if name in self.bindings(scope).bound:
            return True
        return name in self.bindings(self.ctx.tree).bound

    def class_ref(self, node):
        """The ``unreal`` class an expression names (``unreal.C``, a ``from unreal import C`` name), or None."""
        ctx = self.ctx
        if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name) and node.value.id in ctx.unreal_aliases:
            return node.attr if ctx.looks_like_class(node.attr) else None
        if isinstance(node, ast.Name) and node.id in ctx.unreal_from:
            name = ctx.unreal_from[node.id]
            return name if ctx.looks_like_class(name) else None
        return None

    def annotation_type(self, node, scope):
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            try:
                node = ast.parse(node.value, mode="eval").body
            except SyntaxError:
                return None
        direct = self.class_ref(node)
        if direct:
            return direct
        if self.ctx.is_thread_class(node):
            return THREAD
        if isinstance(node, ast.Subscript):
            head = node.value
            head_name = head.attr if isinstance(head, ast.Attribute) else head.id if isinstance(head, ast.Name) else ""
            inner = node.slice
            if head_name == "Optional":
                return self.annotation_type(inner, scope)
            if head_name in ("list", "List", "Array", "Sequence", "Iterable", "set", "Set", "tuple", "Tuple"):
                if isinstance(inner, ast.Tuple):
                    return None
                element = self.annotation_type(inner, scope)
                if element and not element.startswith("<") and docstrings.class_name(element):
                    return "Array[%s]" % element
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr):
            left = self.annotation_type(node.left, scope)
            right = self.annotation_type(node.right, scope)
            if isinstance(node.right, ast.Constant) and node.right.value is None:
                return left
            if isinstance(node.left, ast.Constant) and node.left.value is None:
                return right
        return None

    # ---- values -------------------------------------------------------------------------------------------------

    def value_type(self, node, scope):
        """The type text of an expression, or None."""
        ctx = self.ctx
        if isinstance(node, ast.Constant) and node.value is None:
            return NONE
        if isinstance(node, ast.Name):
            return self.name_type(node.id, scope)
        if isinstance(node, ast.Call):
            return self._call_type(node, scope)
        if isinstance(node, ast.Attribute):
            owner = self.value_type(node.value, scope)
            klass = docstrings.class_name(owner) if owner and not owner.startswith("<") else None
            if klass and ctx.index_usable():
                return ctx.index.property_type(klass, node.attr)
            return None
        if isinstance(node, ast.Subscript):
            owner = self.value_type(node.value, scope)
            if owner and not owner.startswith("<"):
                return docstrings.element_class(owner)
            return None
        if isinstance(node, ast.IfExp):
            left = self.value_type(node.body, scope)
            right = self.value_type(node.orelse, scope)
            return left if left == right else None
        return None

    def _call_type(self, node, scope):
        ctx = self.ctx
        func = node.func
        klass = self.class_ref(func)
        if klass:
            return klass                                     # unreal.C(...)
        if ctx.is_thread_class(func):
            return THREAD
        if ctx.is_executor_class(func):
            return EXECUTOR
        if isinstance(func, ast.Name) and func.id in ctx.local_thread_classes:
            return THREAD
        if isinstance(func, ast.Attribute):
            owner_class = self.class_ref(func.value)
            if owner_class and func.attr in ("cast", "get_default_object"):
                return owner_class                           # unreal.C.cast(...), unreal.C.get_default_object()
            if isinstance(func.value, ast.Name) and func.value.id in ctx.unreal_aliases:
                if func.attr.endswith("_subsystem") and node.args:
                    subsystem = self.class_ref(node.args[0])
                    if subsystem:
                        return subsystem                     # unreal.get_editor_subsystem(unreal.C)
                if ctx.index_usable():
                    return ctx.index.func_returns(func.attr)
                return None
            if owner_class:
                return ctx.index.method_returns(owner_class, func.attr) if ctx.index_usable() else None
            owner = self.value_type(func.value, scope)
            owner_class = docstrings.class_name(owner) if owner and not owner.startswith("<") else None
            if owner_class and ctx.index_usable():
                return ctx.index.method_returns(owner_class, func.attr)
        if isinstance(func, ast.Name) and func.id in ctx.unreal_from and ctx.index_usable():
            return ctx.index.func_returns(ctx.unreal_from[func.id])
        return None

    def class_of(self, node, scope):
        """The ``unreal`` class of an expression's value, or None."""
        typed = self.value_type(node, scope)
        if not typed or typed.startswith("<"):
            return None
        return docstrings.class_name(typed)


def _names_in_target(target):
    """The names a target binds: Name, Starred, Tuple and List structure only (obj.attr = ... binds no name)."""
    stack = [target]
    while stack:
        node = stack.pop()
        if isinstance(node, ast.Name):
            yield node.id
        elif isinstance(node, ast.Starred):
            stack.append(node.value)
        elif isinstance(node, (ast.Tuple, ast.List)):
            stack.extend(node.elts)


def _is_isinstance_call(node):
    return (isinstance(node.func, ast.Name) and node.func.id == "isinstance" and len(node.args) == 2
            and not node.keywords)
