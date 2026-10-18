"""A minimal test runner, so the suite has no external dependency.

The interpreter the project builds against ships a ``python314._pth`` and runs in
isolated mode, so ``pip install pytest`` is not available without changing the
interpreter the engine links against. Rather than perturb that, this provides the
smallest thing that works: collect ``test_*`` callables from a module, run them,
report failures with tracebacks and a per-file summary.

Deliberately not a pytest clone. If a test needs fixtures, parametrisation or
plugins later, add them here explicitly rather than growing a framework.
"""

from __future__ import annotations

import importlib
import os
import sys
import time
import traceback
from typing import Callable

GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
DIM = "\033[2m"
RESET = "\033[0m"


class Failure(AssertionError):
    """Raised by the check helpers so failures read as sentences, not asserts."""


def check(condition: bool, message: str) -> None:
    """Assert with a message. Prefer this over bare ``assert`` in tests."""
    if not condition:
        raise Failure(message)


def check_eq(actual, expected, message: str = "") -> None:
    if actual != expected:
        suffix = f" ({message})" if message else ""
        raise Failure(f"expected {expected!r}, got {actual!r}{suffix}")


def check_near(actual: float, expected: float, tol: float, message: str = "") -> None:
    """Compare floats with an explicit tolerance.

    A tolerance is always named at the call site, because "close enough" without
    a stated bound is how a red test gets loosened into meaninglessness.
    """
    if not (abs(actual - expected) <= tol):
        suffix = f" ({message})" if message else ""
        raise Failure(
            f"expected {expected} +/- {tol}, got {actual} "
            f"(delta {abs(actual - expected):.6g}){suffix}"
        )


def check_le(actual: float, bound: float, message: str = "") -> None:
    if not (actual <= bound):
        suffix = f" ({message})" if message else ""
        raise Failure(f"expected <= {bound}, got {actual}{suffix}")


def check_ge(actual: float, bound: float, message: str = "") -> None:
    if not (actual >= bound):
        suffix = f" ({message})" if message else ""
        raise Failure(f"expected >= {bound}, got {actual}{suffix}")


def run_module(module_name: str, *, verbose: bool = False) -> tuple[int, int, list[str]]:
    """Run every ``test_*`` callable in a module. Returns (passed, failed, errors)."""
    module = importlib.import_module(module_name)
    names = sorted(n for n in dir(module) if n.startswith("test_"))

    if not names:
        print(f"{YELLOW}  (no tests found in {module_name}){RESET}")
        return 0, 0, []

    passed = 0
    failures: list[str] = []

    for name in names:
        func: Callable = getattr(module, name)
        if not callable(func):
            continue
        try:
            func()
            passed += 1
            if verbose:
                print(f"    {DIM}pass{RESET} {name}")
        except Failure as exc:
            failures.append(f"{module_name}.{name}\n      {exc}")
            print(f"    {RED}FAIL{RESET} {name}: {exc}")
        except Exception:  # noqa: BLE001 — a crash is a failure to report, not to raise
            tb = traceback.format_exc()
            failures.append(f"{module_name}.{name}\n{tb}")
            print(f"    {RED}ERROR{RESET} {name}")
            for line in tb.strip().splitlines()[-6:]:
                print(f"      {DIM}{line}{RESET}")

    total = passed + len(failures)
    colour = GREEN if not failures else RED
    print(f"  {colour}{passed}/{total} passed{RESET}  {DIM}{module_name}{RESET}")
    return passed, len(failures), failures


def discover(pattern: str = "test_*.py") -> list[str]:
    """Test module names in this directory, excluding helpers."""
    here = os.path.dirname(os.path.abspath(__file__))
    out = []
    for entry in sorted(os.listdir(here)):
        if entry.startswith("test_") and entry.endswith(".py"):
            out.append(entry[:-3])
    return out


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    verbose = "-v" in argv
    targets = [a for a in argv if not a.startswith("-")]

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

    import wlpath

    try:
        module_dir = wlpath.setup()
    except RuntimeError as exc:
        print(f"{RED}{exc}{RESET}")
        return 2

    print(f"{DIM}module: {module_dir}{RESET}")
    print(f"{DIM}python: {sys.version.split()[0]}{RESET}\n")

    names = targets or discover()
    total_passed = 0
    total_failed = 0
    all_failures: list[str] = []
    started = time.time()

    for name in names:
        print(f"{YELLOW}== {name} =={RESET}")
        p, f, failures = run_module(name, verbose=verbose)
        total_passed += p
        total_failed += f
        all_failures.extend(failures)
        print()

    elapsed = time.time() - started
    grand = total_passed + total_failed
    if total_failed:
        print(f"{RED}{'=' * 60}{RESET}")
        print(f"{RED}FAILURES{RESET}")
        for failure in all_failures:
            print(f"  {RED}*{RESET} {failure}")
        print(f"{RED}{'=' * 60}{RESET}")

    colour = GREEN if not total_failed else RED
    print(
        f"{colour}{total_passed}/{grand} checks passed{RESET} "
        f"across {len(names)} suite(s) in {elapsed:.2f}s"
    )
    return 1 if total_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
