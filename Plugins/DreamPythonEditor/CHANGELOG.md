<!-- SPDX-License-Identifier: Apache-2.0 -->
# Dream Python Editor changelog

Release notes per version (TDD §12.2 SemVer: the 0.x previews may break in minor versions). A line that starts with
`API:` documents a change of the public `dream_editor` API and names the digest of
`Tests/python/api/dream_editor_api.json` that records it (T-API-006: an API change without such a line fails tier 2).
Format: `API: <version> <snapshot digest> <what changed>`.

## 0.1.0 (M1 preview)

API: 0.1.0 d65e65667505 First public surface (§12.2 M1 subset): `ScriptCancelled`, `open`, `new_tab`, `clear_output`, `run`, `suppress`, `progress`, `check_cancel` (returns True where Stop cannot be delivered, §5.10.2), `sleep`, `__version__`.
API: 0.1.0 6fb9579626a0 Renamed (PO-06 phase 3): the product "Sumi" is now Dream Python Editor and the package `sumi_editor` is `dream_editor`, same names and signatures; the docstrings name the new product. `import sumi_editor` still works through a deprecated shim that returns the `dream_editor` module itself.
