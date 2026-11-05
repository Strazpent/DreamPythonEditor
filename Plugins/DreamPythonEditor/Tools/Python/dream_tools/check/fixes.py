# SPDX-License-Identifier: Apache-2.0
"""``textDocument/codeAction`` (TDD §7.3.3, §7.8.3; INT-09).

For every SUM diagnostic of the latest analysis that touches the requested range: the rule's fix when it has one
(SUM102 ``Use set_editor_property`` is ``isPreferred``), then one ``Suppress SUMxxx on this line`` per code. Each
action carries a versioned ``WorkspaceEdit`` (``documentChanges`` with the server's version of the document), so the
client drops a fix computed for an older version (§7.8.3).
"""

from . import noqa


def _intersects(start, end, range_start, range_end):
    return start <= range_end and range_start <= end


def lsp_diagnostic(document, diagnostic, encoding):
    item = {
        "range": document.range_to_lsp(diagnostic.start, diagnostic.end, encoding),
        "severity": diagnostic.severity,
        "code": diagnostic.code,
        "source": "dream-check",
        "message": diagnostic.message,
    }
    if diagnostic.fix is not None:
        item["data"] = {"fixable": True}
    return item


def _action(document, title, diagnostic, edits, encoding, preferred=False):
    action = {
        "title": title,
        "kind": "quickfix",
        "diagnostics": [lsp_diagnostic(document, diagnostic, encoding)],
        "edit": {
            "documentChanges": [{
                "textDocument": {"uri": document.uri, "version": document.version},
                "edits": [{"range": document.range_to_lsp(start, end, encoding), "newText": text}
                          for start, end, text in edits],
            }],
        },
    }
    if preferred:
        action["isPreferred"] = True
    return action


def code_actions(document, diagnostics, range_start, range_end, encoding):
    """The CodeAction literals for ``diagnostics`` (the document's latest analysis) touching the range."""
    hits = [d for d in diagnostics
            if d.code.startswith("SUM") and not d.code.startswith("SUM9") and _intersects(d.start, d.end, range_start, range_end)]
    fixes = []
    suppress = []
    unsafe = noqa.unsafe_comment_lines(document.text) if hits else set()
    seen_codes = set()
    for diagnostic in hits:
        if diagnostic.fix is not None and diagnostic.fix.edits:
            fixes.append(_action(document, diagnostic.fix.title, diagnostic, diagnostic.fix.edits, encoding,
                                 diagnostic.fix.preferred))
        line = diagnostic.start[0]
        key = (diagnostic.code, line)
        if key in seen_codes or line in unsafe:
            continue
        seen_codes.add(key)
        edit = noqa.noqa_edit(document.line(line), line, diagnostic.code)
        suppress.append(_action(document, "Suppress %s on this line" % diagnostic.code, diagnostic, [edit], encoding))
    fixes.sort(key=lambda action: not action.get("isPreferred", False))
    return fixes + suppress
