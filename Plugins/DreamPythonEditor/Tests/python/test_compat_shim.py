# SPDX-License-Identifier: Apache-2.0
"""(rename, PO-06 phase 3, A246) ``import sumi_editor`` keeps old scripts working after the rename to Dream Python Editor:
the shim warns once (DeprecationWarning) and hands back the ``dream_editor`` module itself."""

import importlib
import sys
import unittest
import warnings

import dream_editor


class CompatShim(unittest.TestCase):
    def setUp(self):
        sys.modules.pop("sumi_editor", None)

    def tearDown(self):
        sys.modules.pop("sumi_editor", None)

    def test_import_returns_the_dream_editor_module_with_a_deprecation_warning(self):
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            shim = importlib.import_module("sumi_editor")
        self.assertIs(shim, dream_editor)
        self.assertIs(sys.modules["sumi_editor"], dream_editor)
        deprecations = [w for w in caught if issubclass(w.category, DeprecationWarning)]
        self.assertEqual(len(deprecations), 1, [str(w.message) for w in caught])
        self.assertIn("dream_editor", str(deprecations[0].message))

    def test_old_names_are_the_same_objects(self):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            import sumi_editor  # noqa: F401 - the old name under test
            from sumi_editor import ScriptCancelled, check_cancel, progress
        self.assertIs(ScriptCancelled, dream_editor.ScriptCancelled)
        self.assertIs(check_cancel, dream_editor.check_cancel)
        self.assertIs(progress, dream_editor.progress)

    def test_a_second_import_does_not_warn_again(self):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", DeprecationWarning)
            importlib.import_module("sumi_editor")
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            importlib.import_module("sumi_editor")
        self.assertEqual([w for w in caught if issubclass(w.category, DeprecationWarning)], [])


if __name__ == "__main__":
    unittest.main()
