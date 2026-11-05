# SPDX-License-Identifier: Apache-2.0
"""Deprecation of public API (TDD §12.2, D29).

``deprecated(since=..., removal_engine=..., removal_version=...)`` marks a public function: each call site gets one
``DeprecationWarning``, and the decorator refuses a removal schedule earlier than D29 allows (at least one minor
release and one engine version after ``since``). CI reads ``__dream_deprecated__`` to block an early removal.
"""

import functools
import sys
import warnings

__all__ = ["deprecated", "removal_allowed", "ENGINE_VERSION"]

# The engine this build targets (the uplugin's EngineVersion, major.minor).
ENGINE_VERSION = "5.8"


def _parse(version, what):
    try:
        parts = tuple(int(part) for part in str(version).split("."))
    except ValueError:
        raise ValueError(f"deprecated(): {what} {version!r} is not a dotted version") from None
    if len(parts) < 2:
        raise ValueError(f"deprecated(): {what} {version!r} needs at least major.minor")
    return parts


def _next_minor(version):
    return (version[0], version[1] + 1)


def deprecated(*, since, removal_engine, removal_version, message=None, since_engine=ENGINE_VERSION):
    """Decorator for a deprecated public function (§12.2)."""
    since_v = _parse(since, "since")
    removal_v = _parse(removal_version, "removal_version")
    since_engine_v = _parse(since_engine, "since_engine")
    removal_engine_v = _parse(removal_engine, "removal_engine")
    if removal_v[:2] < _next_minor(since_v):
        raise ValueError(f"deprecated(): removal_version {removal_version} is earlier than one minor release after "
                         f"{since} (D29)")
    if removal_engine_v[:2] <= since_engine_v[:2]:
        raise ValueError(f"deprecated(): removal_engine {removal_engine} is not later than engine {since_engine} "
                         "(D29)")

    def decorate(fn):
        seen = set()
        text = message or (f"{fn.__qualname__}() is deprecated since Dream Python Editor {since} and will be removed in Dream Python Editor "
                           f"{removal_version} (Unreal Engine {removal_engine})")

        @functools.wraps(fn)
        def wrapper(*args, **kwargs):
            caller = sys._getframe(1)
            site = (caller.f_code.co_filename, caller.f_lineno)
            if site not in seen:
                seen.add(site)
                warnings.warn(text, DeprecationWarning, stacklevel=2)
            return fn(*args, **kwargs)

        wrapper.__dream_deprecated__ = {
            "since": since,
            "since_engine": since_engine,
            "removal_version": removal_version,
            "removal_engine": removal_engine,
        }
        return wrapper

    return decorate


def removal_allowed(meta, current_version, current_engine):
    """True when a deprecated function may be removed in ``current_version`` on ``current_engine`` (D29)."""
    return (_parse(current_version, "current_version")[:2] >= _parse(meta["removal_version"], "removal_version")[:2]
            and _parse(current_engine, "current_engine")[:2] >= _parse(meta["removal_engine"], "removal_engine")[:2])
