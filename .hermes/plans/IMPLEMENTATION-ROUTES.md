# IMPLEMENTATION ROUTES — all remaining work, VB / NB / AVB / Item 14

> **Scope.** Every genuinely missing or unverified piece of work across four
> audited areas, with **all viable routes** to completing each one — not one
> recommendation. This document assumes the reader has read none of the four
> source plans. It cites `file:line` throughout.
>
> **Source plans** (read these for intent, not for status):
> - `.hermes/plans/2026-09-30_161447-visual-blocks.md` — Visual Blocks (VB.0–VB.6)
> - `.hermes/plans/2026-09-30_162256-npc-behavioral-visual-blocks.md` — NPC behavioural (NB.0–NB.8)
> - `.hermes/plans/2026-10-01_161235-auditory-visual-blocks.md` — Auditory (AVB.0–AVB.11)
> - `.hermes/plans/item-14-long-tail-systems.md` — Item 14 phases 14.1–14.6
> - `.hermes/plans/REMAINING-WORK-P0-P3.md` — prior consolidated audit
> - `.hermes/plans/vb-general-scripting.md` — the flow-graph reconciliation note
>
> **Nothing in this document is implemented by it.** No source, no tests, no
> engine code was changed to produce it. The only file created is this one.

---

## 0. How to read this, and the status vocabulary

### 0.1 Three states, never conflated

The single most expensive confusion in this repo's history has been calling
"implemented in source" or "a suite is green" the same thing as "it works".
Every work item below is tagged with one of:

| Tag | Meaning |
|---|---|
| **BUILT** | Symbols exist in source at the cited lines. Says nothing about whether a binary containing them has been linked, or whether anything calls them. |
| **VERIFIED** | A named test exists, I re-ran it, and I quote the specific assertion that would fail on regression. |
| **MISSING** | Absent. I grepped/listed and it is not there. |
| **UNVERIFIED** | I could not establish the state without building or running the editor, and building/running the editor is out of scope for this document. |

If a claim in a prior document conflicts with the table in §1, the table wins;
where they conflict I say so explicitly.

### 0.2 Verification I performed for this document

Everything in §1 was checked directly against source on this machine. I ran
the interpreter the project links against, through the project's own runner:

```
C:/Users/rtoue/AppData/Local/Python/pythoncore-3.14-64/python.exe
```

I did **not** start any build (MSBuild must not overlap other work), did not
kill any process, and did not touch `Renderer3D.*`, `DX12Nova.*`,
`NVRHIContext.cpp`, or anything else.

> **The module under test.** The canonical
> `bin/Release-windows-x86_64/DemonCore-Editor/Wasteland.pyd` is **locked by
> running Hermes kernels**, so anything run against it exercises a *previous*
> build. Every result in this document was obtained with
> `WASTELAND_MODULE_DIR=C:/dev/Wasteland/.buildlogs/linkprobe`, which
> `wlpath.py:44-48` prefers when set. **MSBuild `RC=0` does not prove the
> artifact exists** — a link can fail and still leave `RC=0` with a
> zero-byte `.pyd` that fails to import with *"DLL load failed: %1 is not a
> valid Win32 application"*. Validate size and MZ magic, never the exit code.

### 0.3 The four failure modes this document is written around

These are not hypothetical. Each one has produced a wrong claim in this
codebase, so each work item below names the assertion that would catch it.

1. **`grep "FAIL"` matches a passing assertion.** A suite that names a check
   *"returns FAILURE"* greps positive. Match the checker's own `  FAIL ` prefix.
2. **`RC=0` from MSBuild does not mean the artifact exists.** A link can fail
   `LNK1149` and still leave `RC=0` with a zero-byte `.pyd`, which then fails
   to import with the maximally misleading *"DLL load failed: %1 is not a valid
   Win32 application"*. **Validate file size and MZ magic, never the exit code.**
3. **Tests have been wrong more often than the code in this session.** Four
   navmesh tests passed while proving nothing: one because the wall's own top
   was baked as "ground"; one because a detour clipping a corner is *shorter*
   than the straight line, so a length-based assertion passed; one because the
   blocked band was measured globally instead of at the wall's own x; one
   because `FindNearestWalkable`'s ring search quietly makes any point
   reachable. **Every assertion below states what it measures.**
5. **A red suite may be a stale binary, not a code bug.** The canonical
   `Wasteland.pyd` is kernel-locked; `wlpath.py:44-48` documents the
   `WASTELAND_MODULE_DIR` escape. I measured a **false red 4/8** on a *correct*
   physics accumulator because the module predated `Scene.cpp` by eight
   minutes. **Before changing any code because a suite is red, prove the module
   under test is newer than the source you are editing.** See §2.2.
4. **A suite that prints all-green then exits 139 is a PASS.** `Scene::OnRuntimeStop()`
   and standalone `ScriptEngine::OnUpdateEntity` segfault for pre-existing
   reasons. Never call either. Judge the printed checks, not the exit code.

---

## 1. Verified current state

Everything here was checked against source or by running a suite on this
machine. Anything I could not check is marked UNVERIFIED.

### 1.1 Source inventory (BUILT)

| Path | Lines/bytes | What it is |
|---|---|---|
| `Wasteland/src/Wasteland/VisualBlocks/Graph.h/.cpp` | 25792 / 29584 B | Graph, node, link, variable, blackboard, pin defs |
| `VisualBlocks/GraphSerializer.h/.cpp` | 3884 / 48039 B | YAML load/save for `.wlblocks` and `.wlbtree` |
| `VisualBlocks/NodeRegistry.h/.cpp` | 15733 / 20073 B | the one registry; `Get()` self-registers (`NodeRegistry.h:176-186`, `RegisterAllNodes` at `:242`) |
| `VisualBlocks/NodeLibrary.cpp` | 440 lines, 17 `TypeId` | behaviour composites/decorators/conditions/tasks + `MoveTo` |
| `VisualBlocks/NodeLibraryFlow.cpp` | 36565 B, 15 `TypeId` | flow palette — `Flow/Event`, `Flow/Variable`, `Flow/Control`, `Flow/Math`, `Flow/Logic`, `Flow/Utility` |
| `VisualBlocks/NodeLibraryV1.cpp` | 16981 B, 14 `TypeId` | NB.4 v1 taxonomy, aliases, `SenseNearest` `HalfAngle`/`RequireLOS` (`:312`, `:318`) |
| `VisualBlocks/Validator.h/.cpp` | 1136 lines | rules incl. `CheckNoSharedChildren` (4 hits), `IsAllowedOperator` (`Validator.h:88`) |
| `VisualBlocks/Compiler.cpp` | **1801 lines**, 68979 B | flow + tree emitters, runtime shim at `:176` (`R"WL_SHIM(`) |
| `VisualBlocks/BehaviorTree.h/.cpp` | 2728 / 10139 B | `.wlbtree` façade: load → validate → emit |
| `Scene/BehaviorTreeSystem.h/.cpp` | 2216 / 8153 B | scene-side tree attach/tick |
| `Navigation/NavMesh.h/.cpp` | 7058 / 26599 B | bake, A*, string pulling, `IsGround` (`.cpp:114`) |
| `Audio/AudioClip.{h,cpp}` `Mixer.{h,cpp}` `AudioSystem.{h,cpp}` | — | device-independent audio core; `Mixer::Mix(float*, int)` at `Mixer.h:259` |
| `Scene/SaveGame.{h,cpp}` | 5482 / 34306 B | `.wlsave`; `kCurrentSchemaVersion = 1` at `SaveGame.h:57` |
| `Scripting/Bindings.cpp` | 244143 B, 4543 lines | the whole Python surface |
| `Age/` | 23 files | age-assurance C++ module |
| `DemonCore-Editor/src/Panels/NPCBehaviorPanel.{h,cpp}` | 23994 / 78275 B | tree-list panel |
| `DemonCore-Editor/src/Panels/VisualBlocksPanel.{h,cpp}` | 17553 / 60323 B | canvas panel |
| `DemonCore-Editor/src/Panels/NodeCanvas.{h,cpp}` | 13599 / 61120 B | reusable canvas component |
| `DemonCore-Editor/assets/scripts/wl_behavior.py` | 33170 B | shared tree evaluator, hand-written |
| `assets/trees/*.wlbtree` | 3 files | `EnemyChase` (19 node refs), `EnemyGuard`, `PatrolGuard` |
| `assets/graphs/DamageFlash.wlblocks` | 1 file | the only shipped flow graph |

**Note on a path in the task brief.** The brief says
`DemonCore-Editor/editor/`. That directory **does not exist**. The panels are
at `DemonCore-Editor/src/Panels/`. The brief also says
`DemonCore-Editor/assets/blocks/` — that directory **does not exist**; the one
flow graph lives in `assets/graphs/`.

### 1.2 Suites — I re-ran these (VERIFIED)

```
tools/t.sh visualblocks
  test_visual_blocks.py          passed 54, failed 0
  test_visual_blocks_flow.py     passed 99, failed 0
  test_visual_blocks_edit.py     OK

tests/headless, bare runner.py, against .buildlogs/linkprobe
  test_audio_clip 35/35 · test_audio_mixer 26/26 · test_ground_snap 4/4
  test_los_contract 3/3 · test_navmesh 10/10 · test_pathfinding_node 6/6
  test_perception_cache 13/13 · test_physics_3d 5/5
  test_physics_timestep 3/3 · test_shim_pathfinding 7/7 · test_tree_attach 5/5
  ---------------------------------------------------------------------
  117/117 checks passed across 11 suite(s) in 0.29s
scratch/test_node_library.py     1209 passed, 0 failed   (exit 0)
```

The 1209-check suite contains the systematic check *"every shim call in
generated code matches its signature"* (`test_node_library.py:573-643`), which
parses each `R"WL_SHIM"` helper signature and counts arguments at every call
site. That class of bug — a helper signature drifting from its emitter call
sites — is invisible until an NPC enters that branch at runtime.

### 1.3 What is genuinely MISSING (grep-verified, this session)

| Claim to check | Command | Result |
|---|---|---|
| Any audio output backend | `grep -rniE 'miniaudio\|WASAPI\|IAudioClient\|SDL_Audio\|OpenAL\|FMOD\|RtAudio\|waveOut\|WinMM\|mmio' Wasteland/src/` | **0 hits** |
| Any AVB subsystem | `grep -rn 'Avb\|wlavb\|wltrack' Wasteland/ premake5.lua` | **0 hits** (one binary hit inside a vendored `.git/objects/pack`, irrelevant) |
| `imgui-node-editor` vendored | `ls Wasteland/vendor/` → Box2D Box3D entt GLAD GLFW glm imgui ImGuizmo nvrhi pybind11 spdlog stb_image yaml-cpp | **absent** |
| WAV writer | `grep -rn 'SaveWav\|WriteWav\|WriteWave\|SaveWave' Wasteland/src/` | **0 hits** — reader only |
| Trace toggle in either panel | `grep -i trace VisualBlocksPanel.cpp NPCBehaviorPanel.cpp` | **0 hits** in both |
| Trace hook in the runtime | `wl_behavior.py:717 set_trace`, `:726 trace_to_list` | present |
| `FollowPath` task | `grep -rn FollowPath Wasteland/src DemonCore-Editor/src assets` | **0 hits** |
| Local avoidance / separation | `grep -inE 'avoid\|separat\|RVO' Compiler.cpp` | **0 hits** (only prose in comments) |
| Apply-impulse / set-velocity | `grep -rn 'ApplyImpulse\|SetVelocity\|AddForce' Wasteland/src/` | **0 hits** |
| Inventory tests | `grep -rln 'AddItem\|InventoryComponent' tests/` | **0 hits** |
| Save-game tests | `find -name 'test_savegame*'` across repo + hermes cache | **not found** |
| Terrain system | `grep -rn 'Terrain\|Heightfield\|Chunk' Wasteland/src --include=*.h` | only water-simulation comments (`Components.h:1422`, `:1780`) |
| Age ImGui panel | `grep -rn GuardianTools DemonCore-Editor/src` | **0 hits** — logic referenced only inside `Age/` |
| `AgeGate.py` wired to a scene | `grep -rn AgeGate assets/scenes/*.wastescene` | **0 hits** |
| `assets/scripts/generated/` | `ls` | **does not exist** — `CompileAndWrite` writes beside the graph (`VisualBlocksPanel.cpp:319`) |
| `SyncPendingPhysics2D` | `Scene.cpp:790-806` | **explicitly documented as missing**, see §5.1 |

### 1.4 Corrections to prior documents (these matter, they change scope)

**C1 — AVB.0's premise is now false.** The AVB plan §0.4 claims *"there is no
`test_audio.py` and no Python test suite anywhere in the repo"*. That was true
when written. Today `tests/headless/` exists with `runner.py`, `wlpath.py`, and
two audio suites totalling **61 checks, all green** (I ran them). **AVB.0 is
effectively done.** Effort drops from M to S.

**C2 — the `test_savegame.py`, 57 checks" claim in item-14 §14.3 is unsupported.**
`SaveGame.{h,cpp}` is BUILT and bound (`Bindings.cpp:3938-3981`) and the
components exist (`Components.h:2372 PersistentComponent`, `:2390
QuestFlagsComponent`, `:2445 WorldTimerComponent`). But **the suite does not
exist on disk anywhere** — not in `tests/`, not in the hermes scratch cache. 14.3
is therefore *built-but-unproven*, not done.

**C3 — the Debug build is stale, not absent.** `vb-general-scripting.md` says
"Editor Debug build has never been run". `bin/Debug-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe`
**exists** (44,968,448 B, 2026-10-03 22:02) and a Debug `Wasteland.pyd` exists
(42,793,472 B, 2026-10-02 21:54). Both are **older than the Release module**
(7,675,904 B, 2026-10-04 02:54) and older than several sources
(`NodeLibraryV1.cpp` 02:44, `Scene.cpp` 03:02). So: a Debug build ran at some
point, but it predates the current tree. Treat Debug as **not-currently-verified**,
not as missing.

**C4 — a real harness defect, found and reproduced in this session, and now
**FIXED**. `tests/headless/runner.py` collects `test_*` **functions**
(`runner.py:74`) and calls them (`:88`). Two suites were **script-style** — they
executed checks at module scope and ended with `sys.exit(...)`. Importing one
raised `SystemExit` inside `run_module`, which is caught by neither
`except Failure` nor `except Exception` (`runner.py:92-100`), so it propagated
out and **terminated the whole run**.

I reproduced it: a bare `runner.py` printed 4 suite banners and then died —
`Segmentation fault`, exit 139, with **no aggregate line at all**. Four prior
audits reported totals from a runner that could silently stop early — the same
class of error as the documented `grep "FAIL"` false-red, in the *other*
direction. **The segfault was the pre-existing shutdown issue, not the cause**;
the cause was `SystemExit` escaping.

**RESOLVED.** Both suites are now `def test_*`
(`test_physics_timestep.py:69/:91/:103`, `test_los_contract.py:106/:122/:143`)
and a bare `runner.py` reports **`117/117 checks passed across 11 suite(s)`**.
The *susceptibility* remains — `runner.py` still has no `SystemExit` guard —
so keep the 3-line guard as a backstop. §2.1.

**C5 — **RESOLVED: `test_physics_timestep.py` was red, and the reason was a stale
binary, not a code bug.** I first measured **4/8** against correct source; the
numbers and the fix are in §2.2. Re-run against
`WASTELAND_MODULE_DIR=.buildlogs/linkprobe`: **3/3 passed**. **This is the
fifth failure mode and the one most likely to be repeated:** the answer was
*rebuild and re-run*, not *change the code*. Had I "fixed" the
already-correct accumulator I would have broken working physics.

**C6 — a contradiction inside the source file survives.** The block comment at
`Scene.cpp:842` still says *"FRAME-RATE DEPENDENT, DELIBERATELY SO: there is no
fixed-timestep accumulator"*, directly above an implementation that has one.
One-line fix; see §2.2 Route C.

**C7 — **three check counts in the plans are for files that do not exist.**
item-14 cites `test_savegame.py` (57 checks), `test_inventory.py`, and
`test_audio.py` (112 checks). **None is on disk**, in the repo or in the hermes
cache. The real audio coverage is `test_audio_{clip,mixer}.py` = **61**. This is
the plan-cites-a-file-that-was-never-written failure mode, and ~1000 lines of
shipped code ride on it. See §8.

**C8 — **the NB.3/NB.4/NB.5 evidence base lives outside the repository.** Four
suites totalling 1428 checks (1209 + 56 + 101 + 62) sit in the hermes scratch
cache, which **auto-prunes at 24 hours**, while the implementation they cover is
**untracked in git** (§4, ST-1/ST-2). Both halves are true at once.
---

---

## 2. Cross-cutting work — do these before anything else

The two items in this section were **fixed during this work** and are recorded
here as closed, because their lesson is the most transferable thing in this
document. §2.3 remains open and is the only P0 with no headless substitute.

---

### 2.1 ~~Fix `tests/headless/runner.py` so one suite cannot abort the aggregate~~ — **DONE (Route B)**

**Status: the abort no longer happens.** The two script-style suites were
converted to `def test_*` functions (`test_physics_timestep.py:69/:91/:103`,
`test_los_contract.py:106/:122/:143`) and a bare `runner.py` now reports
**`117/117 checks passed across 11 suite(s)` in 0.29s** with no abort and no
lost aggregate line.

**The original defect, for the record.** `runner.py` collects `test_*`
**functions** (`runner.py:74`) and calls them (`:88`). Two suites were
**script-style** — they executed checks at module scope and ended with
`sys.exit(...)`. Importing one raised `SystemExit` inside `run_module`, which is
caught by neither `except Failure` nor `except Exception`
(`runner.py:92-100`), so it propagated out and **terminated the whole run**.

I reproduced it: a bare `runner.py` printed 4 suite banners and then died —
`Segmentation fault`, exit 139, with **no aggregate line at all**. Four prior
audits reported totals from a runner that could silently stop early — the same
class of error as the documented `grep "FAIL"` false-red, in the *other*
direction. **The segfault was the pre-existing shutdown issue, not the cause**;
the cause was `SystemExit` escaping.

**Why Route A's guard still matters.** `runner.py:74` still calls
`importlib.import_module` with no `SystemExit` guard, and `:92-100` still
catches only `Failure` and `Exception`. The *instances* are fixed; the
*susceptibility* is not. The next script-style suite anyone writes reintroduces
a silent early abort.

*Files:* `tests/headless/runner.py`. Wrap `importlib.import_module` at `:74`
and add a `SystemExit` handler alongside the existing `except Exception` at
`:95`, mapping `e.code` (0 → pass, non-zero → failure).
*Test:* assert the aggregate line appears **and** that the suite count equals
`len(glob("test_*.py"))` — today 11 suites, so the line must read
`across 11 suite(s)`.
*Risk:* a suite calling `sys.exit(0)` at module scope while holding `Wasteland`
state has not run its later checks — already true of that suite's design, and
the guard makes it visible rather than silent.
*Effort:* **S** (3 lines). **Do it as a backstop.**

---

### 2.2 ~~Resolve the physics-timestep contradiction~~ — **DONE, VERIFIED**

**Status: closed.** `Scene::StepPhysics` used to clamp dt to 1/30 and discard
the excess, so measured 1.0 s of free fall gave 100% of real time at
144/60/30 fps but only **45% / 25% / 11% at 20 / 15 / 10 fps** — a machine at
10 fps ran in quarter-speed, with no error and a green suite. Fixed with a
bounded accumulator: `kFixedStep = 1/60` (`Scene.cpp:913`), `kMaxSubSteps = 8`
(`:914`), bank-and-spend loop at `:916-929`, solver split into
`Scene::StepPhysicsFixed` (`Scene.cpp:935`, declared `Scene.h:69`). New suite
`tests/headless/test_physics_timestep.py` (3 tests, `:69`, `:91`, `:103`).

**Verified by me, against `WASTELAND_MODULE_DIR=.buildlogs/linkprobe`:**

```
test_physics_timestep   3/3 passed
117/117 checks passed across 11 suite(s) in 0.29s
```

> **This is the worked example for §0.3's fifth failure mode.** My *first*
> headless run reported `test_physics_timestep` **4/8 red** against *correct*
> code:
>
> ```
>   PASS: 144fps (4.914m, 100% of ideal)   PASS: 60fps (4.925m)
>   PASS: 30fps  (4.946m, 101%)            FAIL: 20fps (2.207m, 45%)
>   FAIL: 15fps (1.247m, 25%)              FAIL: 10fps (0.559m, 11%)
>   FAIL: all rates within 0.39m of each other (spread 4.387m)
>   PASS: a 10s stall does not integrate 10s of fall
> ```
>
> The accumulator was in source, but the `.pyd` under test was dated **02:54**
> while `Scene.cpp` was modified at **03:02** — the binary predated the fix by
> eight minutes. The canonical module is **kernel-locked**; `wlpath.py:44-48`
> documents the `WASTELAND_MODULE_DIR` escape. **Acting on that red would have
> broken working physics.** `RC=0`, "the suite is red", and "the module is
> current" are three independent claims and none implies the others. Hence ST-3.

**One residue remains, and it is a live trap for the next reader:**
`Scene.cpp:842` still carries the block comment *"FRAME-RATE DEPENDENT,
DELIBERATELY SO: there is no fixed-timestep accumulator"*, sitting directly
above the implementation that has one. Anyone trusting that comment reaches the
opposite conclusion.

**Route C (the only one still open) — delete or rewrite that comment.**
*Files:* `Scene.cpp:842-847`.
*Test:* none possible — it is a comment; the guard is a human read.
*Risk:* none. *Effort:* **S**. **Do it regardless of anything else in §2.**

---

### 2.2b ~~`_los_blocked` read the wrong Raycast tuple slot~~ — **DONE, VERIFIED**

**Status: closed.** The contract is
`(hit, entity, point, normal, distance, hittable)` — entity is index **1**,
normal is index **3**, hittable is index **5**. The shim read `hit[3]` (a
`Vec3`, never `None`), which made the world-geometry branch **unreachable**, and
it tested `entity is None` where the binding returns a default-constructed
`Entity`. Net effect: `HasLineOfSight` reported a target **directly ahead** as
BLOCKED, and only worked by accident on collider-less targets.

The test mock was the second half of the bug: shaped
`(True, Vec3, Vec3, None, …)` — **agreeing with the bug rather than the
contract**. Both fixed. New suite `tests/headless/test_los_contract.py`
(3 tests, `:106`, `:122`, `:143`) uses **real geometry with Static rigid
bodies** — see risk register **R9** for why that detail is load-bearing.

Also fixed earlier in the session: `HasLineOfSight`'s emitter called `_has_los`
with 3 args after NB.7 widened the helper to 4, so **the node was dead at
runtime** and compiled clean. All 21 leaf emitters are now AST-audited, 0 arity
mismatches.

**What this pair teaches, and why both are in this document.** Neither bug
produced an error — both produced *confident wrong answers*. The LOS one was
invisible because a mock shaped like the bug cannot fail. The timestep one was
invisible because the only test asserted at 30 fps, exactly the clamp boundary.
**A test that agrees with the implementation proves nothing about the
contract.** This is the root cause behind ST-1 (the 1209-check suite is the only
thing that guards this class), COV-1/2/3 (~1000 lines have no guard at all),
and R14/R15.

---

### 2.3 Editor proof for both visual panels — the only P0 with no headless substitute

**What it is.** `VisualBlocksPanel` (60 KB) and `NPCBehaviorPanel` (78 KB) are
BUILT, registered in `DemonCore-Editor.vcxproj` (23 `ClCompile` entries
including `src\Panels\NodeCanvas.cpp`, `src\Panels\NPCBehaviorPanel.cpp`,
`src\Panels\VisualBlocksPanel.cpp`), and wired in `EditorLayer.cpp:824`
(`SetVisible(workflow == Workflow::Visual)`), `:1010` (`OpenGraph`),
`:1159-1161` (`OnImGuiRender`), `:2614` (`OpenTree`). Content-browser dispatch
is at `ContentBrowserPanel.cpp:617` (`.wlbtree`) and `:620` (`.wlblocks`).

**None of that is evidence any of it works.** There is no screenshot of the
current panels on disk: the only `.png` files in the hermes scratch cache are
dated **2026-10-01 16:22–16:41** (`shot1.png`, `shot2.png`, `s_after.png`,
`s_pal.png`, `npc_top.png`, `c_centre.png`, `c_topleft.png`), while
`VisualBlocksPanel.cpp` was last written **2026-10-04 01:32**. The
NPCBehaviorPanel screenshots predate the current panel by three days.

**Why now.** The original NB.6 defect — *"unknown node type" for `Sequence`,
`Invert`, `Wait` on a tree that validates 0/0 in tests* — was **only** ever found
by opening the editor. The fix (self-registering `NodeRegistry::Get()`,
`NodeRegistry.h:176-186`) is BUILT and is the right fix, but it has **never been
seen working in the running editor**. A registry validated only through the one
entry point that initialises it passes no matter how broken the others are, and
that is exactly what happened once already.

**Route A — scripted screenshot capture at 4K against a real asset.**
*Files:* the scratch-only capture scripts already exist (`scratch/shot.py`,
`scratch/snapshot.py`) — **no product code**.
*Symbols:* none in the engine.
*Steps:* launch the editor with a window, open `assets/trees/EnemyChase.wlbtree`
in the NPC Behavior panel, capture; then open
`assets/graphs/DamageFlash.wlblocks` in Visual Blocks, drag a node, drag a
wire, hit Compile, capture the generated Python.
*Verification:* the NPC Behavior panel's error list must read **0 errors, 0
warnings** for a tree that validates clean headlessly. The flow panel must show a
node **actually following the cursor** — canvas hit-testing is the likely silent
bug, because a canvas that draws perfectly and cannot be clicked looks identical
to a working one in a still.
*Risk:* the editor self-closes ~5–20 s after launch in the automation
environment (`OnWindowClose received!`), so capture must happen inside that
window. Every prior NB.6 screenshot was taken this way.
*Effort:* **S/M**.

**Route B — an in-editor self-check that runs on panel open and logs.**
*Files:* `NPCBehaviorPanel.cpp`, `VisualBlocksPanel.cpp`.
*Symbols:* on `OpenTree`/`OpenGraph`, immediately call
`NodeRegistry::Get().All().size()` and the validator, and log both.
*Test:* assert the log line reports a non-zero registry size and 0 findings for
the shipped assets. This catches the **exact** regression class that bit NB.6
without needing a human looking at pixels.
*Risk:* low, and it is the only route that could run in CI.
*Effort:* **S**.

**Route C — a headless ImGui rendering harness** (draw the panel into an
offscreen context and diff against a golden image). *Effort:* **L**. Rejected on
cost, but listed because it is the only route that makes canvas hit-testing a
*regression test* rather than a one-time observation.

**Dependencies.** None for A/B. C depends on a stable offscreen ImGui path that
does not exist.
**Recommendation: A, immediately followed by B.** A answers the question the
user actually cares about; B stops the question recurring.
**Runner-up: B alone**, if no one can spare an editor run — but then say so out
loud, because *"we never looked"* is the honest state and *"it builds"* is not
an answer.

---

---

## 3. Visual Blocks (VB.0–VB.6) — remaining work

**Where the area actually stands.** VB.1–VB.4 are BUILT **and** VERIFIED:
`Graph`, `GraphSerializer`, `NodeRegistry`, `Validator`, `Compiler`, and two
node libraries exist; the flow-graph path is 99/99 and the tree path 54/0; the
1209-check node-library suite is green. VB.5's panel is BUILT but never seen
running (§2.3). VB.0 (vendor `imgui-node-editor`) was **deliberately declined**
— see VB-1. VB.6's trace toggle and AI authoring are MISSING — see VB-4, VB-5.

So the remaining Visual Blocks work is **six small items, none blocking anything
else in this document.**

---

### VB-1 — Decide the canvas dependency question (VB.0)

**What it is.** The plan's VB.0 is "vendor `imgui-node-editor`". It is **not
vendored**: `Wasteland/vendor/` contains Box2D, Box3D, entt, GLAD, GLFW, glm,
imgui, ImGuizmo, nvrhi, pybind11, spdlog, stb_image, yaml-cpp — nothing else.
`premake5.lua` has no `node_editor` reference. A bespoke canvas was written
instead: `NodeCanvas.{h,cpp}`, 13.6 KB + 61 KB, **1493 lines**, registered in
the editor project. `NodeCanvas.h:47` records the decision: *"imgui-node-editor:
the dependency would arrive with its own build-order"* problem.

**Why decide now.** AVB.10 wants a canvas. If the answer stays "bespoke", AVB
builds on `NodeCanvas` — which is a *flow* canvas tied to
`VisualBlocks/Graph.h` and `NodeRegistry.h` in its header (`NodeCanvas.h:8-10`).
That is a real coupling decision: AVB's type system genuinely differs
(`Trigger` vs `Exec`, fannable-to-zero vs ordered). Deciding after AVB has a
canvas means refactoring one of them.

**Route A — keep bespoke; decouple `NodeCanvas` from `VisualBlocks::Graph`.**
*Files:* `NodeCanvas.h/.cpp`.
*Symbols:* replace the includes with a neutral interface (node id, label, pin
list, position, selected, wired-to-set). Panels adapt their `Graph` into it.
*Test:* a headless test building the neutral node list and asserting hit
testing maps a screen point to the right node id — the one property of the
canvas testable **without a window**, and today untested.
*Risk:* medium. `NodeCanvas.cpp` is 61 KB; every consumer adapts. **Do not do
this in the same session as §2.3's UI work.**
*Effort:* **M**.

**Route B — keep bespoke, keep the coupling, reuse as-is for AVB.**
*Files:* none now.
*Buys:* zero new code.
*Costs:* AVB's canvas carries a `VisualBlocks::Graph` dependency it does not
want, and the two graph types stay informally coupled through one component.
*Effort:* **S**, with a known debt.

**Route C — vendor `imgui-node-editor` and migrate both surfaces.**
*Files:* `Wasteland/vendor/imgui-node-editor/`, `premake5.lua` (with the
existing `filter "files:vendor/**.cpp" flags { "NoPCH" }` treatment), both panels.
*Buys:* a maintained canvas with selection boxes, minimap, context menus.
*Costs:* a new dependency and a build-order risk, for a UI whose unverified
parts already work. The plan's mitigation (vendor and compile *first* so a
version incompatibility surfaces in hour one) is weaker now — the bespoke
canvas is already written.
*Effort:* **L** to vendor + migrate; **S** if vendored and left unused.

**Dependencies.** None. AVB-10 depends on the answer.
**Recommendation: Route B now, Route A later if a second canvas is coming.**
Vendoring now to replace working code is expensive with the least payoff.
**Runner-up: A** — the decoupling is cheap insurance and the hit-test test is
the first real test of canvas behaviour this project has.

---

### VB-2 — Flow-graph end-to-end through a live scene

**What it is.** `test_visual_blocks_flow.py:552-599` proves the shipped
`.wlblocks` loads, validates, compiles, `ast.parse`s, and **instantiates** in
Python. It does not prove a flow graph **runs on an entity in a scene**. The
plan's VB.4 step 1 (Rotator) and step 2 (EnemyChase-as-flow-graph) have no
scene-level equivalent — and `Enemy.py` was migrated onto a **tree**
(`assets/scripts/Enemy.py:42-43`, `TREE_PATH = "trees/EnemyChase.wlbtree"`), so
the flow-graph acceptance test the plan claimed is **UNVERIFIED**.

**Why now.** The whole argument for `.wlblocks` is *"the node library is general
enough to express what a hand-written script expresses."* That is currently an
assertion about 200 lines of Python that has only run instantiated, not driven.

**Route A — attach a flow graph to an entity and tick it**, mirroring
`test_tree_attach.py:123` (`test_a_tree_attaches_and_deals_damage`, 5/5 today).
*Files:* none in the engine — `CompileFlow` output already goes beside the graph
(`VisualBlocksPanel.cpp:319`) and `ScriptComponent` already points at a `.py`.
*Test:* author a `.wlblocks` whose generated class advances a transform, attach
it, call `OnUpdateRuntime`, assert the transform changed by the expected amount.
*Verification:* read the value **back off the entity**, not off the component
passed in — components handed to `AddOrReplace*` are **snapshot copies** and
reading `.Current` off the one you passed shows the attach-time value forever.
*Risk:* low. The tree equivalent already passes; this is the best-evidenced
path in the document.
*Effort:* **S**.

**Route B — port `Enemy_legacy_reference.py` to a flow graph as a second
artifact, alongside the tree.**
*Test:* run tree and flow entities from identical spawns for 600 ticks; assert
both deal the same hit count.
*Risk:* the plan's own NB.5 finding applies — the tree and the reference landed
**58 vs 52** hits over 600 ticks and *should not* match exactly. Assert the
invariant, not the reference's number.
*Effort:* **M**.

**Route C — declare it out of scope in the plan.** One line. Legitimate, but it
must be written down or the next audit re-raises it. *Effort:* **S**.

**Dependencies.** None. Independent of AVB.
**Recommendation: Route A.** **Runner-up: B** if the goal is proving the node
library is *general* rather than merely *working*.

---

### VB-3 — The flow palette stops at 15 nodes, none touching the engine

**What it is.** `NodeLibraryFlow.cpp` registers 15 `TypeId`s across
`Flow/Event`, `Flow/Variable`, `Flow/Control`, `Flow/Math`, `Flow/Logic`,
`Flow/Utility`: `FlowOnEvent`, `SetVariable`, `GetVariable`, `Branch`, `While`,
`ForEach`, two loop-generated math nodes (`:455`, `:520`), `Negate`, `Clamp`,
`And`, `Or`, `Not`, `Log`, `Break`.

**Not present:** any engine-facing node. No `GetPosition`, no `SetTransform`, no
`Raycast`, no `PlaySound`, no component access. A flow graph can compute and
branch but **cannot observe or act on the world**. The plan's VB.2 category list
included "Scene"; only Math shipped.

**Route A — add scene/entity nodes one at a time, each with a read-back
assertion.**
*Files:* `NodeLibraryFlow.cpp`.
*Symbols:* `GetPosition`, `SetTransform`, `GetHealth`, `Raycast`, `PlaySound` —
each a registry entry.
*Test:* per node, assert on **what changed**, in a live scene.
*Risk:* the NB.4 trap applies in full — **eight invented APIs** in the first
behaviour shim, every one compiling clean and raising only when an NPC entered
that branch. The dangerous kind was `scene.GetEntities()`, which returns `[]`
for runtime-created entities: not an exception, an **empty result**, so every
consumer is silently blind and the trace looks clean. Use
`GetAllEntitiesWithTransform()`. Run
`cpp-pybind11-bindings/scripts/audit_script_bindings.py` per node.
*Effort:* **M** for a first useful five.

**Route B — document the boundary and stop.** *Effort:* **S**. Justified if the
tree path is the product and flow graphs are a learning surface.

**Route C — generate flow nodes from a table rather than by hand.**
*Buys:* adding a math node becomes one line.
*Costs:* the flow emitter's contract is **opposite** the tree emitter's (real
control flow with body placeholders vs a dict entry describing a topology slot)
— `NodeRegistry.h:290-302` documents this and it is the reason they are separate
translation units. A macro layer hiding that distinction is how the two came to
look interchangeable when they are not. *Effort:* **M**.

**Dependencies.** None.
**Recommendation: Route A, five nodes, then re-evaluate.** `GetPosition` and
`SetTransform` are exactly what §2.3 needs to make a flow graph move something
on screen, so this also unblocks the UI proof.
**Runner-up: B** if the tree path is the product.

---

### VB-4 — Trace toggle (VB.6, part one)

**What it is.** `wl_behavior.py` has the hook: `Tree.set_trace(fn)` at `:717`,
`trace_to_list(tree, sink)` at `:726`, with `enter` / `exit:<result>` / `abort`
events at `:707-715`. **Neither panel has any trace UI** — `grep -i trace`
returns **0 hits** in both `VisualBlocksPanel.cpp` and `NPCBehaviorPanel.cpp`.
The capability is BUILT and unreachable.

**Why now.** A visual language with no per-node trace is unfixable by the people
who have to fix it. Both plans make the argument independently.

**Route A — a trace toggle in `NPCBehaviorPanel` wired to `trace_to_list`.**
*Files:* `NPCBehaviorPanel.h/.cpp`.
*Symbols:* a checkbox; on enable install
`tree.set_trace(lambda node_id, event, t: sink.append((node_id, event)))`; a
scrolling list view of `sink`.
*Test:* none headless — a trace is a *runtime* view. The headless proof is that
`trace_to_list` fires once per node entry and emits the node id, which
`wl_behavior.py`'s contract already states.
*Risk:* the panel never runs anything, by design (§2.3). A trace needs a
**playing** scene, so someone must decide who installs the hook: the panel
(needs a live tree handle) or `BehaviorTreeSystem` (has one, per entity).
*Effort:* **S/M**.

**Route B — expose the trace through Python only, no UI.**
*Files:* `Bindings.cpp` — `BehaviorTreeSystem::SetTrace(bool)` and
`GetTraceLog()`.
*Test:* after 10 ticks the log contains an `enter` event per visited node, in
declaration order.
*Buys:* scriptable, testable headlessly, usable by the AI/agent path.
*Costs:* no human-facing UI, which was the point. *Effort:* **S**.

**Route C — ship neither; rely on Python tracebacks.** **S**. Cheapest, weakest:
a traceback gives the line that raised, not the *path* through the tree.

**Dependencies.** B unblocks nothing and is a prerequisite for headless-testing
A's output.
**Recommendation: B then A.** B is testable, which makes A's UI a display of
something already proven. **Runner-up: A alone**, if only the human matters.

---

### VB-5 — AI authoring of `.wlblocks` / `.wlbtree` (VB.6, part two)

**What it is.** `ToolServer` already exposes `read_file`, `write_file`,
`edit_file`, `list_files` (`ToolServer.cpp:514-556`, schemas at `:417-428`) with
project-relative path confinement (`:78-99`) and asset-root normalisation.
`AIChatPanel` is 49 KB and wired. **Nothing mentions `.wlblocks` or
`.wlbtree`** — 0 grep hits in `ToolServer.cpp` and 0 in `AIChatPanel.cpp`.

An agent can write a `.py` today and cannot author a graph. The plan calls this
*"nearly free once `.wlblocks` is a documented text format"* — half true. The
format is text, but **it is not documented**, and no schema doc exists in
`docs/` (which holds only `AGE_ASSURANCE.md`, `BUILD_ISOLATION.md`,
`NVRHI_SETUP.md`, `RTX_KIT_LICENSES.md`).

**Route A — write the schema doc; the agent path then works for free.**
*Files:* `docs/VISUAL_BLOCKS_SCHEMA.md` — pins, types, node taxonomy, abort
modes, blackboard declarations, worked examples from the three shipped
`.wlbtree` files and the one `.wlblocks`.
*Test:* a test extracting every node type and pin id from the registry and
asserting the doc mentions each, so it cannot rot silently — the same lesson as
the stale-entry check in `test_no_identity_types.py`.
*Risk:* docs rot; that assertion removes the risk. *Effort:* **S**.

**Route B — add graph-aware tools to `ToolServer`.**
*Symbols:* `validate_graph(path)` returning findings verbatim,
`list_graph_nodes(path)`.
*Buys:* the agent gets the validator's own error messages as tool output instead
of guessing. *Costs:* small; the validator is already bound to Python
(`Bindings.cpp:4380-4394`: `Validate`, `ValidateTree`, `ValidateFlow`).
*Effort:* **S**.

**Route C — a headless lint gate on every shipped asset.**
*Test:* glob `assets/**/*.wlbtree` and `*.wlblocks`, assert 0 errors each.
Run against the three shipped trees today — all validate 0 errors / 0 warnings
per the NB.3 record. **This is the cheapest regression guard in the document.**
*Risk:* none. *Effort:* **S**.

**Dependencies.** A blocks nothing; B is only useful after A; C independent.
**Recommendation: A + C together, then B.** C retroactively guards every tree
already on disk. **Runner-up: C alone**, if documentation slips.

---

### VB-6 — Where `.wlblocks` generated output lives

**What it is.** `CompileAndWrite` writes the generated Python **beside the
graph** (`VisualBlocksPanel.cpp:319`, `:323`). `assets/scripts/generated/` **does
not exist**. There is exactly one shipped `.wlblocks`
(`assets/graphs/DamageFlash.wlblocks`) and **no `.wastescene` references it** —
`grep -rn DamageFlash assets/scenes/*.wastescene` returns 0 hits.

**Route A — write to `assets/scripts/generated/` and add it to the asset
mirror.**
*Risk:* the mirror bug from NB.6 is the precedent — a wrong `$(SolutionDir)`
copied **nothing, silently**, masked because other asset folders already
existed. Verify by **deleting the destination and rebuilding**.
*Effort:* **S**.

**Route B — leave it beside the graph and fix the plan's text.** The plan says
`assets/scripts/generated/`. Either the plan or the code must change; do not
leave them disagreeing. *Effort:* **S**.

**Dependencies.** None.
**Recommendation: B.** Chasing a directory rename buys nothing and re-opens a
mirror step that has already bitten once.
**Runner-up: A**, if generated files are ever meant to be excluded from VCS.

---

## 4. Structural rot — the highest-risk finding in this document

> **Prioritised above new features, below user-visible breakage.** The
> NB.3/NB.4/NB.5 evidence base — 1209+ checks — lives **outside the repository**
> in a directory that **auto-prunes at 24 hours**, and the implementation it
> covers is **untracked in git**. Both are true simultaneously, and either alone
> would be serious.

---

### ST-1 — Land the scratch suites in `tests/` before they evaporate

**What it is.** Four suites and their runner live at
`C:\Users\rtoue\AppData\Local\hermes\cache\scratch\`:

| File | Checks | The only proof of |
|---|---|---|
| `test_node_library.py` | **1209** | every NB.4 leaf emitter; contains the systematic shim-arity audit (`:573-643`) that caught the `_has_los` 3-vs-4-arg bug |
| `test_wlbtree_compiler.py` | 56 | NB.3 load/validate/emit, determinism, round-trip |
| `test_visualblocks.py` | 101 | registry, literals, YAML round-trip, every rejection rule, `compile()` of generated Python |
| `test_enemy_tree.py` | 62 | NB.5 end-to-end, 600-tick differential, damage cadence |
| `run_all_suites.py` | — | the aggregate; normalises CRLF, reports a marker-less suite as **UNCOUNTED** |

I re-ran `test_node_library.py` this session: **1209 passed, 0 failed, exit 0.**

**Why this is the highest-risk item here.** Two independent silent failure modes:

1. **The directory prunes at 24 h idle.** When it goes, the evidence base for
   NB.3/NB.4/NB.5 goes with it, and the only remaining record that those phases
   were verified is prose in a plan file. The 1209-check suite is the sole guard
   against a leaf emitter drifting from its shim signature — the exact bug class
   that made `HasLineOfSight` dead at runtime.
2. **A suite run against a stale module reports on the wrong binary.** The
   canonical `.pyd` is kernel-locked; `wlpath.py:44-48` documents the
   `WASTELAND_MODULE_DIR` escape for exactly this. I hit it myself: my first
   headless run reported `test_physics_timestep` **4/8 red** on a *correct*
   accumulator, because the `.pyd` predated `Scene.cpp` by eight minutes.
   **A green suite against the wrong module is as meaningless as a red one.**

**Route A — move the four suites into `tests/visualblocks/`; move the runner to
`tools/`.**
*Files:* new `tests/visualblocks/`; `tools/run_all_suites.py` modelled on
`run_all_suites.py:29-60`; `tools/t.sh` gains a case.
*Symbols:* none in the engine — all four suites already `sys.path`-inject their
own bootstrap.
*Verification:* the suites pass unchanged from their new home with the **same
totals** (1209 / 56 / 101 / 62), 0 failures, 0 UNCOUNTED.
*Risk:* the suites are tuned to their scratch cwd (`test_node_library.py`
compiles generated code in a temp dir; `test_visualblocks.py` locates
`DemonCore-Editor/assets`). Relocating means fixing paths — mechanical, but it
is the work.
*Effort:* **S** (half a day).

**Route B — copy, don't move; keep scratch as a scratch pad.**
*Buys:* zero breakage.
*Costs:* two copies drift. Mitigate by marking the in-repo copy canonical.
*Effort:* **S**.

**Route C — vendor as a subtree/submodule.** The scratch cache is not a
repository; this is Route A with extra steps. *Effort:* **S**, pointless.

**Route D — do nothing.** Acceptable only if NB.3–NB.5 never regress. Given
`test_node_library.py` caught a real dead-node bug *within the last day*, that
intent is not credible.

**Dependencies.** None. **Do this before any NB work.**
**Recommendation: Route A** — the only route that puts the evidence base under
version control, which is the entire point.
**Runner-up: B**, if a relocation would fight path assumptions today — but then
A becomes mandatory before the next person touches NB.

---

### ST-2 — `git add` the implementation, which is entirely untracked

**What it is.** `git status --short` shows the whole NB/VisualBlocks/navigation
stack untracked (`??`) or heavily modified:

```
?? DemonCore-Editor/assets/scripts/wl_behavior.py
?? DemonCore-Editor/assets/trees/
?? DemonCore-Editor/src/Panels/NPCBehaviorPanel.{h,cpp}
?? DemonCore-Editor/src/Panels/NodeCanvas.{h,cpp}
?? DemonCore-Editor/src/Panels/GraphHistory.{h,cpp}
?? DemonCore-Editor/src/Panels/ValueEditor.{h,cpp}
 M Wasteland/src/Wasteland/Scene/Scene.cpp   (+2431 / -85)
```

`VisualBlocks/`, `BehaviorTreeSystem.*`, `Navigation/` are untracked or
substantially rewritten. **None of this is in git.**

**Why now — this is a correctness hazard, not hygiene.** There is a documented
instance: *"That external build had reverted part of my work. `git status`
aside, the check that mattered: grep the source for the change. The
`OnComponentAdded<BehaviorTreeComponent>` specialization was gone from
`Scene.cpp` — the only casualty, causing the final `LNK2019`. Re-added and
re-verified."* **An untracked file has no history, so a revert is invisible to
every `git`-based check.** Work has already been lost this exact way.

**Route A — stage and commit in dependency order, with a build in between.**
*Steps:* (1) engine sources → build → run suites → commit. (2) editor panels →
build editor → commit. (3) tests → run → commit. (4) assets and plans.
*Verification:* a **clean checkout at each commit builds and passes**. That is
the only test that means anything here. Concretely,
`git ls-files VisualBlocks/ BehaviorTreeSystem Navigation` must list them.
*Risk:* the working tree has unrelated in-flight renderer edits. **Do not commit
`Renderer3D.*`, `DX12Nova.*` or `NVRHIContext.cpp`** — the user is actively
editing the renderer. **Stage by explicit path, never `git add -A`.**
*Effort:* **S/M**.

**Route B — one commit, everything.** *Effort:* **S**. Cheaper; makes bisecting
a regression impossible.

**Route C — branch first, commit on the branch.** Protects the in-flight
renderer work from a stray commit. *Effort:* **S**.

**Dependencies.** ST-1 first — committing tests against code nobody can retrieve
buys nothing.
**Recommendation: A**, with C's safety if the renderer work is genuinely
in-flight. **Runner-up: C**, which is really "A, on a branch".

---

### ST-3 — `t.sh` does not report the module under test

**What it is.** `tools/t.sh` sets `PY314` and dispatches to runners but sets
**no `WASTELAND_MODULE_DIR`**. `wlpath.py:44-48` prefers it when present,
otherwise globbing `bin/*/DemonCore-Editor` and preferring Release. Because the
canonical `.pyd` is kernel-locked, default resolution lands on a **stale**
module.

**Why now.** I hit this. My first headless run reported `test_physics_timestep`
**4/8 red** against correct code. The correct response was *rebuild*, not
*change code* — and had I "fixed" the already-correct accumulator I would have
made it wrong.

**Route A — print the resolved module dir and its mtime on every run.**
*Files:* `tools/t.sh`, `tests/headless/wlpath.py`. `wlpath.setup()` already
returns the chosen directory (`wlpath.py:64-99`); surface its mtime too.
*Test:* the banner shows a path and timestamp; a deliberately stale override is
visible. *Risk:* none. *Effort:* **S**.

**Route B — hard-fail when the module is older than the newest engine source.**
*Files:* `wlpath.py`.
*Test:* `touch` a source, run, assert non-zero exit with a clear message.
*Buys:* makes "green against the wrong binary" impossible.
*Risk:* false positives mid-build; gate on mtime only when no build is running.
*Effort:* **S/M**.

**Route C — CI that builds and tests in one job.** *Effort:* **M**. Correct
long-term; no CI exists today.

**Recommendation: A now, B next.** A is free and would have saved me the false
red. **Runner-up: B**, which is the actual guarantee.

---

### ST-4 — Test counts in the plans are unreliable in both directions

**What it is.** The four plans quote a history of totals — 673, 1011, 1143,
1300, 1456, 1518, 1209 — and the NB plan records why: *"the shell one-liner
under-reported by **157 checks** and still printed '0 failed suites' … so the
numbers previously reported for this project were **low**, and one figure I
quoted (1243 across 29 suites) never existed."* Three more are fiction of a
different kind:

- item-14 cites `test_savegame.py`, **57 checks** — **the file does not exist**
- item-14 cites `test_audio.py`, **112 checks** — **the file does not exist**
  (real: `test_audio_{clip,mixer}.py` = 61)
- item-14 cites `test_inventory.py` — **does not exist**

**Route A — one canonical counter, one place.** Promote `run_all_suites.py`
into `tools/` and extend it to cover `tests/`, `tests/headless/` and the
visualblocks suites in one pass.
*Verification:* the aggregate equals the sum of the per-suite lines, checked by
hand once. **A total must move when a suite is added, and only then** — a flat
count while suites are added is the failure to assert against.
*Effort:* **S**.

**Route B — delete the historical totals, keep one dated current figure.**
*Effort:* **S**. Cheapest honest option.

**Route C — cite every total with its runner and date inline.** *Effort:* **S**.
Best long-term, highest writing cost.

**Recommendation: A, plus B for the fictional figures specifically.** Writing
"57 checks" for a file that does not exist is the exact failure this repo keeps
making — a claim that reads as evidence and is not.
**Runner-up: C.**

---

### ST-5 — `Wasteland.lib` stub shadowing (build-graph, recurs)

**What it is.** `bin/Release-windows-x86_64/DemonCore-Editor/Wasteland.lib` is a
393 KB **git-tracked placeholder with zero objects**, shadowing the engine's
real 163 MB lib in `bin/Release-windows-x86_64/Wasteland/`. Building
`WastelandPython.vcxproj` alone resolves the stub, so **new engine symbols link
as unresolved externals even though the engine compiled them** — surfacing as
`LNK2019` for symbols that visibly exist.

**Route A — build the engine first (or the editor, which pulls the reference);
the module now emits `WastelandPython.lib`.** Document the build order.
*Verification:* a new engine symbol used by the module links without `LNK2019`.
*Effort:* **S**.

**Route B — delete the stub from git.** *Effort:* **S**, and something may
reference it as a link input — needs care.

**Recommendation: A.** B risks a confusing failure for a problem a documented
build order already solves.

---

## 5. Item 14 — remaining phases

### 5.1 2D physics: late-spawned bodies never simulate (14.1)

**What it is.** 3D was fixed: `Scene::SyncPendingPhysicsBodies`
(`Scene.cpp:808`) scans `m_Registry.view<RigidBody3DComponent>()` each step and
creates any body whose `RuntimeBody == 0`. **2D has no equivalent**, and the
engine says so in its own source at `Scene.cpp:790-806`:

> *"NO 2D EQUIVALENT. There is no SyncPendingPhysics2D, and
> `Rigidbody2DComponent::RuntimeBody` is assigned in exactly one place in the
> whole engine — inside `OnPhysics2DStart`. So a 2D rigidbody added after
> startup gets no Box2D body EVER… The symptom is a 2D body that never falls,
> never collides, and never blocks anything, with no error. This is the exact
> bug the comment above describes for 3D — diagnosed, fixed for 3D, and never
> mirrored to 2D."*

A 2D dropped item or projectile hangs in mid-air, fully non-interactive.

**Route A — mirror `SyncPendingPhysicsBodies` for 2D.**
*Files:* `Scene.h/.cpp`; `CreatePhysicsBody2D(entt::entity)` alongside the
existing `CreatePhysicsBody3D` (`Scene.cpp:1374`).
*Symbols:* `SyncPendingPhysics2D()`, called from `StepPhysics` beside the 3D call.
*Test:* a 2D dynamic body added after `OnSimulationStart()` **falls**; assert
`y` decreases over N ticks. The control is the 3D late-spawn test that passes.
*Risk:* Box2D's world lifecycle may differ enough that a straight mirror is
wrong — read `OnPhysics2DStart` first.
*Effort:* **S**.

**Route B — delete the 2D comment and declare 2D unsupported.** *Effort:* **S**.
Honest, but loses a working feature's usefulness.

**Recommendation: A.** The 3D fix is a known-good pattern 400 lines away in the
same file. **Runner-up: B**, if 2D is genuinely dead and should be removed.

---

### 5.2 apply-impulse / set-velocity are absent (14.1)

**What it is.** Zero hits for `ApplyImpulse|SetVelocity|AddForce` across
`Wasteland/src/`. Scripts can **read** physics; they cannot **drive** it.
Nothing can jump, knock back, or launch.

**The trap, stated plainly: `RuntimeBody` IS bound, and exposing it is not a
fix.** It is a raw process-local Box3D handle. Handing it to Python invites a
stale handle after `OnPhysics3DStop()` — a use-after-free, not an API.

**Route A — typed engine methods that resolve the body from the entity.**
*Files:* `Scene.h/.cpp`, `Bindings.cpp`.
*Symbols:* `Scene::ApplyImpulse(Entity, Vec3, bool wake)`,
`Scene::SetLinearVelocity(Entity, Vec3)`, `Scene::GetLinearVelocity(Entity,
Vec3&)`; each resolves `RigidBody3DComponent::RuntimeBody` internally and
returns `false` when the handle is 0 or the world is invalid.
*Test:* a suspended body (`GravityScale = 0`) given an upward impulse **rises**;
`SetLinearVelocity(0)` stops a falling body within tolerance. Assert on
**position read back off the entity** — components passed to `AddOrReplace*` are
snapshot copies.
*Risk:* low; the `b3World_IsValid` guard pattern exists at `Scene.cpp:809`.
*Effort:* **S**.

**Route B — expose the Box3D handle and accept the hazard.** **S**. **Do not do
this** — see the trap above and §12.

**Route C — add a `PhysicsMotorComponent` driving forces declaratively.**
*Buys:* authoring-friendly, no script needed. *Costs:* a new component, a new
serialization path, more surface than the problem needs. *Effort:* **M**.

**Recommendation: A**, with `wake` on the impulse — a body whose solver is
asleep ignores force otherwise, and the failure is silent.
**Runner-up: C** if the intent is designer-authored physics.

---

### 5.3 `Scene::DropItem` drops nothing (14.2)

**What it is.** `Scene.cpp:2077-2084` in full:

```cpp
int Scene::DropItem(Entity entity, const std::string &itemId, int quantity)
{
    // Dropping removes from the inventory; spawning the world entity is
    // the caller's job, because it needs the prefab and the drop offset,
    // and guessing here would create entities with no way to configure them.
    return RemoveItem(entity, itemId, quantity);
}
```

The comment is defensible in isolation, but **the name promises a drop and the
body performs a remove.** A caller who reads the name spawns nothing and the
item is simply gone. It is bound to Python (`Bindings.cpp:4004-4006`), so the
footgun is reachable from a script.

**Route A — rename to `RemoveItemFromInventory` and add a real `DropItem`.**
*Symbols:* `DropItem(Entity, itemId, quantity, spawnPrefab, offset)` returning
the spawned `Entity`; the existing body becomes a private helper.
*Test:* drop → inventory decreases **and** a world entity exists with the item's
definition; pick it back up → count restores.
*Risk:* needs a prefab concept; `ItemDefinitionComponent` has a `Name` but no
prefab path. *Effort:* **M**.

**Route B — rename only; add a `DropAt` helper taking an explicit spawn
callback.** *Effort:* **S**. Removes the lie immediately.

**Route C — leave it, document it.** *Effort:* **S**. Acceptable only if the docs
say plainly that `DropItem` spawns no entity.

**Recommendation: B immediately** (a rename is cheap and the current name is
actively misleading), then A.
**Runner-up: A** if items are actually dropped in content today.

---

### 5.4 `OnInventoryChanged` does not exist (14.2)

**What it is.** Zero hits across `Wasteland/src/`. The plan calls for change
events "so the HUD and interaction prompts update without polling".

**Route A — a `Scene::OnInventoryChanged` event carrying entity + item id.**
*Files:* `Events/` (already holds `Event.h`, `ApplicationEvent.h`, `KeyEvent.h`,
`MouseEvent.h`), `Scene.cpp`, `Bindings.cpp`.
*Test:* add then remove → fires twice with the right item id; a partial add
**refused by capacity** must **not** fire.
*Effort:* **S/M**.

**Route B — poll from the HUD.** *Effort:* **S**. Fine at this entity count; the
plan's objection was aesthetic.

**Recommendation: A**, but only when a HUD exists. **Runner-up: B** — a polling
HUD is not a bug at ten entities.

---

### 5.5 `.wastescene` audio-source serialization is absent (14.4)

**What it is.** `grep -rn AudioSourceComponent SceneSerializer.cpp` returns
**0 hits**, while `InventoryComponent` and `ItemDefinitionComponent` are both
handled (`:766-787` write, `:1184-1209` read). So clips load by path at runtime
and a saved scene **carries no sound set** — reopening a level loses every
audio source on it.

**Route A — serialize `AudioSourceComponent` and `AudioListenerComponent`
alongside the existing gameplay components.**
*Files:* `SceneSerializer.cpp`, mirroring the `InventoryComponent` pattern at
`:766-787` / `:1184-1209`.
*Test:* save a scene with a source → reload → assert `ClipName`, `Volume`,
`Pitch`, `Loop`, `Spatial`, distances and `PositionOffset` all survive exactly;
a second save is byte-identical to the first.
*Risk:* `PlayOnStart` should probably **not** round-trip blindly — decide
whether reload replays autostart sounds.
*Effort:* **S/M**.

**Route B — resolve clip paths at load time from an asset reference.** *M*.
Better long-term (renaming a WAV stops breaking saves), more work.

**Recommendation: A.** Matches the existing pattern exactly.
**Runner-up: B**, if asset renaming is a real concern.

---

### 5.6 Terrain and streaming (14.6) — greenfield

**What it is.** Nothing exists. The only `Heightfield`/`Terrain` hits in
`Wasteland/src` are water-simulation comments (`Components.h:1422`, `:1780`).
Heightfield, splatting, chunking, LOD rings, async streaming, persistence: all
absent.

**Route A — heightfield terrain on a fixed grid, no streaming.**
*Files:* new `Wasteland/src/Wasteland/Terrain/`.
*Symbols:* `TerrainChunk`, `TerrainSystem` (noise generation, `SampleHeight`,
splat by slope/height), a `TerrainComponent`.
*Test:* sample height at a known point equals the analytic value; a slope query
returns the authored gradient; **and the critical negative** — a point outside
the terrain returns a documented sentinel rather than a plausible wrong number.
*Effort:* **L**.

**Route B — add chunk streaming on top of A.** *Buys:* unbounded world. *Costs:*
async load budgets, unload policy, a persistence decision (*"do not bake terrain
into saves"*). *Effort:* **L**.

**Route C — read terrain from an external heightmap.** *Effort:* **M**. Cheapest
route to something visible; no procedural authoring.

**Recommendation: A, and only when the arena blockout needs more floor.** The
plan's own reality check is right: largest effort, least needed.
**Runner-up: C**, if the need is a specific landscape rather than a system.

---

## 6. Auditory Visual Blocks (AVB.0–AVB.11)

**Where this area stands: AVB.0 is DONE (corrected), AVB.1 is the gate, and
nothing after it exists.** `grep -rn 'Avb\|wlavb\|wltrack'` across
`Wasteland/` and `premake5.lua` returns **0 hits**. Ten of twelve phases are
greenfield. This section does not restate the plan's 60 KB of design; it maps
routes for the parts that are **not** "execute the plan".

---

### AVB-0 — ~~Rebuild the test harness~~ — **DONE, corrected**

**The plan's §0.4 premise is false.** It claims *"there is no `test_audio.py`
and no Python test suite anywhere in the repo"*. True when written; today
`tests/headless/` exists with `runner.py` + `wlpath.py`, and audio is
**`test_audio_clip.py` (35) + `test_audio_mixer.py` (26) = 61 checks, all
green** — I re-ran both. The plan's "112 checks in `test_audio.py`" refers to a
file that **does not exist**; the real number is **61**.

**Effort: S (effectively zero).** Correct the plan's figures and proceed.

---

### AVB-1a — The mixer has no output sample rate — the AVB.1 contradiction

**The most interesting problem in the area. Decide it before AVB.1 is written.**

**The problem.** `grep -n 'SampleRate|sample_rate|m_Rate|GetRate' Mixer.h`
returns **nothing**. The mixer has **no concept of an output rate at all**.
Voices advance by the **clip's own rate**: `AdvanceVoice` does
`voice.FramePosition += frames * (double)voice.Pitch` (`Mixer.cpp:642`), where
`frames` is "already seconds * sample rate" (`:640`); the comment at `:37-38`
confirms the mixer never touches the rate.

So a 48 kHz clip played to a 44.1 kHz device plays **8.8% sharp, silently** —
no error, no log, and no test that could catch it, because there is no rate to
compare against.

**Why it must be decided first.** The plan states twice that `Audio::Mixer` and
`Audio::AudioSystem` are **not modified** (§7, §1.1). That promise is now
**unkeepable**: a correct device backend must know the device rate, and the
mixer must convert. Either the promise changes, or the device resamples, or
clips are pre-converted.

**Route A — give the mixer a device rate and resample on output. Breaks the
"don't modify the mixer" promise.**
*Files:* `Mixer.h/.cpp`, `AudioSystem.h/.cpp`, `Bindings.cpp`.
*Symbols:* `Mixer::SetOutputRate(int)`, `GetOutputRate()`; `AdvanceVoice` steps
by `clipRate / deviceRate * pitch`.
*Test:* a 48 kHz sine through a mixer set to 44.1 kHz yields **exactly** the
same zero-crossing count as at 48 kHz — assert the **measured frequency**, not
the buffer length.
*Buys:* correct everywhere, one resampler, device-rate agnostic.
*Costs:* a resampler in the mix path; touches the one class the plan promised
not to touch; all 61 existing audio checks must still pass (they should — none
assert pitch across rates).
*Effort:* **M**.

**Route B — the device does the conversion. Mixer untouched.**
*Files:* new `Audio/AudioDevice.{h,cpp}` only.
*Symbols:* a resampler in the device pull loop.
*Test:* end-to-end — render 1 s of a 48 kHz tone through the device path at
44.1 kHz and assert the output frequency.
*Buys:* **the plan's promise holds**; the mixer stays exactly as tested.
*Costs:* a second resampler once AVB needs its own; a device-layer bug is harder
to test headlessly than a mixer-layer one.
*Effort:* **M**.

**Route C — load every clip at the device rate.**
*Files:* `AudioClip` load path; `AudioSystem::LoadClip` resamples on load.
*Test:* load a 48 kHz clip while the device is at 44.1 kHz; assert
`clip->GetSampleRate() == 44100` and that a 440 Hz tone still measures 440 Hz.
*Buys:* no resampler in the hot path; mixer untouched.
*Costs:* memory for long music tracks; a device-rate change invalidates every
loaded clip. *Effort:* **M**.

**Route D — document the limitation; mandate 48 kHz assets.** *Effort:* **S**.
Zero code, honest, and it fails the moment someone ships a 44.1 kHz asset —
which is what most tools produce by default.

**Dependencies.** Blocks AVB-1 entirely. Also interacts with AVB.7: a
block-based graph derives its block size *from* the rate, so a device opening at
44.1 kHz changes the block size and therefore its sample accuracy.
**Verification.** As Route A/B/C. Never "it sounds right".
**Recommendation: Route B, with A as the eventual destination.** B honours the
plan's explicit promise, keeps all 61 green audio checks untouched, and the
resampler it needs is the one AVB.7 needs anyway.
**Runner-up: C**, if memory pressure on long music tracks dominates — unknowable
until AVB.8 exists.

---

### AVB-1b — `SetMasterVolume` has no upper bound (pre-existing defect)

**What it is.** `Mixer.h:247`:

```cpp
void SetMasterVolume(float volume) { m_MasterVolume = volume < 0.0f ? 0.0f : volume; }
```

The header's own comment at `:240-246` labels this a **DEFECT**: *"the negative
side is clamped to 0, but there is no upper bound… a runaway
`SetMasterVolume(50.0f)` is accepted and multiplies every sample by 50. If unity
is meant to be the ceiling this needs a matching `min(volume, 1.0f)`."*
Documented, never fixed. Reachable from Python via `Scene` and `AudioSystem`
(`AudioSystem.h:76`).

**Route A — clamp to `[0,1]`.**
*Files:* `Mixer.h:247`.
*Test:* `SetMasterVolume(50.0)` then `Mix()` a known clip; assert the peak. **Note
`Mix()` clamps at the end of each voice write, so an unclamped 50× saturates
rather than exceeding ±1 — assert on a mid-level signal or the test passes
either way.**
*Effort:* **S**.

**Route B — clamp to `[0,4]` for genuine headroom.** *Buys:* +12 dB of boost.
*Costs:* still arbitrary. *Effort:* **S**.

**Route C — leave it, document it.** *Effort:* **S**.

**Recommendation: A**, plus rewrite the comment to say the clamp is intentional.
**Runner-up: B** if a mixer setting will ever expose boost.
This is a **one-character-class change with no dependency on anything** in this
document — the cheapest honest defect fix available, included because the header
already documents the intent and nobody acted on it.

---

### AVB-1c — Audio components are not bindable (real breakage)

**What it is.** `AudioSourceComponent` (`Components.h:2486`) and
`AudioListenerComponent` have `py::class_` registrations but **no
`BIND_GAMEPLAY_COMP`** — I grepped `Bindings.cpp` for both and got **0 hits**,
while every gameplay component has one (e.g. `InventoryComponent` at `:3792`).

**Consequence:** `AddOrReplaceAudioSourceComponent` **does not exist** from
Python. So `Scene::PlayEntitySound` (`Scene.h:274`) and
`AudioSystem::PlayAutoStartSources` (`AudioSystem.h:81`) are **unreachable from
any script** — you cannot place an audio source on an entity you create, so the
entire spatial-audio path is dead to gameplay.

**Why now.** This is user-visible breakage, not a missing feature: the mixer is
complete and tested (61 checks) and there is **no way for a game to use it**
through the component meant to carry the sound.

**Route A — add the two `BIND_GAMEPLAY_COMP` calls to the existing Scene
registration.**
*Files:* `Bindings.cpp`, in the block around `:3792`.
*Symbols:* `AddOrReplaceAudioSourceComponent`, `AddOrReplaceAudioListenerComponent`,
plus HAS/GET/TRYGET/ADD/ADDOR/REM via the macro.
*Test:* create an entity, `AddOrReplaceAudioSourceComponent` with a `ClipName`,
call `PlayEntitySound` → voice id **>= 0**. Then with **no** component → **-1**,
and does **not raise**.
*Risk:* read `Bindings.cpp:3819` — *"UsePathfinding on must degrade to direct
movement instead of…"*. The Scene class has exactly **one** binding and methods
must be added to it; re-registering `py::class_<Scene>` mid-file is a pybind
error, and it has been hit before.
*Effort:* **S**.

**Route B — bind the source via a scene-level factory only.** *S*, and it pushes
work onto every caller. Worse.

**Dependencies.** None. Independent of AVB entirely — fixable **today**, and it
unblocks COV-3's most important assertion.
**Recommendation: A, immediately.** One line each to a defect that makes a
tested subsystem unusable — the highest value-per-minute item in this document.
**Runner-up: B** is not really an option.

---

### AVB-1d — No audio assets exist, so there is nothing to hear

**What it is.** No footstep, weapon, hit, or UI assets. Even with a device
backend the blockout makes no sound.

**Route A — generate placeholder WAVs procedurally.** `tests/headless/wavbuild.py`
already exists (7.8 KB) and builds WAVs.
*Effort:* **S**, and it gives the device backend something to play.
**Recommendation: A when AVB.1 starts**, not before — there is no device yet.

---

### AVB-1 — the device backend itself (the gate)

**The gate.** Zero hits for every backend across `Wasteland/src/`: miniaudio,
WASAPI, IAudioClient, SDL_Audio, OpenAL, FMOD, RtAudio, waveOut, WinMM, mmio.

**Route A — vendor miniaudio.** *Files:* `Wasteland/vendor/miniaudio/`,
`premake5.lua` with the `filter "files:vendor/**.cpp" flags { "NoPCH" }`
treatment; `Audio/AudioDevice.{h,cpp}`.
*Buys:* WASAPI + capture in one, cross-platform, well-trodden.
*Costs:* a third-party dependency and a build-order risk; compile it **first,
alone**, Release and Debug, exactly as the plan says.
*Test:* assert on the **actual** buffer rate a device opens at — a device that
opens at 44.1 kHz when 48 kHz was assumed is a resampling bug that sounds like
pitch drift. Interacts with AVB-1a.
*Effort:* **M**.

**Route B — hand-rolled WASAPI.** **L**, Windows-only.
**Route C — waveOut.** **M**, worse API, no capture.
**Route D — SDL.** **M**, another vendored dependency.

**Recommendation: A**, compiled alone first. **Runner-up: B** only if a vendored
audio library's licence becomes a question — real here, since the repo has
already recorded that the NVIDIA RTX Kit is licence-incompatible with an
open-source Wasteland (`docs/RTX_KIT_LICENSES.md`).

---

### AVB-7 / AVB-10 — two plan premises that are now wrong

**AVB-10's canvas premise is obsolete.** The plan says vendor
`imgui-node-editor`. The repo has a **hand-written 1493-line `NodeCanvas.{h,cpp}`**
already reused by `VisualBlocksPanel` and registered in the editor project.
**Route: build AVB's panel on `NodeCanvas`.** The coupling (its header includes
`VisualBlocks/Graph.h` and `NodeRegistry.h`) is exactly **VB-1 Route A**; doing
that decoupling first makes AVB-10 straightforward.
*Effort:* **S** once VB-1 Route A is done; **M** if not.

**AVB.0's placement is wrong in the phase order.** The plan puts the device
first and gates everything on it — but the harness already exists and the device
is the riskiest item, while **the DSP primitives are testable with no device at
all**. Reordering to **AVB.2 before AVB.1** front-loads value and de-risks the
gate. *Recommendation:* reorder. **Effort:** zero — it is a sequencing change.

---

### AVB-2 through AVB-11 — routes at a glance

| Item | Routes | Effort |
|---|---|---|
| **AVB.2** DSP primitives | **A** as specified (`AvbDsp.h/.cpp`), each asserted on real buffers. **B** vendor a DSP library (SignalSmith / HiFiLibrary) — removes the work, adds a dependency and a licence question. | A **M**, B **S** + dep |
| **AVB.3** modal/comb/grain | **A** as specified. **B** render offline to PCM at build time and ship buffers. The plan itself recommends pre-baking gameplay-critical presets (*"Any runtime noise must come from a seeded PRNG"*) — take it. | A **M**, B **S** |
| **AVB.4** graph/compiler | **A** as specified, **separate** registry/validator/compiler/format from `.wlblocks`. **B** reuse VisualBlocks' and add a `Trigger` pin type — **do not**; §1.2's argument is right that the type systems differ (ordered `Exec` vs sample-accurate `Trigger`, fannable to zero). | A **L** |
| **AVB.5/6** buses, reverb | **A** as specified (buses **before** reverb — §0.2's argument is correct and Epic's architecture agrees). **B** graph reverb first — the plan names this the wrong turn. | **L** |
| **AVB.7** runtime + triggers | **A** as specified. Write the *"trigger on frame 137 starts at exactly 137"* test **first**, before any other runtime test — it is the one that proves sample accuracy. | **L** |
| **AVB.8** DoomArray | **A** as specified. **B** a simpler two-branch version. The four-branch structure is the *sourced* part; do not skip it and still call it homage. | **M** |
| **AVB.9** track editor | **A** as specified. **B** import-audio-file-as-a-take first — the plan's own open question 1 (*"the brief says 'record tracks', which reads as capture"*) makes this a real ambiguity to resolve before building capture. | **L** |
| **AVB.11** record + stems | **A** as specified. **Blocker, verified: there is no WAV writer** — `grep 'SaveWav\|WriteWav\|WriteWave\|SaveWave'` returns **0 hits**. The engine has a WAV *reader* and no writer, and the plan says so. Add `AudioClip::SaveWav`; test by round-tripping against `LoadWav`. | **M** |

**AVB open question worth answering early:** *"Does `BpmToSeconds` feed
`Trigger Repeat` at block rate or sample rate?"* Epic's docs do not say. If it
is block-rate, a 16th note lands up to 480 frames late and the grid audibly
drifts. **Assert the actual rate; do not assume it** — and it interacts with
AVB-1a, since a device-rate change alters the block size.

---

## 7. NPC behavioural — remaining work (NB.6–NB.8)

**Where it stands.** NB.0–NB.5, NB.7 and NB.8's `UsePathfinding` pin are all
**BUILT**. The navmesh is real: `NavMesh.h/.cpp` with `IsGround` (`.cpp:114`),
`Bake`, `FindPath`, `PullString`; `Scene::BakeNavMesh`/`GetNavMesh`/`ClearNavMesh`
(`Scene.h:112-126`); `MoveTo` gains `UsePathfinding` (`NodeLibrary.cpp:368`);
the shim has `_agent_key`/`_navmesh`/`_path_to` (`Compiler.cpp:338/360/378`).
Verified: `test_navmesh 10/10`, `test_pathfinding_node 6/6`,
`test_shim_pathfinding 7/7`, `test_perception_cache 13/13`, `test_tree_attach`
5/5, `test_enemy_migration` 23 checks.

Four items remain. Three are small; one is a shipped-broken node.

---

### NB-9 — `SetTimer` is in `RuntimeTypes()` but no `NodeDef` registers it

**The highest-priority defect in the area: a tree naming it fails to load.**

**Verified.** `BehaviorTree.cpp:45-49` lists `"SetTimer"` and `"Wait"` in
`RuntimeTypes()`. `wl_behavior.py:339` dispatches to `_eval_set_timer`, defined
at `:627`. `NodeLibraryV1.cpp:264` mentions it **only in a comment**. No
`def.TypeId = "SetTimer"` exists in any library file.

The failure is the worst kind for a designer: the node is in the plan's v1
decorator table, the compiler accepts the tree, the runtime has a handler — and
loading fails with **"unknown node type 'SetTimer'"**.

**Route A — register it as a leaf.**
*Files:* `NodeLibraryV1.cpp`.
*Symbols:* `def.TypeId = "SetTimer"` with pins for the blackboard key and
`Seconds`; the emitter writes `self._tree.node_states[...]` per the existing
decorator pattern at `:264`.
*Test:* a tree using `SetTimer` **loads**, and after `Seconds` the blackboard key
reads `now + seconds`. Assert the **value**, not merely that it loaded.
*Risk:* the deadline must be stamped at suspension, not re-derived each visit —
the exact bug `Wait` had (`BehaviorTree.cpp:45-49` records it).
*Effort:* **S**.

**Route B — remove it from `RuntimeTypes()`.** **S**. Honest but removes a node
the plan promised and the runtime supports. **Not recommended.**

**Route C — add a cross-check test that fails when the two lists diverge.**
*Files:* a suite reading `BehaviorTree::RuntimeTypes()` from Python
(`Bindings.cpp:4445` exposes it) and asserting every name resolves via
`NodeRegistry::Find`.
*Buys:* the **general** fix — catches the next node added to one list and not
the other, applying NB.3's own lesson (*"adding a primitive to one side and not
the other fails as a test rather than as an NPC that silently never acts"*)
permanently.
*Effort:* **S**, and it must ship with **every** node addition.

**Recommendation: A + C together.** A fixes the shipped tree; C stops it
happening. A alone leaves the trap armed.
**Runner-up: C alone**, if no shipped tree needs `SetTimer`.

---

### NB-10 — `FollowPath`, local avoidance and separation are absent

**What it is.** `grep -rn FollowPath` across `Wasteland/src`,
`DemonCore-Editor/src` and `assets` returns **0 hits**. No avoidance, no
separation — `grep -inE 'avoid|separat|RVO' Compiler.cpp` finds only prose.

**Route A — add a `FollowPath` task reusing the existing path shim.**
*Files:* `NodeLibraryV1.cpp`.
*Symbols:* a `FollowPath` def whose body calls the `_path_to` machinery already
at `Compiler.cpp:378` — the waypoint advance, drift check and re-plan logic
(`:473-526`) is written once and reusable.
*Test:* with a wall between start and goal, `FollowPath` routes **around** it;
assert the path never crosses the wall's own x at the wall's own y. **Do not**
assert on path length — a detour clipping a corner is *shorter* than the straight
line, and that assertion is one of the four that passed while proving nothing.
*Effort:* **S/M**.

**Route B — separation only, as a steering nudge in the shim.**
*Files:* `Compiler.cpp`'s shim.
*Test:* two agents given the same goal **end at different positions**; assert
the **minimum separation between them at the end**, not either one's exact
coordinate.
*Risk:* O(n²) at scale; the perception cache (`Compiler.cpp:650`) is the
existing pattern for sharing per-scene scans.
*Effort:* **M**.

**Route C — full RVO.** **Do not.** The plan puts "simple separation before any
full RVO", and no roster is large enough to need it.

**Dependencies.** A on nothing (the navmesh is done).
**Recommendation: A, then B if agents visibly stack.**
**Runner-up: B alone**, if the symptom is crowding rather than a missing node.

---

### NB-11 — NB.7 hearing is unimplemented

**What it is.** The plan's NB.7 lists *"Hearing: radius, scaled by an event's
loudness pin."* Zero loudness or hearing references exist anywhere in
`VisualBlocks/`. Vision (cone + range + `Raycast` LOS) **is** implemented and
verified 13/13.

**Route A — a `Hear` node plus an `EmitSound` loudness source.**
*Files:* `NodeLibraryV1.cpp`, `Compiler.cpp` shim.
*Test:* an event emitted at radius R is perceived at 0.5R and **not** at 2R;
assert on whether the blackboard key was **set**, not on a value — the
`bb.set(key, None)` bug class means validity is tracked separately from value.
*Effort:* **M**.

**Route B — declare hearing out of scope.** **S**. Legitimate: vision is
implemented and tested; hearing has no consumer today.

**Recommendation: B now, A when a stealth mechanic needs it.** Writing an unused
perception channel is the EQS-shaped trap the plan itself warns about.
**Runner-up: A** if NPCs are meant to be alerted by gunfire.

---

### NB-12 — NB.6 panel proof

Covered by §2.3. The panel is BUILT (78 KB, wired at `EditorLayer.cpp:823`,
`:1074-1083`, `:1159-1160`, `:2614`) with a blackboard view
(`NPCBehaviorPanel.h:255 DrawBlackboard`) but **no trace UI** (0 grep hits) and
no current screenshot.

---

## 8. Verification debt on working subsystems — highest user-visible risk

> **Ranked first per the priority order**, because these are not missing
> features. These are ~1000 lines of shipped code with **zero automated
> coverage**, and one of them (save/load) can silently destroy a player's
> progress. Every figure below is grep-verified this session.

| Subsystem | Size | Cited suite | Reality |
|---|---|---|---|
| `Scene/SaveGame.{h,cpp}` | 34 KB / 5.5 KB | `test_savegame.py`, **57 checks** | **file does not exist** |
| Inventory (`Scene.cpp:1809-2084` + `Components.h:2261-2350`) | ~400 lines | `test_inventory.py` | **file does not exist** |
| `Audio::AudioSystem` + ~8 `Scene` audio wrappers | `AudioSystem.h` + `Scene.h:271-283` | none | **no test at all** |
| Audio DSP | `test_audio.py`, **112 checks** | — | **file does not exist**; real coverage is 61 |

Confirmed: `grep -rln 'SaveGame\|wlsave\|InventoryComponent\|AddItem' tests/`
returns **0 hits**, and `find -name 'test_savegame*'` across the repo **and** the
hermes cache returns nothing.

**This is the exact failure this repo keeps making.** A plan states a check count
for a file that was never written, the number is quoted downstream as evidence,
and the gap is invisible because nothing fails.

---

### COV-1 — Save/load has no test at all

**What it is.** `SaveGame.{h,cpp}` is BUILT and bound
(`Bindings.cpp:3938-3981`: `SaveGame`, `LoadGame`, `GameSlotExists`,
`CurrentSchemaVersion`, `GetWorldTime`, `SetWorldTime`), with
`kCurrentSchemaVersion = 1` (`SaveGame.h:57`), slot-name sanitisation, and three
components (`Components.h:2372` `PersistentComponent`, `:2390`
`QuestFlagsComponent`, `:2445` `WorldTimerComponent`).

**Why this is the sharpest risk in the document.** The plan's own notes identify
the failure modes precisely: *"A failed load leaves the running game untouched
-- it does not clear the scene first, so 'unsupported save' cannot become
'corrupt session'."* and *"Half-loading a save the build does not understand
silently drops earned progress, and the player has no way to tell that from
normal progression."*

**Both are untested.** A regression in either is a player losing a save, with no
error.

**Route A — write the suite the plan already specified.**
*Files:* `tests/headless/test_savegame.py` (new, ~57 checks). *Symbols:* none —
the bindings exist.
*Assertions, each stated so it cannot pass vacuously:*
1. Save → mutate **every** mutable component → load → assert each restored
   **exactly**. Re-read from the entity, not from a passed-in component
   (snapshot-copy trap).
2. Transient state stays zeroed: `RigidBody3DComponent::RuntimeBody`,
   `HealthComponent::CooldownTimer`, `WeaponComponent::FireCooldown`,
   `IsReloading`, `ReloadTimer`. Assert each is 0/false after a load.
3. A **future** schema version is rejected **with the world untouched** — assert
   by loading a save, then checking the scene still holds its pre-load state.
4. A **past** version likewise.
5. A **corrupt** file fails without raising and leaves the scene intact.
6. A `.wastescene` is **byte-identical** after a save+load — the one that proves
   the "save is never scene data" separation, which is the whole reason
   `SaveGame` is a separate class.
7. A save naming a **deleted** entity still loads and applies the rest.
8. Slot sanitisation: `"../../../escape"` becomes `.._.._escape`; assert no file
   escapes the saves directory.
*Verification:* the suite reports `N/N`; the failure that matters most is #3 —
assert the world is **unchanged**, not merely that an error was raised.
*Effort:* **M**. The single most valuable test-writing task in the document.

**Route B — a native C++ test project**, matching the `Age*Tests.vcxproj`
pattern.
*Buys:* no module needed; runs alongside the age suites; immune to the
ABI/lock fragility entirely.
*Costs:* a new project + a `WastelandTests.slnx` entry. *Effort:* **M**.

**Route C — property-based round-trip testing.** *Buys:* finds state nobody
thought to enumerate — precisely how transient-state bugs are found.
*Costs:* no property library available (the interpreter is isolated mode;
`pip install` would perturb the linked interpreter). *Effort:* **L**. Rejected
for that reason, but listed because it is the right shape.

**Dependencies.** None.
**Recommendation: Route A.** It uses bindings that already exist and
`tools/t.sh headless` already runs.
**Runner-up: B**, if the ABI/lock fragility of the Python route proves painful.

---

### COV-2 — Inventory has no test at all

**What it is.** ~400 lines, BUILT and bound: `ItemStack`,
`ItemDefinitionComponent`, `InventoryComponent` (`Components.h:2261-2350`);
`Scene::AddItem` (`:1902`), `RemoveItem` (`:2041`), `DropItem` (`:2077`),
`GetItemCount` (`:1809`), `GetInventoryWeight` (`:1841`), `IsInventoryFull`
(`:1860`); bound at `Bindings.cpp:3996-4020`; serialized at
`SceneSerializer.cpp:766-787` (write) and `:1184-1209` (read).

**Why now.** The subtle logic is already documented as such:
`kWeightEpsilon = 1e-4` exists because *"five 0.1 items total
0.5000000014901161, which leaves 0.4999999985 of headroom and silently costs the
player an item they can plainly afford"*. **That exact bug is untested.**

**Route A — one suite covering the plan's own stated test list.**
*Files:* `tests/headless/test_inventory.py` (new).
*Assertions:*
1. Pickup respects **stack limits**: adding `MaxStack+1` of a MaxStack=10 item
   yields two stacks, not one clamped stack.
2. Pickup respects **capacity**: `IsInventoryFull` true at `MaxSlots`, and
   `AddItem` returns a count **less than requested** — assert the **return
   value**, the only signal available. Above capacity it adds what fits and
   reports the rest.
3. **Weight epsilon**: five 0.1-weight items against 0.5 capacity must all fit.
   This is the float bug the comment describes and the most valuable assertion
   in the suite.
4. Dropping a **partial** stack preserves the remainder.
5. An unknown item id gets a MaxStack=1 stack (documented behaviour in
   `AddItem`) rather than being refused — assert the *documented* behaviour,
   since a silent refusal would be worse.
6. **Serializer round-trip**: stacks and `MaxWeight`/`MaxSlots` survive exactly.
   `SceneSerializer.cpp` handles both directions; neither is tested.
*Verification:* assert **return values and read-back state**, never "did not
throw" — `AddItem` returning 0 for "no inventory" and 0 for "full" is
deliberately identical, so a return-value-only test cannot distinguish them.
**Check the entity has an `InventoryComponent` too.**
*Effort:* **M**.

**Route B — fold into the save/load suite**, since both are state round-trips.
*Effort:* **S/M**, and it muddies both. Only if one person does both.

**Dependencies.** None. COV-1 first if one person does both — the
snapshot-copy trap and re-read-from-entity discipline are shared.
**Recommendation: Route A**, written by whoever writes COV-1, immediately after.
**Runner-up: B.**

---

### COV-3 — `AudioSystem` and the `Scene` audio wrappers are untested

**What it is.** `AudioSystem.{h,cpp}` (4153 / 17559 B) — listener following,
source resolution, autostart, clip loading — plus `Scene::PlaySound` /
`PlaySoundAt` / `PlayEntitySound` / `StopSound` / `LoadAudioClip`
(`Scene.h:271-283`). The **mixer and clip** are covered (61 checks); **the
scene-facing layer is not.**

**Why now.** `PlayEntitySound` is the function AVB-1c makes unreachable, and it
is the path spatial gameplay uses. AVB-1a's device work sits directly on top of
this layer.

**Route A — a suite for the scene-facing layer.**
*Files:* `tests/headless/test_audio_system.py` (new).
*Assertions:*
1. `PlaySound("missing")` returns **-1** (documented: *"a sound that was never
   imported should not take the game down"*).
2. `PlaySound` of a real clip returns a voice id **>= 0**.
3. `PlayEntitySound` on an entity with **no** `AudioSourceComponent` returns
   -1 and does **not raise**.
4. `PlayEntitySound` **with** a component returns a voice id — *this assertion
   cannot currently be written*, because AVB-1c means
   `AddOrReplaceAudioSourceComponent` does not exist. **Fix AVB-1c first or
   this suite carries a permanent skip.**
5. `ResolveSourcePosition` honours `PositionOffset` — assert the **offset**, not
   an inferred panning result (it is public precisely so this is testable
   without inference).
6. `PlayAutoStartSources` fires **only** sources with `PlayOnStart`, and fires
   exactly once (documented as start-of-runtime, not per-frame).
*Verification:* the listener test must assert the listener **follows the entity**
across a move, not merely that it resolved to something.
*Effort:* **S/M**.

**Route B — defer until AVB-1 lands.** **S**. Reasonable — but AVB-1c makes it
*writable* today, so deferring leaves a testable function untested for no reason.

**Dependencies.** Assertion 4 depends on **AVB-1c**. Everything else is
independent and writable now.
**Recommendation: Route A, with assertion 4 blocked on AVB-1c.** Write 1, 2, 3,
5, 6 now; add 4 the moment the component binds.
**Runner-up: B** if AVB-1 is imminent — then write it as part of AVB-1.

---

## 9. Consolidated dependency graph

```
                    ┌─────────────────────────────────────────┐
                    │ ST-1 land scratch suites in tests/      │  ← do FIRST
                    │ ST-2 git add the implementation         │  ← evidence
                    └──────────────────┬──────────────────────┘    is worthless
                                       │                         without code
       ┌───────────────────────────────┼───────────────────────────┐
       │                               │                           │
       ▼                               ▼                           ▼
 ┌──────────────┐              ┌────────────────┐          ┌──────────────┐
 │ ST-3 module  │              │ COV-1 save     │          │ VB-5 lint on │
 │ under test   │              │  (~600 lines,  │          │ shipped trees│
 └──────┬───────┘              │   0 tests)     │          └──────┬───────┘
        │                      └───────┬────────┘                 │
        │                              │                    ┌──────▼───────┐
        │                              │                    │ NB-9 SetTimer│
        │                              │                    │ (shipped     │
        ▼                              ▼                    │  BROKEN)     │
 ═══════════════════════════════════════════════════       └──────┬───────┘
  Everything below here is INDEPENDENT of everything above.         │
  Nothing in this row blocks anything else in this row.            ▼
 ═══════════════════════════════════════════════════      (registered; plus the
        │                              │                    RuntimeTypes cross-check)
        ▼                              ▼                           │
 ┌──────────────┐              ┌────────────────┐          ┌────────▼───────┐
 │ AVB-1c bind  │──enables──▶  │ COV-3          │          │ NB-10          │
 │ AudioSource  │  assertion 4│ AudioSystem    │          │ FollowPath     │
 │ (2 lines)    │             │ suite          │          │ + avoidance    │
 └──────┬───────┘             └────────────────┘          └────────────────┘
        │
        ▼
 ┌──────────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐
 │ AVB-1b       │   │ 5.1 2D       │   │ 5.2 impulse  │   │ COV-2        │
 │ masterVol    │   │    physics   │   │  /velocity   │   │ inventory    │
 │ clamp (1 ch) │   │              │   │ (NOT handle) │   │ suite        │
 └──────────────┘   └──────────────┘   └──────────────┘   └──────────────┘
        │
        ▼
 ┌──────────────────────────────────────────────────────────────────────┐
 │ AVB-1 DECISION: sample rate. Must land BEFORE the device backend.   │
 │   A) rate in mixer (breaks the "don't modify Mixer" promise)         │
 │   B) rate in device (promise holds)   ← RECOMMENDED                  │
 │   C) resample at load (memory cost for long music tracks)           │
 │   D) document + mandate 48kHz assets (cheapest, fragile)            │
 └───────────────────────────────┬──────────────────────────────────────┘
                                 ▼
                      ┌──────────────────────┐
                      │ AVB-1 device backend │ ← THE GATE for AVB.2–11
                      │  (miniaudio, compiled│    10 phases, ~58 KB of plan
                      │   alone first, R+D)  │    depend on this one item
                      └──────────┬───────────┘
                                 ▼
        ┌──────────┬──────────┬──────────┬──────────┬──────────┐
        ▼          ▼          ▼          ▼          ▼          ▼
     AVB.2      AVB.3      AVB.4      AVB.5/6    AVB.7      AVB.8
     DSP        modal/     graph/     buses +    runtime    DoomArray
     primitives comb/grain compiler  reverb     + triggers
        │          │          │          │          │          │
        └──────────┴──────────┴──────────┴──────────┴──────────┘
                                 │
                                 ▼
                     AVB.9 track editor
                     AVB.11 record + stems
                     (BLOCKED: no WAV writer — verified 0 hits)

 ┌──────────────────────────────────────────────────────────────────────┐
 │ VB-1 Route A: decouple NodeCanvas from VisualBlocks::Graph         │
 │   └─▶ makes AVB-10's panel trivial (plan's vendor premise obsolete) │
 ├──────────────────────────────────────────────────────────────────────┤
 │ 14.6 terrain/streaming — depends on EVERYTHING above. Genuinely last.│
 └──────────────────────────────────────────────────────────────────────┘
```

**What blocks what, in one line each:**

- **Nothing in the bottom half blocks anything in the top half.** The
  user-visible fixes (audio components, master volume, 2D physics, impulse,
  coverage suites) are all independent and all small.
- **ST-1 → ST-2** is the only true ordering constraint outside AVB: tests
  committed without the code they test verify nothing.
- **AVB-1c → COV-3 assertion 4** is the one cross-dependency in near-term work,
  and it gates *writing a test*, not shipping a feature.
- **AVB-1's rate decision → AVB-1 device → all ten remaining AVB phases.**
  Everything auditory sits behind one design decision plus one dependency.
- **14.6 depends on all of it** and is honestly last.

---

## 10. Recommended execution order

Five phases: user-visible breakage, then structural rot, then proof, then
decisions, then new capability. This front-loads the AVB decisions before the
implementation they constrain.

### Phase 1 — Unbreak and protect (1–2 days)

| # | Item | Effort |
|---|---|---|
| 1 | **AVB-1c** bind `AudioSourceComponent` + `AudioListenerComponent` — makes a tested subsystem usable | S |
| 2 | **AVB-1b** clamp `SetMasterVolume` upper bound; fix the DEFECT comment | S |
| 3 | **NB-9** register `SetTimer` (A) + add the `RuntimeTypes` ↔ registry cross-check (C) | S |
| 4 | **5.1** `SyncPendingPhysics2D` | S |
| 5 | **5.2** `ApplyImpulse` / `SetLinearVelocity` / `GetLinearVelocity` | S |
| 6 | **§2.2 residue** delete the stale "there is no fixed-timestep accumulator" comment at `Scene.cpp:842` | S |
| 7 | **ST-3 Route A** print the resolved module dir + mtime in `t.sh` | S |

Every item is a small, independently shippable fix to something either broken or
untested-but-trivial. **None touches the renderer.**

### Phase 2 — Stop the rot (1–2 days)

| # | Item | Effort |
|---|---|---|
| 8 | **ST-1** land the four scratch suites + runner in `tests/`, wire `tools/t.sh` | S |
| 9 | **ST-2** `git add` by explicit path in dependency order, build between stages, commit. **Never `git add -A`** — the user is editing the renderer | S/M |
| 10 | **ST-4** one canonical counter; correct the fictional 57/112-check figures in the plans | S |
| 11 | **VB-5 Route C** a lint test asserting every shipped `.wlbtree`/`.wlblocks` validates clean | S |
| 12 | **§2.1 residue** keep the `SystemExit` guard in `runner.py` as a backstop | S |

**ST-1 before ST-2** is the one ordering that matters: committing tests against
code nobody can retrieve buys nothing.

### Phase 3 — Prove what ships (2–3 days)

| # | Item | Effort |
|---|---|---|
| 13 | **COV-1** `test_savegame.py` — 8 assertion groups | M |
| 14 | **COV-2** `test_inventory.py` — including the weight-epsilon regression | M |
| 15 | **COV-3** AudioSystem suite (assertion 4 unblocked by item 1) | S/M |
| 16 | **§2.3** editor proof: screenshots at 4K, both panels, real assets; plus the self-check logging registry size on panel open | S/M |
| 17 | **5.3** rename `DropItem` (Route B), then decide on A | S → M |
| 18 | **VB-4** trace: Python binding first (testable), then the panel toggle | S/M |

### Phase 4 — Decisions before construction (1–2 days)

| # | Item | Effort |
|---|---|---|
| 19 | **AVB-1 rate decision** — Route B recommended. Also answers the plan's `BpmToSeconds` rate question | M |
| 20 | **VB-1** canvas decision; Route A decoupling if a second canvas is coming | S → M |
| 21 | **VB-2** flow-graph end-to-end through a live scene | S |
| 22 | **VB-5 Routes A+B** schema doc + graph-aware `ToolServer` tools | S |
| 23 | **NB-10** `FollowPath`; separation if agents visibly stack | S/M → M |
| 24 | **VB-3** five scene/entity flow nodes (unblocks the UI demo) | M |

### Phase 5 — New capability (weeks)

AVB.2 DSP primitives (reordered **before** the device — see §6) → AVB.1 device →
AVB.3 → AVB.4 → AVB.5/6 → AVB.7 → AVB.8 → AVB.9 → AVB.11. Then 14.6 terrain,
which depends on all of it.

**The reorder worth arguing for:** the plan puts AVB.1 (device) first and gates
everything on it. But the device is the riskiest item and the DSP primitives are
testable with no device at all. **Do AVB.2 first.** It front-loads value and
de-risks the gate for free.

---

## 11. Risk register

For each risky route: the most likely way it goes wrong, and its early warning.

| # | Route | Most likely failure | Early warning |
|---|---|---|---|
| **R1** | **Any `Bindings.cpp` edit** | Re-registering `py::class_<Scene>` mid-file. There is exactly **one** Scene binding and methods go on it (`Bindings.cpp:3819`); a second registration is a pybind error. Also: a `.def()` whose lambda takes no `Scene&` makes pybind expect a method signature plus implicit `this`, the annotation count mismatches, and **MSVC reports it from inside `pybind11.h` with no line number in our file**. | Build error with no file:line in our code. Make it `def_static`. |
| **R2** | **Any new engine component type** | `OnComponentAdded`'s primary template is `static_assert(false)` (`Scene.cpp:3001-3003`), so the linker wants the symbol and **the whole module fails `LNK1120`**. A new component needs an explicit specialization or nothing links. | `LNK1120` on a module that otherwise compiles clean. |
| **R3** | **ST-2 commit** | `git add -A` sweeps in the user's in-flight renderer work — `Renderer3D.*`, `DX12Nova.*`, `NVRHIContext.cpp` — which must not be touched. | `git status` after staging. Stage by explicit path. |
| **R4** | **ST-1 relocate suites** | Suites are tuned to their scratch cwd; a moved suite silently globs nothing and reports **0 checks passed**, which looks green. | Any suite printing `0/N`. `run_all_suites.py` already reports marker-less output as **UNCOUNTED** — use it. |
| **R5** | **Any build** | Two MSBuild runs fail with `C1041` / `MSB6003` / `C1083` from an orphaned `cl.exe`. **Piping MSBuild into `grep`/`tail` returns before the build finishes**, and the next command races it. | `C1041`, `MSB6003`, `C1083`. Use `tools/msb.sh`; capture with `>` or `background=true` + `notify`. |
| **R6** | **Any build** | `RC=0` with a **zero-byte** `.pyd`. A link can fail `LNK1149` and still exit 0; the artifact then fails to import with *"DLL load failed: %1 is not a valid Win32 application"* — maximally misleading. | **Validate size and MZ magic. Never the exit code alone.** |
| **R7** | **Testing against the canonical module** | It is **kernel-locked**, so tests hit a stale build. I hit this: a correct accumulator produced a **false red 4/8** because the `.pyd` predated `Scene.cpp` by 8 minutes. Acting on that red would have broken working code. | Banner shows module path + mtime. Set `WASTELAND_MODULE_DIR`. |
| **R8** | **Any shim/helper change** | A helper signature drifts from its emitter call sites. `HasLineOfSight` called `_has_los` with 3 args after NB.7 widened it to 4 — **dead node at runtime**, compiling clean. | `test_node_library.py`'s systematic arity audit (`:573-643`). **This is why ST-1 is P0.** |
| **R9** | **Any occlusion / LOS test** | **A collider with NO Static `RigidBodyComponent` is not in the broadphase**, so the test asserts nothing. This is why `test_los_contract.py` uses real geometry with Static rigid bodies — the *earlier* mock was shaped `(True, Vec3, Vec3, None, …)`, **agreeing with the bug rather than the contract**. | A "passing" LOS test whose blocker is a bare collider. Assert the trace actually **hit** something. |
| **R10** | **Any helper that swallows exceptions** | The generated shim needs `Wasteland` injected into its globals or every `Wasteland.Vec3` raises `NameError`, gets swallowed by a bare `except`, and the helper answers **"not blocked" for every input** — indistinguishable from a correct clear line. | A helper test that passes for *any* input. Assert at least one input returning the non-default answer. |
| **R11** | **Navmesh assertions** | **Path length is not a detour proof** — a detour clipping a corner is *shorter* than the straight line. Measure whether the path **crosses the wall's own x at the wall's own y**. | A length-based detour assertion passing. |
| **R12** | **Navmesh reachability assertions** | `FindNearestWalkable`'s **ring search quietly makes any point reachable**, so an "unreachable goal" test proves nothing. | An unreachable-goal test that passes. Assert the ring radius is exceeded. |
| **R13** | **Navmesh bake** | The wall's own top is baked as "ground" — a wall top trivially passes headroom. `IsGround` (`NavMesh.cpp:114`) is the fix; removing it regresses silently. | A path that goes *over* the obstacle. |
| **R14** | **Blackboard assertions** | `bb.set(key, None)` marks the key **written**, so a `BlackboardIsSet` gate reads true forever. Validity is tracked **separately** from value. | A negative case asserting only the **value**, which reads `None` either way. Assert `key not in bb.valid`. |
| **R15** | **Component assertions** | Components handed to `AddOrReplace*` are **snapshot copies** — reading `.Current` off the object you passed shows the attach-time value forever. A working tree reported 0 damage this way. | Always re-read from the entity: `entity.GetHealthComponent().Current`. |
| **R16** | **Grepping for failures** | `grep "FAIL"` matches a **passing** assertion named "…returns FAILURE". Match the checker's own `"  FAIL "` prefix. | A green suite reported red. |
| **R17** | **Test authoring** | `check_le(a, b)` is an **upper** bound — asserting "damage happened" with `check_le(0.0, dealt)` asserts the opposite, and a zero-damage tree passes. | Lower bounds need `check()`. |
| **R18** | **Blackboard deadline tests** | `now` is a running sum of deltas and drifts ~1e-15/tick, so a raw `<` stays true **one tick past** the deadline — a 1.0 s cooldown firing every 1.2 s. | Use `deadline_passed()` with the 1e-6 tolerance (`wl_behavior.py:583`). |
| **R19** | **AVB-1 device backend** | The device opens at **44.1 kHz** when 48 kHz was assumed → silent 8.8% pitch drift, and with **no rate concept in the mixer** there is nothing to compare against. | Assert the **actual** buffer rate at device open. Interacts with AVB-1a. |
| **R20** | **AVB feedback branches** | DoomArray branch D is a **deliberate self-oscillating resonator**. Untested at max drive and feedback it is a NaN generator poisoning everything downstream. | Stability-guard tests at **maximum** settings. |
| **R21** | **AVB determinism** | A runtime PRNG without a seed makes every audio test flaky and every replay wrong. | Seed it; assert bit-identical re-render. |
| **R22** | **Any UI verification by build log** | "A clean build proves nothing here." The original NB.6 defect — *unknown node type for `Sequence`, `Invert`, `Wait` on a tree that validates 0/0* — was found **only** from a screenshot. The registry self-registration fix has **never been seen running**. | Screenshots at 4K with a real graph. |
| **R23** | **Editing the renderer** | The user is actively editing `Renderer3D.*`, `DX12Nova.*`, `NVRHIContext.cpp`. **Do not touch, revert, or reformat.** If a renderer change looks like the cause of a problem, **report it, do not fix it**. | Any diff in those files. |
| **R24** | **Suite teardown** | A suite printing all-green then exiting **139** is a **PASS**. `Scene::OnRuntimeStop()` and standalone `ScriptEngine::OnUpdateEntity` segfault for pre-existing reasons — **never call either**. | Judge printed checks, not exit code. |
| **R25** | **Killing processes** | The hermes kernel holds `Wasteland.pyd` open — **expected, never kill it**. | n/a |
| **R26** | **COV-1 / COV-2 new suites** | A suite that constructs a scene but never calls `OnSimulationStart()` reports **0/0 checks** — a green proving nothing. This already happened once to `test_physics_3d.py`. | Any suite printing `0/N`. Assert a non-zero check count. |
| **R27** | **COV-3 assertion 4** | Written before AVB-1c, it must be **skipped**, and a permanent skip is how a coverage gap hides in plain sight. | A suite with a skip and no comment explaining the dependency. |

---

## 12. Do not do

Approaches that look reasonable and are traps here, each grounded in something
that actually happened in this codebase.

1. **Do not "fix" the physics timestep.** It is correct in source and already
   fixed; my measured red was a **stale binary**. Rebuild and re-run. Changing
   correct code because a stale artifact disagreed is how the box-hull audit
   ended up claiming physics was never stepped at all.

2. **Do not expose `RuntimeBody` to Python as an apply-impulse shortcut.** It is
   a raw process-local Box3D handle — a stale one after `OnPhysics3DStop()` is a
   use-after-free, not an API. Resolve the body inside the engine (§5.2 Route A).

3. **Do not add a graph reverb before buses.** Epic deliberately routes
   reverb/IR effects through **submix buses**: *"Spatialization, submix effect
   processing, wave file and codec management … are all things not handled by
   MetaSounds."* Building graph reverb first means migrating it later, and
   migrating audio DSP eats a phase.

4. **Do not merge `.wlavb` into `.wlblocks`.** The type systems genuinely
   differ: an `Exec` wire carries **ordered control flow**; a `Trigger` wire
   carries a **sample-accurate timestamp inside a block and may have zero
   receivers**. One format that is an execution graph *or* a flow graph forces
   one of them to become the other. Shared *code* (a canvas) is fine; shared
   *abstraction* is not.

5. **Do not build a VM, a native C++ BT runtime, or a Python-interpreter
   escape hatch.** The transpile decision's proof obligation is that
   `Bindings.cpp` gains **zero** new entries for a whole behaviour tree — and it
   does not. *"Do not build a VM to solve a problem this codebase does not yet
   have."*

6. **Do not build `imgui-node-editor` now.** A working 1493-line `NodeCanvas`
   already exists and is already reused. Vendoring a third-party canvas to
   replace shipped code is expensive with the least payoff — **unless** a second
   canvas is genuinely coming (AVB.10), in which case decouple the existing one
   (VB-1 Route A) rather than replace it.

7. **Do not `grep -c "^  PASS "` for suite totals.** The documented failure: CRLF
   suites put `\r` where the pattern expects a space, **under-reporting by 157
   checks while still printing "0 failed suites"** — the verdict looked healthy
   while the count was wrong. Use `run_all_suites.py`.

8. **Do not quote a check count without the file.** Three are in circulation for
   files that **do not exist**: `test_savegame.py` (57), `test_inventory.py`,
   `test_audio.py` (112 — real: 61). A count is evidence only if the file is.

9. **Do not trust a navmesh assertion based on path length or reachability.**
   R11 and R12. Measure geometry, not magnitude.

10. **Do not add a blackboard negative case that checks only the value.** It
    reads `None` either way; validity is separate. Assert `key not in bb.valid`.

11. **Do not write an occlusion test with a bare collider.** No Static
    `RigidBodyComponent` → not in the broadphase → the test asserts nothing
    (R9). And **do not let a helper's bare `except` stand** — a swallowed
    `NameError` makes `_los_blocked` answer "not blocked" for every input,
    indistinguishable from a correct clear line (R10).

12. **Do not leave suites in the hermes scratch cache.** 24-hour prune, and it is
    where the entire NB.3–NB.5 evidence base currently lives.

13. **Do not modify the renderer.** Report anything that looks renderer-caused.

14. **Do not pipe MSBuild into `grep`/`tail`.** It returns before MSBuild
    finishes; the next command races the build.

---

## 13. Effort summary

**S ≈ under an hour · M ≈ half a day · S/M ≈ a day · L ≈ multiple days.**
"Done" means verified, not written.

### 13.1 By area

| Area | Done/verified | Remaining S | Remaining M | Remaining L | Area total |
|---|---|---|---|---|---|
| **Cross-cutting** (§2) | timestep ✅ · LOS contract ✅ · runner abort ✅ | 3×S | — | — | **S** |
| **Visual Blocks** (§3) | VB.1–VB.4 runtime ✅ · panel BUILT | 5×S | 1×M | — | **M** |
| **Structural rot** (§4) | — | 4×S | 1×S/M | — | **M** |
| **Verification debt** (§8) | audio DSP 61 ✅ | — | 2×M, 1×S/M | — | **M** |
| **Item 14** (§5) | physics stepping ✅ · navmesh ✅ | 3×S, 2×S/M | 1×M | 1×L (14.6) | **M**; 14.6 is **L** |
| **NPC behavioural** (§7) | NB.0–5,7,8 built ✅ · `UsePathfinding` ✅ | 2×S | 2×S/M, 1×M | — | **M** |
| **Auditory** (§6) | AVB.0 ✅ (61 checks) | 3×S | 6×M | 6×L | **L** — 10 phases remain |
| **TOTAL** | | **~20 S** | **~14 M** | **7 L** | **≈ 3–4 weeks** |

### 13.2 By priority tier

| Tier | Items | Effort | Rationale |
|---|---|---|---|
| **1 — User-visible breakage** | AVB-1c (audio components unbindable) · AVB-1b (master volume) · NB-9 (`SetTimer` broken) · 5.1 (2D physics) · 5.2 (impulse) · 5.3 (`DropItem` lies) · 5.5 (audio serialization) | **~7 S + 1 M** | Tested subsystems no game can actually use |
| **2 — Verification debt** | COV-1 (save/load) · COV-2 (inventory) · COV-3 (AudioSystem) | **2 M + 1 S/M** | ~1000 lines shipped with zero coverage; save/load can destroy progress |
| **3 — Structural rot** | ST-1 (suites outside repo) · ST-2 (untracked code) · ST-3 · ST-4 · ST-5 | **~5 S + 1 S/M** | Evidence evaporates in 24 h; implementation has no history |
| **4 — Proof** | §2.3 editor screenshots · VB-4 trace · VB-5 schema · 5.4 events · NB-12 | **~4 S + 1 S/M** | Needed to trust what is built |
| **5 — New capability** | NB-10 · NB-11 · VB-3 · 14.6 · **AVB.1–AVB.11** | **~5 M + 7 L** | 14.6 is last; AVB is 10 phases behind one decision |
| **TOTAL** | | **~21 S · 14 M · 7 L** | **≈ 3–4 weeks** |

### 13.3 The three cheapest high-value items

If only three things get done this week:

1. **AVB-1c** — two `BIND_GAMEPLAY_COMP` lines. Makes a complete, 61-check-tested
   audio system reachable from any script. Currently **no game can use it**.
2. **ST-1** — move four suites into `tests/`. The 1209-check suite that caught
   the `_has_los` dead-node bug sits in a directory that prunes in 24 hours.
3. **COV-1** — write `test_savegame.py`. ~600 lines of save/load with no test,
   where the documented failure modes are *"half-loading a save the build does
   not understand silently drops earned progress"*.

---

## 14. Status summary, restated honestly

| | Count |
|---|---|
| **Verified by me this session** | **117 headless checks / 11 suites** · **1209** node-library checks · 54 + 99 visualblocks · audio 61 |
| **Fixed and closed during this work** | physics timestep accumulator · `_los_blocked` tuple contract · `HasLineOfSight` emitter arity (21 leaf emitters AST-audited, 0 mismatches) · runner `SystemExit` abort |
| **Missing, mapped above with routes** | ~21 S · 14 M · 7 L |
| **UNVERIFIED** (needs a build or an editor run) | Editor Debug build currency · both panels at runtime · AVB (nothing exists to verify) · 14.6 (nothing exists to verify) · Age panel surface |

**Nothing in this document asserts a renderer behaviour.** Per R23: if a renderer
change looks like the cause of a problem, report it — do not fix it.
