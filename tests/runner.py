"""Minimal check runner for the top-level tests/ suites.

Separate from tests/headless/runner.py because those suites need the engine
module bootstrap (wlpath) while the suites here are static audits that run
against source text -- or, like test_enemy_migration.py, do their own engine
setup. Sharing one runner would force every static audit to import the module
it is trying to check.

The `check` / `check_eq` API matches tests/headless/runner.py exactly so a
suite can be moved between the two trees without editing its assertions.
"""

from __future__ import annotations

import os
import sys
import traceback

_passes = 0
_failures: list[str] = []
_suite = ""


def check(condition: bool, message: str) -> bool:
    """Record one check. A false condition is a FAILURE, never an exception."""
    global _passes
    if condition:
        _passes += 1
        print("  PASS: %s" % message)
        return True
    _failures.append(message)
    print("  FAIL: %s" % message)
    return False


def check_eq(actual, expected, message: str) -> bool:
    return check(actual == expected,
                 "%s (got %r, want %r)" % (message, actual, expected))


def check_ne(actual, unexpected, message: str) -> bool:
    return check(actual != unexpected,
                 "%s (got %r, must differ from %r)" % (message, actual, unexpected))


def check_in(member, container, message: str) -> bool:
    return check(member in container,
                 "%s (%r not found)" % (message, member))


def check_ge(actual, floor, message: str) -> bool:
    return check(actual >= floor,
                 "%s (got %r, want >= %r)" % (message, actual, floor))


def check_le(actual, ceiling, message: str) -> bool:
    return check(actual <= ceiling,
                 "%s (got %r, want <= %r)" % (message, actual, ceiling))


def suite_begin(name: str) -> None:
    global _suite
    _suite = name
    print("== %s ==" % name)


def report() -> int:
    total = _passes + len(_failures)
    if _failures:
        print("FAILURES:")
        for f in _failures:
            print("  * %s" % f)
    print("%d/%d checks passed" % (_passes, total))
    return 0 if not _failures else 1


def run_module(mod, name: str | None = None) -> tuple[int, int]:
    """Call every test_* in `mod`. Returns (passed, failed).

    A SystemExit escaping a suite TERMINATES THE WHOLE RUN and loses the
    aggregate line -- that happened here and it is why the import is wrapped.
    """
    global _passes, _failures
    suite_begin(name or mod.__name__)
    before_p, before_f = _passes, len(_failures)
    for attr in sorted(dir(mod)):
        if not attr.startswith("test_"):
            continue
        fn = getattr(mod, attr)
        if not callable(fn):
            continue
        try:
            fn()
        except SystemExit as exc:
            _failures.append("%s called sys.exit(%r) -- a suite must not do "
                             "that; it kills the aggregate" % (attr, exc.code))
            print("  FAIL: %s raised SystemExit" % attr)
        except Exception:
            _failures.append("%s raised:\n%s" % (attr, traceback.format_exc()))
            print("  ERROR %s" % attr)
    return _passes - before_p, len(_failures) - before_f


def main(names: list[str]) -> int:
    import importlib
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path:
        sys.path.insert(0, here)
    total_p = total_f = 0
    for name in names:
        mod_name = name[:-3] if name.endswith(".py") else name
        try:
            mod = importlib.import_module(mod_name)
        except SystemExit:
            print("  FAIL: %s called sys.exit() at import time" % name)
            total_f += 1
            continue
        except Exception:
            print("  ERROR importing %s:\n%s" % (name, traceback.format_exc()))
            total_f += 1
            continue
        p, f = run_module(mod, mod_name)
        total_p += p
        total_f += f
    if len(names) > 1:
        print("%d/%d checks passed across %d suite(s)"
              % (total_p, total_p + total_f, len(names)))
    return report()


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        args = sorted(f for f in os.listdir(".")
                      if f.startswith("test_") and f.endswith(".py"))
    sys.exit(main(args))
