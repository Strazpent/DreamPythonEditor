# SPDX-License-Identifier: Apache-2.0
"""One analysis of one document (TDD §7.3.3): parse, run every rule family, apply ``# noqa``.

``ast.parse(text, feature_version=(3, 11))``; a syntax error is one ``syntax-error`` diagnostic and no rule runs.
Otherwise the families run one after the other on a shared Context (imports, parents, scopes, inference); a family
that raises becomes one SUM901 and the others still report (T-DIAG-026). SUM101-SUM103 need the snapshot; without it
SUM900 says why, for documents that import ``unreal``.
"""

import ast

from . import noqa as noqa_module
from .infer import Inference
from .snapshot import BUILDING, OK

ERROR = 1
WARNING = 2
INFORMATION = 3
HINT = 4

SYNTAX_CODE = "syntax-error"

DEFAULT_CONFIG = {
    "oneUndoStepPerRun": True,
    "bufferKind": "file",
    "namespaceMode": "session",
    "sum102NotifyVariant": False,
}


class Fix:
    """One quick fix: a title and edits as ((line, byte col), (line, byte col), new text), non-overlapping."""

    __slots__ = ("title", "edits", "preferred")

    def __init__(self, title, edits, preferred=False):
        self.title = title
        self.edits = edits
        self.preferred = preferred


class Diagnostic:
    """Positions are (0-based line, UTF-8 byte column)."""

    __slots__ = ("code", "severity", "message", "start", "end", "fix")

    def __init__(self, code, severity, message, start, end, fix=None):
        self.code = code
        self.severity = severity
        self.message = message
        self.start = start
        self.end = end
        self.fix = fix

    def __repr__(self):
        return "Diagnostic(%s %d:%d %r)" % (self.code, self.start[0] + 1, self.start[1], self.message)


#: Fields that never hold a node the rules visit: expression contexts, operators, identifiers and flags.
_SKIPPED_FIELDS = frozenset({"ctx", "op", "ops", "id", "attr", "name", "arg", "module", "names", "level", "kind",
                             "type_comment", "is_async", "conversion", "asname", "type_ignores"})
_CHILD_FIELDS = {}


class Context:
    """What every rule family shares for one parsed document."""

    def __init__(self, text, tree, config, index):
        self.text = text
        self.tree = tree
        self.config = dict(DEFAULT_CONFIG)
        if isinstance(config, dict):
            self.config.update({k: v for k, v in config.items() if k in DEFAULT_CONFIG})
        self.index = index
        self._index_usable = None
        self.cache = {}             # per-analysis memo tables of the rule families
        self.lines = text.split("\n")
        self.parents = {}
        self.scopes = {}            # id(node) -> the function, lambda or module whose code holds it
        self.sites = {}             # id(scope) -> the binding sites of that scope (infer.Inference reads them)
        self.calls = []
        self.attributes = []
        self.names = []
        self.functions = []
        self.classes = []
        self.loops = []
        self.withs = []
        self.assignments = []
        self.import_froms = []
        self.string_spans = []      # (first line, last line) of every string literal that spans lines
        self.module_aliases = {}    # imports, file-wide (flow-insensitive): local name -> module
        self.from_names = {}        # local name -> (module, name)
        self._walk(tree)
        self.unreal_aliases = {local for local, module in self.module_aliases.items() if module == "unreal"}
        self.unreal_from = {local: name for local, (module, name) in self.from_names.items() if module == "unreal"}
        self.imports_unreal = bool(self.unreal_aliases or any(m == "unreal" for m, _ in self.from_names.values()))

        # Thread subclasses defined in this file (to a fixpoint: a subclass of a subclass counts).
        self.local_thread_classes = set()
        changed = True
        while changed:
            changed = False
            for klass in self.classes:
                if klass.name in self.local_thread_classes:
                    continue
                if any(self.is_thread_class(base) or (isinstance(base, ast.Name) and base.id in self.local_thread_classes)
                       for base in klass.bases):
                    self.local_thread_classes.add(klass.name)
                    changed = True
        self.infer = Inference(self)

    # Node types sorted into the per-type lists, and the binding sites infer.Inference reads.
    _SITE_TYPES = (ast.Assign, ast.AnnAssign, ast.NamedExpr, ast.For, ast.AsyncFor, ast.With, ast.AsyncWith,
                   ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef, ast.Import, ast.ImportFrom, ast.Global,
                   ast.Nonlocal, ast.ExceptHandler)

    def _walk(self, tree):
        """One pass over the tree: parents, the scope of every node, per-type lists, binding sites and imports.

        Hot path (most of an analysis): child fields come from a per-type cache that leaves out the fields that never
        hold a node worth visiting (``ctx``, operators, names), and Name and Constant nodes store no scope of their
        own (``scope_of`` reads their parent's).
        """
        parents = self.parents
        scopes = self.scopes
        sites = self.sites
        lists_get = {
            ast.Call: self.calls, ast.Attribute: self.attributes, ast.Name: self.names,
            ast.FunctionDef: self.functions, ast.AsyncFunctionDef: self.functions, ast.Lambda: self.functions,
            ast.ClassDef: self.classes, ast.For: self.loops, ast.AsyncFor: self.loops, ast.While: self.loops,
            ast.With: self.withs, ast.AsyncWith: self.withs, ast.Assign: self.assignments,
            ast.AugAssign: self.assignments, ast.AnnAssign: self.assignments, ast.ImportFrom: self.import_froms,
        }.get
        site_types = self._SITE_TYPES
        function_types = (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda)
        scoped_types = function_types + (ast.ClassDef,)
        leaf_types = (ast.Name, ast.Constant)
        ast_type = ast.AST
        name_type, constant_type, call_type = ast.Name, ast.Constant, ast.Call
        child_fields = _CHILD_FIELDS
        # (node, scope that holds it, scope whose bindings it adds to or None inside a class body)
        stack = [(tree, tree, tree)]
        push = stack.append
        pop = stack.pop
        sites[id(tree)] = []
        while stack:
            node, scope, binding_scope = pop()
            node_type = type(node)
            if node_type is name_type:
                self.names.append(node)
                continue
            if node_type is constant_type:
                if isinstance(node.value, (str, bytes)) and node.end_lineno is not None and node.end_lineno > node.lineno:
                    self.string_spans.append((node.lineno - 1, node.end_lineno - 1))
                continue
            scopes[id(node)] = scope
            bucket = lists_get(node_type)
            if bucket is not None:
                bucket.append(node)
            if binding_scope is not None and (node_type in site_types
                                              or (node_type is call_type and _is_isinstance(node))):
                sites[id(binding_scope)].append(node)
            if node_type is ast.Import:
                for alias in node.names:
                    if alias.asname:
                        self.module_aliases[alias.asname] = alias.name
                    else:
                        root = alias.name.split(".")[0]
                        self.module_aliases[root] = root
            elif node_type is ast.ImportFrom:
                if node.module and node.level == 0:
                    for alias in node.names:
                        if alias.name != "*":
                            self.from_names[alias.asname or alias.name] = (node.module, alias.name)
            elif node_type is ast.JoinedStr and node.end_lineno is not None and node.end_lineno > node.lineno:
                self.string_spans.append((node.lineno - 1, node.end_lineno - 1))

            fields = child_fields.get(node_type)
            if fields is None:
                fields = tuple(f for f in node_type._fields if f not in _SKIPPED_FIELDS)
                child_fields[node_type] = fields
            if node_type in scoped_types:
                if node_type is ast.ClassDef:
                    inner_scope, inner_binding, outer_fields = scope, None, ("decorator_list", "bases", "keywords")
                else:
                    inner_scope = inner_binding = node
                    sites[id(node)] = []
                    outer_fields = ("decorator_list", "returns")
                for field in fields:
                    value = getattr(node, field, None)
                    if value is None:
                        continue
                    if field in outer_fields:
                        child_scope, child_binding = scope, binding_scope
                    else:
                        child_scope, child_binding = inner_scope, inner_binding
                    for item in (value if type(value) is list else (value,)):
                        if isinstance(item, ast_type):
                            parents[id(item)] = node
                            push((item, child_scope, child_binding))
                continue
            for field in fields:
                value = getattr(node, field, None)
                if value is None:
                    continue
                if type(value) is list:
                    for item in value:
                        if isinstance(item, ast_type):
                            parents[id(item)] = node
                            push((item, scope, binding_scope))
                elif isinstance(value, ast_type):
                    parents[id(value)] = node
                    push((value, scope, binding_scope))

    # ---- the snapshot -------------------------------------------------------------------------------------------

    def index_usable(self):
        if self._index_usable is None:
            self._index_usable = bool(self.index is not None and self.index.usable())
        return self._index_usable

    def looks_like_class(self, name):
        if self.index_usable():
            return self.index.is_class(name)
        return bool(name) and name[0].isupper()

    # ---- names --------------------------------------------------------------------------------------------------

    def qualname(self, node):
        """The dotted name an expression resolves to through this file's imports (``time.sleep`` for ``sleep``
        after ``from time import sleep``), or None."""
        parts = []
        while isinstance(node, ast.Attribute):
            parts.append(node.attr)
            node = node.value
        if not isinstance(node, ast.Name):
            return None
        if node.id in self.module_aliases:
            head = self.module_aliases[node.id]
        elif node.id in self.from_names:
            module, name = self.from_names[node.id]
            head = module + "." + name
        else:
            return None
        return ".".join([head] + list(reversed(parts)))

    def is_thread_class(self, node):
        return self.qualname(node) == "threading.Thread"

    def is_executor_class(self, node):
        name = self.qualname(node) or ""
        return name.startswith("concurrent.futures.") and name.endswith(("ThreadPoolExecutor", "ProcessPoolExecutor"))

    def is_unreal_name(self, node):
        return isinstance(node, ast.Name) and (node.id in self.unreal_aliases or node.id in self.unreal_from)

    # ---- tree ---------------------------------------------------------------------------------------------------

    def parent(self, node):
        return self.parents.get(id(node))

    def ancestors(self, node):
        current = self.parents.get(id(node))
        while current is not None:
            yield current
            current = self.parents.get(id(current))

    def scope_of(self, node):
        """The function or lambda whose code holds ``node``, else the module."""
        scope = self.scopes.get(id(node))
        if scope is None:
            parent = self.parents.get(id(node))     # Name and Constant nodes: their parent's scope
            scope = self.scopes.get(id(parent)) if parent is not None else None
        return scope if scope is not None else self.tree

    def enclosing_loop(self, node):
        """The innermost loop whose body holds ``node`` within the same function, or None."""
        child = node
        for ancestor in self.ancestors(node):
            if isinstance(ancestor, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)):
                return None
            if isinstance(ancestor, (ast.For, ast.AsyncFor, ast.While)) and child in ancestor.body:
                return ancestor
            child = ancestor
        return None

    # ---- positions ----------------------------------------------------------------------------------------------

    @staticmethod
    def start(node):
        return node.lineno - 1, node.col_offset

    @staticmethod
    def end(node):
        return node.end_lineno - 1, node.end_col_offset

    def segment(self, node):
        """The source text of a node, from its positions (multi-line values keep their lines)."""
        try:
            first, first_col = node.lineno - 1, node.col_offset
            last, last_col = node.end_lineno - 1, node.end_col_offset
        except AttributeError:
            return None
        if last is None or last_col is None or not (0 <= first <= last < len(self.lines)):
            return None
        if first == last:
            return self.lines[first].encode("utf-8", "surrogatepass")[first_col:last_col].decode("utf-8", "surrogatepass")
        head = self.lines[first].encode("utf-8", "surrogatepass")[first_col:].decode("utf-8", "surrogatepass")
        tail = self.lines[last].encode("utf-8", "surrogatepass")[:last_col].decode("utf-8", "surrogatepass")
        return "\n".join([head] + self.lines[first + 1:last] + [tail])

    def multiline_strings(self):
        """(first line, last line) of every string literal that spans lines (collected by the walk)."""
        return self.string_spans

    def line_bytes(self, line):
        text = self.lines[line] if 0 <= line < len(self.lines) else ""
        return len(text.encode("utf-8", "surrogatepass"))

    def attr_range(self, node):
        """The range of an Attribute node's attribute name (its last ``.name``)."""
        end = self.end(node)
        width = len(node.attr.encode("utf-8"))
        return (end[0], max(0, end[1] - width)), end

    def chain_top(self, node):
        """The outermost Attribute whose chain starts at ``node`` (``unreal.A.b`` for the ``unreal`` Name)."""
        current = node
        parent = self.parent(current)
        while isinstance(parent, ast.Attribute) and parent.value is current:
            current = parent
            parent = self.parent(current)
        return current

    def diagnostic(self, code, severity, message, node_or_range, fix=None):
        if isinstance(node_or_range, tuple):
            start, end = node_or_range
        else:
            start, end = self.start(node_or_range), self.end(node_or_range)
        return Diagnostic(code, severity, message, start, end, fix)


def _is_isinstance(call):
    func = call.func
    return type(func) is ast.Name and func.id == "isinstance" and len(call.args) == 2 and not call.keywords


def _syntax_diagnostic(text, error):
    lines = text.split("\n")
    name = type(error).__name__
    message = "%s: %s" % (name, getattr(error, "msg", None) or str(error))
    line_no = (getattr(error, "lineno", None) or 1) - 1
    line_no = min(max(line_no, 0), max(len(lines) - 1, 0))
    line = lines[line_no] if lines else ""
    offset = getattr(error, "offset", None) or 1
    char = min(max(offset - 1, 0), len(line))
    start = (line_no, len(line[:char].encode("utf-8", "surrogatepass")))
    end_line = getattr(error, "end_lineno", None)
    end_offset = getattr(error, "end_offset", None)
    if end_line and end_offset and (end_line - 1, end_offset - 1) > (line_no, char) and end_line - 1 < len(lines):
        end_text = lines[end_line - 1]
        end_char = min(max(end_offset - 1, 0), len(end_text))
        end = (end_line - 1, len(end_text[:end_char].encode("utf-8", "surrogatepass")))
    else:
        end_char = min(char + 1, len(line))
        end = (line_no, len(line[:end_char].encode("utf-8", "surrogatepass")))
    return Diagnostic(SYNTAX_CODE, ERROR, message, start, end)


def _families():
    from . import rules_paths, rules_threads, rules_transaction, rules_unreal
    return (
        ("unreal", rules_unreal.run, True),
        ("transaction", rules_transaction.run, False),
        ("threads", rules_threads.run, False),
        ("paths", rules_paths.run, False),
    )


def analyze(text, config=None, index=None, families=None):
    """The diagnostics of one text, sorted by position. ``families`` replaces the rule families (tests)."""
    try:
        tree = ast.parse(text, filename="<dream-check>", feature_version=(3, 11))
    except SyntaxError as error:
        return [_syntax_diagnostic(text, error)]
    except (ValueError, OverflowError, RecursionError, MemoryError) as error:
        return [Diagnostic(SYNTAX_CODE, ERROR, "%s: %s" % (type(error).__name__, error), (0, 0), (0, 0))]

    ctx = Context(text, tree, config, index)
    diagnostics = []
    for name, run, needs_snapshot in (families if families is not None else _families()):
        if needs_snapshot and not ctx.index_usable():
            continue
        try:
            diagnostics.extend(run(ctx))
        except Exception as error:  # noqa: BLE001 - SUM901: one family failing never hides the others
            diagnostics.append(Diagnostic("SUM901", INFORMATION,
                                          "dream-check rule family '%s' failed: %s: %s" % (name, type(error).__name__, error),
                                          (0, 0), (0, 0)))
    if ctx.imports_unreal and not ctx.index_usable():
        building = index is not None and index.status() == BUILDING
        message = "Unreal API data is being built" if building else "Unreal API data is unavailable"
        if index is not None and index.status() == OK and index.load_error:
            message = "Unreal API data is unavailable (%s)" % index.load_error
        diagnostics.append(Diagnostic("SUM900", INFORMATION,
                                      message + "; SUM101-SUM103 are off until it is ready.", (0, 0), (0, 0)))

    suppress = noqa_module.noqa_map(text)
    if suppress:
        diagnostics = [d for d in diagnostics if not noqa_module.is_suppressed(suppress, d.start[0], d.code)]
    diagnostics.sort(key=lambda d: (d.start, d.end, d.code))
    return diagnostics
