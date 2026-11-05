# SPDX-License-Identifier: Apache-2.0
"""Dream Python Editor internals (TDD §2.2, §5.3, §6.1). Nothing here is public API (D29).

Modules: ``native`` (the ``_dream`` binding and the null ``_dream``), ``runner`` (prepare and run), ``namespaces``,
``autoprint``, ``streams`` (the run swaps), ``task`` (cooperative API and the sliced sleep), ``compat`` (the only
place that reads ``sys.version_info``), ``deprecation``. Frames from this directory never appear in a user's
traceback (§5.3.7).
"""
