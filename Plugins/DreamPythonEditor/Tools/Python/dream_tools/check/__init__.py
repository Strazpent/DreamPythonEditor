# SPDX-License-Identifier: Apache-2.0
"""dream-check: Dream's own LSP server for the SUM lint codes (TDD §7.3.3; U5, INT-07, INT-08, LRS-07).

Modules: ``framing`` (Content-Length frames), ``document`` (texts, positions, URIs), ``docs`` (the shared docstring
parser, ``dream_docstrings``), ``snapshot`` (the Stage 1 snapshot index, RT-13), ``infer`` (local type inference),
``analysis`` (the analysis driver and the rule families' shared context), ``rules_*`` (one module per rule family),
``noqa``, ``fixes`` (code actions) and ``server`` (the loop, the debounce and the parent watch).
"""

VERSION = "1.0.0"
