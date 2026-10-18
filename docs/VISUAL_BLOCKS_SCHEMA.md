# Visual Blocks graph format and node reference

This is the hand-authoring reference for `.wlblocks` flow graphs and `.wlbtree` behaviour trees. Both extensions use the same YAML serializer and the same `Graph` model. `kind` chooses which validator/compiler semantics apply; the filename extension does not change the YAML schema.

The authoritative schema is the current `VisualBlocks::NodeRegistry`, the `GraphSerializer`, and `Validator` in `Wasteland/src/Wasteland/VisualBlocks/`. The node tables below list every registered type and every pin ID. `tests/test_visual_blocks.py::test_schema_registry_drift` compares this document to `VisualBlocks.RegisteredNodes()` so a registry change cannot silently leave the reference incomplete.

## IDs, types, and pin signatures

Three different identifiers appear in a graph:

- **Instance ID** (`nodes[].id`) identifies one placed node. Links and tree children refer to this ID.
- **Node type ID** (`nodes[].type`) names a `NodeDef` in the registry. It is a serialized compatibility key; preserve it when editing existing files.
- **Pin ID** (`nodes[].pins[].pin`, `links[].from.pin`, `links[].to.pin`) is the stable registry pin key. Pin labels are presentation only. Match the ID's spelling and case exactly.

Pin type spellings in the persisted schema are `exec`, `bool`, `int`, `float`, `string`, `vec2`, `vec3`, `vec4`, `entity`, and `any`. Directions are `in` and `out`. The only numeric implicit conversion is `int` to `float`; `any` accepts data types but not `exec`. The validator checks link direction/type and the required input pins. Registry defaults are shown below as raw default literals. A required input still needs an instance literal or link even when the registry lists a default; optional inputs can use their registry default without an instance `pins` entry.

In the tables, each pin is written as **`ID` direction type** followed by any `required` marker and default. Registry order is preserved. The behaviour-tree `Value` output is a palette/schema placeholder; tree topology is authored with `children`, not links between those outputs.

### Behaviour-tree node registry

| Node type ID | Palette category / role | Registered pin IDs |
|---|---|---|
| `IsDead` | `Behaviour/Condition` · condition | `Value` out bool |
| `IsLowHealth` | `Behaviour/Condition` · condition | `Fraction` in float required (default `0.25`); `Value` out bool |
| `HasLineOfSight` | `Behaviour/Condition` · condition | `Target` in string required (default `target`); `MaxDistance` in float required (default `30.0`); `Value` out bool |
| `BlackboardIsSet` | `Behaviour/Condition` · condition | `Key` in string required (default `target`); `Value` out bool |
| `Compare` | `Behaviour/Condition` · condition | `A` in float required (default `0.0`); `B` in float required (default `0.0`); `Op` in string required (default `<=`); `Value` out bool |
| `Gate` | `Behaviour/Condition` · condition | `Every` in int required (default `4`); `Value` out bool |
| `DistanceTo` | `Behaviour/Condition` · condition · alias of `ConditionDistanceTo` | Effective pins from its alias target: `Target` in string required (default `target`); `Range` in float required (default `2.0`); `Op` in string required (default `<=`); `Value` out float |
| `FaceTarget` | `Behaviour/Task` · task | `Target` in string required (default `target`); `TurnRate` in float required (default `540.0`); `Value` out bool |
| `ApplyDamage` | `Behaviour/Task` · task | `Target` in string required (default `target`); `Damage` in float required (default `12.0`); `Value` out bool |
| `SenseNearest` | `Behaviour/Task` · task | `Key` in string required (default `target`); `Radius` in float required (default `20.0`); `HalfAngle` in float (default `0`); `RequireLOS` in bool (default `False`); `Found` out entity; `Value` out bool |
| `MoveToTransform` | `Behaviour/Task` · task | `Position` in vec3 required (default `0,0,0`); `Speed` in float required (default `3.0`); `Arrival` in float required (default `0.5`); `Value` out bool |
| `FollowPath` | `Behaviour/Task` · task | `Target` in string required (default `target`); `Speed` in float (default `3.0`); `Value` out bool |
| `PlayAnimation` | `Behaviour/Task` · task | `Animation` in string required (default `idle`); `Loop` in bool required (default `true`); `Value` out bool |
| `CallPython` | `Behaviour/Task` · task | `Method` in string required (default `OnBehaviorStep`); `Value` out bool |
| `SetTimer` | `Behaviour/Task` · task | `Key` in string required (default `attackAt`); `Seconds` in float required (default `1.0`); `Value` out bool |
| `TickEvery` | `Behaviour/Decorator` · decorator | `Seconds` in float required (default `0.25`); `Value` out bool |
| `Sequence` | `Behaviour/Composite` · composite | `Value` out float |
| `Selector` | `Behaviour/Composite` · composite | `Value` out float |
| `Parallel` | `Behaviour/Composite` · composite | `Policy` in string required (default `all`); `Value` out float |
| `Invert` | `Behaviour/Decorator` · decorator | `Value` out float |
| `ForceSuccess` | `Behaviour/Decorator` · decorator | `Value` out float |
| `ForceFailure` | `Behaviour/Decorator` · decorator | `Value` out float |
| `Repeat` | `Behaviour/Decorator` · decorator | `Count` in int (default `2`); `Value` out float |
| `Cooldown` | `Behaviour/Decorator` · decorator | `Seconds` in float required (default `1.0`); `Value` out float |
| `Timeout` | `Behaviour/Decorator` · decorator | `Seconds` in float required (default `1.0`); `Value` out float |
| `Wait` | `Behaviour/Task` · task | `Seconds` in float required (default `1.0`); `Value` out float |
| `ConditionBool` | `Behaviour/Condition` · condition | `Value` in bool required (default `true`); `Value` out float |
| `ConditionCompare` | `Behaviour/Condition` · condition | `Left` in float required (default `0.0`); `Right` in float required (default `0.0`); `Op` in string required (default `<`); `Value` out float |
| `ConditionHealthBelow` | `Behaviour/Condition` · condition | `Threshold` in float required (default `30.0`); `Value` out float |
| `ConditionDistanceTo` | `Behaviour/Condition` · condition | `Target` in string required (default `target`); `Range` in float required (default `2.0`); `Op` in string required (default `<=`); `Value` out float |
| `MoveTo` | `Behaviour/Task` · task | `Target` in string required (default `target`); `Speed` in float (default `3.0`); `UsePathfinding` in bool (default `False`); `Value` out float |
| `Attack` | `Behaviour/Task` · task | `Target` in string required (default `target`); `Damage` in float (default `10.0`); `Value` out float |
| `SetBlackboard` | `Behaviour/Task` · task | `Key` in string required; `Value` in float required (default `0.0`); `Value` out float |

`DistanceTo` is a compatibility alias: the registry resolves it to `ConditionDistanceTo`, so it uses the target node's effective pins and behaviour. Its alias registration itself has no local pin vector.

### Flow-graph node registry

| Node type ID | Palette category | Registered pin IDs |
|---|---|---|
| `FlowOnEvent` | `Flow/Event` | `Out` out exec |
| `SetVariable` | `Flow/Variable` | `In` in exec required; `Out` out exec; `Name` in string required; `Value` in any (default `0`) |
| `GetVariable` | `Flow/Variable` | `Name` in string required; `Value` out float |
| `Branch` | `Flow/Control` | `In` in exec required; `Condition` in bool required; `True` out exec; `False` out exec |
| `While` | `Flow/Control` | `In` in exec required; `Condition` in bool required; `Body` out exec; `Done` out exec |
| `ForEach` | `Flow/Control` | `In` in exec required; `Count` in int required (default `0`); `IndexVar` in string (default `"i"`); `Body` out exec; `Done` out exec |
| `Add` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out float |
| `Subtract` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out float |
| `Multiply` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out float |
| `Divide` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out float |
| `Modulo` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out float |
| `LessThan` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out bool |
| `LessOrEqual` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out bool |
| `GreaterThan` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out bool |
| `GreaterOrEqual` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out bool |
| `Equal` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out bool |
| `NotEqual` | `Flow/Math` | `A` in float (default `0`); `B` in float (default `0`); `Value` out bool |
| `Negate` | `Flow/Math` | `A` in float (default `0`); `Value` out float |
| `Clamp` | `Flow/Math` | `Value` in float (default `0`); `Min` in float (default `0`); `Max` in float (default `1`); `Out` out float |
| `And` | `Flow/Logic` | `A` in bool (default `True`); `B` in bool (default `True`); `Value` out bool |
| `Or` | `Flow/Logic` | `A` in bool (default `False`); `B` in bool (default `False`); `Value` out bool |
| `Not` | `Flow/Logic` | `A` in bool (default `False`); `Value` out bool |
| `GetPosition` | `Flow/Scene` | `Position` out vec3 |
| `SetTransform` | `Flow/Scene` | `In` in exec required; `Out` out exec; `Translation` in vec3 required; `Rotation` in vec3 (default `(0, 0, 0)`); `Scale` in vec3 (default `(1, 1, 1)`) |
| `GetHealth` | `Flow/Scene` | `Value` out float |
| `Raycast` | `Flow/Scene` | `Origin` in vec3 required; `Direction` in vec3 required; `MaxDistance` in float required; `Hit` out bool |
| `PlaySound` | `Flow/Scene` | `In` in exec required; `Out` out exec; `ClipName` in string required |
| `Log` | `Flow/Utility` | `In` in exec required; `Out` out exec; `Message` in string required |
| `Break` | `Flow/Control` | `In` in exec required; `Out` out exec |

## YAML document shape

Canonical files write these root fields. When present, `kind` is exactly `tree` or `flow`; a default `Graph` starts as `flow`. `schema` is an integer defaulting to `1`; the serializer writes that value, while the loader reads it without a migration/version gate. `name` is the optional display name.

```yaml
kind: tree
schema: 1
name: Example
nodes: []
links: []
```

The serializer recognizes these root keys: `kind`, `schema`, `name`, `variables`, `blackboard`, `nodes`, and `links`. Unknown root keys are load errors. The `variables` section is for flow-graph instance variables; `blackboard` declares behaviour-tree shared keys. Canonical declarations include a non-empty `name`, a lowercase pin `type`, and optional raw-text `default`; the loader accepts an omitted type and then uses `float`.

```yaml
variables:
  - name: flashTimer
    type: float
    default: '0.0'
blackboard:
  - name: target
    type: entity
```

A node requires a unique instance `id` and a non-empty `type`. Unknown type IDs can load for forward compatibility but are reported by validation. Optional `x`/`y` are editor coordinates; `title` is a display label; `event` names a flow entry event; `latent` declares a tree node that can return `RUNNING`; `abort` accepts `none`, `self`, or `lower`; `children` is an ordered list of node instance IDs for a tree. Each `pins` entry binds a raw-text `value` to an input pin ID. Omit optional inputs you want to leave at their registry default; every required data input still needs an explicit literal or link, even if its pin definition has a default.

```yaml
nodes:
  - id: timer
    type: Wait
    latent: true
    pins:
      - pin: Seconds
        value: '0.5'
```

A flow link is a directed connection between registry pin IDs. Canonical endpoint maps contain `node` (the instance ID) and `pin` (the stable pin ID):

```yaml
links:
  - from:
      node: ev
      pin: Out
    to:
      node: update
      pin: In
```

The canonical form includes both fields on every link endpoint. The current loader requires `node` but accepts a missing `pin` as an empty string; validation then reports that the named node has no such pin. Do not rely on that permissive load path: author every endpoint with both `node` and `pin`.

The serializer preserves node, link, variable, literal, and child order. A tree's `children` order is priority order (first child first); never sort it. Flow exec pins also encode control flow, while data links connect expression outputs to input pins.

## Validation and failure behavior

`GraphSerializer::Load(path, graph, error)` populates a `Graph` or returns a structured load error; it does not validate node types or pin compatibility. A node type unknown to this build can load and round-trip, then the Validator reports it. `Validator::Validate(graph, result)` dispatches on `graph.Kind`; `ValidateTree` and `ValidateFlow` are the specialised checks. Findings remain in discovery order. `ValidationResult::Report(fileName)` includes the summary and every error and warning; warnings do not make validation fail.

Tree validation checks registered types/literals/required pins, one root, child cycles and shared children, composite/decorator/leaf structure, selected latent declarations, comparison operators, and selected declared blackboard-key references. The current reference check only examines explicit `Target` literals; it does not catch undeclared names in other key-bearing pins such as `Key` on `BlackboardIsSet`, `SenseNearest`, `SetBlackboard`, or `SetTimer`. Flow validation checks for exactly one non-empty event field, registered types/literals/required pins, link direction and type compatibility, one producer per input, and warns about unreachable statements. It counts non-empty `event` fields but does not verify that the string names a runtime callback or require the field to be on a `FlowOnEvent` node. A `validate_graph` tool response is this validator report, not a rewritten summary of it.

Load errors differ from validator findings: malformed YAML, unknown schema keys, malformed pin declarations, missing node IDs/types, and dangling node references are serializer errors before validation runs. Some incomplete link references (a missing `pin` key) are accepted by the loader and rejected later by validation. Validation is report-only and does not repair a graph.

## Editor graph tools

The existing editor `ToolServer` exposes `validate_graph(path)` and `list_graph_nodes(path)`. Both are read-only, load through `GraphSerializer` into the engine `Graph`, and reject paths that do not resolve to regular `.wlblocks`/`.wlbtree` files beneath the active `Project::Get().GetAssetRoot()`. Project-relative paths such as `assets/trees/EnemyChase.wlbtree` are preferred; an absolute path is also accepted only when its resolved target stays inside that asset root. Symlinks are checked after canonical resolution.

`validate_graph` returns the validator's complete `ValidationResult::Report`, including all errors and warnings in their original finding order and with their original message text. `list_graph_nodes` lists graph instance IDs/type IDs and, for each resolvable type (including aliases), the registry's pin IDs, directions, types, required markers, and defaults; tree child IDs are printed in their declared priority order.

## Worked shipped examples

These are excerpts copied from the four shipped files under `DemonCore-Editor/assets/`; the tree excerpts omit surrounding node definitions and are not standalone-valid graphs. The full files remain the executable examples and are linted by `tests/headless/test_shipped_graphs.py`.

### `assets/trees/EnemyChase.wlbtree` — priority and blackboard validity

The `target` key is an `entity` without a default: it remains unwritten until sensing finds a target. Root children are kept in priority order. `ForceFailure` lets `SenseNearest` write the key while making the acquisition branch fail, so the selector can continue to `engage` on the same tick.

```yaml
kind: tree
schema: 1
name: EnemyChase
blackboard:
  - name: target
    type: entity
nodes:
  - id: root
    type: Selector
    children: [dead, hasNoTarget, engage]
  - id: hasNoTarget
    type: ForceFailure
    children: [findTarget]
  - id: findTarget
    type: SenseNearest
    pins:
      - pin: Key
        value: target
      - pin: Radius
        value: '30.0'
  - id: walk
    type: MoveTo
    latent: true
    pins:
      - pin: Target
        value: target
      - pin: Speed
        value: '3.0'
links: []
```

The real graph contains the intervening root/engage/approach/attack nodes as well; the excerpt focuses on its root, sensing branch, and latent movement. `Target`, `Key`, `Radius`, and `Speed` are pin IDs, not labels.

### `assets/trees/EnemyGuard.wlbtree` — typed defaults and latent tasks

This tree demonstrates several typed blackboard defaults and uses `Wait` as a latent task. Its `ConditionDistanceTo` nodes use the `Target`, `Range`, and `Op` pins, while `MoveTo` consumes the same declared `target` key.

```yaml
kind: tree
schema: 1
name: EnemyGuard
blackboard:
  - name: target
    type: entity
    default: ''
  - name: attackCooldown
    type: float
    default: '0.0'
  - name: patrolIndex
    type: int
    default: '0'
nodes:
  - id: root
    type: Selector
    children: [dead, idle, engage]
  - id: waitIdle
    type: Wait
    latent: true
    pins:
      - pin: Seconds
        value: '1.0'
  - id: inRange
    type: ConditionDistanceTo
    pins:
      - pin: Target
        value: target
      - pin: Range
        value: '2.0'
      - pin: Op
        value: '<='
```

### `assets/trees/PatrolGuard.wlbtree` — a distinct tree shape

The patrol guard holds its post and only turns toward a nearby intruder; unlike `EnemyChase`, it has no `MoveTo` node. The root's `alert` and `holdPost` branches are alternative behaviours.

```yaml
kind: tree
schema: 1
name: PatrolGuard
blackboard:
  - name: intruder
    type: entity
nodes:
  - id: root
    type: Selector
    children: [dead, alert, holdPost]
  - id: dead
    type: IsDead
  - id: alert
    type: Sequence
    children: [hasIntruder, faceIt]
  - id: hasIntruder
    type: BlackboardIsSet
    pins:
      - pin: Key
        value: intruder
  - id: faceIt
    type: FaceTarget
    pins:
      - pin: Target
        value: intruder
      - pin: TurnRate
        value: '240.0'
  - id: holdPost
    type: Sequence
    children: [sweep, wait]
  - id: sweep
    type: SenseNearest
    pins:
      - pin: Key
        value: intruder
      - pin: Radius
        value: '9.0'
  - id: wait
    type: Wait
    latent: true
    pins:
      - pin: Seconds
        value: '0.4'
```

### `assets/graphs/DamageFlash.wlblocks` — event, exec flow, and data flow

`FlowOnEvent` uses its `event` field for the entry. Exec links use `Out`/`In`; the `Value` output from `GetVariable` and `Add` feeds data input pins. `Branch` has separate `True` and `False` exec outputs.

```yaml
kind: flow
schema: 1
name: DamageFlash
variables:
  - name: flashTimer
    type: float
    default: 0
  - name: wasHit
    type: bool
    default: False
nodes:
  - id: ev
    type: FlowOnEvent
    x: 60
    y: 80
    event: OnUpdate
  - id: mark_hit
    type: SetVariable
    x: 380
    y: 80
    pins:
      - pin: Name
        value: wasHit
      - pin: Value
        value: True
  - id: arm_timer
    type: SetVariable
    x: 700
    y: 80
    pins:
      - pin: Name
        value: flashTimer
      - pin: Value
        value: 0.35
  - id: read_timer
    type: GetVariable
    x: 1020
    y: 80
    pins:
      - pin: Name
        value: flashTimer
  - id: dec
    type: Add
    x: 1340
    y: 80
    pins:
      - pin: B
        value: -0.016
  - id: write_timer
    type: SetVariable
    x: 1660
    y: 80
    pins:
      - pin: Name
        value: flashTimer
  - id: read_back
    type: GetVariable
    x: 1980
    y: 80
    pins:
      - pin: Name
        value: flashTimer
  - id: still_lit
    type: GreaterThan
    x: 2300
    y: 80
    pins:
      - pin: B
        value: 0
  - id: br
    type: Branch
    x: 2620
    y: 80
    pins:
      - pin: Condition
        value: False
  - id: clear_hit
    type: SetVariable
    x: 2940
    y: 20
    pins:
      - pin: Name
        value: wasHit
      - pin: Value
        value: False
  - id: log_end
    type: Log
    x: 2940
    y: 140
    pins:
      - pin: Message
        value: flash ended
links:
  - from:
      node: ev
      pin: Out
    to:
      node: mark_hit
      pin: In
  - from:
      node: mark_hit
      pin: Out
    to:
      node: arm_timer
      pin: In
  - from:
      node: arm_timer
      pin: Out
    to:
      node: write_timer
      pin: In
  - from:
      node: write_timer
      pin: Out
    to:
      node: br
      pin: In
  - from:
      node: br
      pin: True
    to:
      node: clear_hit
      pin: In
  - from:
      node: clear_hit
      pin: Out
    to:
      node: log_end
      pin: In
  - from:
      node: read_timer
      pin: Value
    to:
      node: dec
      pin: A
  - from:
      node: dec
      pin: Value
    to:
      node: write_timer
      pin: Value
  - from:
      node: read_back
      pin: Value
    to:
      node: still_lit
      pin: A
  - from:
      node: still_lit
      pin: Value
    to:
      node: br
      pin: Condition
```

`read_back.Value` feeds the `still_lit` comparison, whose result drives `br.Condition`; the `True` exec path clears `wasHit` and logs the end of the flash. Flow variables and node literals are serialized as text and interpreted against the registered pin/declaration type during validation/compilation.
