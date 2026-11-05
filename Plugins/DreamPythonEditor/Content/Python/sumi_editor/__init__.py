# SPDX-License-Identifier: Apache-2.0
"""Deprecated compatibility name: the product was renamed from "Sumi" to "Dream Python Editor" (PO-06 phase 3).

``import sumi_editor`` keeps old scripts working: it imports ``dream_editor`` and puts that very module in
``sys.modules["sumi_editor"]``, so every name (``progress``, ``check_cancel``, ``ScriptCancelled``, ...) is the same
object under either name. Import ``dream_editor`` in new code; this shim will be removed in a later release.
"""

import sys as _sys
import warnings as _warnings

import dream_editor as _dream_editor

_warnings.warn("sumi_editor is deprecated: import dream_editor instead (the product is now Dream Python Editor)",
               DeprecationWarning, stacklevel=2)

_sys.modules[__name__] = _dream_editor
