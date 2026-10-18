# Visual Blocks — general scripting layer (VB.5 + flow nodes)

Status: runtime COMPLETE and verified. Editor canvas + panel BUILT and wired
(see the reconciliation note below -- this header was stale).

Runtime verified on 2026-10-01:
- `tests/test_visual_blocks_flow.py` — 92/92 (control flow, variables, math,
  comparison, logic, loop semantics, determinism)
- `tests/test_visual_blocks.py` — 54/54 (behaviour trees; no regression)

## Audit that drove the scope

`.wlblocks` (general scripting) was a stub. `.wlbtree` (behaviour) was real.

| Layer | `.wlbtree` | `.wlblocks` |
|---|---|---|
| Nodes | 31 | **2** (`FlowOnEvent`, `Log`) |
| Control flow | shared evaluator in `wl_behavior.py` | **flat statement walk** |
| Variables | blackboard | `GraphVariable` → `self.<name>` only |
| Editor | `NPCBehaviorPanel` (tree list) | **none** |
| Canvas | n/a (right for a hierarchy) | **not vendored, none** |

The flat walk (`Compiler::CompileFlow`) followed exec links in declaration order
and emitted one statement per node at a fixed indent. That cannot express
`if`/`else` or `while`, so a general graph could not hold real logic.

## Decisions (user-confirmed)

1. **Minimal custom ImGui canvas**, in-repo against ImGui 1.89.3. No new
   dependency and no build-order risk. It is a **reusable component**, not code
   inside one panel, so every visual-blocks surface can use it.
2. **Full scripting core**, staged: control flow + variables first, then
   math/comparison/logic/string, then entity/transform/physics.
3. **Separate panels, separate extensions.** `.wlbtree` → NPC Behavior,
   `.wlblocks` → Visual Blocks. Shared registry, validator and serializer;
   separate UI, because a canvas and a tree list are different metaphors.

## Layout

- `Wasteland/src/Wasteland/VisualBlocks/NodeLibraryFlow.cpp` (new) — flow nodes
- `DemonCore-Editor/src/Panels/NodeCanvas.{h,cpp}` (new) — reusable canvas
- `DemonCore-Editor/src/Panels/VisualBlocksPanel.{h,cpp}` (new) — `.wlblocks`
- `Wasteland/src/Wasteland/VisualBlocks/Compiler.cpp` — structured emission

## Build order

```bash
MSB="/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe"
"$MSB" Wasteland/Wasteland.vcxproj                -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
"$MSB" WastelandPython.vcxproj                    -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
"$MSB" DemonCore-Editor/DemonCore-Editor.vcxproj  -p:Configuration=Release -p:Platform=x64 -p:BuildProjectReferences=false
```

**One build at a time.** A Debug and a Release build sharing one `bin/` tree
produces rotating `Permission denied` / `C1041` / `C1083` failures that read as
code errors. See `docs/BUILD_ISOLATION.md`.

Build the Python project LAST-but-one and confirm the `.pyd` exists before
running suites — the editor's post-build step can remove it.


## Runtime work: DONE

Flow palette grew 2 → 24 nodes (`Flow/Variable`, `Flow/Control`, `Flow/Math`,
`Flow/Compare`, `Flow/Logic`).

### Compiler defects found and fixed while verifying

Each of these produced *valid Python that did the wrong thing*, which is why
the suite asserts read-back values rather than "it compiled".

1. **`"Value"` was skipped when emitting temps.** The tree emitter skips a
   dict entry named `Value` because there it *is* the node rather than a
   wire. A flow expression node also names its result pin `Value`, so every
   math/comparison node emitted no assignment and consumers read a temp
   nobody had computed. Carrying a convention between two emitters with
   opposite contracts was the trap.
2. **Data emission was one level deep.** `add(get(i), 1)` emitted the outer
   expression while its own inputs stayed undefined. Now recursive, marked
   before recursing so a cycle terminates.
3. **Recursion threaded a `depth`, but indentation is ABSOLUTE.** Passing
   `depth + 1` emitted a nested temp one tab deeper than its consumer —
   "unexpected indent".
4. **Branch bodies were double-indented.** The shell writer adds the base
   indent to every line, and bodies already carried absolute tabs. Bodies are
   now RELATIVE. This only surfaced once a branch had a body, so a flat
   graph compiled fine and hid it.
5. **A loop condition was computed once, outside the loop.** `while
   self._vb_test:` on a temp assigned above the loop spins forever on the
   value from before the first iteration — the body runs but never
   terminates. Now emitted as `while True:` + in-body `if not <test>: break`,
   detected from the shell text so a new loop node needs no compiler change.
6. **Rewriting the shell dropped the body marker.** Assigning a bare
   `"while True:"` discarded the tail where the marker lives, leaving the
   loop with no body.
7. **`SetVariable.Value` was hardcoded `Float`**, so storing a comparison
   result was inexpressible — the bool could not connect to a float pin. Added
   `PinType::Any` (appended, since ordinals persist in saved graphs),
   assignable both ways but never to/from `Exec`.
8. **`And`/`Or` emitted bare `(A and B)`.** Python yields an *operand*: `(0.0
   or 1.0)` is `1.0`. On a `Bool` pin that is a silent type lie that reads
   correctly for bools and hands a float downstream. Now wrapped in `bool()`.

### Test-side corrections (the graphs were wrong, not the compiler)

Recorded because each looked like a compiler bug first:

- Math nodes have **no exec pins** — they produce a value on a wire.
- Exec reaches only what is *wired*; parallel writers off one event run once.
- `ForEach` accumulation needs the running total fed back into `Add.A`.
- A `Break` wired straight off the body runs on every pass.
- Store precedes test ⇒ the break fires on the pre-increment value (4, not 3).
  Added a counterfactual test (same loop, no Break ⇒ reaches its cap of 10) so
  "exited early" is a claim with a comparison behind it.

## Reconciliation, 2026-10-02 (items 1-3 are DONE)

Verified on disk, not from the plan:

1. `Panels/NodeCanvas.{h,cpp}` — **BUILT**, 23,159 bytes, registered in
   `DemonCore-Editor.vcxproj`.
2. `Panels/VisualBlocksPanel.{h,cpp}` — **BUILT**, 27,909 bytes, registered, and
   wired into both `EditorLayer.h` and `EditorLayer.cpp` (so `.wlblocks` is
   already re-routed off `NPCBehaviorPanel`).
3. `NPCBehaviorPanel` remains a separate panel, as the plan's decision 3 wanted —
   a canvas for `.wlblocks`, a tree list for `.wlbtree`.

**Still genuinely open:**

4. Editor **Debug** build has never been run for any of this; only Release/x64.
5. Final UI screenshot with a real graph loaded, verifying canvas hit-testing
   (node drag, wire draw) — never done, and a clean build proves nothing here.
6. The Python suites in `tests/` (`test_visual_blocks.py` 54/54,
   `test_visual_blocks_flow.py` 92/92, `test_visual_blocks_edit.py`) predate the
   panel work and have not been re-run against the current module.
