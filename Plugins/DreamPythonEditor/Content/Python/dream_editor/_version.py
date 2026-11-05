# SPDX-License-Identifier: Apache-2.0
"""Package version and the native ABI this package expects (TDD §12.2)."""

__version__ = "0.1.0"           # the plugin's SemVer (DreamPythonEditor.uplugin VersionName)

# _dream.ABI_VERSION must equal this, or importing dream_editor raises ImportError (§12.2 "ABI check").
ABI_VERSION = 1
