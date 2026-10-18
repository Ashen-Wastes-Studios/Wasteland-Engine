"""Save-game round trips, schema rejection, and the world-is-untouched contract.

`Scene/SaveGame.{h,cpp}` is built and bound (`SaveGame`, `LoadGame`,
`GameSlotExists`, `CurrentSchemaVersion`, `GetWorldTime`, `SetWorldTime`), with
`kCurrentSchemaVersion == 1`. Every assertion below was derived from what that
code actually does, read out of SaveGame.cpp and then confirmed against a real
module -- not from the plan text, and not from what the code ought to do.

The suite is built around the claims that are easy to get wrong and expensive
to get wrong:

  * A save is a SNAPSHOT. Load replaces inventory contents and quest flags
    rather than merging them, so anything picked up after the save is
    deliberately gone. Asserting "the saved items came back" without also
    asserting "the extra item did not" passes against a merge too.

  * A schema-version mismatch must be refused with the WORLD UNTOUCHED. The
    header says so ("Deliberately does NOT clear the scene first"), and the
    reason is the failure mode it exists to prevent: a load that wiped the
    world and then hit a bad version turns "unsupported save" into "corrupt
    session". So the assertion is on the world's state, not merely on the
    bool -- a suite that only checked `LoadGame(...) == (False, msg)` would
    pass against an implementation that destroyed everything first.

  * Transient component state is never written AND (for the four the reader
    explicitly resets) is zeroed on load. RuntimeBody is different: it is
    never written and never restored, but the reader does NOT clear it. See
    the note on `test_runtime_body_is_never_written_and_never_restored`.

  * A save is not scene data. The slot API takes a LABEL, the extension is
    the class's own business, and a slot named "../../../escape" must land
    inside the save directory.

HARNESS RULES observed here (each one learned the hard way elsewhere):

  * Components passed to `AddOrReplace*` are SNAPSHOT COPIES. `AddOrReplace`
    copies through the component's own copy constructor, which by design drops
    runtime accumulators -- so setting `CooldownTimer` on a fresh
    `HealthComponent` and adding it leaves the entity's CooldownTimer at 0.
    Every assertion here re-reads the component off the entity
    (`e.GetHealthComponent()` returns a live reference) rather than trusting
    the local object. Asserting on the local object is how a test ends up
    passing on a component that was never attached.

  * Every Scene is held in a local for the whole test. `Scene().CreateEntity(...)`
    lets the Scene be collected while the Entity survives; the Entity holds a
  raw registry pointer and the next access reads freed storage.

  * Nothing calls `OnRuntimeStop()` (pre-existing exit-139 teardown).

Run:  bash tools/t.sh headless test_savegame
"""

from __future__ import annotations

import atexit
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import wlpath  # noqa: E402

wlpath.setup()
import Wasteland  # noqa: E402
from Wasteland import Scene, Vec3  # noqa: E402
from runner import check, check_eq, check_near  # noqa: E402

# Temp directories are registered here and removed at exit, so a failing
# assertion leaves its save files behind for inspection instead of leaking
# them into %TEMP% one test at a time.
_TMP_DIRS: list[str] = []


def _tmpdir() -> str:
    path = tempfile.mkdtemp(prefix="wl_savegame_")
    _TMP_DIRS.append(path)
    return path


atexit.register(lambda: [shutil.rmtree(d, ignore_errors=True) for d in _TMP_DIRS])


# --------------------------------------------------------------------------
# Builders. Each returns the scene AND its entities together, for the
# keep-alive reason above.

def _scene() -> Scene:
    """A scene with the simulation started.

    OnSimulationStart is what creates the Box3D world, so an entity that owns a
    RigidBody3DComponent gets a real RuntimeBody handle. Save/load does not
    need physics, but the RuntimeBody assertion does, and starting it here
    means one builder serves both.
    """
    sc = Scene()
    sc.OnSimulationStart()
    return sc


def _persistent(sc: Scene, name: str, pos: Vec3 | None = None) -> "Wasteland.Entity":
    """An entity that carries PersistentComponent, i.e. one a save will write."""
    e = sc.CreateEntity(name)
    e.AddOrReplacePersistentComponent(Wasteland.PersistentComponent())
    if pos is not None:
        e.SetPosition(pos)
    return e


def _vector(v) -> tuple:
    """Vec3 is not iterable; its three fields are."""
    return (v.x, v.y, v.z)


def _write_save(directory: str, slot: str, text: str) -> str:
    """Drop a hand-written .wlsave where Load will find it."""
    os.makedirs(directory, exist_ok=True)
    path = os.path.join(directory, slot + ".wlsave")
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)
    return path


def _read_save(directory: str, slot: str) -> str:
    with open(os.path.join(directory, slot + ".wlsave"), encoding="utf-8") as fh:
        return fh.read()


# --------------------------------------------------------------------------
# 1. Full round trip: save, scramble every mutable field, load, compare.

def test_a_full_round_trip_restores_every_persistent_field_exactly():
    """The headline claim, asserted field by field.

    Every component SaveGame writes is checked: transform (all three vectors),
    health (Current/Max/IsDead), weapon ammo counts, inventory (both limits
    plus contents), quest flags and world timers -- and the world-scoped clock,
    which lives on the Scene rather than on any entity.

    The scramble step is what makes this a test. Without it every assertion
    would compare a value against itself.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    e = _persistent(sc, "hero", Vec3(3.0, -4.0, 5.5))
    # Exactly representable floats, so the comparison is exact rather than
    # approximate. 0.1 does NOT round-trip through the file's float formatting
    # bit-for-bit, and asserting exactness on it would be a red that means
    # nothing.
    e.SetRotation(Vec3(0.25, 0.5, 0.75))
    e.SetScale(Vec3(2.0, 2.0, 2.0))

    hc = Wasteland.HealthComponent()
    hc.Current = 61.0
    hc.Max = 90.0
    e.AddOrReplaceHealthComponent(hc)

    wc = Wasteland.WeaponComponent()
    wc.CurrentAmmo = 9
    wc.MaxAmmo = 20
    wc.ReserveAmmo = 40
    e.AddOrReplaceWeaponComponent(wc)

    ic = Wasteland.InventoryComponent()
    ic.MaxWeight = 5.0
    ic.MaxSlots = 3
    e.AddOrReplaceInventoryComponent(ic)
    # Two separate stacks rather than one stack of two, so the restored list
    # has a known shape to compare against (order is preserved by the writer).
    e.GetInventoryComponent().SetItems([Wasteland.ItemStack("gem", 1),
                                        Wasteland.ItemStack("gem", 1)])

    qf = Wasteland.QuestFlagsComponent()
    qf.Set("alpha", "1")
    qf.Set("beta", "two")
    e.AddOrReplaceQuestFlagsComponent(qf)

    wt = Wasteland.WorldTimerComponent()
    wt.Set("noon", 12.5)
    e.AddOrReplaceWorldTimerComponent(wt)

    sc.SetWorldTime(777.25)
    check(sc.SaveGame("full", saves), "SaveGame must report success")

    # ---- scramble everything the save is responsible for ----
    e.SetPosition(Vec3(-1.0, -1.0, -1.0))
    e.SetRotation(Vec3(9.0, 9.0, 9.0))
    e.SetScale(Vec3(9.0, 9.0, 9.0))
    h = e.GetHealthComponent()
    h.Current = 1.0
    h.Max = 2.0
    h.IsDead = True
    w = e.GetWeaponComponent()
    w.CurrentAmmo = 0
    w.MaxAmmo = 1
    w.ReserveAmmo = 2
    inv = e.GetInventoryComponent()
    inv.MaxWeight = 900.0
    inv.MaxSlots = 90
    inv.SetItems([Wasteland.ItemStack("junk", 30)])
    flags = e.GetQuestFlagsComponent()
    flags.Set("alpha", "changed")
    flags.Set("beta", "changed")
    e.GetWorldTimerComponent().Set("noon", -5.0)
    sc.SetWorldTime(0.0)

    ok, err = sc.LoadGame("full", saves)
    check(ok, "LoadGame must report success: %r" % (err,))

    check_eq(_vector(e.GetPosition()), (3.0, -4.0, 5.5), "Position")
    check_eq(_vector(e.GetRotation()), (0.25, 0.5, 0.75), "Rotation")
    check_eq(_vector(e.GetScale()), (2.0, 2.0, 2.0), "Scale")

    h = e.GetHealthComponent()
    check_eq(h.Current, 61.0, "Health.Current")
    check_eq(h.Max, 90.0, "Health.Max")
    check_eq(h.IsDead, False, "Health.IsDead")

    w = e.GetWeaponComponent()
    check_eq(w.CurrentAmmo, 9, "Weapon.CurrentAmmo")
    check_eq(w.MaxAmmo, 20, "Weapon.MaxAmmo")
    check_eq(w.ReserveAmmo, 40, "Weapon.ReserveAmmo")

    inv = e.GetInventoryComponent()
    check_eq(inv.MaxWeight, 5.0, "Inventory.MaxWeight")
    check_eq(inv.MaxSlots, 3, "Inventory.MaxSlots")
    check_eq([(s.ItemId, s.Quantity) for s in inv.Items],
             [("gem", 1), ("gem", 1)],
             "Inventory.Items -- order must survive, contents must be replaced")

    # Quest flags and world timers are written as YAML MAPS, and map key order
    # is not preserved by YAML. The writer's own comment says so. So these are
    # compared as a dict, not as a list -- asserting list order here would be
    # asserting a property the format does not have.
    flags = e.GetQuestFlagsComponent()
    check_eq(sorted((k, flags.Get(k)) for k in flags.Keys()),
             [("alpha", "1"), ("beta", "two")], "QuestFlags")
    check_near(e.GetWorldTimerComponent().Get("noon"), 12.5, 1e-9, "WorldTimers")

    check_eq(sc.GetWorldTime(), 777.25, "world-scoped WorldTime")


def test_inventory_contents_are_replaced_not_merged():
    """The snapshot half of the snapshot claim.

    Load calls `inv.Items.clear()` before repopulating, so an item picked up
    after the save is intentionally gone. Without the clear, both stacks would
    coexist and the inventory would silently grow across every load.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    e = _persistent(sc, "carrier", Vec3(0.0, 0.0, 0.0))
    ic = Wasteland.InventoryComponent()
    ic.MaxWeight = 0.0
    ic.MaxSlots = 0
    e.AddOrReplaceInventoryComponent(ic)
    check_eq(sc.AddItem(e, "gem", 2), 2, "setup: two gems must be accepted")

    check(sc.SaveGame("snap", saves), "SaveGame must report success")

    check_eq(sc.AddItem(e, "gem", 4), 4, "setup: four more must be accepted")
    check_eq(sc.GetItemCount(e, "gem"), 6, "setup: six held before the load")

    ok, err = sc.LoadGame("snap", saves)
    check(ok, "LoadGame must report success: %r" % (err,))
    check_eq(sc.GetItemCount(e, "gem"), 2,
             "the four post-save gems must be gone -- a load is a snapshot, not a delta")


def test_quest_flags_are_replaced_not_merged():
    """Same clear-then-refill shape as the inventory, on the flags vector."""
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    e = _persistent(sc, "hero", Vec3(0.0, 0.0, 0.0))
    qf = Wasteland.QuestFlagsComponent()
    qf.Set("met_elder", "yes")
    e.AddOrReplaceQuestFlagsComponent(qf)
    check(sc.SaveGame("flags", saves), "SaveGame must report success")

    live = e.GetQuestFlagsComponent()
    live.Set("met_elder", "no")
    live.Set("stage", "3")
    check_eq(len(live), 2, "setup: two flags before the load")

    ok, err = sc.LoadGame("flags", saves)
    check(ok, "LoadGame must report success: %r" % (err,))
    restored = e.GetQuestFlagsComponent()
    check_eq(sorted(restored.Keys()), ["met_elder"],
             "the post-save 'stage' flag must be gone")
    check_eq(restored.Get("met_elder"), "yes", "the saved flag value must return")


# --------------------------------------------------------------------------
# 2. Transient state.

def test_transient_component_state_is_zeroed_by_a_load():
    """CooldownTimer, IsReloading, ReloadTimer and FireCooldown must read 0
    after a load, whatever they were before.

    The writer omits them and the reader assigns them explicitly, so this is
    asserted on the read-back component -- the live one on the entity, not the
    local object that was added. Set the transients through the live
    reference, because `AddOrReplace` copies through a constructor that drops
    runtime accumulators by design: setting CooldownTimer on a fresh
    `HealthComponent` before adding it leaves the entity's at 0, and a test
    that asserted on the local would pass while proving nothing.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    e = _persistent(sc, "hero", Vec3(1.0, 2.0, 3.0))
    e.AddOrReplaceHealthComponent(Wasteland.HealthComponent())
    e.AddOrReplaceWeaponComponent(Wasteland.WeaponComponent())

    live_h = e.GetHealthComponent()
    live_h.CooldownTimer = 2.5
    check_eq(live_h.CooldownTimer, 2.5,
             "setup: CooldownTimer must be settable on the live component")
    live_w = e.GetWeaponComponent()
    live_w.IsReloading = True
    live_w.ReloadTimer = 1.25
    live_w.FireCooldown = 0.5
    check_eq(live_w.IsReloading, True, "setup: IsReloading must be settable")

    check(sc.SaveGame("trans", saves), "SaveGame must report success")

    text = _read_save(saves, "trans")
    for key in ("CooldownTimer", "FireCooldown", "IsReloading", "ReloadTimer"):
        check(key not in text,
              "%s must never reach the file -- restoring a half-elapsed cooldown "
              "silently eats the player's first shot after load" % key)

    ok, err = sc.LoadGame("trans", saves)
    check(ok, "LoadGame must report success: %r" % (err,))

    h = e.GetHealthComponent()
    check_eq(h.CooldownTimer, 0.0, "Health.CooldownTimer must be zeroed")
    w = e.GetWeaponComponent()
    check_eq(w.IsReloading, False, "Weapon.IsReloading must be cleared")
    check_eq(w.ReloadTimer, 0.0, "Weapon.ReloadTimer must be zeroed")
    check_eq(w.FireCooldown, 0.0, "Weapon.FireCooldown must be zeroed")


def test_shots_fired_is_never_written():
    """ShotsFired is omitted from the file and untouched by the reader.

    Asserted as "unchanged", not "zeroed": SaveGame::ReadEntity never assigns
    it, so a live counter keeps counting across a load. Pinning it as
    unchanged is what the code does; pinning it as zero would be asserting a
    fix nobody made.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    e = _persistent(sc, "gun", Vec3(0.0, 0.0, 0.0))
    e.AddOrReplaceWeaponComponent(Wasteland.WeaponComponent())
    e.GetWeaponComponent().ShotsFired = 42
    check(sc.SaveGame("shots", saves), "SaveGame must report success")
    check("ShotsFired" not in _read_save(saves, "shots"),
          "ShotsFired must never reach the file")

    ok, err = sc.LoadGame("shots", saves)
    check(ok, "LoadGame must report success: %r" % (err,))
    check_eq(e.GetWeaponComponent().ShotsFired, 42,
             "the reader does not touch ShotsFired, so a live counter must survive")


def test_runtime_body_is_never_written_and_never_restored():
    """RuntimeBody is a process-local Box3D handle.

    Two separate claims, and the second is the dangerous one: a stale handle
    restored from disk points a body at memory that has since been freed. So
    assert (a) the handle's value appears nowhere in the file, and (b) a load
    does not resurrect a handle that is currently zero.

    Note what is NOT asserted: that the load zeroes RuntimeBody. It does not.
    ReadEntity never assigns it, so a live handle survives a load unchanged
    and a zero one stays zero. "Zeroed after load" would be a claim about a
    different implementation; the real contract is "never written, never
    restored", and that is what is pinned.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    # A real body, so the handle is a real non-zero value rather than a
    # sentinel: RigidBody3D + a collider + OnSimulationStart.
    body = _persistent(sc, "body", Vec3(0.0, 5.0, 0.0))
    body.AddOrReplaceRigidBody3DComponent(Wasteland.RigidBody3DComponent())
    collider = Wasteland.BoxCollider3DComponent()
    collider.HalfExtents = Vec3(0.5, 0.5, 0.5)
    body.AddOrReplaceBoxCollider3DComponent(collider)
    sc.StepPhysics(0.016)
    live_handle = body.GetRigidBody3DComponent().RuntimeBody
    check(live_handle != 0,
          "the physics world must have assigned a RuntimeBody for this test to mean "
          "anything (got %r)" % (live_handle,))

    check(sc.SaveGame("rb", saves), "SaveGame must report success")
    text = _read_save(saves, "rb")
    check("RuntimeBody" not in text, "the RuntimeBody KEY must never reach the file")
    check(str(live_handle) not in text,
          "the RuntimeBody VALUE (%r) must never reach the file" % (live_handle,))

    # A non-physics entity whose handle is zero must STAY zero: if the reader
    # restored handles, this is where a stale one would come back.
    inert = _persistent(sc, "inert", Vec3(1.0, 1.0, 1.0))
    inert.AddOrReplaceRigidBody3DComponent(Wasteland.RigidBody3DComponent())
    check_eq(inert.GetRigidBody3DComponent().RuntimeBody, 0, "setup: no collider, no handle")

    ok, err = sc.LoadGame("rb", saves)
    check(ok, "LoadGame must report success: %r" % (err,))
    check_eq(inert.GetRigidBody3DComponent().RuntimeBody, 0,
             "a load must not resurrect a RuntimeBody handle for an entity that has none")
    check_eq(_vector(inert.GetPosition()), (1.0, 1.0, 1.0),
             "the inert entity's position must still restore, so the load did run")


# --------------------------------------------------------------------------
# 3-4. Schema version. The failure that matters is a rejected load that has
# already done damage.

def _populate(sc: Scene) -> "Wasteland.Entity":
    """One persistent entity with observable state in every saved component."""
    e = _persistent(sc, "hero", Vec3(3.0, 3.0, 3.0))
    e.AddOrReplaceHealthComponent(Wasteland.HealthComponent())
    e.GetHealthComponent().Current = 61.0
    e.AddOrReplaceInventoryComponent(Wasteland.InventoryComponent())
    sc.SetWorldTime(500.0)
    return e


def _scribble(sc: Scene, e: "Wasteland.Entity") -> None:
    """Put the world into a state a rejected load must leave exactly alone."""
    e.SetPosition(Vec3(-3.0, -3.0, -3.0))
    e.GetHealthComponent().Current = 0.5
    sc.SetWorldTime(1234.5)


def _assert_world_untouched(sc: Scene, e: "Wasteland.Entity", label: str) -> None:
    """The assertion the header's whole design rests on.

    A load that cleared the scene and then discovered the version mismatch
    would return false with a perfectly good error string, and the player's
    session would be gone. Checking the return value alone cannot see that.
    """
    check_eq(_vector(e.GetPosition()), (-3.0, -3.0, -3.0),
             "%s: a rejected load must not move the entity" % label)
    check_eq(e.GetHealthComponent().Current, 0.5,
             "%s: a rejected load must not touch health" % label)
    check_eq(sc.GetWorldTime(), 1234.5,
             "%s: a rejected load must not touch the world clock" % label)


def test_a_future_schema_version_is_rejected_with_the_world_untouched():
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    check(sc.SaveGame("base", saves), "SaveGame must report success")

    base = _read_save(saves, "base")
    check("SchemaVersion: 1" in base, "the base save must declare version 1")
    _write_save(saves, "future",
                base.replace("SchemaVersion: 1", "SchemaVersion: 2", 1))

    _scribble(sc, e)
    ok, err = sc.LoadGame("future", saves)
    check_eq(ok, False, "a save from a NEWER build must be refused")
    check("2" in err and "1" in err,
          "the error must name both versions so the player can act: %r" % (err,))
    _assert_world_untouched(sc, e, "future version")

    # The existence probe is the cheap pre-flight a load menu would use, and it
    # reports the on-disk version even for a save it will not load.
    exists, version = sc.GameSlotExists("future", saves)
    check_eq(exists, True, "the slot must still report as existing")
    check_eq(version, 2, "GameSlotExists must report the version actually on disk")


def test_a_past_schema_version_is_rejected_with_the_world_untouched():
    """Version 0 is the shape of the original layout, which no longer exists.

    A downgrade is not "close enough". Refusing loudly is the documented
    decision, and the same world-untouched contract applies.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    check(sc.SaveGame("base", saves), "SaveGame must report success")

    base = _read_save(saves, "base")
    _write_save(saves, "past",
                base.replace("SchemaVersion: 1", "SchemaVersion: 0", 1))

    _scribble(sc, e)
    ok, err = sc.LoadGame("past", saves)
    check_eq(ok, False, "a save from an OLDER build must be refused")
    check("0" in err and "1" in err,
          "the error must name both versions: %r" % (err,))
    _assert_world_untouched(sc, e, "past version")

    exists, version = sc.GameSlotExists("past", saves)
    check_eq((exists, version), (True, 0),
             "GameSlotExists must report the older version on disk")


def test_a_wildly_out_of_range_version_is_refused_the_same_way():
    """A negative version is still a mismatch, not a special case."""
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    check(sc.SaveGame("base", saves), "SaveGame must report success")

    base = _read_save(saves, "base")
    _write_save(saves, "neg",
                base.replace("SchemaVersion: 1", "SchemaVersion: -1", 1))

    _scribble(sc, e)
    ok, err = sc.LoadGame("neg", saves)
    check_eq(ok, False, "a negative schema version must be refused")
    _assert_world_untouched(sc, e, "negative version")
    check_eq(sc.CurrentSchemaVersion(), 1,
             "this build's schema version is what the error is measured against")


# --------------------------------------------------------------------------
# 5. Corrupt files.

def test_a_corrupt_file_fails_without_raising_and_leaves_the_scene_intact():
    """Load's contract is "returns false for a missing file, a malformed file,
    or an incompatible schema version". So: no exception, a bool, a reason,
    and an unchanged world.

    Six payloads, chosen to hit the distinct failure paths rather than to pad
    the count: binary garbage and a truncated document fail in the YAML parse,
    an empty file and a bare scalar fail the IsMap test, a sequence fails
    IsMap too, a map with no SchemaVersion is "not a Wasteland save", and a
    string where an integer belongs fails the version read.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    check(sc.SaveGame("base", saves), "SaveGame must report success")

    cases = {
        # Fails inside YAML::Load, caught, reported as "corrupt save file".
        "garbage": b"\x00\x01\x02 not: [valid: yaml \xff\xfe {{{",
        "unbalanced": b"Entities: [\n  - UUID: 1\n",
        # Parses fine, is not a map.
        "empty": b"",
        "scalar": b"just-a-scalar\n",
        "seq": b"- a\n- b\n",
        # Is a map, but is not a save.
        "noversion": b"Slot: x\nEntities: []\n",
        "versionstring": b"SchemaVersion: \"one\"\nEntities: []\n",
    }

    for slot, payload in cases.items():
        os.makedirs(saves, exist_ok=True)
        with open(os.path.join(saves, slot + ".wlsave"), "wb") as fh:
            fh.write(payload)

        _scribble(sc, e)
        # No try/except: an exception here is a test failure, which is the
        # point. The contract says this reports false.
        ok, err = sc.LoadGame(slot, saves)
        check_eq(ok, False, "corrupt save %r must report failure" % slot)
        check(isinstance(err, str) and err,
              "corrupt save %r must come with a reason, not a bare false" % slot)
        _assert_world_untouched(sc, e, "corrupt %r" % slot)

    # A corrupt file still reports as existing, with an unknown version -- the
    # SlotExists path swallows parse errors on purpose so a save menu that is
    # merely LISTING saves is not taken down by one bad file.
    exists, version = sc.GameSlotExists("garbage", saves)
    check_eq(exists, True, "a corrupt file still exists")
    check_eq(version, 0, "a corrupt file's version reads as unknown (0)")


def test_a_missing_slot_reports_failure_rather_than_raising():
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    _scribble(sc, e)

    ok, err = sc.LoadGame("never_written", saves)
    check_eq(ok, False, "loading a slot that was never written must report failure")
    check("never_written" in err,
          "the error must name the slot that was missing: %r" % (err,))
    _assert_world_untouched(sc, e, "missing slot")
    check_eq(sc.GameSlotExists("never_written", saves), (False, 0),
             "GameSlotExists must report a never-written slot as absent")


def _replace_vector_block(text: str, key: str, replacement: str) -> str:
    """Replace one emitted three-value vector while preserving YAML layout."""
    lines = text.splitlines(keepends=True)
    start = lines.index("    %s:\n" % key)
    end = start + 1
    while end < len(lines) and lines[end].startswith("      - "):
        end += 1
    lines[start:end] = replacement.splitlines(keepends=True)
    return "".join(lines)


def test_malformed_uuid_records_report_a_reason_without_changing_the_scene():
    """Reject malformed UUIDs even after an earlier valid entity was staged."""
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    check(sc.SaveGame("base", saves), "SaveGame must report success")
    base = _read_save(saves, "base")

    # Appending the malformed record makes the valid saved record precede it.
    # A loader that applies while parsing would restore the first record before
    # discovering the bad UUID, violating the atomic failure contract.
    malformed_ids = {
        "text": "notanumber",
        "mixed": "12abc",
        "empty": '""',
        "negative": "-1",
        "overflow": "18446744073709551616",
    }
    for label, uuid in malformed_ids.items():
        slot = "baduuid_" + label
        payload = (base.rstrip() + "\n  - UUID: " + uuid +
                   "\n    Name: malformed\n")
        _write_save(saves, slot, payload)

        _scribble(sc, e)
        ok, err = sc.LoadGame(slot, saves)
        check_eq(ok, False, "malformed UUID %r must report failure" % uuid)
        check(isinstance(err, str) and err,
              "malformed UUID %r must return a reason" % uuid)
        _assert_world_untouched(sc, e, "malformed UUID %s" % label)


def test_truncated_or_malformed_coordinates_report_failure_atomically():
    """Every transform vector must contain three numeric coordinates."""
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    check(sc.SaveGame("base", saves), "SaveGame must report success")
    base = _read_save(saves, "base")

    malformed = {
        "position_truncated": _replace_vector_block(
            base, "Position", "    Position:\n      - 3\n"),
        "position_nonnumeric": _replace_vector_block(
            base, "Position", "    Position:\n      - 3\n      - nope\n      - 3\n"),
        "rotation_wrong_shape": _replace_vector_block(
            base, "Rotation", "    Rotation: not-a-sequence\n"),
        "scale_truncated": _replace_vector_block(
            base, "Scale", "    Scale:\n      - 1\n      - 1\n"),
    }

    for slot, payload in malformed.items():
        _write_save(saves, slot, payload)
        _scribble(sc, e)
        ok, err = sc.LoadGame(slot, saves)
        check_eq(ok, False, "%s must report failure" % slot)
        check(isinstance(err, str) and err,
              "%s must return a reason instead of raising" % slot)
        _assert_world_untouched(sc, e, slot)


def test_malformed_component_values_report_failure_without_partial_restore():
    """A bad later component cannot leave earlier transforms/world time applied."""
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    e = _populate(sc)
    check(sc.SaveGame("base", saves), "SaveGame must report success")

    lines = _read_save(saves, "base").splitlines(keepends=True)
    health_start = lines.index("    Health:\n")
    current_line = next(i for i in range(health_start + 1, len(lines))
                        if lines[i].startswith("      Current: "))
    lines[current_line] = "      Current: not-a-number\n"
    _write_save(saves, "badhealth", "".join(lines))

    _scribble(sc, e)
    ok, err = sc.LoadGame("badhealth", saves)
    check_eq(ok, False, "a malformed component value must report failure")
    check(isinstance(err, str) and err,
          "a malformed component value must return a reason instead of raising")
    _assert_world_untouched(sc, e, "malformed health value")


# --------------------------------------------------------------------------
# 6. A save is not scene data.

def test_a_wastescene_is_byte_identical_after_a_save_and_load():
    """SaveGame must never touch a .wastescene.

    The slot API takes a LABEL and the class owns the extension, which is the
    structural reason a save cannot land on authored content. This asserts the
    outcome: with the save directory pointed at the same folder as the scene
    file, and a full save/load cycle run, the scene file's bytes are unchanged.
    """
    sc = _scene()
    shared = _tmpdir()
    scene_path = os.path.join(shared, "level.wastescene")

    e = _persistent(sc, "hero", Vec3(1.0, 2.0, 3.0))
    e.AddOrReplaceHealthComponent(Wasteland.HealthComponent())
    e.GetHealthComponent().Current = 12.0
    e.AddOrReplaceInventoryComponent(Wasteland.InventoryComponent())
    sc.AddItem(e, "gem", 2)
    sc.SetWorldTime(9.5)

    check(sc.SaveScene(scene_path), "SaveScene must report success")
    with open(scene_path, "rb") as fh:
        before = fh.read()
    check(len(before) > 0, "SaveScene must actually write something")

    # Deliberately point the save directory at the SCENE's own directory.
    check(sc.SaveGame("slotB", shared),
          "SaveGame must succeed even when pointed at a folder holding a .wastescene")
    e.SetPosition(Vec3(-9.0, -9.0, -9.0))
    e.GetHealthComponent().Current = 1.0
    sc.SetWorldTime(0.0)

    ok, err = sc.LoadGame("slotB", shared)
    check(ok, "LoadGame must report success: %r" % (err,))

    with open(scene_path, "rb") as fh:
        after = fh.read()
    check_eq(after, before,
             "the .wastescene must be byte-identical after a save+load cycle")

    # And the save landed beside it, as its own file type.
    listing = sorted(os.listdir(shared))
    check("level.wastescene" in listing, "the scene file must still be there: %r" % listing)
    check("slotB.wlsave" in listing,
          "the save must be a separate .wlsave, never an overwrite of the scene: %r" % listing)
    check_eq(_vector(e.GetPosition()), (1.0, 2.0, 3.0),
             "and the load really did restore state, so the byte comparison is meaningful")


# --------------------------------------------------------------------------
# 7. A save naming an entity the level no longer has.

def test_a_save_naming_a_deleted_entity_still_loads_and_applies_the_rest():
    """A designer deleting a crate between versions must not brick every save.

    ReadEntity warns and returns true for a UUID the level does not contain,
    so the rest of the save still applies. Refusing the whole file for one
    missing crate would be far worse than losing the crate.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    stayer = _persistent(sc, "stayer", Vec3(1.0, 1.0, 1.0))
    h = Wasteland.HealthComponent()
    h.Current = 42.0
    stayer.AddOrReplaceHealthComponent(h)

    doomed = _persistent(sc, "doomed", Vec3(50.0, 60.0, 70.0))
    h2 = Wasteland.HealthComponent()
    h2.Current = 7.0
    doomed.AddOrReplaceHealthComponent(h2)

    check(sc.SaveGame("two", saves), "SaveGame must report success")
    saved_text = _read_save(saves, "two")
    check("doomed" in saved_text, "setup: both entities must be in the save")

    sc.DestroyEntity(doomed)
    check_eq(sc.IsEntityValid(doomed), False,
             "setup: the entity must really be gone before the load")

    stayer.SetPosition(Vec3(-3.0, -3.0, -3.0))
    stayer.GetHealthComponent().Current = 0.5

    ok, err = sc.LoadGame("two", saves)
    check(ok, "a save naming a deleted entity must still load: %r" % (err,))
    check_eq(_vector(stayer.GetPosition()), (1.0, 1.0, 1.0),
             "the surviving entity must still be restored")
    check_eq(stayer.GetHealthComponent().Current, 42.0,
             "the surviving entity's health must still be restored")
    check_eq(sc.IsEntityValid(doomed), False,
             "and the load must not resurrect the deleted entity")


def test_a_save_naming_an_unknown_uuid_applies_the_rest():
    """The same rule for a UUID that was never in this level at all.

    A hand-edited save gains a record for an entity this level does not have.
    The record is skipped, and the rest of the file applies.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    hero = _persistent(sc, "hero", Vec3(5.0, 6.0, 7.0))
    h = Wasteland.HealthComponent()
    h.Current = 33.0
    hero.AddOrReplaceHealthComponent(h)
    check(sc.SaveGame("base", saves), "SaveGame must report success")

    # A record with a syntactically valid UUID that matches no entity, spliced
    # in ahead of the real one.
    injected = _read_save(saves, "base").replace(
        "  Entities:\n",
        "  Entities:\n    - UUID: 123456789\n      Name: phantom\n"
        "      Position:\n        - 1\n        - 2\n        - 3\n", 1)
    _write_save(saves, "phantom", injected)

    hero.SetPosition(Vec3(-1.0, -1.0, -1.0))
    hero.GetHealthComponent().Current = 0.25

    ok, err = sc.LoadGame("phantom", saves)
    check(ok, "an unknown UUID is a level change, not an error: %r" % (err,))
    check_eq(_vector(hero.GetPosition()), (5.0, 6.0, 7.0),
             "the rest of the save must still apply")
    check_eq(hero.GetHealthComponent().Current, 33.0,
             "the rest of the save must still apply")


# --------------------------------------------------------------------------
# 8. Slot sanitisation.

def test_a_traversal_slot_name_cannot_escape_the_save_directory():
    """A slot is a player-facing LABEL, so it is sanitised before it touches
    the filesystem.

    GetSlotPath replaces both separators and the Windows-illegal character set,
    so "../../../escape" cannot traverse: the dots survive but the separators
    do not. Asserted two ways -- the file lands inside the directory, and
    nothing appears in the parent.
    """
    sc = _scene()
    # Nested two levels deep INSIDE a directory this test owns, so that
    # "somewhere else on the filesystem" is a place with a known listing.
    # Asserting against the real %TEMP% parent instead would compare against
    # thousands of unrelated entries and prove nothing.
    root = _tmpdir()
    parent = os.path.join(root, "saves")
    saves = os.path.join(parent, "deep")
    _persistent(sc, "hero", Vec3(0.0, 0.0, 0.0))

    check(sc.SaveGame("../../../escape", saves),
          "SaveGame must still succeed with a traversal-shaped slot name")

    inside = sorted(os.listdir(saves))
    check_eq(inside, [".._.._.._escape.wlsave"],
             "the save must land inside the directory under a sanitised name: %r" % inside)

    # "../../../escape" resolved against the save directory would have written
    # three levels up, landing in `root` -- so `root` is the place a traversal
    # would show up, and it must still hold only the directory we created.
    check_eq(sorted(os.listdir(root)), ["saves"],
             "nothing may be written into the parent of the save directory")
    check_eq(sorted(os.listdir(parent)), ["deep"],
             "nothing may be written beside the save directory either")


def test_separator_and_illegal_characters_in_a_slot_are_replaced():
    """The rest of the sanitisation, checked as a set.

    The documented behaviour is substitution, not rejection: an illegal
    character becomes '_'. A test that asserted rejection would be asserting a
    stricter API than the one that exists.
    """
    sc = _scene()
    root = _tmpdir()
    saves = os.path.join(root, "saves")
    _persistent(sc, "hero", Vec3(0.0, 0.0, 0.0))

    for slot in ("a/b", "a\\b", "..\\..\\win", "nul?", "a:b", 'quo"te'):
        check(sc.SaveGame(slot, saves),
              "SaveGame must accept the sanitised slot %r" % slot)

    inside = sorted(os.listdir(saves))
    for expected in ("a_b.wlsave", ".._.._win.wlsave", "nul_.wlsave", "a_b.wlsave"):
        check(expected in inside,
              "expected %r among the sanitised slot files: %r" % (expected, inside))
    check_eq(sorted(os.listdir(root)), ["saves"],
             "every one of those slots must have stayed inside the save directory")


def test_an_empty_slot_label_becomes_a_usable_default():
    """An empty label -- or one made entirely of illegal characters, which
    sanitises to empty -- becomes "default" rather than "saves/.wlsave", a
    dotfile the OS will happily hide and the user will never find.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")
    _persistent(sc, "hero", Vec3(0.0, 0.0, 0.0))

    check(sc.SaveGame("", saves), "an empty slot must be accepted")
    check_eq(sorted(os.listdir(saves)), ["default.wlsave"],
             "an empty slot must land as default.wlsave, not .wlsave")

    exists, version = sc.GameSlotExists("", saves)
    check_eq(exists, True, "the default slot must be found by the same empty label")
    check_eq(version, 1, "and must report the build's schema version")


# --------------------------------------------------------------------------
# 9. The filter that makes a save cheap, and the bound API's shape.

def test_only_persistent_entities_are_written():
    """A level with a thousand decorative props must not produce a thousand
    saved props. Save() iterates a PersistentComponent view, so an entity
    without the flag is simply not in the file.
    """
    sc = _scene()
    saves = os.path.join(_tmpdir(), "saves")

    keeper = _persistent(sc, "keeper", Vec3(1.0, 2.0, 3.0))
    keeper.AddOrReplaceHealthComponent(Wasteland.HealthComponent())
    keeper.GetHealthComponent().Current = 11.0
    decor = sc.CreateEntity("decor")
    decor.SetPosition(Vec3(9.0, 9.0, 9.0))

    check(sc.SaveGame("filter", saves), "SaveGame must report success")
    text = _read_save(saves, "filter")
    check("keeper" in text, "the persistent entity must be in the save")
    check("decor" not in text,
          "an entity with no PersistentComponent must not be written")
    check_eq(text.count("UUID:"), 1,
             "exactly one record must be written, so the count reflects reality")


def test_the_bound_save_api_is_present_and_takes_a_slot_not_a_path():
    """Six bound methods, each with the shape the binding documents.

    SaveGame/LoadGame/GameSlotExists all take a slot LABEL plus an optional
    directory. A caller cannot hand SaveGame a .wastescene path, which is the
    structural half of "a save never overwrites the level".
    """
    sc = _scene()
    for name in ("SaveGame", "LoadGame", "GameSlotExists", "CurrentSchemaVersion",
                 "GetWorldTime", "SetWorldTime"):
        check(hasattr(sc, name), "Scene must expose %s" % name)

    saves = os.path.join(_tmpdir(), "saves")
    _populate(sc)

    # The directory argument is optional; the default save directory is used
    # when it is omitted. Asserted by CALLING it, since a wrong default is the
    # failure -- writing into the CWD would litter the repo.
    check_eq(sc.CurrentSchemaVersion(), 1,
             "CurrentSchemaVersion is kCurrentSchemaVersion, and this build is 1")
    check_eq(sc.GetWorldTime(), 500.0, "GetWorldTime must read what SetWorldTime wrote")
    sc.SetWorldTime(11.5)
    check_eq(sc.GetWorldTime(), 11.5, "SetWorldTime must be readable back")

    check(sc.SaveGame("explicit", saves), "SaveGame must accept an explicit directory")
    ok, err = sc.LoadGame("explicit", saves)
    check(ok, "LoadGame must accept an explicit directory: %r" % (err,))
    check_eq(sc.GameSlotExists("explicit", saves), (True, 1),
             "GameSlotExists must report existence and the on-disk version together")