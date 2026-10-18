#!/usr/bin/env python3
"""Check DX12 Nova snapshot state-transition source contracts.

This is a lightweight C++ source-contract checker, not a D3D12 runtime proof:
it checks ordering and state-tracking expressions in DX12Nova.cpp/.h but does
not compile the backend, submit GPU work, or validate driver/API behavior.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "DX12Nova.cpp"
HEADER_PATH = ROOT / "Wasteland" / "src" / "Platform" / "NVRHI" / "DX12Nova.h"
COPY_DEST = r"D3D12_RESOURCE_STATE_COPY_DEST"
READ_STATE = r"snapshotReadState"


def mask_cpp_comments_and_literals(text: str) -> str:
    """Replace comments and C++ string/character literals with spaces.

    Newlines are retained so the result remains useful for diagnostics and
    brace matching is not confused by comment or literal contents.
    """
    out = list(text)
    i = 0
    state = "code"
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if ch == "/" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "line_comment"
                continue
            if ch == "/" and nxt == "*":
                out[i] = out[i + 1] = " "
                i += 2
                state = "block_comment"
                continue
            if ch == '"':
                out[i] = " "
                i += 1
                state = "string"
                continue
            if ch == "'":
                out[i] = " "
                i += 1
                state = "char"
                continue
            i += 1
            continue
        if state == "line_comment":
            if ch == "\n":
                state = "code"
            else:
                out[i] = " "
            i += 1
            continue
        if state == "block_comment":
            if ch == "*" and nxt == "/":
                out[i] = out[i + 1] = " "
                i += 2
                state = "code"
            else:
                if ch != "\n":
                    out[i] = " "
                i += 1
            continue
        # String or character literal. Honor escapes so an escaped quote does
        # not accidentally expose the rest of the literal as C++ code.
        terminator = '"' if state == "string" else "'"
        if ch == "\\" and i + 1 < len(text):
            if ch != "\n":
                out[i] = " "
            if text[i + 1] != "\n":
                out[i + 1] = " "
            i += 2
        elif ch == terminator:
            out[i] = " "
            i += 1
            state = "code"
        else:
            if ch != "\n":
                out[i] = " "
            i += 1
    return "".join(out)


def matching_brace(text: str, open_index: int) -> int | None:
    """Return the closing brace index paired with text[open_index]."""
    if open_index >= len(text) or text[open_index] != "{":
        return None
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return None


def function_body(clean_source: str, name: str) -> str | None:
    """Extract a DX12Nova member function body from comment-masked source."""
    signature = re.search(r"\bDX12Nova\s*::\s*" + re.escape(name) + r"\s*\(", clean_source)
    if not signature:
        return None
    paren_start = clean_source.find("(", signature.start())
    depth = 0
    close_paren = None
    for index in range(paren_start, len(clean_source)):
        if clean_source[index] == "(":
            depth += 1
        elif clean_source[index] == ")":
            depth -= 1
            if depth == 0:
                close_paren = index
                break
    if close_paren is None:
        return None
    brace_start = clean_source.find("{", close_paren + 1)
    if brace_start < 0:
        return None
    brace_end = matching_brace(clean_source, brace_start)
    if brace_end is None:
        return None
    return clean_source[brace_start + 1 : brace_end]


def loop_bodies(code: str, header_pattern: str) -> list[str]:
    """Extract bodies of loops matching a regex against their for-header."""
    result = []
    for match in re.finditer(r"for\s*\([^)]*\)", code):
        if not re.search(header_pattern, match.group(0)):
            continue
        brace_start = code.find("{", match.end())
        if brace_start < 0:
            continue
        brace_end = matching_brace(code, brace_start)
        if brace_end is not None:
            result.append(code[brace_start + 1 : brace_end])
    return result


def find_pattern(code: str, pattern: str, start: int = 0) -> re.Match[str] | None:
    return re.search(pattern, code[start:], re.DOTALL)


def check_sequence(code: str, patterns: list[str]) -> bool:
    """Whether each pattern appears in order, with no intervening reorder."""
    cursor = 0
    for pattern in patterns:
        match = find_pattern(code, pattern, cursor)
        if match is None:
            return False
        cursor += match.end()
    return True


def _check_contract(header_text: str, source_text: str) -> list[str]:
    """Return source-contract violations; accepts text to enable mutation tests."""
    errors: list[str] = []
    header = mask_cpp_comments_and_literals(header_text)
    source = mask_cpp_comments_and_literals(source_text)

    if not re.search(
        r"D3D12_RESOURCE_STATES\s+snapshotState\s*=\s*D3D12_RESOURCE_STATE_COPY_DEST\s*;",
        header,
    ):
        errors.append("Image must initialize its tracked snapshot state to COPY_DEST.")

    transition_snapshot = function_body(source, "TransitionSnapshot")
    if transition_snapshot is None:
        errors.append("TransitionSnapshot helper is missing or has no body.")
    else:
        if not re.search(
            r"if\s*\(\s*!image\.snapshot\s*\|\|\s*image\.snapshotState\s*==\s*after\s*\)\s*return\s*;",
            transition_snapshot,
        ):
            errors.append("TransitionSnapshot must skip null snapshots and no-op same-state transitions.")
        if not check_sequence(
            transition_snapshot,
            [
                r"Transition\s*\(\s*image\.snapshot\.Get\(\)\s*,\s*image\.snapshotState\s*,\s*after\s*\)\s*;",
                r"image\.snapshotState\s*=\s*after\s*;",
            ],
        ):
            errors.append("TransitionSnapshot must transition from and then update the tracked state.")

    create_images = function_body(source, "CreateImages")
    if create_images is None:
        errors.append("CreateImages function is missing or has no body.")
    else:
        creation_patterns = {
            "images[i]": r"CreateCommittedResource\s*\([^;]*?D3D12_RESOURCE_STATE_COPY_DEST\s*,\s*nullptr\s*,\s*IID_PPV_ARGS\s*\(\s*&images\[i\]\.snapshot\s*\)\s*\)",
            "history": r"CreateCommittedResource\s*\([^;]*?D3D12_RESOURCE_STATE_COPY_DEST\s*,\s*nullptr\s*,\s*IID_PPV_ARGS\s*\(\s*&history\.snapshot\s*\)\s*\)",
            "moments": r"CreateCommittedResource\s*\([^;]*?D3D12_RESOURCE_STATE_COPY_DEST\s*,\s*nullptr\s*,\s*IID_PPV_ARGS\s*\(\s*&moments\.snapshot\s*\)\s*\)",
        }
        reset_patterns = {
            "images[i]": r"images\[i\]\.snapshotState\s*=\s*D3D12_RESOURCE_STATE_COPY_DEST\s*;",
            "history": r"history\.snapshotState\s*=\s*D3D12_RESOURCE_STATE_COPY_DEST\s*;",
            "moments": r"moments\.snapshotState\s*=\s*D3D12_RESOURCE_STATE_COPY_DEST\s*;",
        }
        for owner, creation_pattern in creation_patterns.items():
            created = re.search(creation_pattern, create_images, re.DOTALL)
            reset = re.search(reset_patterns[owner], create_images)
            if created is None or reset is None or reset.start() < created.end():
                errors.append(f"Recreated {owner} snapshot must be created in and tracked as COPY_DEST.")

    dispatch = function_body(source, "Dispatch")
    output = function_body(source, "Output")
    clear = function_body(source, "Clear")
    commit_history = function_body(source, "CommitHistory")
    if dispatch is None:
        errors.append("Dispatch function is missing or has no body.")
    if clear is None:
        errors.append("Clear function is missing or has no body.")
    elif not check_sequence(
        clear,
        [
            r"TransitionSnapshot\s*\(\s*history\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
            r"CopyResource\s*\(\s*history\.snapshot\.Get\(\)\s*,\s*images\[1\]\.texture\.Get\(\)\s*\)\s*;",
            r"TransitionSnapshot\s*\(\s*moments\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
            r"CopyResource\s*\(\s*moments\.snapshot\.Get\(\)\s*,\s*moments\.texture\.Get\(\)\s*\)\s*;",
        ],
    ):
        errors.append("Clear must ensure history and moments snapshots are COPY_DEST before CopyResource.")
    if commit_history is None or not check_sequence(
        commit_history,
        [
            r"TransitionSnapshot\s*\(\s*history\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
            r"CopyResource\s*\(\s*history\.snapshot\.Get\(\)\s*,\s*images\[1\]\.texture\.Get\(\)\s*\)\s*;",
        ],
    ):
        errors.append("CommitHistory must ensure its destination snapshot is COPY_DEST before CopyResource.")
    if output is None:
        errors.append("Output function is missing or has no body.")

    combined_state = (
        r"const\s+D3D12_RESOURCE_STATES\s+snapshotReadState\s*=\s*"
        r"static_cast\s*<\s*D3D12_RESOURCE_STATES\s*>\s*\(\s*"
        r"D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE\s*\|\s*"
        r"D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE\s*\)\s*;"
    )
    if dispatch is not None:
        if not re.search(combined_state, dispatch):
            errors.append("Dispatch must define the combined pixel and non-pixel shader-read state.")

        image_loops = loop_bodies(dispatch, r"int\s+i\s*=\s*0\s*;\s*i\s*<\s*8\s*;")
        image_copy_loop = next(
            (body for body in image_loops if "images[i].snapshot.Get()" in body and "TransitionSnapshot" in body),
            None,
        )
        if image_copy_loop is None or not check_sequence(
            image_copy_loop,
            [
                r"TransitionSnapshot\s*\(\s*images\[i\]\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
                r"Transition\s*\(\s*images\[i\]\.texture\.Get\(\)\s*,\s*D3D12_RESOURCE_STATE_UNORDERED_ACCESS\s*,\s*D3D12_RESOURCE_STATE_COPY_SOURCE\s*\)\s*;",
                r"CopyResource\s*\(\s*images\[i\]\.snapshot\.Get\(\)\s*,\s*images\[i\]\.texture\.Get\(\)\s*\)\s*;",
                r"Transition\s*\(\s*images\[i\]\.texture\.Get\(\)\s*,\s*D3D12_RESOURCE_STATE_COPY_SOURCE\s*,\s*D3D12_RESOURCE_STATE_UNORDERED_ACCESS\s*\)\s*;",
                r"TransitionSnapshot\s*\(\s*images\[i\]\s*,\s*snapshotReadState\s*\)\s*;",
            ],
        ):
            errors.append(
                "Dispatch image snapshots must be tracked into COPY_DEST before refresh and into combined shader-read state after refresh."
            )

        dispatch_call = re.search(r"commandList\s*->\s*Dispatch\s*\(", dispatch)
        history_read = re.search(
            r"TransitionSnapshot\s*\(\s*history\s*,\s*snapshotReadState\s*\)\s*;", dispatch
        )
        moments_copy = re.search(
            r"CopyResource\s*\(\s*moments\.snapshot\.Get\(\)\s*,\s*moments\.texture\.Get\(\)\s*\)\s*;",
            dispatch,
        )
        moments_copy_dest_before = re.search(
            r"TransitionSnapshot\s*\(\s*moments\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
            dispatch,
        )
        moments_read = re.search(
            r"TransitionSnapshot\s*\(\s*moments\s*,\s*snapshotReadState\s*\)\s*;", dispatch
        )
        if dispatch_call is None or history_read is None or history_read.start() >= dispatch_call.start():
            errors.append("Dispatch history snapshot must enter combined shader-readable state before Dispatch.")
        if (
            dispatch_call is None
            or moments_copy is None
            or moments_copy_dest_before is None
            or moments_read is None
            or not (
                moments_copy_dest_before.start()
                < moments_copy.start()
                < moments_read.start()
                < dispatch_call.start()
            )
        ):
            errors.append("Dispatch must refresh moments before making its snapshot compute-readable and dispatching.")
        if dispatch_call is not None:
            history_copy_dest = re.search(
                r"TransitionSnapshot\s*\(\s*history\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
                dispatch[dispatch_call.end() :],
            )
            moments_copy_dest = re.search(
                r"TransitionSnapshot\s*\(\s*moments\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
                dispatch[dispatch_call.end() :],
            )
            flush = re.search(r"FlushCommandList\s*\(\s*\)\s*;", dispatch[dispatch_call.end() :])
            if (
                history_copy_dest is None
                or moments_copy_dest is None
                or flush is None
                or not (history_copy_dest.start() < moments_copy_dest.start() < flush.start())
            ):
                errors.append(
                    "History and moments snapshots must return to COPY_DEST after Dispatch and before submission."
                )

    if output is not None:
        if not re.search(combined_state, output):
            errors.append("Output must define the combined pixel and non-pixel shader-read state.")
        if not check_sequence(
            output,
            [
                r"TransitionSnapshot\s*\(\s*images\[index\]\s*,\s*D3D12_RESOURCE_STATE_COPY_DEST\s*\)\s*;",
                r"Transition\s*\(\s*images\[index\]\.texture\.Get\(\)\s*,\s*D3D12_RESOURCE_STATE_UNORDERED_ACCESS\s*,\s*D3D12_RESOURCE_STATE_COPY_SOURCE\s*\)\s*;",
                r"CopyResource\s*\(\s*images\[index\]\.snapshot\.Get\(\)\s*,\s*images\[index\]\.texture\.Get\(\)\s*\)\s*;",
                r"Transition\s*\(\s*images\[index\]\.texture\.Get\(\)\s*,\s*D3D12_RESOURCE_STATE_COPY_SOURCE\s*,\s*D3D12_RESOURCE_STATE_UNORDERED_ACCESS\s*\)\s*;",
                r"TransitionSnapshot\s*\(\s*images\[index\]\s*,\s*snapshotReadState\s*\)\s*;",
                r"FlushCommandList\s*\(\s*\)\s*;",
            ],
        ):
            errors.append(
                "Output must track its snapshot into COPY_DEST before copying and into combined shader-read state before submission."
            )
    return errors


def run_mutation_controls(header_text: str, source_text: str) -> list[str]:
    """Prove representative state-contract regressions are rejected."""
    controls = [
        (
            "original fixed-before-state mismatch",
            "TransitionSnapshot(images[i], D3D12_RESOURCE_STATE_COPY_DEST);",
            "Transition(images[i].snapshot.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);",
            "Dispatch image snapshots must be tracked into COPY_DEST",
        ),
        (
            "missing image compute-readable transition",
            "TransitionSnapshot(images[i], snapshotReadState);",
            "/* mutation: image read transition removed */",
            "Dispatch image snapshots must be tracked into COPY_DEST",
        ),
        (
            "missing history compute-readable transition",
            "TransitionSnapshot(history, snapshotReadState);",
            "/* mutation: history read transition removed */",
            "Dispatch history snapshot must enter combined shader-readable state",
        ),
        (
            "missing moments recreation reset",
            "moments.snapshotState = D3D12_RESOURCE_STATE_COPY_DEST;",
            "/* mutation: moments recreation state reset removed */",
            "Recreated moments snapshot must be created in and tracked as COPY_DEST",
        ),
        (
            "missing post-dispatch moments return",
            "TransitionSnapshot(history, D3D12_RESOURCE_STATE_COPY_DEST);\n    TransitionSnapshot(moments, D3D12_RESOURCE_STATE_COPY_DEST);",
            "TransitionSnapshot(history, D3D12_RESOURCE_STATE_COPY_DEST);\n    /* mutation: moments COPY_DEST return removed */",
            "History and moments snapshots must return to COPY_DEST after Dispatch",
        ),
    ]
    failures: list[str] = []
    for name, old, new, expected_diagnostic in controls:
        if source_text.count(old) != 1:
            failures.append(f"Mutation control '{name}' could not find exactly one source target.")
            continue
        mutated = source_text.replace(old, new, 1)
        violations = _check_contract(header_text, mutated)
        if not any(expected_diagnostic in error for error in violations):
            failures.append(f"Mutation control '{name}' was not rejected by its intended contract check.")
    return failures


def main() -> int:
    try:
        header_text = HEADER_PATH.read_text(encoding="utf-8")
        source_text = SOURCE_PATH.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"FAIL: cannot read DX12 Nova source: {exc}", file=sys.stderr)
        return 1

    contract_errors = _check_contract(header_text, source_text)
    mutation_errors = run_mutation_controls(header_text, source_text)
    if contract_errors or mutation_errors:
        for error in contract_errors:
            print(f"FAIL: {error}", file=sys.stderr)
        for error in mutation_errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("PASS: DX12 Nova snapshot source-contract checks (not D3D12 runtime proof).")
    print("PASS: 5 mutation controls rejected representative snapshot-state regressions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
