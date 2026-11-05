# SPDX-License-Identifier: Apache-2.0
"""Python-version specifics (TDD §2.5 rule 7: ``sys.version_info`` is read only here; D15).

Slice A targets the engine's CPython 3.11.8; the helpers below keep the version-dependent details in one place.
"""

import sys
import warnings

PY_VERSION = sys.version_info[:2]

# 3.11+: tracebacks carry column positions (carets, RT-14).
HAS_CARETS = PY_VERSION >= (3, 11)

# The codeop flags (Include/cpython/compile.h); codeop exposes them but calls them unsupported outside the stdlib.
PYCF_DONT_IMPLY_DEDENT = 0x200
PYCF_ALLOW_INCOMPLETE_INPUT = 0x4000

_opcodes = {}

# The two warning-display slots that catch_warnings(record=True) swaps (§5.3.2): showwarning, and the impl that
# _showwarnmsg falls back to while showwarning is the stdlib's own.
WARNING_DISPLAY_SLOTS = ("showwarning", "_showwarnmsg_impl")


def has_setup_annotations(code):
    """True when the module code object itself (not a nested def or class) runs SETUP_ANNOTATIONS (§5.3.2).

    3.11 bytecode is 2-byte code units; ``co_code`` has its inline caches zeroed (CACHE is opcode 0), so a scan of the
    even offsets sees every real instruction once.
    """
    if "SETUP_ANNOTATIONS" not in _opcodes:
        import opcode   # first statement-mode prepare, not boot (§3.4 boot budget)

        _opcodes["SETUP_ANNOTATIONS"] = opcode.opmap.get("SETUP_ANNOTATIONS")
    target = _opcodes["SETUP_ANNOTATIONS"]
    if target is None:
        return False
    raw = code.co_code
    return any(raw[i] == target for i in range(0, len(raw), 2))


def warning_display_slots():
    """The (name, current value) pairs of the warning-display slots that exist in this Python."""
    return [(name, getattr(warnings, name)) for name in WARNING_DISPLAY_SLOTS if hasattr(warnings, name)]
