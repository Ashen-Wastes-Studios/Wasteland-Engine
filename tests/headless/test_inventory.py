"""Inventory stacking, capacity, and the float-weight tolerance.

`Scene::AddItem`, `RemoveItem`, `RemoveItemFromInventory`, `DropItemAt`,
`PickUpDroppedItem`, `GetItemCount`, `HasItem`,
`GetInventoryWeight`, `IsInventoryFull` and `FindItemDefinition` are bound over
`ItemStack`, `ItemDefinitionComponent` and `InventoryComponent`, and are
serialized by SceneSerializer. Every assertion here was derived from
Scene.cpp's implementation and then confirmed against a real module.

The claims that carry the weight, and why a naive test misses them:

  * AddItem returns THE COUNT ACCEPTED, NOT THE COUNT REQUESTED. Capacity,
    stack limits and weight can each make it accept less -- or none. An
    implementation that always returned `quantity` would satisfy "adding items
    works" and fail every interesting case, so the return value is asserted
    directly and never inferred from a count read afterwards.

  * The 0.1-weight case is the one worth having a suite for at all.
    `kWeightEpsilon` exists because authored weights are floats: five 0.1
    items accumulate to 0.5000000014901161 in the source's own arithmetic,
    leaving 0.4999999985 of headroom and silently costing the player an item
    they can plainly afford. In float32 the accumulation actually lands on or
    just over 0.5 depending on order, and this suite pins the OUTCOME: five
    0.1-weight items into a 0.5 inventory MUST all fit, and a sixth must not.

  * `AddItem` returns 0 for BOTH "this entity has no inventory" and "the
    inventory is full", deliberately. A return-value-only test cannot tell
    them apart -- it would pass against an implementation that refused to
    touch an entity with no InventoryComponent for the wrong reason. So every
    rejection test here also checks the entity, which is the only
    discriminator the API offers.

HARNESS RULES observed here:

  * Components passed to `AddOrReplace*` are SNAPSHOT COPIES. Mutating the
    local `InventoryComponent` after adding it changes nothing on the entity;
    every assertion re-reads via `e.GetInventoryComponent()`, which returns a
    live reference.

  * `InventoryComponent.Items` is a COPY-OUT list, so `inv.Items.append(...)`
    mutates a temporary. Contents are changed through the Scene helpers,
    which is the path that enforces stacking and capacity, or through
    `SetItems`.

  * Every Scene is held in a local for the whole test -- `Scene().CreateEntity(...)`
    would leave the Entity pointing into freed storage.

Run:  bash tools/t.sh headless test_inventory
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
from runner import check, check_eq, check_ge, check_near  # noqa: E402

_TMP_DIRS: list[str] = []


def _tmpdir() -> str:
    path = tempfile.mkdtemp(prefix="wl_inventory_")
    _TMP_DIRS.append(path)
    return path


atexit.register(lambda: [shutil.rmtree(d, ignore_errors=True) for d in _TMP_DIRS])


# --------------------------------------------------------------------------
# Builders

def _scene() -> Scene:
    """A scene with the simulation started, so the suite is provably running.

    A suite that constructs a scene but never starts it reports 0/0 -- a green
    that proves nothing. Starting here means the physics world exists and the
    assertions are against a scene in the state the game actually uses.
    """
    sc = Scene()
    sc.OnSimulationStart()
    return sc


def _define(sc: Scene, item_id: str, max_stack: int, weight: float) -> "Wasteland.Entity":
    """Author an ItemDefinitionComponent. `FindItemDefinition` keys on Name."""
    d = sc.CreateEntity("def_" + item_id)
    dd = Wasteland.ItemDefinitionComponent()
    dd.Name = item_id
    dd.DisplayName = item_id.title()
    dd.MaxStack = max_stack
    dd.Weight = weight
    d.AddOrReplaceItemDefinitionComponent(dd)
    return d


def _carrier(sc: Scene, name: str, max_weight: float, max_slots: int) -> "Wasteland.Entity":
    """An entity that can carry. 0 for either limit means unlimited."""
    e = sc.CreateEntity(name)
    inv = Wasteland.InventoryComponent()
    inv.MaxWeight = max_weight
    inv.MaxSlots = max_slots
    e.AddOrReplaceInventoryComponent(inv)
    return e


def _stacks(e: "Wasteland.Entity") -> list:
    """The entity's stacks as (ItemId, Quantity), read from the LIVE component."""
    return [(s.ItemId, s.Quantity) for s in e.GetInventoryComponent().Items]


# --------------------------------------------------------------------------
# 1. Stack limits.

def test_adding_over_max_stack_opens_a_second_stack_rather_than_oversizing_one():
    """MaxStack=10, adding 11 gives [10, 1] -- not one clamped stack of 10.

    An implementation that clamped would silently destroy an item, and the
    return value would still read 11 unless the item actually went in
    somewhere. Both are asserted: the return value, and the stack shape.
    """
    sc = _scene()
    _define(sc, "bolt", 10, 0.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "bolt", 11), 11,
             "all 11 must be accepted -- an unlimited-slot inventory has no reason to refuse")
    check_eq(_stacks(e), [("bolt", 10), ("bolt", 1)],
             "11 bolts at MaxStack 10 must be TWO stacks, never one clamped stack")
    check_eq(sc.GetItemCount(e, "bolt"), 11,
             "GetItemCount sums across matching stacks, so it must read 11")
    check_eq(sc.HasItem(e, "bolt", 11), True, "HasItem must be true at exactly 11")
    check_eq(sc.HasItem(e, "bolt", 12), False, "and false at 12")


def test_a_partial_stack_is_topped_up_before_a_new_one_is_opened():
    """Adding 5 more to a [10, 1] inventory must grow the second stack.

    Pass 1 of AddItem tops up existing partial stacks before opening new ones,
    which is what stops "1 ammo" from occupying a second slot. Asserted as the
    resulting shape, because a clamped implementation and a topping-up one
    produce the same COUNT here -- only the shape differs.
    """
    sc = _scene()
    _define(sc, "bolt", 10, 0.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "bolt", 11), 11, "setup")
    check_eq(_stacks(e), [("bolt", 10), ("bolt", 1)], "setup")

    check_eq(sc.AddItem(e, "bolt", 5), 5, "five more must be accepted")
    check_eq(_stacks(e), [("bolt", 10), ("bolt", 6)],
             "the PARTIAL stack must be topped up -- a third stack here would mean "
             "the partial stack was skipped and capacity wasted")
    check_eq(len(e.GetInventoryComponent().Items), 2,
             "exactly two stacks: 10 is full, so the remainder goes to the second")


def test_a_zero_max_stack_means_one_unbounded_stack():
    """MaxStack <= 0 is the codebase's convention for "no limit".

    This is a bug that was already fixed once: the `maxStack <= 0` case was
    skipped in pass 1, so 500 keys became "1 key" plus "500 keys" in two rows.
    The regression is specifically that the SECOND add of the same item grows
    the first stack.
    """
    sc = _scene()
    _define(sc, "key", 0, 0.01)
    e = _carrier(sc, "keyring", 100.0, 0)

    check_eq(sc.AddItem(e, "key", 500), 500, "setup: 500 keys must be accepted")
    check_eq(_stacks(e), [("key", 500)], "setup: one unbounded stack")

    check_eq(sc.AddItem(e, "key", 1), 1, "one more must be accepted")
    check_eq(_stacks(e), [("key", 501)],
             "an unlimited item must grow ONE stack without bound -- a second row "
             "here is the exact bug this case was written for")


# --------------------------------------------------------------------------
# 2. Capacity.

def test_a_full_inventory_reports_fewer_than_requested_and_adds_what_fits():
    """The return value is the ONLY signal that capacity was hit.

    Two slots, MaxStack 10, twenty-five requested: twenty fit, five are
    refused. Asserted on the return value AND on the resulting stacks, so a
    bug that reported 25 while silently dropping five would still be caught.
    """
    sc = _scene()
    _define(sc, "screw", 10, 0.0)
    e = _carrier(sc, "carrier", 0.0, 2)

    check_eq(sc.IsInventoryFull(e), False, "an empty inventory is not full")

    accepted = sc.AddItem(e, "screw", 25)
    check_eq(accepted, 20,
             "AddItem must report the count ACCEPTED (20 of 25), not the count requested")
    check(accepted < 25, "a full inventory must report a short add")
    check_eq(_stacks(e), [("screw", 10), ("screw", 10)],
             "exactly what fits must be in the inventory")
    check_eq(sc.IsInventoryFull(e), True, "at MaxSlots the inventory must report full")

    check_eq(sc.AddItem(e, "screw", 1), 0,
             "a full inventory must accept nothing")
    check_eq(_stacks(e), [("screw", 10), ("screw", 10)],
             "and must not have changed")
    check_eq(sc.GetItemCount(e, "screw"), 20, "the twenty that fit are still held")


def test_add_item_returning_zero_must_be_disambiguated_by_the_entity():
    """AddItem reports 0 for "no inventory" and for "full" alike.

    Deliberate, per the source: "nothing happened is reported identically for
    no inventory and inventory full, so a caller wanting to tell those apart
    must check the entity itself." So this suite does exactly that -- which is
    the point. A return-value-only test cannot distinguish the two, so each
    rejection is paired with a HasInventoryComponent check.
    """
    sc = _scene()
    _define(sc, "coin", 0, 1.0)

    # No inventory at all.
    plain = sc.CreateEntity("plain")
    check_eq(plain.HasInventoryComponent(), False, "setup: this entity has no inventory")
    check_eq(sc.AddItem(plain, "coin", 1), 0, "no inventory must report 0")
    check_eq(plain.HasInventoryComponent(), False,
             "and the entity must STILL have no inventory -- AddItem must not create one")
    check_eq(sc.GetItemCount(plain, "coin"), 0, "GetItemCount on a non-carrier reads 0")
    check_eq(sc.GetInventoryWeight(plain), 0.0, "GetInventoryWeight on a non-carrier reads 0")
    check_eq(sc.IsInventoryFull(plain), False,
             "an entity with no inventory is not 'full' -- it has no limits to exceed")
    check_eq(sc.RemoveItem(plain, "coin", 1), 0, "RemoveItem on a non-carrier reports 0")
    check_eq(sc.RemoveItemFromInventory(plain, "coin", 1), 0,
             "RemoveItemFromInventory on a non-carrier reports 0")
    check_eq(sc.DropItemAt(plain, "coin", 1).IsValid(), False,
             "DropItemAt on a non-carrier must spawn nothing, so the returned "
             "entity is invalid")

    # Full: same 0, but the entity DOES have an inventory. The pair is the
    # discriminator.
    full = _carrier(sc, "full", 1.0, 1)
    check_eq(sc.AddItem(full, "coin", 3), 1,
             "a 1.0-weight coin fits once in a 1.0 inventory, and no more")
    check_eq(full.HasInventoryComponent(), True, "setup: this entity DOES have an inventory")
    check_eq(sc.IsInventoryFull(full), True, "and it is full")
    check_eq(sc.AddItem(full, "coin", 1), 0,
             "a full inventory reports the SAME 0 as a missing inventory")


def test_degenerate_arguments_are_refused_without_creating_anything():
    """An empty id, or a non-positive quantity, is a no-op returning 0."""
    sc = _scene()
    _define(sc, "gem", 0, 0.5)
    e = _carrier(sc, "carrier", 10.0, 10)

    check_eq(sc.AddItem(e, "", 1), 0, "an empty item id must be refused")
    check_eq(sc.AddItem(e, "gem", 0), 0, "a quantity of 0 must be refused")
    check_eq(sc.AddItem(e, "gem", -3), 0, "a negative quantity must be refused")
    check_eq(_stacks(e), [],
             "none of those may leave a stack behind -- a zero-quantity row still "
             "consumes a slot")
    check_eq(sc.GetInventoryWeight(e), 0.0, "and the inventory must weigh nothing")


# --------------------------------------------------------------------------
# 3. The weight epsilon.

def test_five_tenth_weight_items_all_fit_a_half_weight_inventory():
    """The assertion this suite exists for.

    kWeightEpsilon is 1e-4 and it exists because five 0.1-weight items
    accumulate to 0.5000000014901161 in the source's arithmetic, leaving
    0.4999999985 of headroom -- one float rounding error short of a whole
    number of items, which reads to a player as an off-by-one bug rather than
    as precision.

    So: five must fit, and a sixth must not. Both halves. A tolerance that was
    merely too generous would pass the first and fail the second.
    """
    sc = _scene()
    _define(sc, "pebble", 0, 0.1)
    e = _carrier(sc, "carrier", 0.5, 10)

    # Added one at a time on purpose: each add re-reads the accumulated
    # weight, which is where the rounding error actually bites. A single
    # AddItem(x, 5) would compute one product instead of five sums.
    for i in range(5):
        accepted = sc.AddItem(e, "pebble", 1)
        check_eq(accepted, 1,
                 "add #%d of five 0.1-weight items must be accepted -- refusing here "
                 "is the exact float-accumulation bug kWeightEpsilon exists to prevent"
                 % (i + 1))

    check_eq(sc.GetItemCount(e, "pebble"), 5, "all five must be held")
    # The accumulated weight lands at or fractionally above 0.5 in float32.
    check_ge(sc.GetInventoryWeight(e), 0.5,
             "five 0.1-weight items must weigh at least 0.5 in float32 arithmetic")
    check_near(sc.GetInventoryWeight(e), 0.5, 1e-6,
               "and must weigh 0.5 for all that")

    check_eq(sc.IsInventoryFull(e), True, "at capacity the inventory must report full")
    check_eq(sc.AddItem(e, "pebble", 1), 0,
             "a sixth 0.1-weight item must NOT fit in 0.5 of headroom -- the epsilon "
             "must not have turned into a free item")
    check_eq(sc.GetItemCount(e, "pebble"), 5, "and the count must not have moved")


def test_the_weight_tolerance_does_not_admit_a_genuinely_overweight_item():
    """The other direction. kWeightEpsilon must never let a real overflow through.

    The source's own bound for this: "a 0.5 item against 0 available still
    fails by miles." One 0.5-weight item added to an inventory already at
    exactly 0.5 must be refused, not admitted by a 1e-4 tolerance.
    """
    sc = _scene()
    _define(sc, "brick", 0, 0.5)
    e = _carrier(sc, "carrier", 0.5, 10)

    check_eq(sc.AddItem(e, "brick", 1), 1, "setup: one brick exactly fills it")
    check_eq(sc.GetInventoryWeight(e), 0.5, "setup: the inventory weighs its limit")
    check_eq(sc.AddItem(e, "brick", 1), 0,
             "a second 0.5-weight brick must be refused with zero headroom")
    check_eq(sc.GetItemCount(e, "brick"), 1, "and only one brick may be held")


def test_a_partial_add_reports_the_short_count_rather_than_refusing_wholesale():
    """Weight capacity can accept SOME of a request, and must say how much.

    Two 0.3-weight items into 0.5: one fits whole, the second does not fit at
    all, so one is accepted and one is reported back as the shortfall. The
    case where a partial add genuinely splits is covered by the two slots of
    `test_a_full_inventory_...`; this one pins that weight, unlike slots, is
    all-or-nothing within a single stack.
    """
    sc = _scene()
    _define(sc, "slab", 0, 0.3)
    e = _carrier(sc, "carrier", 0.5, 10)

    accepted = sc.AddItem(e, "slab", 2)
    check_eq(accepted, 1,
             "only one 0.3-weight slab fits alongside the other in 0.5")
    check_eq(_stacks(e), [("slab", 1)], "one slab must be held")
    check_eq(sc.AddItem(e, "slab", 1), 0,
             "a third slab has no headroom left, so it must be refused")


def test_a_zero_max_weight_means_the_weight_system_is_off():
    """MaxWeight <= 0 disables weight entirely, separately from MaxSlots.

    Distinguished from "weightless item": here the CAPACITY is off, so a
    genuinely heavy item can be carried without limit.
    """
    sc = _scene()
    _define(sc, "anvil", 0, 500.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "anvil", 3), 3,
             "with MaxWeight 0 there is no weight limit at all")
    check_eq(sc.GetInventoryWeight(e), 1500.0, "setup: the inventory is genuinely heavy")
    check_eq(sc.IsInventoryFull(e), False,
             "an over-weight inventory with no MaxWeight is still not full")


# --------------------------------------------------------------------------
# 4. Dropping and removing.

def test_dropping_part_of_a_stack_preserves_the_remainder():
    """Drop 3 of 10, keep 7, in ONE stack.

    The remainder must survive and the emptied stack must be dropped rather
    than left as a zero-quantity row -- a row at Quantity 0 still consumes a
    slot, so keeping it silently eats the player's capacity.
    """
    sc = _scene()
    _define(sc, "ration", 0, 0.5)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "ration", 10), 10, "setup: ten rations")
    check_eq(_stacks(e), [("ration", 10)], "setup: one stack")

    check_eq(sc.RemoveItemFromInventory(e, "ration", 3), 3,
             "removing three must report three removed")
    check_eq(sc.GetItemCount(e, "ration"), 7, "seven must remain")
    check_eq(_stacks(e), [("ration", 7)],
             "the remainder must stay in ONE stack, not split or zeroed")

    # Over-dropping takes what is there and reports only what it took.
    check_eq(sc.RemoveItemFromInventory(e, "ration", 100), 7,
             "over-removing must report the seven it actually took, not the hundred asked")
    check_eq(sc.GetItemCount(e, "ration"), 0, "nothing remains")
    check_eq(_stacks(e), [],
             "an emptied stack must be REMOVED, not left as a Quantity 0 row that "
             "still consumes a slot")
    check_eq(e.GetInventoryComponent().GetStackCount(), 0.0,
             "so the freed slot is genuinely free again")
    check_eq(sc.IsInventoryFull(e), False, "and the inventory is no longer full")

    # And the freed capacity is genuinely usable again.
    check_eq(sc.AddItem(e, "ration", 4), 4, "the inventory must accept items again")
    check_eq(_stacks(e), [("ration", 4)], "in a fresh single stack")


def test_remove_item_consumes_from_the_last_stack_first():
    """LIFO, which is what matches player expectation.

    "Drop the pack you just picked up, not the last one you found." Two
    stacks of the same item is the only shape where the order is observable,
    so the assertion is on which stack shrank.
    """
    sc = _scene()
    _define(sc, "arrow", 3, 0.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    # MaxStack 3, so the second add of 3 cannot top up the first -- a partial
    # stack is never left for the order to be observable in.
    check_eq(sc.AddItem(e, "arrow", 3), 3, "setup: a full stack of 3")
    check_eq(sc.AddItem(e, "arrow", 3), 3, "setup: a second stack of 3")
    check_eq(_stacks(e), [("arrow", 3), ("arrow", 3)], "setup: two stacks")

    check_eq(sc.RemoveItem(e, "arrow", 1), 1, "one arrow must be removed")
    check_eq(_stacks(e), [("arrow", 3), ("arrow", 2)],
             "the LAST stack must shrink first -- newest pickups land in new stacks, "
             "so dropping the pack you just found is what a player expects")
    check_eq(sc.GetItemCount(e, "arrow"), 5, "the total must drop by exactly one")

    check_eq(sc.RemoveItem(e, "arrow", 1), 1, "one more")
    check_eq(_stacks(e), [("arrow", 3), ("arrow", 1)], "and again from the last stack")

    # Exhausting the last stack removes it, revealing the one underneath.
    check_eq(sc.RemoveItem(e, "arrow", 1), 1, "the last of the partial stack")
    check_eq(_stacks(e), [("arrow", 3)], "the emptied stack must be gone")
    check_eq(sc.RemoveItem(e, "arrow", 5), 3, "removing 5 with only 3 left takes 3")
    check_eq(_stacks(e), [], "and leaves nothing")


def test_removing_an_item_that_is_not_held_reports_zero_and_changes_nothing():
    sc = _scene()
    _define(sc, "herb", 0, 0.0)
    e = _carrier(sc, "carrier", 0.0, 0)
    check_eq(sc.AddItem(e, "herb", 2), 2, "setup")

    check_eq(sc.RemoveItem(e, "weapon_sword", 1), 0, "an absent item must report 0")
    check_eq(sc.RemoveItem(e, "herb", 0), 0, "a quantity of 0 must report 0")
    check_eq(_stacks(e), [("herb", 2)], "and neither may change the inventory")


# --------------------------------------------------------------------------
# 5. Unknown item ids -- the documented behaviour, asserted as documented.

def test_an_unknown_item_id_gets_a_max_stack_of_one_rather_than_being_refused():
    """A misspelled id is carried VISIBLY instead of being silently dropped.

    The source is explicit: "It is better to carry a misspelled id visibly in
    the inventory than to refuse the pickup silently; MaxStack of 1 keeps the
    damage contained." So an unknown id is ACCEPTED, one per stack, and weighs
    nothing.

    Asserted as the documented behaviour, including the containment: seven
    requested becomes one per stack, not seven in one unbounded pile.
    """
    sc = _scene()
    e = _carrier(sc, "carrier", 0.0, 5)

    check_eq(sc.FindItemDefinition("nosuchthing"), None,
             "setup: this id must genuinely have no definition")
    check_eq(sc.HasItem(e, "nosuchthing"), False, "setup: and must not be held")

    accepted = sc.AddItem(e, "nosuchthing", 7)
    check_eq(accepted, 5,
             "an unknown id must not be REFUSED -- five slots allow five stacks of one")
    check_eq(_stacks(e),
             [("nosuchthing", 1)] * 5,
             "an unknown item must get MaxStack 1, so each occurrence is its own row "
             "instead of one unbounded pile hiding the typo")
    check_eq(sc.GetInventoryWeight(e), 0.0,
             "an unknown id weighs 0.0, since there is no definition to read a weight from")
    check_eq(sc.FindItemDefinition("nosuchthing"), None,
             "adding an item must not create a definition for it")


def test_a_weightless_item_bypasses_the_weight_limit():
    """The consequential half of the unknown-id default, worth pinning on its own.

    AddItem's weight guard is `MaxWeight > 0 && unitWeight > 0`, so a
    zero-weight item is never weight-checked. Right for a quest key; also
    true for every unknown id, since those default to weight 0.0. Asserted so
    that a future change to the guard shows up here rather than as "the player
    suddenly cannot pick anything up".
    """
    sc = _scene()
    _define(sc, "questkey", 0, 0.0)   # authored as weightless on purpose
    e = _carrier(sc, "carrier", 1.0, 0)

    check_eq(sc.AddItem(e, "questkey", 4), 4,
             "a weightless item must not be limited by MaxWeight")
    check_eq(sc.GetInventoryWeight(e), 0.0, "and must contribute nothing to the weight")
    check_eq(sc.IsInventoryFull(e), False,
             "so an otherwise-empty inventory with MaxWeight 1 is not full")


# --------------------------------------------------------------------------
# 6. Derived queries.

def test_get_item_count_sums_across_every_matching_stack():
    """The sum, not the first stack's quantity.

    "GetItemCount sums rather than returning the first stack's Quantity, so an
    entity may legitimately hold several stacks of the same itemId." A
    first-stack implementation would report 10 where the answer is 11.
    """
    sc = _scene()
    _define(sc, "bolt", 10, 0.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "bolt", 10), 10, "setup")
    check_eq(sc.AddItem(e, "bolt", 10), 10, "setup: a second full stack")
    check_eq(_stacks(e), [("bolt", 10), ("bolt", 10)], "setup: two stacks of ten")
    check_eq(sc.GetItemCount(e, "bolt"), 20,
             "GetItemCount must SUM the stacks, not report the first one")
    check_eq(sc.AddItem(e, "bolt", 10), 10, "setup: a third")
    check_eq(sc.GetItemCount(e, "bolt"), 30, "and keep summing")


def test_has_item_compares_with_greater_or_equal_so_a_zero_threshold_always_passes():
    """A documented edge case, pinned because it is a trap for callers.

    "The comparison is >=, so a quantity of 0 or less returns TRUE for any
    entity, including one holding none of the item." A caller meaning "has
    any" must pass 1.
    """
    sc = _scene()
    _define(sc, "coin", 0, 1.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.HasItem(e, "coin"), False,
             "an item held zero times is not had, at the default threshold of 1")
    check_eq(sc.HasItem(e, "coin", 0), True,
             "HasItem with a threshold of 0 passes even for an item held ZERO times -- "
             "the comparison is >=, and callers meaning 'has any' must pass 1")
    check_eq(sc.HasItem(e, "coin", -1), True, "and a negative threshold likewise passes")

    check_eq(sc.AddItem(e, "coin", 2), 2, "setup")
    check_eq(sc.HasItem(e, "coin", 2), True, "true at exactly what is held")
    check_eq(sc.HasItem(e, "coin", 3), False, "false above it")


def test_is_inventory_full_reports_on_slots_and_on_weight_independently():
    """Two separate limits, and each must be able to be the binding one.

    Also pinned: zero means UNLIMITED for both, which is why a non-carrier
    reads as not-full rather than as full.
    """
    sc = _scene()
    _define(sc, "coin", 0, 1.0)
    # MaxStack 1, so each feather needs its own slot. An unlimited-stack item
    # would fit all three in one row and the slot limit would never bind.
    _define(sc, "feather", 1, 0.01)

    # Slots bind.
    slotty = _carrier(sc, "slotty", 100.0, 2)
    check_eq(sc.IsInventoryFull(slotty), False, "an empty inventory is not full")
    check_eq(sc.AddItem(slotty, "feather", 3), 2, "two slots, three requested")
    check_eq(_stacks(slotty), [("feather", 1), ("feather", 1)],
             "the one that did not fit must not have left a row behind")
    check_eq(sc.IsInventoryFull(slotty), True, "the slot limit must bind")

    # Weight binds, with slots wide open.
    heavy = _carrier(sc, "heavy", 2.0, 0)
    check_eq(sc.IsInventoryFull(heavy), False, "an empty inventory is not full")
    check_eq(sc.AddItem(heavy, "coin", 2), 2, "setup: two coins at 1.0 each")
    check_near(sc.GetInventoryWeight(heavy), 2.0, 1e-6, "setup: exactly at the limit")
    check_eq(sc.IsInventoryFull(heavy), True,
             "with MaxSlots 0 the weight limit must still be able to bind")
    check_eq(sc.AddItem(heavy, "coin", 1), 0, "and nothing more fits")

    # Both unlimited: never full.
    limitless = _carrier(sc, "limitless", 0.0, 0)
    check_eq(sc.AddItem(limitless, "coin", 50), 50, "no limit of either kind")
    check_eq(sc.IsInventoryFull(limitless), False,
             "0 means unlimited for both limits, so this can never be full")


def test_the_bound_inventory_api_is_present_with_the_documented_defaults():
    """Seven bound Scene methods, and the quantity default that gives them
    their meaning.

    quantity defaults to 1 throughout, so AddItem(e, "pistol") means one
    pistol. Asserted by CALLING without the argument -- a default that
    changed to 0 would make every one of these a silent no-op, and a
    presence check alone would not see it.
    """
    sc = _scene()
    for name in ("AddItem", "RemoveItem", "RemoveItemFromInventory", "DropItemAt",
                 "PickUpDroppedItem", "GetItemCount", "HasItem",
                 "GetInventoryWeight", "IsInventoryFull", "FindItemDefinition"):
        check(hasattr(sc, name), "Scene must expose %s" % name)

    _define(sc, "pistol", 1, 2.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "pistol"), 1,
             "AddItem must default quantity to 1 -- 'one pistol', not 'nothing'")
    check_eq(sc.GetItemCount(e, "pistol"), 1, "setup: one pistol held")
    check_eq(sc.HasItem(e, "pistol"), True, "HasItem must default its threshold to 1")
    check_eq(sc.RemoveItemFromInventory(e, "pistol"), 1,
             "RemoveItemFromInventory must default quantity to 1")
    check_eq(sc.GetItemCount(e, "pistol"), 0, "setup: nothing held")

    # FindItemDefinition returns a live reference, and None for an unknown id.
    defn = sc.FindItemDefinition("pistol")
    check(defn is not None, "FindItemDefinition must find an authored definition")
    check_eq(defn.MaxStack, 1, "MaxStack must be readable off the returned reference")
    check_near(defn.Weight, 2.0, 1e-6, "Weight must be readable too")
    check_eq(sc.FindItemDefinition("nope"), None, "an unknown id must read as None")


def test_components_are_snapshot_copies_not_live_references():
    """Why every other test re-reads the component off the entity.

    AddOrReplace copies the component in, so a local object mutated afterwards
    changes nothing on the entity. Get, by contrast, returns a live reference.
    A test that asserted on its local object would be testing the wrong thing
    entirely, and this one makes that explicit so the pattern stays visible.
    """
    sc = _scene()
    e = _carrier(sc, "carrier", 5.0, 3)
    check_eq(e.GetInventoryComponent().MaxSlots, 3, "setup")

    inv = Wasteland.InventoryComponent()
    inv.MaxWeight = 9.0
    inv.MaxSlots = 9
    e.AddOrReplaceInventoryComponent(inv)

    check_eq(e.GetInventoryComponent().MaxWeight, 9.0,
             "what AddOrReplace was given must be what the entity holds")
    inv.MaxWeight = 12345.0
    check_eq(e.GetInventoryComponent().MaxWeight, 9.0,
             "mutating the local copy afterwards must NOT reach the entity -- "
             "AddOrReplace takes a snapshot, which is why tests re-read via Get")

    # Get, however, is live: this is the only supported way to change capacity.
    e.GetInventoryComponent().MaxWeight = 42.0
    check_eq(e.GetInventoryComponent().MaxWeight, 42.0,
             "Get returns a live reference, so writing through it must take effect")


# --------------------------------------------------------------------------
# 7. Serializer round trip.

def test_stacks_and_limits_survive_the_scene_serializer_exactly():
    """SceneSerializer writes and reads InventoryComponent wholesale.

    Both limits AND the contents, compared exactly after a real save/load into
    a fresh scene -- which is the only way to exercise the reader rather than
    the writer alone.
    """
    root = _tmpdir()
    src = _scene()
    _define(src, "shell", 60, 0.05)
    _define(src, "ore", 1, 4.0)
    carrier = _carrier(src, "carrier", 12.5, 4)

    check_eq(src.AddItem(carrier, "shell", 25), 25, "setup: one stack of 25")
    check_eq(src.AddItem(carrier, "ore", 2), 2, "setup: two single-item stacks")

    path = os.path.join(root, "round.wastescene")
    check(src.SaveScene(path), "SaveScene must report success")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")

    # GetEntities returns nothing in a headless scene; the transform view is
    # the one that enumerates, so the loaded entity is found through it.
    entities = dst.GetAllEntitiesWithTransform()
    names = [e.GetName() for e in entities]
    check("carrier" in names, "the loaded scene must contain the carrier: %r" % names)

    back = [e for e in entities if e.GetName() == "carrier"][0]
    inv = back.GetInventoryComponent()
    check_eq(inv.MaxWeight, 12.5, "MaxWeight must round-trip exactly")
    check_eq(inv.MaxSlots, 4, "MaxSlots must round-trip exactly")
    check_eq([(s.ItemId, s.Quantity) for s in inv.Items],
             [("shell", 25), ("ore", 1), ("ore", 1)],
             "every stack must round-trip, in order, including two stacks of the "
             "same id -- IsValid does not deduplicate")
    check_eq(dst.GetItemCount(back, "shell"), 25, "GetItemCount must agree after the load")

    # The definitions came back too, so weight is computable on the far side --
    # 25 * 0.05 + 2 * 4.0. If they had not, this would read 0.0.
    check_near(dst.GetInventoryWeight(back), 9.25, 1e-4,
               "the restored item definitions must make the restored weight computable")
    check_eq(dst.IsInventoryFull(back), False, "and the limits must still describe it")


def test_an_empty_inventory_round_trips_as_empty_rather_than_going_missing():
    """The reader creates the component before it knows the contents.

    "an inventory whose limits are saved but whose contents were not would be
    a very confusing bug", and a scene with an empty inventory is
    distinguishable from one with no InventoryComponent key at all.
    """
    root = _tmpdir()
    src = _scene()
    carrier = _carrier(src, "carrier", 7.0, 3)
    check_eq(carrier.GetInventoryComponent().Items, [],
             "setup: the inventory must really be empty")

    path = os.path.join(root, "empty.wastescene")
    check(src.SaveScene(path), "SaveScene must report success")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")
    entities = [e for e in dst.GetAllEntitiesWithTransform() if e.GetName() == "carrier"]
    check_eq(len(entities), 1, "the carrier must be present exactly once")

    inv = entities[0].GetInventoryComponent()
    check_eq(inv.MaxWeight, 7.0, "the limits must survive even with no contents")
    check_eq(inv.MaxSlots, 3, "both of them")
    check_eq(list(inv.Items), [],
             "an empty inventory must come back empty, not missing or invented")
    check_eq(dst.IsInventoryFull(entities[0]), False, "and still not be full")

# --------------------------------------------------------------------------
# 8. DropItemAt: a drop that actually puts something in the world.
#
# The reason this section exists: Scene::DropItem used to be a function whose
# body was `return RemoveItem(...)`. Its name promised a drop, the binding
# comment claimed it "spawns world geometry", and no entity was ever created.
# A caller who trusted either spawned nothing and the items were simply gone.
#
# It is renamed to RemoveItemFromInventory, whose body is unchanged, and the
# real drop is DropItemAt. The tests below pin the DOCUMENTED behaviour --
# which is the only thing that makes a rename worth anything.

def test_drop_item_at_removes_from_the_inventory_AND_spawns_a_pile():
    """The claim the old name made, asserted on both halves at once.

    Asserting only "the count went down" would pass against the old lie, and
    asserting only "an entity appeared" would pass against an implementation
    that copied rather than removed. Both, on the same call, is the whole
    point.
    """
    sc = _scene()
    _define(sc, "ration", 0, 0.5)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "ration", 10), 10, "setup: ten rations held")
    check_eq(sc.RemoveItemFromInventory(e, "ration", 1), 1,
             "setup: the renamed remove-only method still works and spawns nothing")
    check_eq(sc.GetItemCount(e, "ration"), 9, "setup: nine remain")

    pile = sc.DropItemAt(e, "ration", 3)
    check_eq(pile.IsValid(), True,
             "DropItemAt must return a REAL entity -- the old DropItem returned "
             "a count and spawned nothing at all")
    check_eq(sc.GetItemCount(e, "ration"), 6,
             "and the inventory must be down by exactly what was dropped")

    # The pile must SAY what it is, or a script has no way to find out what it
    # just dropped -- which is the same invisibility the old bug had.
    check_eq(pile.HasDroppedItemComponent(), True,
             "the spawned entity must carry DroppedItemComponent")
    d = pile.GetDroppedItemComponent()
    check_eq(d.ItemId, "ration", "the pile must name the item dropped")
    check_eq(d.Quantity, 3, "and record how many -- 3, not the 3 requested by luck")


def test_a_pile_records_the_count_ACTUALLY_dropped_never_the_request():
    """Over-dropping takes what is there; the pile must not over-claim.

    A pile that said 100 when 7 were dropped would conjure 93 items out of
    nothing on the next pickup, so the number written is the number removed.
    """
    sc = _scene()
    _define(sc, "ore", 1, 4.0)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "ore", 7), 7, "setup: seven ore")

    pile = sc.DropItemAt(e, "ore", 100)
    check_eq(pile.IsValid(), True, "a partial drop still spawns -- it is not a failure")
    check_eq(pile.GetDroppedItemComponent().Quantity, 7,
             "the pile must claim the SEVEN actually removed, not the hundred asked for")
    check_eq(sc.GetItemCount(e, "ore"), 0, "and the inventory is empty")


def test_a_drop_spawns_nothing_when_there_is_nothing_to_drop():
    """The negative case, which is the only one that distinguishes the API.

    Every rejection returns an INVALID Entity rather than a valid one holding
    zeros. A test that only checked "the inventory did not change" would pass
    against an implementation that spawned an empty pile for a miss -- which
    is a ghost entity in the world that nothing can ever pick up.
    """
    sc = _scene()
    _define(sc, "coin", 0, 1.0)

    # No inventory at all.
    plain = sc.CreateEntity("plain")
    check_eq(sc.DropItemAt(plain, "coin", 1).IsValid(), False,
             "an entity with no inventory must drop nothing")

    # Has an inventory, but not that item.
    e = _carrier(sc, "carrier", 0.0, 0)
    check_eq(sc.DropItemAt(e, "coin", 1).IsValid(), False,
             "an item id the carrier does not hold must drop nothing")

    # Bad arguments.
    check_eq(sc.AddItem(e, "coin", 2), 2, "setup: two coins")
    check_eq(sc.DropItemAt(e, "coin", 0).IsValid(), False,
             "a zero quantity is not a drop")
    check_eq(sc.DropItemAt(e, "coin", -5).IsValid(), False,
             "a NEGATIVE quantity is not a drop -- and must not remove items")
    check_eq(sc.DropItemAt(e, "", 1).IsValid(), False,
             "an empty item id is not a drop")
    check_eq(sc.GetItemCount(e, "coin"), 2,
             "after all those refusals the two coins must still be held")

    # And exactly one entity was created by none of the refusals.
    names = [x.GetName() for x in sc.GetAllEntitiesWithTransform()]
    check_eq([n for n in names if n.startswith("Dropped_")], [],
             "no refusal may leave a pile behind: %r" % names)


def test_the_pile_lands_at_the_carriers_position_plus_the_offset():
    """Where the pile appears is part of the contract.

    Offset alone would ignore where the carrier is; carrier position alone
    would ignore the caller's intent. The offset is added in WORLD space and
    is not rotated by the carrier, because it is an absolute world delta.
    """
    sc = _scene()
    _define(sc, "shell", 0, 0.1)
    e = _carrier(sc, "carrier", 0.0, 0)
    check_eq(sc.AddItem(e, "shell", 3), 3,
             "setup: the carrier must actually hold something to drop")
    e.SetPosition(Vec3(10.0, 2.0, -5.0))

    pile = sc.DropItemAt(e, "shell", 1, Vec3(0.5, 0.0, 0.25))
    check_eq(pile.IsValid(), True, "setup: the drop must succeed")

    pos = pile.GetPosition()
    check_near(pos.x, 10.5, 1e-4, "pile X must be the carrier's X plus the offset")
    check_near(pos.y, 2.0, 1e-4, "pile Y must be the carrier's Y -- the offset adds nothing here")
    check_near(pos.z, -4.75, 1e-4, "pile Z must be the carrier's Z plus the offset")

    # The default offset is zero, so the pile lands exactly on the carrier.
    pile2 = sc.DropItemAt(e, "shell", 1)
    pos2 = pile2.GetPosition()
    check_near(pos2.x, 10.0, 1e-4, "a default offset of zero must land on the carrier")
    check_near(pos2.y, 2.0, 1e-4, "on every axis")


def test_a_dropped_stack_is_serialized_so_it_survives_a_reload():
    """DropItemAt is useless if the pile vanishes on the next save.

    This is the same class of bug as the audio one next door: a component the
    engine creates at runtime and the serializer never heard of. Asserted on
    a real save/load into a fresh scene, because a writer-only assertion
    would pass against a reader that silently drops the node.
    """
    root = _tmpdir()
    src = _scene()
    _define(src, "shell", 0, 0.1)
    carrier = _carrier(src, "carrier", 0.0, 0)
    check_eq(src.AddItem(carrier, "shell", 5), 5, "setup: five shells")
    pile = src.DropItemAt(carrier, "shell", 2)
    check_eq(pile.IsValid(), True, "setup: the pile must exist before the save")

    path = os.path.join(root, "drop.wastescene")
    check(src.SaveScene(path), "SaveScene must report success")

    dst = _scene()
    check(dst.LoadScene(path), "LoadScene must report success")

    entities = dst.GetAllEntitiesWithTransform()
    piles = [x for x in entities if x.HasDroppedItemComponent()]
    check_eq(len(piles), 1, "exactly one pile must come back: %r"
             % [x.GetName() for x in entities])
    back = piles[0]
    check_eq(back.GetName(), "Dropped_shell", "and it must keep its name")
    check_eq(back.GetDroppedItemComponent().ItemId, "shell",
             "the pile must still say what it is")
    check_eq(back.GetDroppedItemComponent().Quantity, 2,
             "and how many -- a pile that reloads empty is worse than one that "
             "does not reload at all")

    pos = back.GetPosition()
    check_near(pos.x, 0.0, 1e-4, "the pile's transform must round-trip too")


def test_dropped_item_round_trips_byte_identically():
    """A second save must produce the same bytes as the first.

    This is the check that catches an asymmetric writer/reader pair -- a field
    written but not read makes the second save differ, and a field read but
    not written loses data on the first reload. Comparing the two FILES, not
    two component reads, is what makes it a real test of the round trip.
    """
    root = _tmpdir()
    src = _scene()
    _define(src, "ore", 0, 1.0)
    carrier = _carrier(src, "carrier", 0.0, 0)
    check_eq(src.AddItem(carrier, "ore", 3), 3, "setup: three ore")
    check_eq(src.DropItemAt(carrier, "ore", 2).IsValid(), True, "setup: one pile")

    first = os.path.join(root, "first.wastescene")
    second = os.path.join(root, "second.wastescene")
    check(src.SaveScene(first), "first save must succeed")

    mid = _scene()
    check(mid.LoadScene(first), "the load must succeed")
    check(mid.SaveScene(second), "the second save must succeed")

    with open(first, "r", encoding="utf-8", newline="") as fh:
        a = fh.read()
    with open(second, "r", encoding="utf-8", newline="") as fh:
        b = fh.read()

    check("DroppedItemComponent" in a,
          "setup: the first file must actually contain the pile, or this test "
          "proves nothing")

    # Compared as a MULTISET of lines, not as raw text. The entity ORDER of a
    # re-save differs from the first even for components this change never
    # touched -- SceneSerializer writes entities in registry-view order, and
    # EnTT's view order after a load is not the file's order. That is
    # pre-existing and outside this task; asserting raw equality would be
    # asserting a property the serializer does not have, and would have gone
    # red for a reason unrelated to the pile.
    #
    # A multiset comparison still catches every asymmetry this test exists for:
    # a field written but not read changes a VALUE on a line, and a field read
    # but not written removes a line entirely. Neither can hide behind a
    # reordering.
    check(sorted(a.splitlines()) == sorted(b.splitlines()),
          "save -> load -> save must preserve the file's content exactly; a "
          "difference means the reader loses or invents something")


def test_picking_a_pile_back_up_restores_the_count():
    """The other direction, so the drop is a round trip rather than a delete.

    This is what makes DropItemAt different from RemoveItemFromInventory in a
    way a caller can rely on: nothing is destroyed, it moves.
    """
    sc = _scene()
    _define(sc, "ration", 0, 0.5)
    e = _carrier(sc, "carrier", 0.0, 0)

    check_eq(sc.AddItem(e, "ration", 8), 8, "setup: eight rations")
    pile = sc.DropItemAt(e, "ration", 3)
    check_eq(sc.GetItemCount(e, "ration"), 5, "five remain after dropping three")

    check_eq(sc.PickUpDroppedItem(pile, e), 3,
             "picking the pile up must move all three")
    check_eq(sc.GetItemCount(e, "ration"), 8,
             "and the count must be RESTORED, not merely increased")
    check_eq(sc.IsEntityValid(pile), False,
             "a fully collected pile must be destroyed -- or emptied piles "
             "accumulate as invisible litter")


def test_a_partial_pickup_leaves_the_remainder_on_the_ground():
    """Taking 2 of 5 must leave 3, and the pile must survive.

    A pickup that destroyed the pile regardless would delete items whenever
    the carrier was nearly full, which is the exact class of silent data loss
    that made the original DropItem worth renaming.
    """
    sc = _scene()
    _define(sc, "shell", 0, 1.0)
    src = _carrier(sc, "source", 0.0, 0)
    dst = _carrier(sc, "dest", 1.5, 1)   # one slot, room for one 1.0-weight shell

    check_eq(sc.AddItem(src, "shell", 5), 5, "setup: five shells on the source")
    pile = sc.DropItemAt(src, "shell", 5)
    check_eq(pile.GetDroppedItemComponent().Quantity, 5, "setup: a pile of five")

    # MaxSlots is 1 and the destination is empty, so AddItem can take exactly
    # one -- a genuine partial pickup rather than a contrived one.
    check_eq(sc.PickUpDroppedItem(pile, dst), 1,
             "a capacity-limited pickup must take only what fits and report THAT")
    check_eq(sc.GetItemCount(dst, "shell"), 1, "the destination holds the one")
    check_eq(sc.IsEntityValid(pile), True,
             "a partially collected pile must NOT be destroyed")
    check_eq(pile.GetDroppedItemComponent().Quantity, 4,
             "the four that did not fit must still be on the ground, not deleted")


def test_pickup_refuses_a_source_that_is_not_a_pile():
    """The negative case: nothing that is not a pile may be picked up.

    Without this, PickUpDroppedItem could destroy an arbitrary entity that
    happened to be passed, which is a much worse failure than returning 0.
    """
    sc = _scene()
    _define(sc, "coin", 0, 1.0)
    e = _carrier(sc, "carrier", 0.0, 0)
    check_eq(sc.AddItem(e, "coin", 2), 2, "setup: two coins")

    plain = sc.CreateEntity("plain")
    check_eq(sc.PickUpDroppedItem(plain, e), 0,
             "an entity with no DroppedItemComponent is not a pile")
    check_eq(sc.IsEntityValid(plain), True,
             "and must NOT be destroyed by the attempt")

    # A pile whose id does not resolve still moves -- the drop does not
    # require an authored definition -- but it moves nothing, because the
    # destination is the thing that must hold it.
    pile = sc.DropItemAt(e, "coin", 2)
    check_eq(pile.IsValid(), True, "setup: a real pile")
    noinv = sc.CreateEntity("noinv")
    check_eq(sc.PickUpDroppedItem(pile, noinv), 0,
             "a carrier with no inventory accepts nothing")
    check_eq(pile.GetDroppedItemComponent().Quantity, 2,
             "and the pile must be untouched, not consumed")
