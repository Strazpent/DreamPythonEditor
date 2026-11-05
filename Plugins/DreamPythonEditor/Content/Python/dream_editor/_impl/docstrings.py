# SPDX-License-Identifier: Apache-2.0
"""The shared parser for the docstrings PythonScriptPlugin generates (TDD §7.4.4; D5, D6).

The only parser of these formats: the Stage 1 snapshot (``stubs``), ``dream-check`` (loaded by path as
``dream_docstrings``, so this module may import only the standard library, T-STUB-022), the live resolver and hover.
The grammar follows 07d Q2 and the shapes PythonScriptPlugin writes (``PyGenUtil.cpp`` BuildFunctionDocString,
PythonizeTooltip, AppendSourceInformationDocString, FGeneratedWrappedPropertyDoc::AppendDocString):

    method_doc    := ("x." | "X.") NAME "(" params ")" " -> " ret  "\\n" tooltip? deprecated? args? returns?
    ret           := type_text | "None" | "(" [NAME "="] type_text ("," NAME "=" type_text)* ")" [" or None"]
    args          := "\\n\\nArgs:" ("\\n    " NAME " (" type_text "): " text)+
    returns       := "\\n\\nReturns:\\n    " [type_text ": "] text ("\\n\\n    " NAME " (" type_text "): " text)*
    glue_doc      := NAME "(" typed_params ")" " -> " type_text " -- " text
    property_doc  := "(" type_text "):" " "+ "[" ("Read-Write" | "Read-Only") "] " text
    class_doc     := tooltip? deprecated? cpp_source? editor_props?
    editor_props  := "**Editor Properties:** (see get_editor_property/set_editor_property)" "\\n"
                     ("\\n- ``" NAME "`` (" type_text "):" " "+ "[" RW "] " text ("\\n  " text)*)+
    deprecated    := "deprecated: " text           (renames: "deprecated: '<old>' was renamed to '<new>'.")

The engine writes ``"): "`` and then ``" [Read-Write]"`` (PythonizeTooltip, AppendPropertyPythonReadWriteState), so
real property and editor-property docs carry two spaces before the bracket; one or more spaces are accepted.

``X.`` means static, ``x.`` instance. Every parse function returns ``None`` when its input does not have the shape it
parses and never raises; callers degrade to untyped output. Text is normalised first (``\\r\\n`` and ``\\r`` become
``\\n``). Type texts are returned as written, without a leading ``unreal.``.
"""

import re
from collections import namedtuple

__all__ = [
    "Param", "MethodDoc", "GlueDoc", "PropertyDoc", "EditorProperty", "SourceInfo", "ClassDoc", "TypeRef",
    "normalize", "parse_method_doc", "parse_glue_doc", "parse_property_doc", "parse_class_doc",
    "strip_editor_properties", "parse_deprecated", "is_deprecated", "deprecation_message", "parse_type_text",
    "class_name", "element_class", "class_of_type", "BUILTIN_TYPES",
]

#: One parameter. ``default`` is the default's source text (``None`` when there is none); ``type`` and ``doc`` come
#: from the ``Args:`` block (``None`` when absent).
Param = namedtuple("Param", "name default type doc")

#: A UFunction's doc. ``returns`` is the single return type text (``None`` for ``-> None`` or a tuple);
#: ``return_items`` lists ``(name or None, type_text)`` of an output tuple (empty otherwise); ``optional`` is the
#: `` or None`` suffix; ``returns_doc`` the text of the ``Returns:`` block; ``deprecated`` the deprecation message.
MethodDoc = namedtuple("MethodDoc", "static name params returns return_items optional tooltip returns_doc deprecated")

#: A glue method's ``name(self, x: T = d) -> R -- text`` doc. ``params`` excludes ``self`` and ``cls``.
GlueDoc = namedtuple("GlueDoc", "name params returns text")

#: A property getset's doc: ``type`` text, ``read_only`` flag, tooltip ``text`` and ``deprecated`` message.
PropertyDoc = namedtuple("PropertyDoc", "type read_only text deprecated")

#: One entry of a class's Editor Properties list.
EditorProperty = namedtuple("EditorProperty", "name type read_only tooltip")

#: ``kind`` is ``"cpp"`` (plugin, module, file) or ``"asset"`` (package).
SourceInfo = namedtuple("SourceInfo", "kind plugin module file package")

#: A class, struct, enum or delegate doc. ``source`` is a SourceInfo or ``None``; ``editor_properties`` a tuple.
ClassDoc = namedtuple("ClassDoc", "tooltip source editor_properties deprecated")

#: A parsed type text: ``TypeRef("Array", (TypeRef("Actor", ()),))``; ``type(C)`` is ``TypeRef("type", (C,))``.
TypeRef = namedtuple("TypeRef", "name args")

#: Type names that never name an ``unreal`` class.
BUILTIN_TYPES = frozenset({
    "bool", "int", "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float", "double",
    "str", "bytes", "None", "object", "tuple", "list", "dict", "set", "Any", "type", "Type", "Iterable", "Mapping",
    "Union", "Tuple", "List", "Optional", "Callable", "'undefined'", "_T", "_ElemType", "_KeyType", "_ValueType",
})

_EDITOR_PROPS_HEADER = "**Editor Properties:**"
_CPP_SOURCE_HEADER = "**C++ Source:**"
_ASSET_SOURCE_HEADER = "**Asset Source:**"

_NAME = r"[A-Za-z_][A-Za-z0-9_]*"
_FIRST_LINE_RE = re.compile(r"^(?P<prefix>[xX])\.(?P<name>" + _NAME + r")\((?P<params>.*)\) -> (?P<ret>.+)$")
_GLUE_RE = re.compile(r"^(?P<name>" + _NAME + r")\((?P<params>.*?)\)(?: -> (?P<ret>.+?))? -- (?P<text>.*)$", re.S)
_PROPERTY_RE = re.compile(r"^\((?P<type>.+?)\): +\[(?P<rw>Read-Write|Read-Only)\] ?(?P<text>.*)$", re.S)
_EPROP_RE = re.compile(r"^- ``(?P<name>[^`]+)`` \((?P<type>.+?)\): +\[(?P<rw>Read-Write|Read-Only)\] ?(?P<text>.*)$")
_ARG_RE = re.compile(r"^    (?P<name>" + _NAME + r")(?: \((?P<type>.*)\))?: ?(?P<text>.*)$")
_RENAMED_RE = re.compile(r"deprecated: '(?P<old>[^']+)' was renamed to '(?P<new>[^']+)'\.")
_SOURCE_FIELD_RE = re.compile(r"^- \*\*(?P<key>Plugin|Module|File|Package)\*\*: ?(?P<value>.*)$")
_TYPE_TOKEN_RE = re.compile(r"\s*(?:(?P<name>'undefined'|[A-Za-z_][A-Za-z0-9_.]*)|(?P<punct>[\[\](),]))")


def normalize(doc):
    """``doc`` with ``\\r\\n`` and lone ``\\r`` turned into ``\\n``; ``""`` for anything that is not a ``str``."""
    if not isinstance(doc, str):
        return ""
    return doc.replace("\r\n", "\n").replace("\r", "\n")


def _strip_unreal(text):
    text = text.strip()
    return text.replace("unreal.", "")


def _split_top_level(text, separator=","):
    """Splits at separators outside brackets and quotes. Returns None on unbalanced input."""
    parts = []
    depth = 0
    quote = None
    current = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        if quote:
            current.append(char)
            if char == "\\" and index + 1 < length:
                current.append(text[index + 1])
                index += 2
                continue
            if char == quote:
                quote = None
        elif char in "'\"":
            quote = char
            current.append(char)
        elif char in "([{":
            depth += 1
            current.append(char)
        elif char in ")]}":
            depth -= 1
            if depth < 0:
                return None
            current.append(char)
        elif char == separator and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(char)
        index += 1
    if depth != 0 or quote:
        return None
    parts.append("".join(current))
    return parts


def _find_deprecated(lines):
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("deprecated:"):
            return stripped[len("deprecated:"):].strip()
    return None


def _parse_ret(ret):
    """(returns, return_items, optional) from the text after ' -> ', or None."""
    ret = ret.strip()
    optional = False
    if ret.endswith(" or None"):
        optional = True
        ret = ret[: -len(" or None")].strip()
    if ret == "None":
        return None, (), optional
    if ret.startswith("(") and ret.endswith(")"):
        inner = _split_top_level(ret[1:-1])
        if inner is None:
            return None
        items = []
        for part in inner:
            part = part.strip()
            if not part:
                return None
            match = re.match(r"^(" + _NAME + r")=(.+)$", part)
            if match:
                items.append((match.group(1), _strip_unreal(match.group(2))))
            else:
                items.append((None, _strip_unreal(part)))
        return None, tuple(items), optional
    if not ret:
        return None
    return _strip_unreal(ret), (), optional


def _parse_first_line_params(text):
    params = []
    if not text.strip():
        return params
    parts = _split_top_level(text)
    if parts is None:
        return None
    for part in parts:
        part = part.strip()
        if not part:
            return None
        name, sep, default = part.partition("=")
        name = name.strip()
        if not re.fullmatch(_NAME, name):
            return None
        params.append(Param(name, default if sep else None, None, None))
    return params


def parse_method_doc(doc):
    """A UFunction doc (``x.name(a, b=1) -> T`` plus the Google-style blocks) as a MethodDoc, or None."""
    try:
        text = normalize(doc)
        if not text:
            return None
        lines = text.split("\n")
        match = _FIRST_LINE_RE.match(lines[0].strip())
        if not match:
            return None
        params = _parse_first_line_params(match.group("params"))
        ret = _parse_ret(match.group("ret"))
        if params is None or ret is None:
            return None
        returns, return_items, optional = ret

        body = lines[1:]
        tooltip_lines = []
        arg_docs = {}
        arg_types = {}
        returns_lines = []
        section = "tooltip"
        current_arg = None
        for line in body:
            stripped = line.strip()
            if stripped == "Args:":
                section = "args"
                current_arg = None
                continue
            if stripped == "Returns:":
                section = "returns"
                continue
            if section == "tooltip":
                tooltip_lines.append(line)
            elif section == "args":
                arg = _ARG_RE.match(line)
                if arg:
                    current_arg = arg.group("name")
                    arg_types[current_arg] = _strip_unreal(arg.group("type")) if arg.group("type") else None
                    arg_docs[current_arg] = arg.group("text").strip()
                elif current_arg is not None and stripped:
                    arg_docs[current_arg] = (arg_docs[current_arg] + " " + stripped).strip()
            else:
                returns_lines.append(line)

        deprecated = _find_deprecated(tooltip_lines)
        tooltip = "\n".join(l for l in tooltip_lines if not l.strip().startswith("deprecated:")).strip()
        returns_doc = "\n".join(returns_lines).strip()
        if returns is None and not return_items and returns_doc:
            # A first line without a usable return keeps the Returns: block's type ("T: text").
            head = returns_doc.split("\n", 1)[0]
            type_text, sep, _ = head.partition(": ")
            if sep and parse_type_text(type_text) is not None and type_text.strip() not in ("tuple", "None"):
                returns = _strip_unreal(type_text)
        typed = tuple(Param(p.name, p.default, arg_types.get(p.name), arg_docs.get(p.name)) for p in params)
        return MethodDoc(match.group("prefix") == "X", match.group("name"), typed, returns, return_items, optional,
                         tooltip, returns_doc, deprecated)
    except Exception:  # noqa: BLE001 - the contract is "never raises"
        return None


def parse_glue_doc(doc):
    """A glue method doc ``name(self, x: T = d) -> R -- text`` as a GlueDoc, or None."""
    try:
        text = normalize(doc).strip()
        if not text:
            return None
        match = _GLUE_RE.match(text)
        if not match:
            return None
        parts = _split_top_level(match.group("params"))
        if parts is None:
            return None
        params = []
        for part in parts:
            part = part.strip()
            if not part:
                continue
            head, sep, default = part.partition("=")
            name, colon, type_text = head.partition(":")
            name = name.strip().lstrip("*")
            if not re.fullmatch(_NAME, name):
                return None
            if name in ("self", "cls"):
                continue
            params.append(Param(name, default.strip() if sep else None,
                                _strip_unreal(type_text) if colon else None, None))
        ret = match.group("ret")
        return GlueDoc(match.group("name"), tuple(params), _strip_unreal(ret) if ret else None,
                       match.group("text").strip())
    except Exception:  # noqa: BLE001
        return None


def parse_property_doc(doc):
    """A property getset doc ``(T): [Read-Write] text`` as a PropertyDoc, or None."""
    try:
        text = normalize(doc).strip()
        match = _PROPERTY_RE.match(text)
        if not match:
            return None
        body = match.group("text")
        lines = body.split("\n")
        deprecated = _find_deprecated(lines)
        tooltip = "\n".join(l for l in lines if not l.strip().startswith("deprecated:")).strip()
        return PropertyDoc(_strip_unreal(match.group("type")), match.group("rw") == "Read-Only", tooltip, deprecated)
    except Exception:  # noqa: BLE001
        return None


def _split_class_doc(text):
    """(tooltip part, source part, editor-properties part) of a normalised class doc."""
    props_at = text.find(_EDITOR_PROPS_HEADER)
    head, props = (text, "") if props_at < 0 else (text[:props_at], text[props_at:])
    source_at = -1
    for header in (_CPP_SOURCE_HEADER, _ASSET_SOURCE_HEADER):
        found = head.find(header)
        if found >= 0 and (source_at < 0 or found < source_at):
            source_at = found
    if source_at < 0:
        return head, "", props
    return head[:source_at], head[source_at:], props


def parse_class_doc(doc):
    """A class, struct, enum or delegate doc as a ClassDoc, or None (an empty or non-string doc)."""
    try:
        text = normalize(doc)
        if not text.strip():
            return None
        head, source_text, props_text = _split_class_doc(text)
        head_lines = head.split("\n")
        deprecated = _find_deprecated(head_lines)
        tooltip = "\n".join(l for l in head_lines if not l.strip().startswith("deprecated:")).strip()

        source = None
        if source_text:
            fields = {}
            for line in source_text.split("\n")[1:]:
                field = _SOURCE_FIELD_RE.match(line.strip())
                if field:
                    fields[field.group("key")] = field.group("value").strip()
            kind = "cpp" if source_text.startswith(_CPP_SOURCE_HEADER) else "asset"
            source = SourceInfo(kind, fields.get("Plugin"), fields.get("Module"), fields.get("File"),
                                fields.get("Package"))

        props = []
        if props_text:
            current = None
            for line in props_text.split("\n")[1:]:
                entry = _EPROP_RE.match(line)
                if entry:
                    if current is not None:
                        props.append(current)
                    current = [entry.group("name"), _strip_unreal(entry.group("type")), entry.group("rw") == "Read-Only",
                               entry.group("text").strip()]
                elif current is not None and line.startswith("  ") and line.strip():
                    current[3] = (current[3] + "\n" + line.strip()).strip()
            if current is not None:
                props.append(current)
        return ClassDoc(tooltip, source, tuple(EditorProperty(*p) for p in props), deprecated)
    except Exception:  # noqa: BLE001
        return None


def strip_editor_properties(doc):
    """``doc`` (normalised) without its Editor Properties block; ``""`` for a non-string."""
    try:
        text = normalize(doc)
        at = text.find(_EDITOR_PROPS_HEADER)
        return text if at < 0 else text[:at].rstrip()
    except Exception:  # noqa: BLE001
        return ""


def parse_deprecated(doc):
    """``(old, new)`` from ``deprecated: 'old' was renamed to 'new'.``, or None."""
    try:
        match = _RENAMED_RE.search(normalize(doc))
        return (match.group("old"), match.group("new")) if match else None
    except Exception:  # noqa: BLE001
        return None


def deprecation_message(doc):
    """The text after ``deprecated:`` on a line of its own, or None."""
    try:
        return _find_deprecated(normalize(doc).split("\n"))
    except Exception:  # noqa: BLE001
        return None


def is_deprecated(doc):
    """True when a line of ``doc`` starts with ``deprecated:`` (a rename or a deprecated class or function)."""
    return deprecation_message(doc) is not None


def _parse_type_tokens(tokens, index):
    """(TypeRef, next index) or None."""
    if index >= len(tokens) or tokens[index][0] != "name":
        return None
    name = tokens[index][1]
    index += 1
    if index < len(tokens) and tokens[index] in (("punct", "["), ("punct", "(")):
        close = "]" if tokens[index][1] == "[" else ")"
        index += 1
        args = []
        while True:
            parsed = _parse_type_tokens(tokens, index)
            if parsed is None:
                return None
            arg, index = parsed
            args.append(arg)
            if index < len(tokens) and tokens[index] == ("punct", ","):
                index += 1
                continue
            if index < len(tokens) and tokens[index] == ("punct", close):
                index += 1
                break
            return None
        return TypeRef(name, tuple(args)), index
    return TypeRef(name, ()), index


def parse_type_text(text):
    """A TypeRef for a type text (``StaticMesh``, ``Array[Actor]``, ``Map[str, int32]``, ``type(Actor)``,
    ``Optional[Texture]``), with ``unreal.`` prefixes removed, or None."""
    try:
        if not isinstance(text, str):
            return None
        text = _strip_unreal(text)
        tokens = []
        position = 0
        while position < len(text):
            if text[position].isspace():
                position += 1
                continue
            match = _TYPE_TOKEN_RE.match(text, position)
            if not match or match.end() == position:
                return None
            if match.group("name") is not None:
                tokens.append(("name", match.group("name")))
            else:
                tokens.append(("punct", match.group("punct")))
            position = match.end()
        parsed = _parse_type_tokens(tokens, 0)
        if parsed is None or parsed[1] != len(tokens):
            return None
        return parsed[0]
    except Exception:  # noqa: BLE001
        return None


def _class_of_ref(ref):
    if ref is None:
        return None
    if ref.name == "Optional" and len(ref.args) == 1:
        return _class_of_ref(ref.args[0])
    if ref.args or ref.name in BUILTIN_TYPES or "." in ref.name:
        return None
    return ref.name


def class_name(text):
    """The ``unreal`` class a type text names (``StaticMesh``, ``Optional[StaticMesh]``), or None for builtins,
    containers and anything unparseable."""
    return _class_of_ref(parse_type_text(text))


def element_class(text):
    """The element class of ``Array[C]`` or ``Set[C]`` (also inside ``Optional[...]``), or None."""
    ref = parse_type_text(text)
    while ref is not None and ref.name == "Optional" and len(ref.args) == 1:
        ref = ref.args[0]
    if ref is None or ref.name not in ("Array", "Set") or len(ref.args) != 1:
        return None
    return _class_of_ref(ref.args[0])


def class_of_type(text):
    """``C`` for ``type(C)`` (a class-reference property), or None."""
    ref = parse_type_text(text)
    if ref is None or ref.name != "type" or len(ref.args) != 1:
        return None
    return _class_of_ref(ref.args[0])
