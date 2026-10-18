# Auditory Visual Blocks (AVB) — Procedural Audio Node Graphs, Track Editor, and Synthetic Instruments

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.
> **Status:** plan only. Nothing in this plan is implemented.

**Goal:** A UE5-MetaSounds-style node graph ("Auditory Visual Blocks") that synthesises
audio procedurally in real time, a REAPER-style track editor to arrange, see and
record those sounds into tracks, custom synthetic instruments built from effects
chains, and a shipped default instrument that is an artificial homage to the
four-branch corruption array Mick Gordon built for the DOOM (2016) score.

**Architecture:** AVB graphs are a **flow graph evaluated per audio block** — not a
transpiled script. A new C++ graph runtime interprets typed nodes with an
`ExecuteBlock()` call per block and a `Trigger` wire type that gets
**sample-accurate** partition points inside the block. Track editing is a separate
REAPER-derived document (tracks / items / takes / envelopes / bus routing) that
*hosts* AVB instruments as sources. Both sit on top of the existing
device-independent `Mixer`, which stays where it is.

**Tech Stack:** C++17, yaml-cpp (vendored), ImGui 1.89.3 (docking enabled),
imgui-node-editor (to vendor, shared with Visual Blocks VB.0), miniaudio (to
vendor, for the missing device backend + recording), entt ECS, existing
`Audio::Mixer` / `Audio::AudioSystem`.

---

## 0. Read this first — four corrections to the premises

These came out of auditing the repo and reading Epic's and Cockos' primary docs.
They change the design, so they go first. Nothing below is padding.

### 0.1 The DOOM "array" is not a percussion rig

**The brief asked for a synthetic version of a hammered-metal/gong/found-object
percussion array. That rig does not exist in the sources.** What Mick Gordon
actually built — and what is universally called the "DOOM Array" / "DOOM
instrument" / "Doom Machine" — is a **four-parallel-branch effects-corruption
matrix fed by a pure sine wave and white noise**.

- Gordon, GDC 2017 *"DOOM: Behind the Music"*: *"breaking sound down into its kind
  of bare bones components which are sine waves and noise. I then pulsed rhythms
  of sine waves and noise through vast arrays of analog equipment… I'm using the
  circuits of the equipment to corrupt the pure sine waves and noise."*
- DoomWiki: *"a 'DOOM instrument,' consisting of pure sine wave inputs directed
  into four separate, different chains of sound processing elements, including
  distortion pedals, bit crushers, fuzz boxes, analog phasers, tape echo, spring
  reverb, and a feedback loop along with several compressors and a pair of audio
  chain splitters, which were then mixed together."*
- Structure of the four chains: (1) four FX pedals + splitter, (2) four FX pedals
  + compressor, (3) tape echo, FX pedal, reel-to-reel, spring reverb, compressor,
  (4) mini amp + mic + splitter + *pair* of compressors (a deliberate feedback loop).
  Summed → EQ → compressor → DAW.

**This is good news, not bad news.** An effects matrix *is* a node graph. It maps
onto AVB almost 1:1, whereas a foley rig would have needed samples we do not have
and must not ship. The plan implements the array as it actually was, and adds a
**procedural struck-metal layer of our own design** (clearly labelled as an
homage, not a reconstruction) so there is a percussion voice too.

**Unsourced — do not put these in the shipped instrument as fact:** detuned saw
stacks, FM character, pitch-shifted vocal screams. Gordon's *stated* method is sine
+ noise. Saw stacks are a defensible homage addition but must be labelled ours.
Source notes: `C:\Users\rtoue\doom-array-dsp-research.md`.

**Two traps in the sources:**

- **Do not use the Doom Eternal *album* as a mixing reference.** Gordon stated
  that much of it was mixed by Chad Mossholder (id), and it was criticised for
  over-compression. The **in-game mix is the target**; the album is not.
- **The pedal-by-pedal chain is community-transcribed** off the GDC video by forum
  posters, and is unverified. The plan therefore specifies *functional roles*
  (transient / body / space / runaway) rather than pedal models — which is both
  more defensible and more portable.

**The drum sound was multi-bus, and that is the finding that justifies AVB.5.**
Gordon, URM EP261 2:01:57: *"90% of the drum sounds come from Superior Drummer."*
But the interesting part is what he did with it — he took ~5 room-mic positions as
**independent voices**, gated them so rooms pop on the snare hit, re-routed all of
them into **4–5 further effect buses** (one saturated, one gated to attack-click
only, one opening only on the snare), then compressed the lot. One kit hit
produces a saturated body, a dry click and a snare-triggered room slap
*simultaneously*. That is a bus architecture with per-bus conditional gain, and it
is precisely what `Audio::Mixer` does not have.

His enforced rule, which belongs in this plan as a lint check: **every layer must
reinforce the groove** — *"each time I'm adding something, I'm making sure it's
retaining or enforcing the groove."*

### 0.2 MetaSounds has no reverb, convolution or distortion node

Epic deliberately routes large/IR effects through **submix buses**, not graph
nodes — *"Spatialization, submix effect processing (i.e. 'send' effects), wave
file and codec management, memory management, are all things not handled by
MetaSounds."* There is no `Reverb` and no `Convolver` node. Distortion exists as
**`WaveShaper`** (curve enum: Sine / Inverse Tangent / Hyperbolic Tangent / Cubic
Polynomial / Hard Clip, plus `Amount`, `Bias`, `OutputGain`) — the curve is the
primitive and named distortions are presets of it.

**Consequence:** AVB must have an `AudioBus` with a send matrix *before* it has
reverb, exactly as UE does. Building graph reverb first is the classic wrong
turn. Phase **AVB.5** is buses; **AVB.6** is bus effect chains.

### 0.3 A MetaSound graph is a *flow* graph, not an execution graph

This is the single most important architectural difference from Blueprint, and
from the existing Visual Blocks. There is no exec/flow-with-early-return
semantics. Every node processes a **buffer**, and `Execute()` is called once per
block on the audio render thread.

The existing `.wlblocks` system is the opposite: an **execution graph transpiled
to Python**, with `exec` pins, `Branch`→`if/else`, generators for latent delays.
It is excellent for what it does and **cannot host audio.** The two systems share
a *format philosophy* (stable string pin IDs, YAML, error-on-unknown) and
nothing else. The compiler, the validator, the node registry and the file format
are all separate. Section 1.2 makes this boundary explicit and forbids drift.

### 0.4 The claimed test suites do not exist on disk

`item-14-long-tail-systems.md` claims **"1011/1011 across 27 suites"** and a
`test_audio.py` with **112 checks**. I searched the whole tree including `tests/`,
`DemonCore-Editor/`, and `C:/dev`: **there is no `test_audio.py` and no Python test
suite anywhere in the repo.** `tests/` contains only C++ (`test_project.cpp`,
`test_project_seeding.cpp`) and is entirely untracked. Nothing in git history
mentions `test_audio`.

The audio implementation itself *is* real and substantial (`AudioClip.cpp` 310
lines, `Mixer.cpp` 561, `AudioSystem.cpp` 225, all bound into `Bindings.cpp` at
lines 2850–2991). The **verification artifact** is missing, not the feature.

**This must be fixed before AVB.1**, because the project's whole verification
culture is "headless Python suite, real assertions, explicit PYTHONPATH to the
Release `.pyd`" — and that harness is currently absent. Phase **AVB.0** rebuilds
it and re-establishes the audio baseline. Writing 15 phases of new DSP against a
verification bar that does not exist would make every claim in this plan
unfalsifiable.

---

## 1. The architecture

### 1.1 Layering

```
                     ┌──────────────────────────────────────────┐
   AVB.8  Editor     │  TrackEditorPanel   (REAPER-like)        │
                     │    tracks / items / takes / envelopes    │
                     │    ruler, transport, record arm          │
                     ├──────────────────────────────────────────┤
   AVB.7  Editor     │  AuditoryBlocksPanel (node canvas)       │
                     │    palette, canvas, details, trace       │
                     ├──────────────────────────────────────────┤
   AVB.6  Content    │  AvbCompiler  (.wlavb -> .avb.json)      │
                     │  AvbValidator, AvbNodeRegistry           │
                     ├──────────────────────────────────────────┤
   AVB.3-5  Runtime  │  AvbRuntime   (ExecuteBlock per block)   │
                     │  AvbBuses, AvbGraphState, TriggerClock   │
                     ├──────────────────────────────────────────┤
   AVB.2  DSP        │  AvbDsp.h/.cpp  — filters, env, waveshape│
                     │  modal bank, comb, delay, compressor     │
                     ├──────────────────────────────────────────┤
   EXISTING (14.4)   │  Audio::AudioClip / Audio::Mixer         │
                     │  Audio::AudioSystem  (unchanged)         │
                     ├──────────────────────────────────────────┤
   AVB.1  Device      │  miniaudio: playback + capture          │
                     └──────────────────────────────────────────┘
```

`Audio::Mixer` is **not modified** by AVB. It stays the place that decides voice
allocation, priority, attenuation and panning for *gameplay* sounds. AVB is a
separate rendering path whose output enters the mixer as a stereo bus, so
gameplay SFX and procedural music share one master volume and one listener, and
neither system needs to know about the other.

### 1.2 The boundary with Visual Blocks — stated so it cannot drift

| Concern | `.wlblocks` (existing) | `.wlavb` (new) |
|---|---|---|
| Graph kind | Execution graph | Flow graph |
| Backend | Transpiles to Python | Interpreted per block |
| Exec pin | `PinType::Exec`, ordered | `AvbPinType::Trigger`, fannable to zero |
| Nodes | `NodeLibrary.cpp`, 453 lines | `AvbNodeRegistry.cpp`, new |
| Validator | `Validator.cpp`, 640 lines | `AvbValidator.cpp`, new |
| Compiler | `Compiler.cpp`, 874 lines | `AvbCompiler.cpp`, new |
| Editor panel | `VisualBlocksPanel` — **does not exist yet** | `AuditoryBlocksPanel` |

**Both panels need a node canvas and neither exists.** VB.5 in the Visual Blocks
plan is unwritten, and `imgui-node-editor` is not vendored. So **AVB.7 vendors the
canvas and VB.5 gets it for free** — that is the one piece of shared work, and it
is shared *code*, not a shared abstraction. The two graphs keep separate
registries, separate validators, separate compilers and separate file formats.
Merging them would mean one format that is an execution graph *or* a flow graph,
and the type systems genuinely differ (an `exec` wire carries control order; a
`Trigger` wire carries a sample-accurate timestamp inside a block and may have
zero receivers).

### 1.3 The block-size decision

MetaSounds uses a **100 Hz block rate** (CVar `au.MetaSound.BlockRate`, default
100.0f), which at 48 kHz is **480 frames/block — not a round 512**, because
`GetNumFramesPerBlock()` is nudged to satisfy SIMD alignment for
`FAlignedFloatBuffer`. `GetActualBlockRate()` returns what you actually got.

**Steal that lesson, not the number.** Derive frames-per-block from the sample
rate by rounding to the SIMD width, and report the *actual* rate:

```cpp
// kAvbSimdLanes = 8 (AVX) or 4 (SSE). Query at startup.
int frames = ((sampleRate / kTargetBlockRate) + kAvbSimdLanes - 1)
             / kAvbSimdLanes * kAvbSimdLanes;
const float actualBlockRate = (float)sampleRate / (float)frames;
```

At 48 kHz with a 100 Hz target and 8 lanes this yields 480 frames and 100.0 Hz
exactly. Never assume the requested rate is the achieved one; assert it in a test.

### 1.4 Sample-accurate triggers inside a block — the pattern to copy exactly

This is the best architectural nugget in MetaSounds, visible in Epic's own
published `AD Envelope` `Execute()`. A block is *partitioned* at trigger sample
positions, and downstream triggers re-fire at real frame indices:

```cpp
void Execute() {
    m_AttackTrigger.AdvanceBlock();          // reset edge detection
    m_DoneTrigger.AdvanceBlock();
    UpdateParams();
    m_TriggerAttackIn.ExecuteBlock(
        [&](int start, int end) { /* fired BEFORE this range */ },
        [&](int start, int end) {           // a trigger landed in here
            m_EnvState.CurrentSampleIndex = 0;
            AdEnvelope::GetNextEnvelopeOutput(m_EnvState, start, end,
                                              m_FinishedFrames, *m_Out);
            for (int frame : m_FinishedFrames)
                m_DoneTrigger.TriggerFrame(frame);   // re-fire at real frames
            m_AttackTrigger.TriggerFrame(start);
        });
}
```

`AvbTrigger` needs exactly: `AdvanceBlock()`, `TriggerFrame(int)`,
`ExecuteBlock(PreFn, TriggerFn)`. Sample-accurate triggering is then nearly free,
and the sequencer in AVB.9 depends on it — a 16th-note at 140 BPM lands on the
same sample every time, which is what makes a loop grid sound tight instead of
"nearly right".

**One node type, two rates, chosen by pin type.** MetaSounds' documented rule:
*"Operations on Audio data types are executed at a per-sample level."* The same
`Add` node is per-sample on an `Audio` pin and per-block on a `Float` pin. LFO
outputs and most node parameters are explicitly *"clamped at block rate"*;
`Wave Player`'s `Play`/`Stop` are block rate while its `Sync` is sample rate.

Adopt this. It is elegant and nearly free — a node dispatches on the pin type it
was handed — and it means an LFO does not need a sample-rate path at all. Nodes
should therefore declare each pin's rate in the registry, and the compiler should
reject a link that would silently change it.

**A graph is done when it says it is done.** MetaSounds' `OnFinished` comes from
the `UE.Source.OneShot` interface, and *"If the On Finished node is not triggered,
the MetaSound will play indefinitely."* That is the whole lifetime model: silence
means infinite. Adopt the explicit `OnFinished` contract rather than inferring
duration from the longest branch.

---

## 2. File formats

### 2.1 `.wlavb` — an Auditory Visual Block (YAML)

YAML because `.wastescene`, `.wlblocks`, `.wlbtree` and `project.wasteland` all
already are, and yaml-cpp is vendored.

```yaml
schema: 1
name: DoomArray
output: Stereo              # Mono | Stereo — a field, not a runtime choice

# Engine-facing inputs. Gameplay writes these at trigger time.
inputs:
  - { name: Trigger,    type: Trigger, default: null }
  - { name: Frequency,  type: Float,   default: 55.0 }
  - { name: Amplitude,  type: Float,   default: 1.0 }
  - { name: BranchDrive,type: Float,   default: 0.65 }
  - { name: OnFinished, type: Trigger, direction: out }

# Graph-internal state. NOT gameplay-writable — see §2.2.
variables:
  - { name: phase,   type: Float, default: 0.0 }
  - { name: drive,   type: Float, default: 0.0 }

nodes:
  - { id: n1, type: Sine,   x: 0,    y: 0 }
  - { id: n2, type: Noise,  x: 0,    y: 180 }
  - { id: n3, type: Waveshaper, x: 300, y: 60 }
  - { id: n4, type: AudioBusWriter, x: 900, y: 100 }

# Bus sends. REAPER's three taps: pre-FX, post-FX, post-fader.
sends:
  - { from: n3, to: bus_Space, mode: PostFX,   gain: 0.4 }
  - { from: n4, to: bus_Delay, mode: PostFade, gain: 0.25 }

links:
  - { from: { node: n1, pin: Out },   to: { node: n3, pin: In } }
  - { from: { node: n1, pin: Phase }, to: { node: n1, pin: PhaseIn } }  # feedback ok
```

**Pins are addressed by stable string ID, never index.** Same rule as
`.wlblocks`, and for the same reason: adding a pin to a node must not break every
saved graph authored before the change. This is the single easiest thing to get
wrong and the hardest to undo, because the damage only appears in graphs authored
earlier.

**Unknown node types and unknown pins must error on load, never silently drop.**
A graph that quietly loses a node is worse than one that refuses to load.

### 2.2 Inputs vs Variables — the distinction that prevents a whole bug class

Copied from MetaSounds, and worth the trip:

- **Inputs** are gameplay-writable during playback (`GetInput` / `SetOutput`
  nodes, driven from `AudioSourceComponent` or a track).
- **Variables** are graph-internal mutable state, **not** gameplay-visible, and
  *changes are not registered while the graph is running*. A variable written by
  a node in the same block cannot be read by another node in that block unless
  read through **`GetDelayedVariable`**, which reads one block late to break the
  cycle.

Without that rule, a `drive` variable written on trigger and read by a waveshaper
in the same block reads last block's value — an off-by-one-block bug that sounds
like a subtle timing smear and is miserable to find.

### 2.3 `.wltrack` — a track project (YAML)

REAPER's model, which is the right one, is four nested summing stages where
**every stage behaves identically in terms of summing**:

```
Project
 └─ Track[]                    (-1 = master)
     ├─ parent: <track>        folder parent; nests arbitrarily deep
     ├─ mainSend: bool         "sends audio to parent"
     ├─ fx: FxSlot[]           per-track effect chain
     ├─ envelopes: Envelope[]  addressed by name, not index
     ├─ sends: Send[]
     └─ Item[]                 the region on the timeline
         ├─ position, length   SECONDS (see timebase)
         ├─ volume, fadeIn, fadeOut, shape, curvature
         ├─ loopSource, mute, timebase
         └─ Take[]             layered; activeTake selects
             ├─ source         AVB graph ref | clip name | recorded PCM
             ├─ sourceOffset   start-in-source, seconds
             ├─ playRate, pitch(semitones), preservePitch
             └─ fx: FxSlot[]   PER-TAKE fx  ← the REAPER sampler trick
```

**The item/take split is the trick to copy.** Position and length live on the
*item*; source offset, play rate and pitch live on the *take*. Every editing
operation — trim, slip, stretch-to-fit, reverse, comp — composes from those two
records without special cases.

**Timebase is a field, not a rendering mode**: `Time` or `Beats`, per project,
per track and per item. Beats applies to position/length/rate.

### 2.4 Envelopes are a first-class object with shaped points

A point is `(time, value, shape, tension)` — not `(time, value)`. REAPER's point
struct is exactly that, and the extra two fields are what make an envelope feel
like a musical instrument rather than a staircase.

One envelope object, addressed by name, usable per-track **and** per-take:

| Envelope | Applies at |
|---|---|
| `Volume` | post-FX tap |
| `VolumePre` | pre-FX tap |
| `Pan` | post-FX tap |
| `Width` | stereo width, `-1..1` |
| `Mute` | step or ramped |
| `FX:<param>` | any effect parameter |

REAPER's naming detail worth stealing: the **pre-FX** variant is the unversioned
name (`Volume`) and the **post-FX** variant got bumped (`VolumePost`). The name
encodes the tap point, so the pre/post distinction is visible in the UI instead of
being a mode flag.

**Automation modes**, same enum REAPER uses: `Trim/Off, Read, Touch, Write,
Latch`, plus `-1` = inherit from track. Latch is the one people miss — after touch,
automation keeps driving the fader until release.

**Automation items: an envelope lane can contain clipped sub-ranges.** This is
REAPER's most distinctive modelling idea and it is cheap. An envelope's lane holds
`AutomationItem`s — exactly like items inside a track — each with its own
`SourceOffset`, `PlayRate`, `Baseline [0,1]`, `Amplitude [-1,1]`, `Loop`, `Mute`,
and a **`PoolId`** (*"edits are propagated to all other automation items that share
a pool id"*).

One feature, four things bought: volume/pan/filter modulation, automation
read/write modes, take comping, and MIDI-CC recording. Ship it in AVB.9.

**There is no routing-matrix object.** In REAPER it is a *view* over
(source track, category, send index, dest track) tuples. Do the same — a matrix
view over the send list, not a second data structure that has to be kept in sync.

**Track mute is applied twice**, once at the top (so items are effectively muted)
and once at the end (so a synth fed via a receive is muted too), and solo
overrides mute. Worth copying: applying mute only at the top leaves instrument-fed
tracks audible, which is exactly the bug this double application exists to prevent.

### 2.5 Persistence: plain text, serializable from day one

REAPER's project format is a tagged plain-text chunk tree:

```
<ITEM
  POSITION 2
  LENGTH 10.2
  <SOURCE WAVE
    FILE "shotgun_fire.wav"
  >
>
```

Two record types: a **struct** (one line, `TOKEN value…`) and a **block**
(`<TOKEN …> … >`, nestable). It is diffable, hand-editable, and REAPER exposes the
same thing at runtime via `GetTrackStateChunk` / `GetItemStateChunk`.

Give every AVB and track object `Serialize()` / `Deserialize()` **from the first
commit**. It costs almost nothing and it is what makes undo, diffing, headless
tools, and the AI authoring path in AVB.10 possible. `P_EXT:`-style namespaced
extension keys are worth copying too — they let a later feature attach data
without a format migration.

---

## 3. The node library

Node names and parameter sets follow MetaSounds' Function Nodes Reference, so
existing audio knowledge transfers. ~90 nodes is MetaSounds' full count; we ship
in three waves and **stop at the first wave that satisfies the default
instrument**.

### Wave 1 — the DOOM array's actual contents (AVB.2)

Nothing here is speculative; every node exists because the four branches need it.

| Category | Nodes |
|---|---|
| Generators | `Sine`, `Noise`, `LFO`, `Triangle`, `Saw`, `Square`, `Flanger`, `Diffuser` |
| Envelopes | `AD Envelope` (with `Looping`, so it doubles as an LFO), `ADSR Envelope` (separate `TriggerAttack`/`TriggerRelease`, curve factors 1.0=linear, <1 log, >1 exp) |
| Shaping | **`WaveShaper`** (curve enum: Sine / InverseTangent / HyperbolicTangent / Cubic / HardClip) + `Amount`, `Bias`, `OutputGain` |
| Filters | `Biquad Filter`, `State Variable Filter` (LP/HP/BP/BS out), `One-Pole Low Pass`, `One-Pole High Pass`, `Ladder Filter` (Moog-style VA) |
| Dynamics | `Compressor` (Ratio, Threshold dB, Attack, Release, Lookahead, Knee, **Sidechain input**, Envelope Mode MS/RMS/Peak, GainEnvelope out), `Limiter`, `Decibels↔Linear Gain` |
| Delay | `Delay` (dry/wet/feedback/max), `Stereo Delay` (Normal / Cross / Ping Pong) |
| Mixing | `Stereo Mixer` (gains **unclamped**, so they can attenuate and invert), `Mono Mixer`, `Gain`, `Stereo Panner` (Equal Power / Linear), `Mid-Side Encode/Decode` |
| Triggers | `Trigger Repeat`, `Trigger Once`, `Trigger Counter`, `Trigger Delay`, `Trigger Route`, `Trigger Sequence`, `Trigger Select`, `Trigger Toggle`, `Trigger On Threshold`, `Trigger Filter`, `Trigger Any`, `Trigger Accumulate`, `Trigger Compare`, `Trigger Control`, `Trigger Pipe`, `Trigger On Value Change` |
| Math | `Add`, `Multiply`, `Subtract`, `Divide`, `Clamp`, `Min`, `Max`, `Modulo`, `Power`, `Abs`, `Map Range`, `Linear To Log Frequency`, `Filter Q To Bandwidth` |
| IO | `OnPlay`, `OnFinished`, `GetInput`, `SetOutput`, `Get/Set Variable`, `GetDelayedVariable`, `AudioBusReader`, `AudioBusWriter` |

Three trigger semantics worth copying precisely, because the wrong one is
subtly wrong:

- **`Trigger Delay` vs `Trigger Pipe`** — Delay considers only the **most recent**
  trigger, so a new trigger *resets* the timer. That is a retrigger/debounce
  primitive, and it is not the same node as Pipe.
- **`Trigger Repeat`** is *"sample-accurate and arbitrarily-precise rate"* — this
  is the sequencer's only clock (§3, Wave 3).
- **`Trigger Once`** has `Start Closed`, so a graph that should not fire on the
  first block says so explicitly.

**Anti-correlation across instances.** MetaSounds' `Random Get` has an
`Enabled Shared State` flag — *"state is shared across instances of this MetaSound
to avoid playing the same exact variations at the same time."* Twenty enemies
firing the same variation on the same frame is the classic procedural-audio tell.
This one flag fixes it, and it is cheaper than the N=8 variant baking in §4.3.

### Wave 2 — instruments and the metal voice (AVB.3)

| Node | Why |
|---|---|
| **`ModalStrike`** | inharmonic modal bank. `F1`, `Ratios` (free-free bar default `1 : 2.76 : 5.40 : 8.93 : 13.34`), `ModeCount` 4–24, `T60`, `Damping`, `ExciteLengthMs`, `ExciteTiltDbPerOct`, `StrikePosition`, `PitchDriftCents` |
| **`CombBank`** | cascaded irrational-ratio combs (default ratios `1, 1.414, 1.732, 2.236`), `Feedback`, `LoopLp`, fractional delay. The "tin/can" voice |
| **`WavePlayer`** | sample-accurate `Play`/`Stop`, `StartTime`, `Pitch` **in semitones**, `Loop` **togglable mid-playback**, `LoopStart`, `LoopDuration` (`-1` = whole file), outputs `OnFinished`, **`OnNearlyFinished`**, `OnLooped`, `PlaybackLocation` |
| **`BiquadBank` / `Resonator`** | cheap 2-pole resonator: `y[n] = x[n] − 2r·cos(ω)·y[n−1] + r²·y[n−2]`, `ω = 2π·f₀/f_s`, `r = exp(−π·BW/f_s)`, `Stretch` 1.0–1.15 for inharmonicity |
| **`GrainDelay`** | delay-line granular: `GrainSpawn` trigger, `GrainDelay` 0–2000 ms, `GrainDuration`, `PitchShift` semitones, `GrainEnvelope` (Gaussian / Triangle / Down / Up / ExpDecay / ExpAttack), `MaxGrainCount`, `Feedback` |
| **`RingMod`**, **`Bitcrusher`**, **`SampleAndHold`**, **`DelayPitchShift`** | the remaining array-flavoured tools |

### Wave 3 — the track editor (AVB.9)

`Sequencer` (BPM → `Trigger Repeat` → `Trigger Counter` → `Trigger Sequence`),
`NoteInput`, `VelocityCurve`, `ZoneMap`, `InstrumentHost`, `RecordArm`,
`ClipRecorder`, `RendersStems`.

### What is deliberately **not** a node

Following the research, and naming it now so it is not rediscovered in month
three:

- **No reverb or convolution node.** They are bus effects (§0.2, AVB.5/6). A
  graph reverb is the wrong shape for what a reverb actually is.
- **No Channel-Agnostic Types.** Epic's own audio director calls CAT a *critical
  flaw* they are still fixing two engine versions later. One explicit channel
  format (`Mono` / `Stereo`) now; an N-channel variant later if it is ever needed.
- **No JIT graph compilation / proxy-data split.** Ship a graph interpreter that
  walks nodes per block over a flat `std::vector<AvbNode*>`. Measure before
  optimizing.
- **No MetaSound Presets** (parent-graph re-parameterisation with bidirectional
  version tracking). Add a flat "instance params" asset instead, and revisit
  presets only when there are 10+ near-duplicate graphs.
- **No `Pages` / relative render cost / voice limiting.** Needs cost metrics and
  platform profiles.
- **No output watching** (audio→game reverse channel). Cheap to skip, loses little.
- **No time-stretch / elastic audio.** REAPER's `I_STRETCHFLAGS` tonal/transient
  modes are a project in themselves. Ship `playRate` + `pitch` and revisit.

---

## 4. The default instrument: `DoomArray`

This is the "default to an artificial version of Mick Gordon's DOOM array"
requirement, built from Wave 1 + Wave 2 nodes only.

### 4.1 Signal flow

```
                        ┌─► [A] Waveshaper(hard, asymmetric) ─► Bitcrusher ─┐   transient
                        │        gated by input amplitude                  │
  Sine(Frequency) ──┬───┼─► [B] Bitcrusher ─► Phase(4 taps) ─► Compressor ─┤   body
                    │   │        opens on QUIET inter-note content        │
  Noise, band-      │   ├─► [C] Delay(saturating, tape-style) ─► Spring ──┤   space
  matched to ───────┼───┤                                              │
  Frequency         │   └─► [D] Resonator(feedback loop) ─► Limiter ─────┤   runaway
                    │            self-oscillating; HARD limiter          │
                    └───────────────────────────────────────────────────┘
                                     │
              cross-coupling: each branch's envelope modulates the others' gain
                                     │
                          Sum ─► Biquad EQ (fixed tilt) ─► Compressor ─► Out Stereo
```

**Branch roles, not just sounds.** Gordon used splitters and compressors precisely
so branches respond to *different parts of the amplitude range* — that is what
makes one input sound like a moving instrument rather than four prints. Encode
that as per-branch envelope followers, and cross-couple them.

**Band-matched noise.** Sourced and specific: Gordon's sub melody was *"combined
with white noise that matched the frequency of the sub and put it through some
distortion."* The noise rides the pitch. Implement as `Noise` → `Biquad` bandpass
tracking `Frequency`, not a fixed white noise.

**The struck-metal layer, clearly labelled ours.** `ModalStrike` presets, all
routed through the *same* four branches so they inherit the array's timbre
signature:

| Preset | f₁ | Ratios | Modes | T60₁ | Upper-mode τ | Excite |
|---|---|---|---|---|---|---|
| `ANVIL_HIT` | 420 Hz | bar `1, 2.76, 5.40, 8.93, 13.34` | 14 | 0.9 s | `1/(0.8 + 3.0e-6·f²)` | 1.2 ms noise, +4.5 dB/oct, centre strike |
| `TIN_CAN` | 310 Hz | combs `1, 1.414, 1.732, 2.236, 2.646` | 5 combs | g=0.93 | loop LP a=0.82 | 0.8 ms, +6 dB/oct |
| `GONG_WASH` | 95 Hz | plate `1, 1.59, 2.14, 2.30, 2.65…` | 24 | 4.0 s | `1/(0.17 + 1.4e-6·f²)` | 3 ms, mild tilt |

Modal decay weighting is the part that makes it read as metal: `T60_n = 6.91·τ_n`,
`A_n ∝ 1/√f_n`, and **high modes must die first** (Rayleigh-ish damping,
`τ_n = 1/(d₀ + k·f_n²)`) — that decay-ordering is what produces the "shimmer that
fades into a hum". Mode count saturates perceptually by ~24; default 14.

**Pre-bake, do not runtime-synthesise, for anything gameplay-critical.** Render the
metal presets offline to PCM at load. Keep the modal synth for the musical layer
where pitch variation matters. Any runtime noise **must** come from a seeded PRNG,
or replays and tests break.

### 4.2 The mixing rule that is a hard requirement, not a preference

From Gordon on Doom Eternal, and it is a *gameplay* constraint:

> *"a big sustained droning note… just disappears into the background. But if
> there's lots of 'pop, pop, pop' all happening all the time. That's what gives it
> the clarity."*

**Short syncopated bursts, not sustained drones**, specifically so gunfire and
explosions are not masked. In an FPS this is not an aesthetic note — it decides
whether the player can hear an enemy behind them. Encode it as a lint rule on
`DoomArray` presets (AVB.2 test) and as a default-tempo/pattern in the shipped
`.wltrack`.

### 4.3 One instrument bus, so timbre stays a family

The single highest-leverage idea from the research, and it is the real mechanism
behind Gordon's stem reuse: **route every procedural voice through the same
four-branch matrix.** Then any two sounds are related by construction, and one
strike automatically becomes the track's vocabulary. No resampling one hit
everywhere — one instrument, many voices.

Corollary: one-shot variant baking. Pre-render N pre-corrupted variants per metal
strike at load (N=8 costs 8 short buffers) and get 8 distinct voices for free.

---

## 5. Synthetic instruments (REAPER's trick, done properly)

The requirement is "custom artificial synthetic instruments using different
effects". REAPER's answer, and the honest finding from the research, is that it
is **not a special subsystem** — it is a sampler node in an FX chain.

**The whole trick, in one sentence:** *"Each RS5K is a synth voice, and the FX
chain is the synth."* Placing a reverb or compressor **after** the sampler means
every note gets its own effect instance. That is REAPER's per-note FX.

So an AVB instrument is:

```
Instrument
 ├─ zones:  [ { keyRange: 36..36, source: ANVIL_HIT, gain, pan, env }   ]
 │           [ { keyRange: 40..40, source: TIN_CAN,  ... }              ]
 ├─ voicePolicy: { maxVoices, steal: Oldest | Quietest, probability, roundRobin }
 └─ perVoiceChain: [ Waveshaper, Delay, Compressor, … ]   ← owned per voice
```

`InstrumentHost` node instantiates one voice per active note; each voice owns its
own copy of `perVoiceChain` state. That is the direct analogue of a JSFX sampler
in a per-track FX chain, and it is the piece that makes AVB instruments feel like
instruments rather than like one-shots.

Parameters worth having, all straight from ReaSamplomatic5000: `NoteStart`/
`NoteEnd` (zone key range), `PitchOffset` + `PitchBend`, `Attack`/`Decay`/
`Sustain`/`Release`, `VelocityMin`/`VelocityMax` with a gain curve,
`MaxVoices`, **`Probability`** (random drop, for realistic repetition),
**`RoundRobin`** (alternate takes per hit), `Loop` + `Xfade` + `LoopStartOffset`,
`ResampleMode`.

---

## 6. Phasing

Every phase is independently shippable and headlessly verifiable. **The runtime
is proven before any editor UI exists** — the runtime is the hard part, the canvas
is the easy part, and building the canvas first means debugging two new things at
once with no way to tell which is at fault.

```
AVB.0  test harness + baseline   ─┐
AVB.1  device (miniaudio)         │  no editor UI anywhere
AVB.2  DSP primitives (Wave 1)   │  all headless
AVB.3  modal / comb / grain       │
AVB.4  graph model + compiler     │
AVB.5  buses + send matrix        ┘
AVB.6  bus effect chains + reverb
AVB.7  runtime + triggers    ── needs the compiler to work
AVB.8  node library + DoomArray
AVB.9  track model + sequencer ── needs buses
AVB.10 editor panels         ── needs the runtime to already work
AVB.11 record + stem export
```

### AVB.0 — Rebuild the test harness, re-baseline audio

**Objective:** make the project's verification bar real again (§0.4).

- Create `tests/headless/` with a runner, explicit `PYTHONPATH` to the Release
  output, and `test_audio.py` re-derived from what `Mixer.h`/`AudioClip.h`/`Mixer.cpp`
  actually guarantee.
- Assert real DSP, not survival: bit depths and float WAVs, the 8-bit unsigned
  centre (the off-by-128 that turns silence into buzz), opposite-phase stereo
  cancelling on downmix (proving average, not sum), malformed and truncated
  headers, a `data` chunk claiming more than the file holds (clamped, not
  overread), mixing and centring, negative pitch clamping, one-shot retirement vs
  loop wrapping, fades, all four steal/drop cases, attenuation monotonicity and
  exact endpoints, stereo mirror symmetry, NaN guards for a degenerate listener
  basis, clipping at full scale, extreme pitch.
- Record the count as the new baseline. Everything after this plan is measured
  against it.

**Gate:** do not start AVB.1 until this is green. A 15-phase DSP plan with no
executable verification is a wish list.

### AVB.1 — Audio device backend

**Objective:** make sound actually reach speakers, and open a capture path.

- Vendor **miniaudio** into `Wasteland/vendor/miniaudio/`, add to `premake5.lua`
  with the same `filter "files:vendor/**.cpp" flags { "NoPCH" }` treatment
  ImGuizmo and `TextEditor.cpp` already get. Compile it **first, as its own step**,
  in Release and Debug, so a version or platform issue surfaces in the first hour.
- `Audio::AudioDevice` — a playback thread that pulls from the mixer, plus a
  capture callback. It sits **behind** the mixer, never inside it (§"the split is
  the whole design" in `Mixer.h`).
- Assert on the **actual** buffer rate. A device that opens at 44.1 kHz when the
  mixer assumed 48 kHz is a resampling bug that sounds like pitch drift; detect
  it, do not assume it.

**Why first:** every later phase's acceptance test is "it makes the right sound",
and right now the engine cannot make a sound at all.

### AVB.2 — DSP primitives

**Objective:** Wave 1 of the node library as standalone, testable processors.

- `Wasteland/Avb/AvbDsp.h/.cpp` — `Biquad`, `StateVariableFilter`, `OnePole`,
  `LadderFilter`, `WaveShaper` (curve enum + `Amount`/`Bias`/`OutputGain`),
  `AdEnvelope` (with `Looping`), `AdsrEnvelope`, `Compressor` (with **Sidechain**
  input and `GainEnvelope` output), `Limiter`, `Delay` (fractional),
  `StereoDelay` (Normal/Cross/PingPong), `StereoPanner` (Equal Power/Linear),
  `StereoMixer`, `Lfo`, `Sine` (with the `Sync` trigger and `Glide`),
  `Noise`, `BiquadEq`.
- State must live in the node, never in locals — an envelope that resets each
  block is a click generator.

**Tests, per primitive, asserting on real output buffers:**
- Biquad lowpass at 1 kHz on a 1 kHz sine attenuates; at 10 kHz does not.
- `WaveShaper` with `HardClip` at drive 1 clamps to ±1; at drive 0.1 is ≈ identity.
  With `Bias` set, output has a measurable DC offset.
- ADSR: attack reaches peak within 5% of the authored time; release returns to
  **exactly** 0 and stays there (a release that asymptotes leaves a DC step).
- Compressor with a sidechain input ducks on the sidechain and **not** on its own
  input — that is the DOOM cross-coupling primitive, so it is tested directly.
- Feedback `Delay` above the stability threshold must be **caught by a guard**, not
  produce NaN. Self-oscillating branch D depends on this.

### AVB.3 — Modal, comb and granular voices

**Objective:** the struck-metal layer.

- `ModalStrike` — modal bank with inharmonic ratios, `A_n ∝ 1/√f_n`,
  `τ_n = 1/(d₀ + k·f_n²)`, excitation 1–3 ms noise with a `+3..+6` dB/oct tilt,
  strike position `A_n ∝ |sin(nπx₀/L)|`, and 2–8 cent downward drift on the top
  modes over their final 30% of decay.
- `CombBank` — cascaded combs at mutually irrational ratios, fractional delay
  via allpass, one-pole lowpass inside the loop.
- `Resonator` — the 2-pole form, with `Stretch` 1.0–1.15 for inharmonicity.
- `GrainDelay` — delay-line granular, Gaussian/Triangle/Down/Up/ExpDecay/ExpAttack
  grain envelopes, `MaxGrainCount` 1–100.

**Tests (these are the ones that prove it is metal and not a pad):**
- Free-free bar ratios produce peaks at the ratios, not at integer multiples.
  Measure the spectrum, assert `f₂/f₁ ≈ 2.76` within 5%. A harmonic generator
  fails this test, which is the point.
- High modes decay **faster** than the fundamental: assert `T60₂₀ < T60₁`.
  Amplitude falls with frequency: assert `A(f=4f₁) < A(f₁)`.
- Two different `StrikePosition` values produce measurably different spectra from
  the same mode table (centre vs off-centre suppresses odd modes).
- `CombBank` with irrational ratios does **not** produce a periodic waveform:
  assert the autocorrelation has no dominant lag other than 0 over a 200 ms
  window. Rational ratios *do* — that contrast is the property.
- No NaN/Inf at `MaxGrainCount=100` with feedback at maximum.

### AVB.4 — Graph model, serializer, compiler, validator

**Objective:** `.wlavb` in, validated compiled graph out, no editor, no GL.

- `Avb/Graph.h/.cpp` — `AvbGraph`, `AvbNode`, `AvbLink`, `AvbLiteral`,
  `AvbVariable`, `AvbPinType` (`Trigger, Audio, Time, Float, Int, Bool, Enum,
  String, AssetRef`), `AvbPinDir`, `AvbSend`.
- `Avb/GraphSerializer.h/.cpp` — YAML via yaml-cpp, round-trip exact, and
  **error rather than drop** on unknown node type / unknown pin / unknown send.
- `Avb/NodeRegistry.h/.cpp` — one table: type id → pins + category + version +
  factory. `AliasOf` support (node ids are a file format; a rename must not break
  saved graphs — the existing `NodeRegistry.h` already does this well, copy the
  idea).
- `Avb/Validator.h/.cpp` — rejects, each error **naming the node and pin**:
  - `Trigger` wire into a data pin, or the reverse
  - incompatible types
  - a `Trigger` fan-out to zero receivers where one is required (a dangling
    trigger is silence that looks correct in the editor)
  - a required input pin with neither link nor literal
  - a link to a deleted node
  - a cycle in the **data** graph that is not a declared feedback edge
  - an unknown node type or pin
- `Avb/Compiler.h/.cpp` — validate, resolve, flatten to a flat
  `std::vector<AvbNode*>` in a **deterministic topological order**. Ties broken by
  node id, never by pointer or hash order.

**Tests:**
- Round-trip a graph with every field populated; re-read is byte-identical.
- One case per validator rule, each asserting the message names the node.
- Unknown node type on load → error naming the type and the file. No partial load.
- **Compile the same graph twice → byte-identical output.** A non-deterministic
  emitter (unordered iteration, a timestamp, a pointer-derived name) makes every
  diff and every golden test a lie. One assertion catches the whole class.

### AVB.5 — Buses and the send matrix

**Objective:** the layer that makes reverb possible later (§0.2).

- `AvbBus` — an audio bus with an effect chain and the three REAPER taps:
  **pre-FX, post-FX, post-fader (post-pan)**. A bus is both a source and a
  destination; there is no separate "return" concept, exactly as in REAPER.
- Folder semantics: a track marked `mainSend` routes to its parent; nesting is
  arbitrary depth. Any action on a folder applies to the whole submix.
- `AudioBusReader` / `AudioBusWriter` — with these two nodes, MetaSounds' actual
  architecture exists, and they are ~20 lines each.

**Tests:** a bus with a send at each of the three taps produces measurably
different output for the same input; a folder parent receives its children's sum;
muting a folder mutes the subtree; a send gain of 1.0 is exactly unity.

### AVB.6 — Bus effect chains and reverb

**Objective:** the effects that are deliberately not graph nodes.

- `ConvolutionReverb` (IR from a WAV), `SubmixDelay`, `Compressor`, `Eq`,
  `MultibandCompressor` on buses.
- **Schroeder/FDN reverb as the default when no IR is supplied**, so the engine
  has a usable space without shipping an impulse response.

**Test:** a 1 kHz sine through a reverb bus decays monotonically in RMS over
2 s; a nil IR produces a pass-through (a nil-IR crash is the classic bug); a bus
with no sends in it is exactly unity.

### AVB.7 — Graph runtime and triggers

**Objective:** the interpreter. The proof is that a graph produces correct samples.

- `Avb/Runtime.h/.cpp` — block loop, node ordering from the compiled graph,
  per-node state, and the `AvbTrigger` primitive with
  `AdvanceBlock()` / `TriggerFrame(int)` / `ExecuteBlock(Pre, Trigger)`.
- Block size per §1.3, with `GetActualBlockRate()`.
- Determinism: the same graph + same trigger times + same seed → **bit-identical
  output** across runs.

**Tests:**
- A `Sine` → `Gain` → out graph at 48 kHz produces a sine whose measured zero
  crossings match the requested frequency within 0.1%.
- A `Trigger` on frame 137 of a 480-frame block starts the envelope at **exactly**
  frame 137. Assert on the first non-zero output sample index. This is the single
  test that proves sample accuracy, and it is worth writing first.
- Two `Trigger`s inside one block both fire, at their own frames.
- Rendering 1000 blocks twice is bit-identical (no uninitialised state).

### AVB.8 — Wave 2 nodes, and `DoomArray`

**Objective:** the full default instrument, shipping.

- `WavePlayer` with sample-accurate `Play`/`Stop`, semitone `Pitch`, mid-playback
  `Loop` toggle, and `OnFinished` / **`OnNearlyFinished`** / `OnLooped` outputs.
  `OnNearlyFinished` fires the block *before* the end and exists so variations can
  be swapped gaplessly — steal it.
- Register `DoomArray.wlavb`, `DoomArray_Anvil.wlavb`, `DoomArray_TinCan.wlavb`,
  `DoomArray_GongWash.wlavb` in `DemonCore-Editor/assets/avb/`.
- Bake the metal presets to PCM at load with a **seeded** PRNG.

**Tests:**
- `DoomArray` renders non-silent, non-NaN, non-clipping output for a 4-second
  block sequence at 48 kHz.
- Branch roles: branch B's contribution is measurably **larger during the gap
  between notes** than during the attack. That is the sourced behaviour, and it
  is the difference between "four effects in a row" and the actual instrument.
- The cross-coupling is real: raising `BranchDrive` on branch A changes branch B's
  output. If it does not, the sidechain is not wired and the graph is inert.
- **The Doom array ships as the default instrument** — assert that creating a new
  instrument with no arguments yields `DoomArray`, and that its graph validates.
- Lint (§4.2): a preset authored as one long sustained drone **fails** the
  burst-density check with an error naming the preset.

### AVB.9 — Track model, sequencer, and the instrument host

**Objective:** the REAPER document, headless.

- `Avb/Track.h/.cpp` — `Track`, `Item`, `Take`, `Envelope` (points with shape and
  tension), `Send`, `FxSlot`, `Timebase`.
- `Avb/Sequencer.h/.cpp` — **no transport node in the library**; per MetaSounds,
  the clock is assembled from `BpmToSeconds` + `TriggerRepeat` + `TriggerCounter` +
  `TriggerSequence`. Build the same way.
- `Avb/Instrument.h/.cpp` — zones, voice policy, per-voice FX chain.
- `.wltrack` serializer, plain-text chunk style (§2.5).

**Tests:**
- **Cadence by timestamp, not by window.** Record every trigger's frame index and
  assert on the *differences*: at 140 BPM a 16th note is 4285.71 frames at
  48 kHz; assert each gap equals that within one frame, over 64 events. Asserting
  "a hit happened within 0.3 s" is window-sampling — it passes or fails depending
  on where the window falls relative to the boundary, and proves nothing.
- Trimming an item changes `length` only. Slipping changes `sourceOffset` only.
  Composing those two records is the whole editing model, so test both.
- An envelope with 3 shaped points evaluates to the expected curve at the
  midpoint, and its value is a **real per-block gain** at the right tap (test
  pre-FX vs post-FX differ when the effect changes gain).
- `maxVoices: 4` with 8 simultaneous notes → exactly 4 voices sounding, and the
  steal policy chose the *quietest*, not the oldest, when that is configured.
- Round-trip a `.wltrack` with every field populated; byte-identical.
- Two different `StrikePosition`s and two different zone key ranges produce
  measurably different output from the same instrument.

### AVB.10 — Editor panels

**Objective:** the UI, at last.

- **Vendor `imgui-node-editor`** (thedmd, MIT) into `Wasteland/vendor/`, add to
  `premake5.lua` with the existing `NoPCH` filter, compile **first as its own
  step** in Release and Debug. This is shared with the unwritten VB.5, which gets
  it for free — say so in the commit.
- `DemonCore-Editor/src/Panels/AuditoryBlocksPanel.{h,cpp}` — palette by
  category, canvas, details, validator error list pinned to nodes, and a
  **trace toggle** that logs each node as it executes. Budget the trace with the
  runtime, not after: a graph that misbehaves with no per-node trace is unfixable
  by the people who have to fix it.
- `DemonCore-Editor/src/Panels/TrackEditorPanel.{h,cpp}` — REAPER-shaped:
  track headers, ruler with timebase toggle, item lanes with drag/trim/snap,
  envelope lanes, send matrix, transport, and the node-graph panel docked beside
  it so an instrument's graph and its timeline are visible together.
- Content browser: `.wlavb` and `.wltrack` extension dispatch next to the existing
  `.py` check.

**Verification — screenshots of the rendered UI at 4K, not a clean build log.**
Place a node, drag a wire, compile, read the compiled output, run it, see the
entity move. A clean build proves nothing here.

### AVB.11 — Recording and stem export

**Objective:** the last piece of "record tracks".

- `Avb/Recorder.h/.cpp` — capture from the miniaudio input, write
  `Audio::AudioClip` WAV, create a `Take` with `sourceOffset` 0.
  **Requires an `AudioClip::SaveWav`, which does not exist** — the engine has a
  WAV reader and no writer. That is the actual blocker; add it here and test it
  against `LoadWav` with a round-trip that preserves peak and duration.
- Record arm, punch in/out, and comp: takes on one item, `activeTake` selects,
  `playAllTakes` for the layered option.
- Stem export: render the render-matrix subset, with `$project` / `$region` /
  `$track` filename wildcards, `\` creating subdirectories. A tiny feature with
  enormous leverage — `$project\$region-$track` puts each region's files in their
  own folder.
- Offline render must be **sample-identical** to realtime for the same input.

**Tests:** record 1 s of a known tone → the written WAV's peak and duration match
within tolerance; a full round-trip `LoadWav` → mix → `SaveWav` → `LoadWav`
preserves samples; stem export with `$project\$region-$track` produces the
expected directory tree; offline render is bit-identical to realtime.

---

## 7. Files

| Path | Contents |
|---|---|
| `Wasteland/src/Wasteland/Avb/AvbDsp.h/.cpp` | Wave 1 DSP primitives |
| `Wasteland/src/Wasteland/Avb/AvbVoices.h/.cpp` | `ModalStrike`, `CombBank`, `Resonator`, `GrainDelay` |
| `Wasteland/src/Wasteland/Avb/AvbCapture.h/.cpp` | `AvbWaveWriter` — dump any node's output to WAV for offline listening/FFT |
| `Wasteland/src/Wasteland/Avb/Graph.h/.cpp` | `AvbGraph`, `AvbNode`, `AvbLink`, pin types |
| `Wasteland/src/Wasteland/Avb/GraphSerializer.h/.cpp` | `.wlavb` YAML |
| `Wasteland/src/Wasteland/Avb/NodeRegistry.h/.cpp` | node table + aliases + versions |
| `Wasteland/src/Wasteland/Avb/Validator.h/.cpp` | all rules, each naming the node |
| `Wasteland/src/Wasteland/Avb/Compiler.h/.cpp` | deterministic flatten + topo sort |
| `Wasteland/src/Wasteland/Avb/Runtime.h/.cpp` | block loop, `AvbTrigger`, partition |
| `Wasteland/src/Wasteland/Avb/Bus.h/.cpp` | `AvbBus`, three taps, folders |
| `Wasteland/src/Wasteland/Avb/Track.h/.cpp` | `Track`, `Item`, `Take`, `Envelope` |
| `Wasteland/src/Wasteland/Avb/Sequencer.h/.cpp` | BPM clock, `InstrumentHost` |
| `Wasteland/src/Wasteland/Avb/Recorder.h/.cpp` | capture, comp, stem render |
| `Wasteland/src/Wasteland/Audio/AudioDevice.h/.cpp` | **modify `AudioClip`** — add `SaveWav` |
| `DemonCore-Editor/src/Panels/AuditoryBlocksPanel.{h,cpp}` | node canvas |
| `DemonCore-Editor/src/Panels/TrackEditorPanel.{h,cpp}` | timeline |
| `DemonCore-Editor/assets/avb/*.wlavb` | `DoomArray` + metal presets |
| `DemonCore-Editor/assets/tracks/*.wltrack` | shipped demo track |
| `tests/headless/test_avb_*.py` | per-phase suites |
| `Wasteland/vendor/miniaudio/` | vendored device backend |
| `Wasteland/vendor/imgui-node-editor/` | vendored canvas (shared with VB.0/VB.5) |

`premake5.lua` needs: miniaudio in `files` + `includedirs` + the `NoPCH` filter;
the `Avb/**` glob is already covered by `"%{prj.name}/src/**.cpp"`.

`Audio::Mixer` and `Audio::AudioSystem` are **not modified**. `AudioClip` gains
`SaveWav` only.

**`AvbWaveWriter` is not a debug convenience, it is the test instrument.** Every
acceptance test in §6 asserts on spectra, decay times and trigger frame indices —
all of which need the rendered samples outside the process. MetaSounds has the
same node (`Wave Writer`, writes 1–8 ch to `Saved/AudioCaptures`). Build it in
AVB.2, not at the end, because the DSP tests in AVB.2 and AVB.3 are otherwise
unwritable.

---

## 8. Risks

**Nothing is audible until AVB.1.** The engine has a complete, well-tested mixer
and no way to hear it. Every phase after that is blocked on it, which makes
miniaudio the highest-leverage hour in this plan — and the reason AVB.0 must fix
the missing test harness first, or "it sounds right" is unverifiable.

**Sample-accurate triggering is easy to get subtly wrong.** Partitioning a block
at trigger frames and re-firing downstream triggers at real frame indices is a
small amount of code with a large blast radius. Write the "trigger on frame 137
starts at exactly 137" test **first** in AVB.7, before any other runtime test.

**Feedback branches self-destruct.** DoomArray branch D is a deliberate
resonator loop. It needs a limiter and a stability guard, and both need tests with
drive and feedback at maximum. Untested, this is a NaN generator that poisons
everything downstream of it.

**Determinism is not optional.** A runtime PRNG without a seed makes every
audio test flaky and every replay wrong. Seed it, and assert bit-identical
re-render.

**Buses before reverb, not after.** The temptation is to put a reverb node in the
graph because it is easier to demo. It is the wrong shape and it is what UE
deliberately does not do. Building it in the graph first means migrating it later,
and migrating audio DSP is exactly the kind of work that eats a phase.

**Python-performance analogue does not apply.** Unlike `.wlblocks`, AVB is
real-time DSP in C++, so there is no interpreter-overhead argument. The
performance question is voice count and per-node cost, and the honest answer is
"measure it" — a flat `std::vector<AvbNode*>` walked per block is fine for the
dozens of voices this project targets.

**Scope creep toward UE5 is the real risk.** MetaSounds took a team years. The
achievable goal is MetaSounds' *authoring experience* for a small engine's real
needs, and §3's "deliberately not a node" list is the line. Adding CAT, JIT
compilation, or preset propagation later is cheap; discovering mid-phase that the
graph format assumed one of them is not.

**Two node-canvas consumers.** AVB.7 and the unwritten VB.5 both need
`imgui-node-editor`. Vendor it once, and keep the two graphs' registries,
validators, compilers and formats **separate** (§1.2). The moment they share a
format, one of them has to become an execution graph or the other a flow graph,
and the type systems genuinely differ.

---

## 9. Verification standard

Inherited from `item-14-long-tail-systems.md`, and the same bar:

- Headless Python suites, no GL context, no window. Explicit `PYTHONPATH` to the
  Release output. **(Rebuilt in AVB.0 — see §0.4.)**
- **Real assertions on read-back values and output buffers, never "did not
  throw."** Generated or synthesised audio can be syntactically perfect, raise
  nothing, and be silent or wrong.
- **Determinism asserted:** same graph + same seed → bit-identical output.
- **Cadence asserted from recorded timestamps and the gaps between them**, not by
  sampling a window.
- MSBuild **Release and Debug**.
- Never call `Scene::OnRuntimeStop()` or drive `ScriptEngine::OnUpdateEntity`
  standalone — both are known to segfault for pre-existing reasons.
- Collidered entities must exist before `OnRuntimeStart()`.
- **Editor UI: screenshots of the actual rendered graph and timeline at 4K, not a
  clean build log.**

---

## 10. Open questions

1. **Should the "recording" be microphone input, or importing an existing audio
   file as a take?** The brief says "record tracks", which reads as capture, and
   AVB.11 assumes capture. Import is much cheaper and would let the track editor
   be useful before AVB.1. Worth confirming.
2. **Do tracks need a real instrument-per-track, or is one global instrument
   enough for the first cut?** Per-track instruments cost a voice pool per track.
3. **Is a 3D-positional procedural sound in scope?** `AudioSourceComponent` and
   the mixer's `ComputeSpatial` already do spatialisation for clips, but a
   *procedural* source has no `AudioClip` to be spatialised. Bridging AVB output
   into the mixer's spatial path is a real design question, not a detail.
4. **Target block size on the low end.** 480 frames at 48 kHz is MetaSounds'
   choice; for a game mixing many one-shots, a smaller block (higher rate) costs
   more scheduling and a larger one risks audible latency in a trigger-driven
   system. Worth measuring rather than copying.
5. **How much of the DOOM array's *pedal* character is worth reproducing?** The
   pedal names are community-transcribed from a GDC video and unverified. The plan
   uses *functional roles* (transient / body / space / runaway) rather than pedal
   models, which is both more defensible and more portable.
6. **Does `BpmToSeconds` feed `Trigger Repeat` at block rate or sample rate?**
   Unstated in Epic's docs. The plan assumes block-quantised beats with
   sample-accurate repeats, which is what AVB.9's cadence test will confirm or
   refute. If it is block-rate, a 16th note lands up to 480 frames late and the
   grid audibly drifts — so **assert the actual rate, do not assume it.**

---

## 10.1 What the research could not confirm

Carried forward so nobody re-derives it as fact:

- **No MetaSound "Blocks" or "Classes" exist as described.** No `UMetaSoundClass`
  in UE5 — the brief's term conflates **Presets** + **graph nodes** (reuse) with
  the separate `USoundClass`/`USoundMix`. Probably-not-real, moderately confident.
- **No MetaSound reverb, convolution, or distortion node.** Reverb and
  convolution route through submix buses; distortion is `WaveShaper`. High
  confidence.
- **No "On Tick" / "On Audio Generate" callbacks in MetaSounds** — one
  `Execute()` per block on the audio render thread. High confidence (confirmed by
  Epic's audio director, ADC 2025). Those names appear to be Max/MSP or Wwise.
- **REAPER's fade shape names (`0..6`)** are not enumerated in readable sources;
  only "0 = linear" is confirmed. Ship the enum, label the shapes honestly.
- **The DOOM pedal-by-pedal chain** is community-transcribed off a video. Unverified.
- **Gordon's exact settings** — he rejects the question himself: *"I can tell you
  the exact settings that I used, but it's not going to help unless you have that
  same intention"* (URM EP261 0:46:14). Treat every numeric parameter in §4 as
  **our** design choice, not a reconstruction.

---

## 11. Sources

**Mick Gordon / DOOM** (full detail with per-claim sourcing in
`C:\Users\rtoue\doom-array-dsp-research.md`):
- [GDC 2017 — DOOM: Behind the Music](https://www.youtube.com/watch?v=U4FNBMZsqrY)
- [DoomWiki — Doom (Original Game Soundtrack)](https://doomwiki.org/wiki/Doom_(Original_Game_Soundtrack))
- [Patchstorage — DOOM_SYNTH](https://patchstorage.com/doom_synth/) (an existing homage, useful cross-check)
- [Thumbsticks — How Mick Gordon crafted the Doom soundtrack](https://www.thumbsticks.com/mick-gordon-crafted-doom-soundtrack/) (the four-chain signal flow)
- [URM Podcast EP261](https://urm.academy/urmpc261-mick-gordon/) (Superior Drummer multi-mic re-routing; "that was my instrument")
- [Sifter — The ingredients of Mick Gordon's Doom Eternal soundtrack](https://www.sifter.com.au/post/the-ingredients-of-doom-composer-mick-gordon-on-the-soundtrack-of-doom-eternal) (the pop-not-drone clarity rule)
- [Origineffects — The tunes of DOOM](https://origineffects.com/2016/06/21/the-tunes-of-doom-it-djents-interviews-mick-gordon/) (output-transformer clipping as the final stage)
- [Gearnews — DOOM: Making the music of hell](https://www.gearnews.com/doom-making-music-hell/)

**DSP:** van den Doel & Pai, *Modal Synthesis for Vibrating Objects* · van den Doel
et al., *FoleyE Automatic* · Cook, *Real Sound Synthesis* (2003) · Karjalainen,
Välimäki, Esquef, *Efficient Modeling and Synthesis of Bell-like Sounds* (DAFx-02).

**MetaSounds:** [Reference Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/metasounds-reference-guide-in-unreal-engine) ·
[Function Nodes Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/metasound-function-nodes-reference-guide-in-unreal-engine) ·
[Creating Procedural Music with MetaSounds](https://dev.epicgames.com/documentation/en-us/unreal-engine/creating-procedural-music-with-metasounds) ·
[FOperatorSettings](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/MetasoundGraphCore/FOperatorSettings) ·
[ADC 2025 — Channel Agnostic Audio](https://audiodeveloperconference.b-cdn.net/wp-content/uploads/2025/07/ADC-2025-Channel-Agnostic-Audio.pdf).

**REAPER:** [ReaScript API v7.81](https://www.reaper.fm/sdk/reascript/reascripthelp.html) (every field name cited is verbatim from here) ·
[User Guide §5.13–5.14](https://hoard.reaperaccessibility.com/Documentation%20and%20Product%20Layouts/reaper-manual-text-v1.0.htm) ·
[ReaEffects Guide](http://reaper.fm/guides/ReaEffectsGuide.pdf) (RS5K parameters).
