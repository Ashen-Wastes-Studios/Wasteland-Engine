#!/usr/bin/env python3
"""Generate the Ash District blockout that ships as DemoGame.wastescene.

Run from the repo root:

    python tools/make_demogame_ash_district.py

WHAT THIS FILE IS
-----------------
DemoGame.wastescene is the engine's shipped demo level AND the fixture for
`tests/headless/test_feature_coverage.py` and
`tests/headless/test_avb_demogame_scene.py`. Two kinds of content are mixed
in one file:

  * FIXTURES -- the gameplay entities (player, enemies, pickups), the
    component-showcase row, the volumetric volumes, the water plane. These are
    read out of the existing file VERBATIM and re-emitted unchanged, because
    tests assert their exact coordinates (ShowcaseAudioSource must sit at
    x=368, z=1090) and their presence is what the coverage test measures.
  * CITY -- everything else, regenerated from the district plan below.

The city is not a skyline of identical boxes. It is a district plan laid over
a real street hierarchy, because "realistic blockout" and "random boxes on a
grid" look nothing alike at eye level.

THE DISTRICT PLAN (Necroware, post-Prelude, 2197)
-------------------------------------------------
The Ash District is the last free zone: a sprawl of ruins, bunkers and
survivors built on the corpses of pre-war corporate cities. So the blockout
is a LA/SF-style grid that has been fought over, collapsed and resettled --
not a planned cyberpunk set.

  core        | Corporate Plaza / arcology cluster. Element-Zero glass towers,
              | 180-340 m, cyan/violet glow. Some still stand, some are
              | collapsed stubs. Holds the Spire analogue: one 200-storey
              | tower converted to raw Element Zero.
  financial   | The old pre-war high-rise ring around the core. Steel and
              | glass, 80-160 m, a quarter of it collapsed.
  lower       | The Lower Zones: dense hab-blocks built from recycled Element
              | Zero, 25-60 m on a 40 m pitch. Repetitive on purpose -- the
              | walls hum, the ArkMind receivers are in them. Player spawn.
  oldcity     | The pre-war grid that survived the Clusterfuck. Irregular
              | mid-rise, wide arterials, a third of it rubble.
  crucible    | The combat zone (Dogtown analogue). Walled off, dense chaotic
              | shacks 2-8 m, scrap colours, failing neon, walkways,
              | barricades, sandbags, watchtowers, and one hot entertainment
              | district with the bar called The Last Note.
  hills       | SF-style terraced hill district. Terrain steps up to the north
              | in 4 m bands; row houses sit on the terraces.
  industrial  | The Sprawl: warehouses, tanks, stacks, container yards.
  waterfront  | The bay. Seawall, piers, cranes, container sheds.
  wasteland   | The ring outside the city: ruined silhouette towers and
              | craters, dark and unlit, for depth on the horizon.

CONVENTIONS
-----------
  * Units are metres. Ground top surface is y=0, so a building of height h
    sits at y = h/2.
  * Buildings carry NO rigid bodies or colliders. The shipped scene has none
    either; adding two thousand static bodies would be a physics-world cost
    the blockout does not need. See the note in the README if that changes.
  * Entity ids are allocated from disjoint ranges per content class so a
    regeneration cannot collide with a fixture id.
"""
from __future__ import annotations

import functools
import os
import random
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENE_PATH = os.path.join(
    REPO, "DemonCore-Editor", "assets", "scenes", "DemoGame.wastescene"
)

SEED = 2197

# ---------------------------------------------------------------------------
# Formatting helpers
# ---------------------------------------------------------------------------


def num(v: float) -> str:
    """Compact float for YAML. Avoids '-0' and trailing zeros."""
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    if s in ("-0", ""):
        return "0"
    return s


def vec(v) -> str:
    return "[" + ", ".join(num(x) for x in v) + "]"


# ---------------------------------------------------------------------------
# Emitters. Each returns one entity block as a YAML string.
# ---------------------------------------------------------------------------

_ENTITIES: list[str] = []
_USED_IDS: set[int] = set()
_COUNTERS: dict[str, int] = {}


def _alloc(prefix: str) -> int:
    n = _COUNTERS.get(prefix, 0)
    _COUNTERS[prefix] = n + 1
    return n


def emit(eid: int, block: str) -> None:
    if eid in _USED_IDS:
        raise AssertionError(f"duplicate entity id {eid}")
    _USED_IDS.add(eid)
    if not block.endswith("\n"):
        block += "\n"
    _ENTITIES.append(block)


def _head(eid: int, tag: str, pos, scale, rot=(0.0, 0.0, 0.0)) -> str:
    return (
        f"  - Entity: {eid}\n"
        f"    TagComponent:\n"
        f"      Tag: {tag}\n"
        f"    TransformComponent:\n"
        f"      Translation: {vec(pos)}\n"
        f"      Rotation: {vec(rot)}\n"
        f"      Scale: {vec(scale)}\n"
    )


def _material(albedo, metallic, roughness, emission, emission_intensity) -> str:
    return (
        f"    MaterialComponent:\n"
        f"      Albedo: {vec(albedo)}\n"
        f"      TextureWidth: 0\n"
        f"      TextureHeight: 0\n"
        f"      TextureIndex: 0\n"
        f'      TexturePath: ""\n'
        f"      HasGeneratedMaps: false\n"
        f"      NormalStrength: 0\n"
        f"      Metallic: {num(metallic)}\n"
        f"      Roughness: {num(roughness)}\n"
        f"      EmissionColor: {vec(emission)}\n"
        f"      EmissionIntensity: {num(emission_intensity)}\n"
        f"      DisplacementScale: 0\n"
    )


def box(eid, tag, pos, scale, albedo, metallic=0.2, roughness=0.8,
        emission=(0.0, 0.0, 0.0, 1.0), emission_intensity=0.0,
        rot=(0.0, 0.0, 0.0), extra="") -> None:
    """A lit, textured cube -- the workhorse of the blockout."""
    emit(eid, _head(eid, tag, pos, scale, rot)
         + "    CubeRendererComponent:\n"
         + f"      Color: {vec(albedo)}\n"
         + "      TextureWidth: 0\n"
         + "      TextureHeight: 0\n"
         + "      TextureIndex: 0\n"
         + "      TilingFactor: 1\n"
         + _material(albedo, metallic, roughness, emission, emission_intensity)
         + extra)


def cylinder(eid, tag, pos, radius, height, albedo, metallic=0.2,
             roughness=0.8, emission=(0.0, 0.0, 0.0, 1.0),
             emission_intensity=0.0, segments=12, rot=(0.0, 0.0, 0.0),
             extra="") -> None:
    emit(eid, _head(eid, tag, pos, (1.0, 1.0, 1.0), rot)
         + "    CylinderRendererComponent:\n"
         + f"      Color: {vec(albedo)}\n"
         + f"      Radius: {num(radius)}\n"
         + f"      Height: {num(height)}\n"
         + f"      Segments: {segments}\n"
         + "      TextureIndex: 0\n"
         + "      TilingFactor: 1\n"
         + _material(albedo, metallic, roughness, emission, emission_intensity)
         + extra)


def sphere(eid, tag, pos, radius, albedo, metallic=0.2, roughness=0.8,
           emission=(0.0, 0.0, 0.0, 1.0), emission_intensity=0.0,
           sectors=12, stacks=6, extra="") -> None:
    emit(eid, _head(eid, tag, pos, (1.0, 1.0, 1.0))
         + "    SphereRendererComponent:\n"
         + f"      Color: {vec(albedo)}\n"
         + f"      Radius: {num(radius)}\n"
         + f"      Sectors: {sectors}\n"
         + f"      Stacks: {stacks}\n"
         + "      TextureIndex: 0\n"
         + "      TilingFactor: 1\n"
         + _material(albedo, metallic, roughness, emission, emission_intensity)
         + extra)


def icosphere(eid, tag, pos, radius, albedo, metallic=0.2, roughness=0.9,
             emission=(0.0, 0.0, 0.0, 1.0), emission_intensity=0.0,
              subdivisions=1, extra="") -> None:
    emit(eid, _head(eid, tag, pos, (1.0, 1.0, 1.0))
         + "    IcoSphereRendererComponent:\n"
         + f"      Color: {vec(albedo)}\n"
         + f"      Radius: {num(radius)}\n"
         + f"      Subdivisions: {subdivisions}\n"
         + "      TextureIndex: 0\n"
         + "      TilingFactor: 1\n"
         + _material(albedo, metallic, roughness, emission, emission_intensity)
         + extra)


def point_light(eid, tag, pos, color, intensity, range_, falloff=2.0) -> None:
    emit(eid, _head(eid, tag, pos, (1.0, 1.0, 1.0))
         + "    PointLightComponent:\n"
         + f"      Color: {vec(color)}\n"
         + f"      Intensity: {num(intensity)}\n"
         + f"      Range: {num(range_)}\n"
         + f"      Falloff: {num(falloff)}\n")


def light_extra(color, intensity, range_, falloff=2.0) -> str:
    """Point-light YAML to append to a renderer entity.

    A lamp is one entity carrying both the visible emitter and the light it
    casts, which is half the entity count of a separate pole and bulb -- and
    the renderer has no cap problem with it (the collection cap is 12000).
    """
    return ("    PointLightComponent:\n"
            f"      Color: {vec(color)}\n"
            f"      Intensity: {num(intensity)}\n"
            f"      Range: {num(range_)}\n"
            f"      Falloff: {num(falloff)}\n")


# ---------------------------------------------------------------------------
# Palette
# ---------------------------------------------------------------------------

# Ash-covered concrete / pre-war materials. Deliberately desaturated: the
# Necroware world has had two centuries of ash fall and acid rain.
ASH_CONCRETE = [
    (0.145, 0.138, 0.128, 1.0),
    (0.175, 0.166, 0.152, 1.0),
    (0.205, 0.196, 0.180, 1.0),
    (0.128, 0.124, 0.122, 1.0),
    (0.230, 0.214, 0.192, 1.0),
]
PREWAR_WARM = [
    (0.235, 0.196, 0.160, 1.0),
    (0.268, 0.226, 0.184, 1.0),
    (0.206, 0.180, 0.156, 1.0),
    (0.286, 0.246, 0.206, 1.0),
]
SCORCH = [
    (0.055, 0.050, 0.048, 1.0),
    (0.070, 0.062, 0.056, 1.0),
    (0.044, 0.042, 0.042, 1.0),
]
ELEMENT_ZERO_GLASS = [
    (0.052, 0.062, 0.086, 1.0),
    (0.062, 0.070, 0.098, 1.0),
    (0.044, 0.052, 0.074, 1.0),
    (0.070, 0.078, 0.104, 1.0),
]
PREWAR_GLASS = [
    (0.098, 0.112, 0.138, 1.0),
    (0.126, 0.140, 0.166, 1.0),
    (0.082, 0.096, 0.122, 1.0),
]
# The Crucible is scrap: rust, corroded orange, oxidised green, bare metal.
SCRAP = [
    (0.235, 0.118, 0.058, 1.0),
    (0.196, 0.098, 0.052, 1.0),
    (0.128, 0.148, 0.092, 1.0),
    (0.176, 0.176, 0.172, 1.0),
    (0.212, 0.166, 0.112, 1.0),
    (0.148, 0.118, 0.086, 1.0),
    (0.108, 0.122, 0.118, 1.0),
]
# Faded Victorians for the hill district -- SF row houses after twenty years
# of ash. The saturation is pulled down hard; they used to be pastel.
VICTORIAN = [
    (0.242, 0.208, 0.196, 1.0),
    (0.216, 0.226, 0.212, 1.0),
    (0.236, 0.216, 0.176, 1.0),
    (0.206, 0.216, 0.236, 1.0),
    (0.226, 0.196, 0.206, 1.0),
]
INDUSTRIAL = [
    (0.186, 0.186, 0.182, 1.0),
    (0.156, 0.150, 0.144, 1.0),
    (0.212, 0.176, 0.128, 1.0),
    (0.142, 0.148, 0.152, 1.0),
]
CONTAINER = [
    (0.212, 0.098, 0.072, 1.0),
    (0.098, 0.148, 0.176, 1.0),
    (0.176, 0.152, 0.072, 1.0),
    (0.128, 0.128, 0.132, 1.0),
    (0.186, 0.128, 0.092, 1.0),
]

# Neon. The Ash District's neon is FAILING -- dim, flickering, mostly dead.
# The hot palette is reserved for the Corporate Plaza and the one
# entertainment district inside the Crucible.
NEON_HOT = [
    (0.85, 0.12, 0.42, 1.0),
    (0.12, 0.52, 0.92, 1.0),
    (0.92, 0.28, 0.08, 1.0),
    (0.58, 0.16, 0.86, 1.0),
    (0.16, 0.86, 0.52, 1.0),
    (1.00, 0.62, 0.12, 1.0),
    (0.14, 0.62, 0.92, 1.0),
]
NEON_DEAD = [
    (0.32, 0.10, 0.22, 1.0),
    (0.10, 0.24, 0.36, 1.0),
    (0.36, 0.16, 0.08, 1.0),
    (0.24, 0.10, 0.34, 1.0),
]
STREET_WARM = (1.00, 0.62, 0.28, 1.0)
VOID_PURPLE = (0.52, 0.10, 0.62, 1.0)

rng = random.Random(SEED)


def pick(seq):
    return seq[rng.randrange(len(seq))]


def scatter(seq):
    return tuple(rng.uniform(0.90, 1.12) * c for c in seq[:3]) + (1.0,)


# ---------------------------------------------------------------------------
# Street network
# ---------------------------------------------------------------------------

# LA's signature is the wide arterial on a relentless grid. 200 m arterials at
# 40 m wide, 100 m collectors at 20 m wide, and the blocks are what is left.
# The grid does NOT scale with the city: a boulevard is a boulevard whether it
# is downtown or eight kilometres out, and keeping the pitch fixed is what
# makes the extra ground read as more city rather than as the same city
# photographed closer.
ARTERIAL_STEP = 200.0
COLLECTOR_STEP = 100.0
ARTERIAL_W = 40.0
COLLECTOR_W = 20.0

# SIZE. The brief is "about 2.5x Night City, layout similar, expanded out".
# Night City's playable city area (excluding the Badlands) is the most
# defensible figure people quote, and it lands around 20 km^2 -- the 40-50 km^2
# numbers include the Badlands and the inaccessible Orbital Air centre, and the
# "100 km^2" ones are an artefact of confusing a 10 km side with 10 km^2.
# 2.5 x 20.7 km^2 = 51.8 km^2, which is a 7.2 km square, so +/- 3600 m.
CITY_HALF = 3600.0
GROUND_HALF = 5400.0          # ground slab extends past the city into the waste
BAY_X = 3600.0                # seawall; everything east of this is the bay
ROAD_TOP_ARTERIAL = 0.08
ROAD_TOP_COLLECTOR = 0.04


def arterial_lines():
    v = -3600.0
    while v <= 3600.0:
        yield v
        v += ARTERIAL_STEP


def collector_lines():
    v = -3600.0
    while v <= 3600.0:
        yield v
        v += COLLECTOR_STEP


@functools.lru_cache(maxsize=None)
def road_clearance(coord: float) -> float:
    """Distance from `coord` to the nearest road EDGE, in metres.

    Buildings are clamped to this so nothing is authored straddling a street.
    Without it the block grid and the road grid are independent, and a
    hab-block ends up with its front wall in the middle of a collector --
    which reads as a modelling error from the street even though the blockout
    is "correct".
    """
    best = 1e9
    for ln in collector_lines():
        w = ARTERIAL_W if ln % ARTERIAL_STEP == 0 else COLLECTOR_W
        d = abs(coord - ln) - w / 2.0
        if d < best:
            best = d
    return best


def clamp_footprint(x: float, z: float, w: float, d: float):
    """Shrink a footprint to the road clearance at (x, z).

    Returns (w, d), or None if nothing usable fits -- the caller skips the
    building rather than emitting a sliver.

    This is a SAFETY NET, not the primary mechanism. Placement is already
    cell-aligned to the road grid (see SUB_GRIDS), so most footprints pass
    through untouched; the clamp only catches the cells that touch a 40 m
    arterial, where the clearance drops from 40 to 30.
    """
    cw = road_clearance(x)
    cd = road_clearance(z)
    if cw <= 0.0 or cd <= 0.0:
        return None
    nw = min(w, 2.0 * cw * 0.94)
    nd = min(d, 2.0 * cd * 0.94)
    # Reject a footprint that had to be cut in half: a sliver reads as a
    # modelling error, and the cell simply stays empty.
    if nw < w * 0.5 or nd < d * 0.5:
        return None
    return nw, nd


# ---------------------------------------------------------------------------
# District map
# ---------------------------------------------------------------------------

# The layout is the same one, spread out: every rect below is the first-pass
# Ash District (built for a 2.6 km city) scaled by 2.77, which is the linear
# factor that takes 6.76 km^2 to 51.8 km^2. The districts keep their relative
# positions and proportions; only the ground they cover grows.
CRUCIBLE = (-3400.0, -1400.0, -3400.0, -1400.0)   # x0, x1, z0, z1
HILLS = (1150.0, 2700.0, -3400.0, -700.0)
WATERFRONT = (2700.0, 3600.0, -3600.0, 3600.0)
CORE = (-700.0, 700.0, -700.0, 700.0)
FINANCIAL_R = 1730.0
LOWER = (-2700.0, 2700.0, 800.0, 3400.0)
OLDCITY_X = -1730.0
INDUSTRIAL_Z = -1730.0


def district(x: float, z: float) -> str:
    """Which district owns this point. Priority order matters at the seams."""
    x0, x1, z0, z1 = CRUCIBLE
    if x0 <= x <= x1 and z0 <= z <= z1:
        return "crucible"
    x0, x1, z0, z1 = HILLS
    if x0 <= x <= x1 and z0 <= z <= z1:
        return "hills"
    if x >= 2700.0:
        return "waterfront"
    if abs(x) <= 700.0 and abs(z) <= 700.0:
        return "core"
    if max(abs(x), abs(z)) <= FINANCIAL_R:
        return "financial"
    x0, x1, z0, z1 = LOWER
    if x0 <= x <= x1 and z0 <= z <= z1:
        return "lower"
    if z <= INDUSTRIAL_Z:
        if -1400.0 <= x <= 1150.0:
            return "industrial"
    return "oldcity"


# ---------------------------------------------------------------------------
# Clearings -- areas where no building may be placed.
# ---------------------------------------------------------------------------

# The component-showcase row is a fixture with pinned coordinates. It needs an
# open lot to sit in, or it would end up embedded inside a hab-block.
PLAZA = (285.0, 435.0, 1070.0, 1120.0)
# Player spawn and the gameplay entities.
CLEARINGS = [
    # Player spawn plaza. Radius has to cover the PlazaPad (90x70, so a corner
    # sits at sqrt(45^2+35^2) = 57 m from the centre), not just the spawn point.
    # At 26 the pad's own footprint was still being built on: a hab-block came
    # down to x=-49.7 and read as a building standing in the middle of the
    # street, with the monument in front of it.
    (0.0, 1250.0, 62.0),
    (-200.0, 800.0, 14.0),    # Enemy1
    (200.0, 600.0, 14.0),     # Enemy2
    (0.0, -200.0, 14.0),      # Enemy3
    (-100.0, 1100.0, 10.0),   # HealthPotion
    (80.0, 1000.0, 10.0),     # AmmoBox
    (150.0, 900.0, 10.0),     # SwordPickup
    (-60.0, 800.0, 10.0),     # GoldBar
    (100.0, 700.0, 10.0),     # Bandage
    # The Spire analogue needs a plaza of its own.
    (0.0, 0.0, 105.0),
    # The Last Note, and the Crucible's entertainment strip.
    (-2400.0, -2400.0, 95.0),
    # The Financial arcology: eight towers on a 210 m ring, so the footprint is
    # ~570 m across. Without a clearing the ring would be built over.
    (1200.0, -1200.0, 360.0),
    # The Old City's collapsed arcology: same idea, sunk 14 m.
    (-2200.0, 300.0, 380.0),
    # The beacon on the seawall.
    (BAY_X - 14.0, 0.0, 70.0),
    # The industrial stack line and rail yard.
    (0.0, -2900.0, 1500.0),
]


def in_clearing(x: float, z: float) -> bool:
    x0, x1, z0, z1 = PLAZA
    if x0 <= x <= x1 and z0 <= z <= z1:
        return True
    for cx, cz, r in CLEARINGS:
        if (x - cx) ** 2 + (z - cz) ** 2 <= r * r:
            return True
    return False


def near_clearing(x: float, z: float, pad: float) -> bool:
    x0, x1, z0, z1 = PLAZA
    if x0 - pad <= x <= x1 + pad and z0 - pad <= z <= z1 + pad:
        return True
    for cx, cz, r in CLEARINGS:
        if (x - cx) ** 2 + (z - cz) ** 2 <= (r + pad) ** 2:
            return True
    return False


# ---------------------------------------------------------------------------
# Building primitives
# ---------------------------------------------------------------------------

BID = 9600000000000000101


def new_bid() -> int:
    global BID
    v = BID
    BID += 1
    return v


# Entity ids emitted as TIERS of a stacked mass. slab_building records each one
# here; check_grounded skips them, because a tier above the first is legitimately
# off the ground and a flat parse cannot tell it apart from a floating box.
# Without this, tiering the buildings flagged every upper tier in the scene.
_STACKED_TIERS: set[int] = set()


def slab_building(tag, x, z, base_y, w, d, h, albedo, metallic, roughness,
                  emission, emission_intensity, rot=(0.0, 0.0, 0.0),
                  tier_step=0.87, tier_jitter=0.085, force=False):
    """Emit one building as a STACK of tiers, not a single box.

    This is the whole point of a greybox: a building is a MASSING STUDY, and a
    tower reads as a tower because its footprint steps inward as it rises. One
    box per building gave the district a skyline of identical rectangular
    prisms -- correct in plan and dead in elevation, which is why the first
    pass looked like a cube farm rather than a city.

    Each tier is a box with a smaller footprint and a small lateral offset, so
    the silhouette steps rather than slopes. That step is the thing a level
    designer blocks out and an art pass later resolves into setbacks, plant
    rooms and crown structures.

    Tier count comes from height: a 15 m warehouse gets 2, a 300 m arcology
    gets 5. Capped at 5 so a tall building does not become twenty boxes.

    Returns the BASE footprint (w, d), not the top tier's -- callers hang
    balconies, neon and signage on the base wall, which is the widest and the
    only part visible from the street. Crowns and antennas size themselves off
    it and sit at the top, which lands them on the smallest tier.

    The clamp can shrink the footprint, so callers MUST use the returned
    dimensions for anything they hang on the facade. A balcony or a neon strip
    placed at the requested half-width instead floats clear of the wall
    whenever the clamp bit -- which is most of the way the "detached cubes"
    the streets were full of got there.
    """
    # `force` is for SET PIECES. The clamp is a safety net for procedurally
    # placed cells; a landmark is placed deliberately and its footprint is part
    # of its read. The arcology's ring of 150 m towers sits across arterials by
    # design -- the point is that it does not come down to the street -- and
    # clamping it would reject all eight towers and leave only the bridges.
    if force:
        fp = (w, d)
    else:
        fp = clamp_footprint(x, z, w, d)
    if fp is None:
        return None
    w, d = fp
    tiers = max(2, min(5, int(h / 45.0) + 1))
    y = base_y
    cw, cd = w, d
    ox = oz = 0.0
    for i in range(tiers):
        last = i == tiers - 1
        if last:
            th = base_y + h - y          # whatever height is left
        else:
            th = (h / tiers) * rng.uniform(0.80, 1.20)
        _tid = new_bid()          # new_bid() increments BID, then returns it
        _STACKED_TIERS.add(_tid)
        box(_tid, tag, (x + ox, y + th / 2.0, z + oz), (cw, th, cd),
            albedo, metallic, roughness, emission, emission_intensity, rot)
        y += th
        if not last:
            # Step the footprint in, and let the tower wander a little. The
            # wander is clamped to a fifth of the base so tiers never separate.
            cw *= rng.uniform(tier_step - 0.05, tier_step + 0.05)
            cd *= rng.uniform(tier_step - 0.05, tier_step + 0.05)
            ox = max(-w * 0.20, min(w * 0.20,
                                     ox + rng.uniform(-1.0, 1.0) * w * tier_jitter))
            oz = max(-d * 0.20, min(d * 0.20,
                                     oz + rng.uniform(-1.0, 1.0) * d * tier_jitter))
    assert abs(y - (base_y + h)) < 1e-6, (
        f"tiered mass at ({x:.1f},{z:.1f}) tops out at {y:.2f}, "
        f"expected {base_y + h:.2f}")
    return w, d


def ruined_stack(tag, x, z, base_y, w, d, h, albedo, emission,
                 emission_intensity):
    """A collapsed tower: stacked, shrinking, offset boxes plus a rubble skirt.

    A single short box reads as a stub. Real collapse leaves a jagged pile, so
    the stack is broken into 2-4 segments with random offsets, and the base is
    ringed with debris.
    """
    fp = clamp_footprint(x, z, w, d)
    if fp is None:
        return
    w, d = fp
    segs = rng.randint(2, 3)
    y = base_y
    prev_top = base_y
    cw, cd = w, d
    for i in range(segs):
        last = (i == segs - 1)
        frac = 1.0 if last else rng.uniform(0.22, 0.42)
        seg_h = max(3.0, h * frac)
        if not last:
            seg_h = min(seg_h, h * 0.45)
        ox = rng.uniform(-cw * 0.16, cw * 0.16)
        oz = rng.uniform(-cd * 0.16, cd * 0.16)
        # No vertical jitter at all, and each segment overlaps the one below
        # by 8%. The old code lifted every non-final segment by up to 22% of
        # its own height while the cursor advanced by the FULL height, so the
        # segments floated apart -- and a 6% jitter does not fix it either,
        # because the final segment is full height while its predecessor is
        # not, so a small jitter on the big one still opens a gap. The
        # jaggedness comes from the shrinking footprint and the horizontal
        # offset, which is enough.
        oy = 0.0
        bottom = y
        assert bottom <= prev_top + 1e-6, (
            f"ruined_stack gap at ({x:.1f},{z:.1f}): segment bottom "
            f"{bottom:.2f} above previous top {prev_top:.2f}")
        box(new_bid(), tag,
            (x + ox, y + seg_h / 2.0 + oy, z + oz),
            (cw, seg_h, cd), albedo,
            metallic=rng.uniform(0.05, 0.30), roughness=rng.uniform(0.7, 0.95),
            emission=emission, emission_intensity=emission_intensity,
            rot=(0.0, rng.uniform(-0.06, 0.06), 0.0))
        prev_top = bottom + seg_h
        y += seg_h * 0.92
        cw *= rng.uniform(0.62, 0.86)
        cd *= rng.uniform(0.62, 0.86)
    # Rubble skirt around the base. Kept to 1-3 pieces: at 3-7 this alone was
    # ~2 000 entities, a sixth of the whole scene, for debris that reads as
    # texture from any distance a player will ever see it from.
    #
    # The height is drawn FIRST and the position derived from it. Placing the
    # piece at base_y + s * 0.28 while its height came from an independent
    # draw left every short piece hanging in the air -- which is what showed
    # up as detached black cubes dotted across the streets.
    #
    # Only half the ruins get any, and never more than two. Debris is the
    # cheapest thing in the scene to over-produce and the first to stop reading
    # at distance: 723 of these was 5.5% of the scene.
    # Debris is not gravel -- it is the building that used to be here. Pieces
    # are therefore large, flat-ish and TILTED, so a cluster reads as a fallen
    # slab or a sheared wall rather than as a scatter of small cubes. The tilt
    # is what carries that: a box resting flat is a paving stone, a box resting
    # on an edge or a corner is wreckage.
    for _ in range(rng.randint(1, 2) if rng.random() < 0.5 else 0):
        s = rng.uniform(5.0, 13.0)
        h = s * rng.uniform(0.16, 0.42)
        r = rng.uniform(0.45, 0.95)
        px = x + cw * 0.5 * r * 1.35 * (1 if rng.random() < 0.5 else -1)
        pz = z + cd * 0.5 * r * 1.35 * (1 if rng.random() < 0.5 else -1)
        px += rng.uniform(-3.0, 3.0)
        pz += rng.uniform(-3.0, 3.0)
        box(new_bid(), "Rubble",
            (px, base_y + h * 0.34, pz),
            (s * rng.uniform(0.9, 1.6), h, s * rng.uniform(0.9, 1.6)),
            pick(SCORCH), metallic=0.05, roughness=0.95,
            emission=(0.0, 0.0, 0.0, 1.0), emission_intensity=0.0,
            rot=(rng.uniform(-0.38, 0.38), rng.uniform(0.0, 3.14),
                 rng.uniform(-0.38, 0.38)))


def neon_strip(tag, x, y, z, length, axis, color, intensity, rot=(0.0, 0.0, 0.0)):
    """A thin emissive band on a facade -- the cheapest neon in the scene."""
    if axis == "x":
        scale = (length, 0.35, 0.35)
    else:
        scale = (0.35, 0.35, length)
    box(new_bid(), tag, (x, y, z), scale, color, metallic=0.0, roughness=0.4,
        emission=color, emission_intensity=intensity, rot=rot)


def antenna(x, y, z, h=None):
    h = h or rng.uniform(12.0, 34.0)
    cylinder(new_bid(), "Antenna", (x, y + h / 2.0, z), rng.uniform(0.18, 0.42),
             h, (0.10, 0.10, 0.11, 1.0), metallic=0.85, roughness=0.35,
             segments=6)


def cable(x0, z0, x1, z1, y0, y1, radius=0.14):
    """A sagging service cable: a CHAIN of short cylinders along a catenary.

    The first pass emitted one box for the whole run. A box is a beam -- it has
    no sag, no curvature and no diameter, so at any distance it reads as a solid
    bar rather than as a cable. A cable is a CURVE under tension, so it is
    emitted as segments following a parabola between the two anchor points.

    Segment count scales with run length, so a long span is a smooth curve and
    a short one is not paying for segments nobody can see.

    Rotation: a cylinder's axis is local +Y, and the engine builds the transform
    with glm::toMat4(glm::quat(Rotation)), whose Euler constructor is
    Rz(roll) * Ry(yaw) * Rx(pitch). Applying that to (0,1,0) gives
    (sin p * sin yaw, cos p, sin p * cos yaw), so aligning the axis to a segment
    direction (dx,dy,dz) needs pitch = acos(dy) and yaw = atan2(dx, dz). The
    existing box-cable used yaw = -atan2(dz, dx) for a local +X axis, which is
    the same convention seen from the other axis.
    """
    import math
    dx, dz = x1 - x0, z1 - z0
    length = math.hypot(dx, dz)
    if length < 1.0:
        return
    sag = rng.uniform(1.2, 3.2)
    n = max(4, min(14, int(length / 7.0)))
    pts = []
    for i in range(n + 1):
        u = i / n
        pts.append((x0 + dx * u,
                    y0 + (y1 - y0) * u - sag * 4.0 * u * (1.0 - u),
                    z0 + dz * u))
    for i in range(n):
        ax, ay, az = pts[i]
        bx, by, bz = pts[i + 1]
        sx, sy, sz = bx - ax, by - ay, bz - az
        seg = math.sqrt(sx * sx + sy * sy + sz * sz)
        if seg < 0.05:
            continue
        dy = max(-1.0, min(1.0, sy / seg))
        pitch = math.acos(dy)
        yaw = math.atan2(sx, sz)
        cylinder(new_bid(), "Cable",
                 ((ax + bx) / 2.0, (ay + by) / 2.0, (az + bz) / 2.0),
                 radius, seg * 1.06,           # slight overlap: no visible gaps
                 (0.045, 0.042, 0.040, 1.0), metallic=0.3, roughness=0.7,
                 segments=5, rot=(pitch, yaw, 0.0))


# ---------------------------------------------------------------------------
# District generators
# ---------------------------------------------------------------------------


def gen_core(x, z, big):
    """Corporate Plaza. Element-Zero arcologies, some still lit, some dead."""
    if near_clearing(x, z, 8.0):
        return
    w = rng.uniform(55.0, 85.0)
    d = rng.uniform(55.0, 85.0)
    ruined = rng.random() < 0.22
    if ruined:
        ruined_stack("Ruin", x, z, 0.0, w, d, rng.uniform(45.0, 110.0),
                     pick(SCORCH), (0.0, 0.0, 0.0, 1.0),
                     rng.uniform(0.0, 0.25))
        return
    h = rng.uniform(180.0, 340.0)
    glass = pick(ELEMENT_ZERO_GLASS)
    glow = pick(NEON_HOT)
    fp = slab_building("Building", x, z, 0.0, w, d, h, glass,
                       metallic=rng.uniform(0.82, 0.95),
                       roughness=rng.uniform(0.08, 0.26),
                       emission=glow, emission_intensity=rng.uniform(0.55, 1.80))
    if fp is None:
        return
    w, d = fp
    # Setback crown: a smaller lit box on top reads as a skyline crown.
    # 0.35, not 0.55 -- 271 crowns was the single largest decorative line item.
    if rng.random() < 0.35:
        cw = w * rng.uniform(0.42, 0.62)
        cd = d * rng.uniform(0.42, 0.62)
        box(new_bid(), "BuildingCrown", (x, h + cw * 0.5, z), (cw, cw, cd),
            glass, metallic=0.9, roughness=0.15,
            emission=glow, emission_intensity=rng.uniform(1.2, 3.0))
    if rng.random() < 0.25:
        antenna(x + rng.uniform(-w * 0.3, w * 0.3),
                h, z + rng.uniform(-d * 0.3, d * 0.3))
    if rng.random() < 0.25:
        neon_strip("NeonSign", x, rng.uniform(h * 0.35, h * 0.8),
                   z + d / 2.0 + 0.25, w * rng.uniform(0.5, 0.9), "x",
                   glow, rng.uniform(1.8, 4.5))


def gen_financial(x, z, big):
    """The pre-war high-rise ring. Steel and glass, a quarter collapsed."""
    if near_clearing(x, z, 6.0):
        return
    w = rng.uniform(40.0, 70.0)
    d = rng.uniform(40.0, 70.0)
    if rng.random() < 0.25:
        ruined_stack("Ruin", x, z, 0.0, w, d, rng.uniform(22.0, 62.0),
                     pick(SCORCH), (0.0, 0.0, 0.0, 1.0),
                     rng.uniform(0.0, 0.20))
        return
    h = rng.uniform(80.0, 160.0)
    glass = pick(PREWAR_GLASS)
    # Most of the ring is still powered; the dead ones are the collapsed stock.
    glow = pick(NEON_HOT) if rng.random() < 0.62 else pick(NEON_DEAD)
    fp = slab_building("Building", x, z, 0.0, w, d, h, glass,
                       metallic=rng.uniform(0.50, 0.80),
                       roughness=rng.uniform(0.20, 0.50),
                       emission=glow, emission_intensity=rng.uniform(0.30, 1.10))
    if fp is None:
        return
    w, d = fp
    if rng.random() < 0.20:
        # Rooftop plant room / billboard slab.
        box(new_bid(), "BuildingCrown",
            (x, h + rng.uniform(1.5, 4.0), z),
            (w * rng.uniform(0.4, 0.8), rng.uniform(2.0, 6.0),
             d * rng.uniform(0.4, 0.8)),
            pick(ASH_CONCRETE), metallic=0.3, roughness=0.7)
    if rng.random() < 0.2:
        antenna(x, h, z)
    if rng.random() < 0.2:
        neon_strip("NeonSign", x, rng.uniform(h * 0.3, h * 0.85),
                   z + d / 2.0 + 0.2, w * rng.uniform(0.4, 0.8), "x",
                   pick(NEON_DEAD), rng.uniform(0.4, 1.6))


def gen_lower(x, z, big):
    """The Lower Zones. Hab-blocks on a 40 m pitch, dense and repetitive."""
    if near_clearing(x, z, 5.0):
        return
    if big:
        # Outer tier: the Lower Zones stop being courtyards of hab-blocks and
        # become the long low-rise slab housing that surrounds them -- one
        # building filling most of the block. That is what keeps the block
        # reading as built-up without paying four entities for it, and it is
        # also what the edge of a real dense city looks like.
        w = rng.uniform(48.0, 72.0)
        d = rng.uniform(34.0, 58.0)
        h = rng.uniform(14.0, 38.0)
    else:
        w = rng.uniform(22.0, 32.0)
        d = rng.uniform(22.0, 32.0)
        h = rng.uniform(25.0, 60.0)
    alb = pick(ASH_CONCRETE)
    # The Lower Zones are wired into the ArkMind grid, so they are never fully
    # dark -- but the power is stolen, rationed and failing.
    glow = pick(NEON_DEAD) if rng.random() < 0.30 else pick(NEON_HOT)
    fp = slab_building("HabBlock", x, z, 0.0, w, d, h, alb,
                       metallic=rng.uniform(0.05, 0.20),
                       roughness=rng.uniform(0.70, 0.92),
                       emission=glow, emission_intensity=rng.uniform(0.12, 0.45))
    if fp is None:
        return
    w, d = fp
    # Balconies and AC boxes on the facades -- the hab-block texture.
    # 0-1, and only on 45% of blocks. At 0-2 on every block this was 494
    # entities of 1.8 m boxes that read as noise from any distance.
    for _ in range(rng.randint(0, 1 if not big else 0)
                     if rng.random() < 0.45 else 0):
        bx = x + (w / 2.0 + 0.9) * (1 if rng.random() < 0.5 else -1)
        bz = z + rng.uniform(-d * 0.35, d * 0.35)
        by = rng.uniform(4.0, h * 0.75)
        box(new_bid(), "Balcony", (bx, by, bz),
            (1.8, rng.uniform(1.6, 2.4), rng.uniform(1.2, 2.4)),
            pick(ASH_CONCRETE), metallic=0.2, roughness=0.8)
    if rng.random() < 0.08:
        # The ArkMind receiver embedded in the wall. Cyan, always on.
        neon_strip("ArkMindReceiver", x, rng.uniform(6.0, h * 0.6),
                   z + d / 2.0 + 0.15, rng.uniform(2.0, 5.0), "x",
                   (0.10, 0.55, 0.75, 1.0), rng.uniform(1.0, 2.4))
    if rng.random() < 0.05:
        neon_strip("NeonSign", x, rng.uniform(4.0, h * 0.5),
                   z + d / 2.0 + 0.25, w * rng.uniform(0.3, 0.7), "x",
                   pick(NEON_DEAD), rng.uniform(0.05, 0.35))


def gen_oldcity(x, z, big):
    """The pre-war grid that survived. Irregular, wide streets, rubble."""
    if near_clearing(x, z, 6.0):
        return
    if big:
        w = rng.uniform(58.0, 78.0)
        d = rng.uniform(48.0, 70.0)
    else:
        w = rng.uniform(35.0, 65.0)
        d = rng.uniform(35.0, 65.0)
    if rng.random() < 0.35:
        ruined_stack("Ruin", x, z, 0.0, w, d, rng.uniform(12.0, 40.0),
                     pick(SCORCH), (0.0, 0.0, 0.0, 1.0),
                     rng.uniform(0.0, 0.20))
        return
    h = rng.uniform(15.0, 55.0) if not big else rng.uniform(11.0, 34.0)
    alb = pick(PREWAR_WARM)
    glow = pick(NEON_DEAD) if rng.random() < 0.60 else pick(NEON_HOT)
    fp = slab_building("Building", x, z, 0.0, w, d, h, alb,
                       metallic=rng.uniform(0.10, 0.30),
                       roughness=rng.uniform(0.75, 0.95),
                       emission=glow, emission_intensity=rng.uniform(0.08, 0.34))
    if fp is None:
        return
    w, d = fp
    if rng.random() < 0.22:
        # Rooftop billboard: a big emissive plane on a dead frame.
        bw = w * rng.uniform(0.6, 0.95)
        bh = rng.uniform(6.0, 14.0)
        box(new_bid(), "Billboard",
            (x, h + bh / 2.0 + rng.uniform(1.0, 3.0), z),
            (bw, bh, 0.4), pick(NEON_HOT), metallic=0.1, roughness=0.5,
            emission=pick(NEON_HOT), emission_intensity=rng.uniform(0.5, 2.2))
    if rng.random() < 0.12:
        antenna(x, h, z, rng.uniform(6.0, 16.0))


CRUCIBLE_CENTRE = (-2400.0, -2400.0)
ENTERTAINMENT_R = 415.0


def gen_crucible(x, z, big):
    """The Crucible. Dense chaotic shacks, stacked, scrap, failing neon."""
    if big:
        # Outer Crucible: same scrap, bigger shacks. A shantytown's outskirts
        # are sheds and workshops, not bedroom-sized lean-tos.
        w = rng.uniform(7.0, 14.0)
        d = rng.uniform(7.0, 14.0)
    else:
        w = rng.uniform(4.0, 9.0)
        d = rng.uniform(4.0, 9.0)
    rot = (0.0, rng.uniform(-0.22, 0.22), 0.0)
    dist_ent = ((x - CRUCIBLE_CENTRE[0]) ** 2
                + (z - CRUCIBLE_CENTRE[1]) ** 2) ** 0.5
    hot = dist_ent < ENTERTAINMENT_R
    layers = rng.randint(1, 2) if not big else 1
    y = 0.0
    prev_top = 0.0
    cw, cd = w, d
    fp = clamp_footprint(x, z, w, d)
    if fp is None:
        return
    cw, cd = fp
    for i in range(layers):
        h = rng.uniform(2.0, 4.2)
        ox = rng.uniform(-0.8, 0.8)
        oz = rng.uniform(-0.8, 0.8)
        alb = pick(SCRAP)
        glow = pick(NEON_HOT if hot else NEON_DEAD)
        inten = rng.uniform(1.5, 4.5) if hot else rng.uniform(0.10, 0.45)
        assert y <= prev_top + 1e-6, (
            f"shack stack gap at ({x:.1f},{z:.1f}): layer bottom {y:.2f} "
            f"above previous top {prev_top:.2f}")
        box(new_bid(), "Shack", (x + ox, y + h / 2.0, z + oz),
            (cw, h, cd), alb, metallic=rng.uniform(0.3, 0.7),
            roughness=rng.uniform(0.6, 0.9),
            emission=glow, emission_intensity=inten if rng.random() < 0.35
            else 0.0, rot=rot)
        if rng.random() < 0.12:
            neon_strip("NeonSign", x + ox, y + h * 0.85, z + oz + cd / 2.0,
                       cw * rng.uniform(0.4, 0.9), "x", glow,
                       rng.uniform(1.5, 5.0) if hot
                       else rng.uniform(0.1, 0.8), rot=rot)
        prev_top = y + h
        y += h
        cw *= rng.uniform(0.80, 1.05)
        cd *= rng.uniform(0.80, 1.05)
    # Corrugated lean-to against the shack.
    if rng.random() < 0.4:
        box(new_bid(), "Shack", (x + cw * 0.75, 0.9, z + rng.uniform(-1.0, 1.0)),
            (rng.uniform(2.0, 4.0), rng.uniform(1.6, 2.6),
             rng.uniform(1.6, 3.0)),
            pick(SCRAP), metallic=0.55, roughness=0.75,
            rot=(0.0, rng.uniform(-0.4, 0.4), 0.0))


HILL_X0, HILL_Z0 = 1150.0, -3400.0
HILL_CELL = 100.0
HILL_STEPS = 4.0


def hill_elev(x: float, z: float) -> float:
    """Terrain height in the hill district, quantised to 4 m bands."""
    u = (x - HILL_X0) / 1745.0
    v = 1.0 - (z - HILL_Z0) / 2740.0       # 1 at the northern edge
    # Taller than the first pass (20 m of rise over 990 m) because the hill
    # district is 2.7x wider now: the same slope over 2.7x the run would read
    # as a car park. 55 m of rise over 2.7 km is a San Francisco grade.
    e = v * 55.0 + u * 17.0
    return round(e / HILL_STEPS) * HILL_STEPS


def hill_cell(x: float, z: float):
    ix = int((x - HILL_X0) // HILL_CELL)
    iz = int((z - HILL_Z0) // HILL_CELL)
    return ix, iz


def hill_base_y(x: float, z: float) -> float:
    ix, iz = hill_cell(x, z)
    return hill_elev(HILL_X0 + (ix + 0.5) * HILL_CELL,
                     HILL_Z0 + (iz + 0.5) * HILL_CELL)


def gen_hills(x, z, big):
    """SF-style row houses on terraced ground."""
    if near_clearing(x, z, 4.0):
        return
    base = hill_base_y(x, z)
    if big:
        w = rng.uniform(44.0, 68.0)
        d = rng.uniform(30.0, 50.0)
        h = rng.uniform(10.0, 26.0)
    else:
        w = rng.uniform(16.0, 26.0)
        d = rng.uniform(12.0, 22.0)
        h = rng.uniform(12.0, 35.0)
    alb = pick(VICTORIAN)
    glow = pick(NEON_DEAD) if rng.random() < 0.45 else pick(NEON_HOT)
    fp = slab_building("RowHouse", x, z, base, w, d, h, alb,
                       metallic=rng.uniform(0.05, 0.15),
                       roughness=rng.uniform(0.70, 0.90),
                       emission=glow, emission_intensity=rng.uniform(0.06, 0.26))
    if fp is None:
        return
    w, d = fp
    if rng.random() < 0.15:
        # Bay window: a narrow lit box on the front.
        neon_strip("BayWindow", x, base + rng.uniform(4.0, h * 0.6),
                   z + d / 2.0 + 0.2, w * rng.uniform(0.3, 0.6), "x",
                   pick(NEON_DEAD), rng.uniform(0.2, 1.0))
    if rng.random() < 0.06:
        antenna(x, base + h, z, rng.uniform(4.0, 10.0))


def gen_industrial(x, z, big):
    """The Sprawl. Warehouses, tanks, stacks, container yards."""
    if near_clearing(x, z, 8.0):
        return
    if rng.random() < 0.30:
        # Tank farm: a cluster of cylinders. 1-2, not 1-3 -- 169 tanks across
        # 211 warehouse cells was more tank than building.
        for _ in range(rng.randint(1, 2)):
            r = rng.uniform(5.0, 11.0)
            h = rng.uniform(6.0, 14.0)
            ox = rng.uniform(-14.0, 14.0)
            oz = rng.uniform(-14.0, 14.0)
            cylinder(new_bid(), "Tank", (x + ox, h / 2.0, z + oz), r, h,
                     pick(INDUSTRIAL), metallic=rng.uniform(0.4, 0.8),
                     roughness=rng.uniform(0.45, 0.75), segments=16)
        return
    if rng.random() < 0.25:
        # Smokestack.
        h = rng.uniform(30.0, 70.0)
        r = rng.uniform(1.6, 3.4)
        cylinder(new_bid(), "Stack", (x, h / 2.0, z), r, h,
                 pick(SCORCH), metallic=0.4, roughness=0.85, segments=12)
        if rng.random() < 0.5:
            box(new_bid(), "StackCap", (x, h + 1.0, z), (r * 2.6, 2.0, r * 2.6),
                (0.10, 0.10, 0.10, 1.0), metallic=0.5, roughness=0.6)
        return
    if rng.random() < 0.35:
        # Container yard. 1-2 stacks of 1-2 high, not 2-5 stacks of up to 3:
        # 934 containers made this the largest single prop line in the scene.
        for _ in range(rng.randint(1, 2)):
            container_stack(x + rng.uniform(-18.0, 18.0),
                            z + rng.uniform(-18.0, 18.0),
                            rng.choice((1, 1, 2)),
                            (0.0, rng.uniform(-0.2, 0.2), 0.0))
        return
    w = rng.uniform(50.0, 90.0) if not big else rng.uniform(62.0, 78.0)
    d = rng.uniform(50.0, 90.0) if not big else rng.uniform(58.0, 78.0)
    h = rng.uniform(8.0, 25.0) if not big else rng.uniform(7.0, 19.0)
    fp = slab_building("Warehouse", x, z, 0.0, w, d, h, pick(INDUSTRIAL),
                       metallic=rng.uniform(0.40, 0.75),
                       roughness=rng.uniform(0.55, 0.80),
                       emission=pick(NEON_DEAD),
                       emission_intensity=rng.uniform(0.02, 0.14))
    if fp is None:
        return
    w, d = fp
    if rng.random() < 0.4:
        # Roof monitor: a raised strip along the ridge.
        box(new_bid(), "RoofMonitor", (x, h + 1.5, z), (w * 0.8, 3.0, 3.0),
            pick(INDUSTRIAL), metallic=0.5, roughness=0.6)


def gen_waterfront(x, z, big):
    """The bay. Warehouses along the seawall, containers, cranes, piers."""
    if rng.random() < 0.45:
        w = rng.uniform(30.0, 55.0) if not big else rng.uniform(52.0, 76.0)
        d = rng.uniform(25.0, 45.0) if not big else rng.uniform(42.0, 70.0)
        h = rng.uniform(9.0, 18.0)
        slab_building("Warehouse", x, z, 0.0, w, d, h, pick(INDUSTRIAL),
                      metallic=rng.uniform(0.45, 0.75),
                      roughness=rng.uniform(0.55, 0.80),
                      emission=pick(NEON_DEAD),
                      emission_intensity=rng.uniform(0.02, 0.14))
        return
    if rng.random() < 0.5:
        for _ in range(rng.randint(1, 2)):
            container_stack(x + rng.uniform(-16.0, 16.0),
                            z + rng.uniform(-16.0, 16.0),
                            rng.choice((1, 1, 2)),
                            (0.0, rng.uniform(-0.3, 0.3), 0.0))
        return
    # Cold store / silo.
    r = rng.uniform(5.0, 9.0)
    h = rng.uniform(12.0, 22.0)
    cylinder(new_bid(), "Silo", (x, h / 2.0, z), r, h, pick(INDUSTRIAL),
             metallic=rng.uniform(0.5, 0.8), roughness=0.55, segments=14)


def gen_wasteland(x, z, big):
    """The ring outside the city: dark ruined silhouettes and craters."""
    if rng.random() < 0.55:
        w = rng.uniform(30.0, 70.0) if not big else rng.uniform(46.0, 92.0)
        d = rng.uniform(30.0, 70.0) if not big else rng.uniform(46.0, 92.0)
        h = rng.uniform(60.0, 220.0)
        # Unlit, untextured, near-black: depth, not detail.
        ruined_stack("Silhouette", x, z, 0.0, w, d, h,
                     (0.030, 0.028, 0.030, 1.0), (0.0, 0.0, 0.0, 1.0), 0.0)
        return
    if x > BAY_X:
        # The bay gets ruined offshore structures, not burn marks on water.
        w = rng.uniform(30.0, 70.0)
        d = rng.uniform(30.0, 70.0)
        # Based on the seabed, so they rise out of the water rather than
        # hovering a metre and a half above it.
        ruined_stack("Silhouette", x, z, -7.0, w, d, rng.uniform(40.0, 150.0),
                     (0.030, 0.028, 0.030, 1.0), (0.0, 0.0, 0.0, 1.0), 0.0)
        return
    # Crater: a very flat dark disc.
    r = rng.uniform(18.0, 55.0)
    cylinder(new_bid(), "Crater", (x, 0.2, z), r, 0.4,
             (0.045, 0.040, 0.038, 1.0), metallic=0.0, roughness=0.98,
             segments=18)


DISPATCH = {
    "core": gen_core,
    "financial": gen_financial,
    "lower": gen_lower,
    "oldcity": gen_oldcity,
    "crucible": gen_crucible,
    "hills": gen_hills,
    "industrial": gen_industrial,
    "waterfront": gen_waterfront,
    "wasteland": gen_wasteland,
}

# Placement is CELL-ALIGNED to the road grid, not free-running. A city block
# is the 80 m between two collectors, so a building belongs inside one; a
# free pitch grid drifts across the street centrelines and every hab-block
# ends up with its front wall in the road.
CELL = 100.0
CELL_MIN = -3550.0
CELL_MAX = 3550.0


def cell_centers(step: float = CELL):
    c = CELL_MIN
    while c <= CELL_MAX + 1e-6:
        yield c
        c += step


# Sub-positions inside one 100 m cell, per district, as (inner, outer).
# "inner" is the dense city -- the districts as they were drawn for a 2.6 km
# map. "outer" is what the same district does once it is 3 km from the centre,
# where a city stops being blocks of buildings and starts being big low-rise
# structures with yards. Offsets stay clear of the cell edge (40 m half-width,
# 30 m next to an arterial) so nothing is authored across a street.
SUB_GRIDS = {
    "core": ([(0.0, 0.0)], [(0.0, 0.0)]),
    "financial": ([(0.0, 0.0)], [(0.0, 0.0)]),
    "industrial": ([(0.0, 0.0)], [(0.0, 0.0)]),
    "waterfront": ([(0.0, 0.0)], [(0.0, 0.0)]),
    "oldcity": ([(0.0, 0.0)], [(0.0, 0.0)]),
    "lower": ([(-20.0, 0.0), (20.0, 0.0)], [(0.0, 0.0)]),
    "hills": ([(-20.0, 0.0), (20.0, 0.0)], [(0.0, 0.0)]),
    "crucible": ([(-25.0, -25.0), (-25.0, 0.0), (-25.0, 25.0),
                  (0.0, -25.0), (0.0, 0.0), (0.0, 25.0),
                  (25.0, -25.0), (25.0, 0.0), (25.0, 25.0)],
                 [(-18.0, -18.0), (18.0, -18.0),
                  (-18.0, 18.0), (18.0, 18.0)]),
    "wasteland": ([(0.0, 0.0)], [(0.0, 0.0)]),
}

# Density tiers by Chebyshev distance from the city centre:
# (limit, sub-grid index, fill multiplier).
#
# The 100 m grid does not get coarser outward -- the streets are the same
# streets -- but the number of structures per block falls, and past 1800 m the
# generators switch to the "outer" shape: one big low-rise building per block
# instead of a courtyard of small ones. That is what holds the built-up
# COVERAGE up while the entity count stays bounded, and it is also just what a
# real sprawl looks like: downtown is many small dense buildings, the outer
# districts are warehouses and low-rise slabs.
TIERS = [
    (1800.0, 0, 1.00),
    (2700.0, 1, 1.00),
    (3600.0, 1, 0.62),
]


def tier_for(x: float, z: float) -> int:
    d = max(abs(x), abs(z))
    for i, (limit, _, _) in enumerate(TIERS):
        if d <= limit:
            return i
    return len(TIERS) - 1


# (rect, fill probability) per district. Rects are x0, x1, z0, z1. Fill is per
# sub-position, so a district's density is sub-grid size x fill x tier.
DISTRICT_PLAN = [
    ("crucible", CRUCIBLE, 0.60),
    ("hills", HILLS, 0.80),
    ("waterfront", WATERFRONT, 0.55),
    ("core", CORE, 0.95),
    ("financial", (-FINANCIAL_R, FINANCIAL_R, -FINANCIAL_R, FINANCIAL_R), 0.85),
    ("lower", LOWER, 0.85),
    ("oldcity", (-3600.0, OLDCITY_X, -1400.0, 3600.0), 0.70),
    ("industrial", (-1400.0, 1150.0, -3600.0, INDUSTRIAL_Z), 0.75),
    ("wasteland", (-5400.0, -3600.0, -5400.0, 5400.0), 0.55),
    ("wasteland", (-3600.0, 3600.0, -5400.0, -3600.0), 0.55),
    ("wasteland", (-3600.0, 3600.0, 3600.0, 5400.0), 0.55),
    ("wasteland", (3600.0, 5400.0, -5400.0, 5400.0), 0.30),
]


def generate_city() -> None:
    for name, (x0, x1, z0, z1), fill in DISTRICT_PLAN:
        if name == "crucible":
            # The Crucible is built by generate_crucible_city, which lays out its
            # OWN rotated, denser grid. Running the outer city's cell placement
            # here as well would put two street systems on top of each other and
            # destroy the one thing that makes the district read as self-contained.
            continue
        fn = DISPATCH[name]
        inner_subs, outer_subs = SUB_GRIDS[name]
        for cx in cell_centers():
            if not (x0 <= cx <= x1):
                continue
            for cz in cell_centers():
                if not (z0 <= cz <= z1):
                    continue
                _, sub_idx, fill_mult = TIERS[tier_for(cx, cz)]
                subs = inner_subs if sub_idx == 0 else outer_subs
                big = sub_idx == 1
                for ox, oz in subs:
                    x = cx + ox + rng.uniform(-2.0, 2.0)
                    z = cz + oz + rng.uniform(-2.0, 2.0)
                    if district(x, z) != name:
                        continue
                    if in_clearing(x, z):
                        continue
                    if rng.random() >= fill * fill_mult:
                        continue
                    fn(x, z, big)


# ---------------------------------------------------------------------------
# Street furniture
# ---------------------------------------------------------------------------

STREET_ID = 96000000000000020001


def generate_streets() -> None:
    """The road network as explicit geometry: arterials, collectors, sidewalks.

    The ground plane is the dirt; these slabs are what make the blockout read
    as a city rather than as boxes on a plane.

    Slabs are clamped to the CITY, not to the ground slab: the ground extends
    east under the bay, and a road running out over the water reads as a bug.
    """
    global STREET_ID

    def add(scale, pos, albedo, tag, rough=0.9, metal=0.0):
        box(STREET_ID, tag, pos, scale, albedo, metallic=metal,
            roughness=rough)
        return STREET_ID + 1

    half = CITY_HALF
    # Arterials, both axes.
    for v in arterial_lines():
        STREET_ID = add((ARTERIAL_W, 0.04, 2.0 * half),
                        (v, ROAD_TOP_ARTERIAL - 0.02, 0.0),
                        (0.115, 0.112, 0.118, 1.0), "Road")
        STREET_ID = add((2.0 * half, 0.04, ARTERIAL_W),
                        (0.0, ROAD_TOP_ARTERIAL - 0.02, v),
                        (0.115, 0.112, 0.118, 1.0), "Road")
    # Collectors, both axes, a hair lower so the arterials win at crossings.
    for v in collector_lines():
        STREET_ID = add((COLLECTOR_W, 0.04, 2.0 * half),
                        (v, ROAD_TOP_COLLECTOR - 0.02, 0.0),
                        (0.132, 0.128, 0.132, 1.0), "Road")
        STREET_ID = add((2.0 * half, 0.04, COLLECTOR_W),
                        (0.0, ROAD_TOP_COLLECTOR - 0.02, v),
                        (0.132, 0.128, 0.132, 1.0), "Road")
    # --- Streets above streets: the corporate skyways.
    #
    # The brief is a city built to uphold balance sheets rather than house the
    # people in it, so the through-routes that matter run OVERHEAD, on decks
    # the street level cannot reach. A deck slab alone reads as a floating road;
    # the pillar line is what makes it read as infrastructure, so every deck
    # carries one.
    ELEV_DECK_Y = 26.0
    decks = 0
    for i, v in enumerate(arterial_lines()):
        if i % 6 != 0 or abs(v) > 2400.0:
            continue
        decks += 1
        if decks > 4:
            break
        STREET_ID = add((2.0 * half, 1.4, 74.0), (v, ELEV_DECK_Y, 0.0),
                        (0.145, 0.138, 0.132, 1.0), "Skyway", rough=0.85)
        p = -half + 80.0
        while p < half:
            STREET_ID = add((6.0, ELEV_DECK_Y, 6.0),
                            (v, ELEV_DECK_Y / 2.0, p),
                            (0.120, 0.114, 0.108, 1.0), "SkywayPillar",
                            rough=0.75, metal=0.45)
            p += 160.0

    # --- Streets below streets: the sunken arterials.
    #
    # The old road bed, cut down after the Prelude. Reads as a trench with
    # retaining walls; the floor sits 6 m below grade, so the ground slab above
    # is what makes it a cutting rather than a hole.
    SUNK_Y = -6.0
    sunk = 0
    for i, v in enumerate(arterial_lines()):
        if i % 9 != 4 or abs(v) > 2400.0:
            continue
        sunk += 1
        if sunk > 2:
            break
        STREET_ID = add((2.0 * half, 0.6, 46.0), (v, SUNK_Y, 0.0),
                        (0.085, 0.082, 0.078, 1.0), "SunkenRoad", rough=0.95)
        for side in (-1.0, 1.0):
            STREET_ID = add((2.0 * half, 6.6, 2.0),
                            (v + side * 24.0, SUNK_Y + 3.3, 0.0),
                            (0.105, 0.098, 0.090, 1.0), "RetainingWall",
                            rough=0.9)

    # Sidewalks flanking every OTHER arterial. They are 0.1 m slabs running the
    # full 10.8 km of each line; on all 37 arterials that was 148 entities of
    # kerb nobody sees from above.
    for i, v in enumerate(arterial_lines()):
        if i % 2:
            continue
        for side in (-1.0, 1.0):
            off = side * (ARTERIAL_W / 2.0 + 3.0)
            STREET_ID = add((6.0, 0.10, 2.0 * half),
                            (v + off, 0.05, 0.0),
                            (0.215, 0.206, 0.196, 1.0), "Sidewalk")
            STREET_ID = add((2.0 * half, 0.10, 6.0),
                            (0.0, 0.05, v + off),
                            (0.215, 0.206, 0.196, 1.0), "Sidewalk")


LIGHT_ID = 98000000000000006001


def generate_streetlights() -> None:
    """Pole + emissive head + point light, at arterial intersections.

    Two entities per light: the pole is a cylinder with a light component, and
    the head is a small emissive sphere that carries the visible glow. The
    current scene uses one entity per light with no pole at all, which is
    cheaper and reads as floating lamps at any distance.
    """
    global LIGHT_ID
    spots = []
    # Every THIRD arterial (600 m). At the 7.2 km extent a lamp on every one of
    # the 37x37 intersections would be 1 369 lights and 2 738 entities for
    # street furniture nobody sees from 3 km away; every second (400 m) still
    # produced 210 poles + 210 heads. A lamp is 4 m tall and reads as a point
    # from any distance past a few hundred metres.
    for i, vx in enumerate(arterial_lines()):
        if i % 3:
            continue
        for j, vz in enumerate(arterial_lines()):
            if j % 3:
                continue
            if abs(vx) > 2750.0 or abs(vz) > 2750.0:
                continue
            if district(vx, vz) in ("crucible", "wasteland"):
                continue
            spots.append((vx + ARTERIAL_W / 2.0 + 4.0,
                          vz + ARTERIAL_W / 2.0 + 4.0))
    # The Lower Zones get extra coverage: it is the playable district.
    v = -2600.0
    while v <= 2600.0:
        spots.append((v + 24.0, 1000.0))
        spots.append((v + 24.0, 3000.0))
        v += 400.0
    # The Crucible is lit by fires, not by a grid that is still working.
    for _ in range(24):
        spots.append((rng.uniform(-3340.0, -1460.0),
                      rng.uniform(-3340.0, -1460.0)))
    for x, z in spots:
        if near_clearing(x, z, 2.0):
            continue
        h = rng.uniform(6.0, 8.5)
        cylinder(LIGHT_ID, "StreetLightPole", (x, h / 2.0, z), 0.22, h,
                 (0.085, 0.082, 0.080, 1.0), metallic=0.75, roughness=0.45,
                 segments=8)
        LIGHT_ID += 1
        sphere(LIGHT_ID, "StreetLightHead", (x, h + 0.35, z), 0.5,
               STREET_WARM, metallic=0.0, roughness=0.35,
               emission=STREET_WARM, emission_intensity=rng.uniform(2.0, 4.0),
               sectors=10, stacks=5,
               extra=light_extra(STREET_WARM, rng.uniform(9.0, 15.0),
                                 rng.uniform(22.0, 34.0)))
        LIGHT_ID += 1


# ---------------------------------------------------------------------------
# Props -- the post-Prelude detail
# ---------------------------------------------------------------------------

PROP_ID = 96000000000000010001


CONTAINER_W, CONTAINER_H, CONTAINER_D = 6.0, 3.0, 2.6


def container_stack(x, z, levels, rot):
    """A stack of shipping containers sharing ONE footprint.

    The first pass drew the height per container independently (a random pick
    from 0/0/3/6) while scattering the positions, so most of the tall ones had
    nothing underneath: the same floating-cube artefact as the rubble, in a
    different colour. A stack has to be built bottom-up at one spot.
    """
    for level in range(levels):
        box(new_bid(), "Container",
            (x, CONTAINER_H / 2.0 + CONTAINER_H * level, z),
            (CONTAINER_W, CONTAINER_H, CONTAINER_D), pick(CONTAINER),
            metallic=rng.uniform(0.5, 0.8), roughness=0.6, rot=rot)


def prop_wreck(x, z):
    """A burned-out vehicle husk."""
    global PROP_ID
    rot = (0.0, rng.uniform(0.0, 6.283), 0.0)
    body = rng.uniform(3.6, 5.4)
    # Two boxes, no wheels. A wheel is 0.35 m across: invisible beyond a few
    # metres, and four of them per wreck made 760 entities -- 5.8% of the scene
    # -- for detail no camera in this city can resolve. The body sits directly
    # on the tarmac at y = h/2, and the cabin rests on it.
    box(PROP_ID, "Wreck", (x, 0.5, z), (body, 1.0, 2.0),
        pick(SCORCH), metallic=0.35, roughness=0.85, rot=rot)
    PROP_ID += 1
    box(PROP_ID, "Wreck", (x, 1.35, z), (body * 0.62, 0.7, 1.85),
        pick(SCORCH), metallic=0.35, roughness=0.85, rot=rot)
    PROP_ID += 1


def prop_barrel_fire(x, z):
    """A burning barrel: the Crucible's street lighting."""
    global PROP_ID
    cylinder(PROP_ID, "Barrel", (x, 0.55, z), 0.42, 1.1,
             (0.115, 0.075, 0.045, 1.0), metallic=0.5, roughness=0.8,
             segments=10)
    PROP_ID += 1
    sphere(PROP_ID, "Fire", (x, 1.35, z), 0.5,
           (1.0, 0.42, 0.10, 1.0), metallic=0.0, roughness=0.4,
           emission=(1.0, 0.36, 0.08, 1.0),
           emission_intensity=rng.uniform(3.0, 7.0),
           sectors=10, stacks=5,
           extra=light_extra((1.0, 0.38, 0.10, 1.0),
                             rng.uniform(6.0, 14.0),
                             rng.uniform(14.0, 26.0)))
    PROP_ID += 1


def prop_barricade(x, z, length=None, axis="x"):
    global PROP_ID
    length = length or rng.uniform(6.0, 14.0)
    if axis == "x":
        scale = (length, rng.uniform(1.8, 2.6), 0.5)
    else:
        scale = (0.5, rng.uniform(1.8, 2.6), length)
    box(PROP_ID, "Barricade", (x, scale[1] / 2.0, z), scale,
        pick(SCRAP), metallic=rng.uniform(0.3, 0.6), roughness=0.8,
        rot=(0.0, rng.uniform(-0.12, 0.12), 0.0))
    PROP_ID += 1


# Sandbag rows are stacked, so only the bottom row touches the ground; the
# stack is asserted here rather than left to the parse check.
SANDBAG_H = 0.85
SANDBAG_TOP_H = 0.7


def prop_sandbags(x, z, length, axis="x"):
    global PROP_ID
    n = max(1, int(length / 1.1))
    for i in range(n):
        t = (i - (n - 1) / 2.0) * 1.1
        px = x + (t if axis == "x" else 0.0)
        pz = z + (t if axis == "z" else 0.0)
        box(PROP_ID, "Sandbags", (px, SANDBAG_H / 2.0, pz),
            (1.05, SANDBAG_H, 0.62) if axis == "x" else (0.62, SANDBAG_H, 1.05),
            (0.176, 0.160, 0.118, 1.0), metallic=0.0, roughness=0.95,
            rot=(0.0, rng.uniform(-0.2, 0.2), 0.0))
        PROP_ID += 1
        if rng.random() < 0.6:
            # Rests exactly on the row below.
            box(PROP_ID, "Sandbags",
                (px + rng.uniform(-0.2, 0.2),
                 SANDBAG_H + SANDBAG_TOP_H / 2.0,
                 pz + rng.uniform(-0.2, 0.2)),
                (1.0, SANDBAG_TOP_H, 0.6) if axis == "x"
                else (0.6, SANDBAG_TOP_H, 1.0),
                (0.166, 0.150, 0.110, 1.0), metallic=0.0, roughness=0.95,
                rot=(0.0, rng.uniform(-0.2, 0.2), 0.0))
            PROP_ID += 1


def prop_walkway(x, z, length, axis="x", y=None):
    global PROP_ID
    y = y or rng.uniform(4.0, 8.0)
    if axis == "x":
        scale = (length, 0.35, 2.4)
    else:
        scale = (2.4, 0.35, length)
    box(PROP_ID, "Walkway", (x, y, z), scale, (0.155, 0.130, 0.100, 1.0),
        metallic=0.55, roughness=0.75,
        rot=(0.0, rng.uniform(-0.1, 0.1), 0.0))
    PROP_ID += 1
    # No separate rail entity. It was a 0.1 m bar costing one entity per
    # walkway -- 110 of them, the same count as the decks they sit on. The deck
    # is widened instead so the silhouette still reads as a walkway.


def prop_watchtower(x, z):
    global PROP_ID
    h = rng.uniform(14.0, 22.0)
    box(PROP_ID, "Watchtower", (x, h / 2.0, z), (2.2, h, 2.2),
        (0.140, 0.122, 0.098, 1.0), metallic=0.45, roughness=0.8)
    PROP_ID += 1
    box(PROP_ID, "WatchtowerTop", (x, h + 1.4, z), (4.4, 2.8, 4.4),
        (0.170, 0.146, 0.112, 1.0), metallic=0.45, roughness=0.8)
    PROP_ID += 1
    glow = pick(NEON_HOT)
    neon_strip("NeonSign", x, h + 2.9, z + 2.25, 3.0, "x", glow,
               rng.uniform(1.5, 4.0))


def prop_scorch(x, z, r):
    """A flat burn mark on the ground."""
    global PROP_ID
    cylinder(PROP_ID, "Scorch", (x, 0.06, z), r, 0.12,
             (0.030, 0.028, 0.028, 1.0), metallic=0.0, roughness=0.98,
             segments=14)
    PROP_ID += 1


def generate_props() -> None:
    """Scatter the post-Prelude detail: wrecks, fires, barricades, rubble."""
    global PROP_ID
    # Wrecked vehicles along the arterials, mostly outside the core.
    for _ in range(110):
        v = rng.choice(list(arterial_lines()))
        along = rng.uniform(-3550.0, 3550.0)
        if rng.random() < 0.5:
            x, z = v + rng.uniform(-14.0, 14.0), along
        else:
            x, z = along, v + rng.uniform(-14.0, 14.0)
        if district(x, z) == "wasteland":
            continue
        prop_wreck(x, z)
        if rng.random() < 0.35:
            prop_scorch(x + rng.uniform(-2.0, 2.0), z + rng.uniform(-2.0, 2.0),
                        rng.uniform(3.0, 7.0))

    # Burning barrels: the Crucible and the Lower Zones.
    for _ in range(80):
        if rng.random() < 0.6:
            x = rng.uniform(-3340.0, -1460.0)
            z = rng.uniform(-3340.0, -1460.0)
        else:
            x = rng.uniform(-2650.0, 2650.0)
            z = rng.uniform(860.0, 3340.0)
        prop_barrel_fire(x, z)
        if rng.random() < 0.5:
            prop_scorch(x, z, rng.uniform(2.0, 4.5))

    # Barricades sealing the Crucible's perimeter road and its interior lanes.
    x0, x1, z0, z1 = CRUCIBLE
    step = 100.0
    v = z0 + step / 2.0
    while v < z1:
        if abs(v - CRUCIBLE_CENTRE[1]) > 70.0:      # leave the gate open
            prop_barricade(x1, v, rng.uniform(14.0, 30.0), "z")
        v += step
    v = x0 + step / 2.0
    while v < x1:
        if abs(v - CRUCIBLE_CENTRE[0]) > 70.0:
            prop_barricade(v, z1, rng.uniform(14.0, 30.0), "x")
        v += step
    for _ in range(40):
        x = rng.uniform(x0 + 10.0, x1 - 10.0)
        z = rng.uniform(z0 + 10.0, z1 - 10.0)
        prop_barricade(x, z)

    # Sandbag lines at the gate and at the entertainment district.
    prop_sandbags(x1, CRUCIBLE_CENTRE[1] - 70.0, 50.0, "z")
    prop_sandbags(x1, CRUCIBLE_CENTRE[1] + 70.0, 50.0, "z")
    prop_sandbags(CRUCIBLE_CENTRE[0], z1, 50.0, "x")

    # Elevated walkways threading the Crucible.
    for _ in range(60):
        x = rng.uniform(x0 + 20.0, x1 - 20.0)
        z = rng.uniform(z0 + 20.0, z1 - 20.0)
        prop_walkway(x, z, rng.uniform(20.0, 60.0),
                     "x" if rng.random() < 0.5 else "z")

    # Watchtowers on the Crucible perimeter.
    for _ in range(22):
        r = rng.uniform(0.86, 0.98)
        prop_watchtower(CRUCIBLE_CENTRE[0] + (x1 - x0) / 2.0 * r * (1 if rng.random() < 0.5 else -1),
                        CRUCIBLE_CENTRE[1] + (z1 - z0) / 2.0 * r * (1 if rng.random() < 0.5 else -1))

    # Rubble fields in the old city and the wasteland.
    for _ in range(70):
        x = rng.uniform(-3550.0, 3550.0)
        z = rng.uniform(-3550.0, 3550.0)
        d = district(x, z)
        if d in ("oldcity", "industrial", "financial", "wasteland"):
            ruined_stack("RubblePile", x, z, 0.0,
                         rng.uniform(8.0, 20.0), rng.uniform(8.0, 20.0),
                         rng.uniform(2.0, 6.0), pick(SCORCH),
                         (0.0, 0.0, 0.0, 1.0), 0.0)

    # Service cables strung between hab-blocks in the Lower Zones.
    v = -2600.0
    while v <= 2600.0:
        for z0 in (1000.0, 1800.0, 2600.0, 3300.0):
            if rng.random() < 0.45:
                cable(v, z0, v + rng.uniform(20.0, 45.0),
                      z0 + rng.uniform(-8.0, 8.0), rng.uniform(8.0, 16.0),
                      rng.uniform(8.0, 16.0))
        v += 120.0


# ---------------------------------------------------------------------------
# Set pieces
# ---------------------------------------------------------------------------

SET_ID = 96000000000000030001





# ===========================================================================
# CRUCIBLE STRUCTURES
# ===========================================================================
#
# Shacks alone read as a refugee camp. What makes a walled slum read as a
# neighbourhood is the stuff built BETWEEN and ON TOP of the shacks: walkways,
# because the ground is impassable; water tanks, because there is no mains;
# scaffolding, because nothing here is ever finished; stalls, because people
# trade; and cables, because power is stolen and strung wherever it will reach.


def _crucible_cells(inset=90.0):
    """Yield (i, j, x, z) for every occupied cell of the Crucible's own grid.

    Recomputes the same rotated, denser grid that generate_crucible_city lays
    buildings on, so the structures land on real rooftops and real alleys rather
    than on empty ground.
    """
    import math
    x0, x1, z0, z1 = CRUCIBLE
    cx, cz = CRUCIBLE_CENTRE
    ang = math.radians(CRUCIBLE_GRID_ANGLE)
    ca, sa = math.cos(ang), math.sin(ang)
    half = (x1 - x0) / 2.0
    n = int(2.0 * half / CRUCIBLE_GRID_PITCH)
    for i in range(n + 1):
        for j in range(n + 1):
            lx = -half + i * CRUCIBLE_GRID_PITCH
            lz = -half + j * CRUCIBLE_GRID_PITCH
            x = cx + lx * ca - lz * sa
            z = cz + lx * sa + lz * ca
            if not (x0 + inset <= x <= x1 - inset
                    and z0 + inset <= z <= z1 - inset):
                continue
            if in_clearing(x, z):
                continue
            yield i, j, x, z


def crucible_structures():
    """Walkways, water tanks, scaffolding, stalls, chimneys and stolen cables."""
    import math
    scrap = (0.148, 0.128, 0.102, 1.0)
    wood = (0.176, 0.142, 0.100, 1.0)
    rust = (0.196, 0.128, 0.076, 1.0)

    cells = list(_crucible_cells())
    # Height lookup: the shack stack on a cell tops out somewhere between 4.4 and
    # 18.4 m (2-4 layers of 2.2-4.6). Anything built on a roof needs that number.
    tops = {}
    for i, j, x, z in cells:
        tops[(i, j)] = rng.uniform(4.4, 18.4)

    # --- 1. skywalks: the streets above the streets ----------------------
    # A walled slum cannot widen its alleys, so it builds over them. These are
    # the single strongest read that this is a place people live rather than
    # camp: a walkway is a commitment to staying.
    for i, j, x, z in cells:
        if rng.random() > 0.30:
            continue
        for di, dj in ((1, 0), (0, 1)):
            if (i + di, j + dj) not in tops:
                continue
            if rng.random() > 0.42:
                continue
            # The neighbour's world position, recomputed the same way.
            nx, nz = None, None
            for ii, jj, xx, zz in cells:
                if ii == i + di and jj == j + dj:
                    nx, nz = xx, zz
                    break
            if nx is None:
                continue
            y = rng.uniform(4.0, 14.0)
            dx, dz = nx - x, nz - z
            ln = math.hypot(dx, dz)
            if ln < 1.0:
                continue
            yaw = -math.atan2(dz, dx)
            # Deck, then a rail on each side. No rotation: axis-swap instead, for
            # the same reason the walls do not rotate -- the rotated runs were
            # the two that did not render.
            along_z = abs(dz) > abs(dx)
            deck = ((2.2, 0.35, ln) if along_z else (ln, 0.35, 2.2))
            box(new_bid(), "CrucibleWalkway", (x, y, z), deck, wood,
                metallic=0.25, roughness=0.85, rot=(0.0, yaw, 0.0))
            for face in (-1.0, 1.0):
                ox = -dz / ln * face * 1.0
                oz = dx / ln * face * 1.0
                rail = ((0.22, 1.0, ln) if along_z else (ln, 1.0, 0.22))
                box(new_bid(), "CrucibleWalkwayRail",
                    (x + dx / 2.0 + ox, y + 0.8, z + dz / 2.0 + oz),
                    rail, wood, metallic=0.25, roughness=0.85,
                    rot=(0.0, yaw, 0.0))

    # --- 2. water tanks on the roofs -------------------------------------
    # No mains water in a sealed district, so every shack that can holds a tank.
    for i, j, x, z in cells:
        if rng.random() > 0.16:
            continue
        y = tops[(i, j)]
        cylinder(new_bid(), "WaterTank",
                 (x + rng.uniform(-8.0, 8.0), y + 3.0, z + rng.uniform(-8.0, 8.0)),
                 rng.uniform(2.2, 4.0), rng.uniform(4.0, 7.0), rust,
                 metallic=0.55, roughness=0.7, segments=10)
        for lx in (-1.0, 1.0):
            for lz in (-1.0, 1.0):
                box(new_bid(), "WaterTankLeg",
                    (x + lx * 2.2, y + 0.5, z + lz * 2.2),
                    (0.5, 1.0, 0.5), rust, metallic=0.4, roughness=0.8)

    # --- 3. scaffolding: nothing here is ever finished -------------------
    for i, j, x, z in cells:
        if rng.random() > 0.22:
            continue
        y = tops[(i, j)]
        h = rng.uniform(6.0, 14.0)
        w = rng.uniform(8.0, 16.0)
        # Four uprights and three ledger bars: a frame, not a wall.
        for sx in (-1.0, 1.0):
            for sz in (-1.0, 1.0):
                box(new_bid(), "ScaffoldPole",
                    (x + sx * w / 2.0, y + h / 2.0, z + sz * w / 2.0),
                    (0.35, h, 0.35), rust, metallic=0.5, roughness=0.75)
        for k in range(3):
            ly = y + h * (k + 1) / 4.0
            box(new_bid(), "ScaffoldLedger", (x, ly, z), (w, 0.28, 0.28),
                rust, metallic=0.5, roughness=0.75)
            box(new_bid(), "ScaffoldLedger", (x, ly, z), (0.28, 0.28, w),
                rust, metallic=0.5, roughness=0.75)

    # --- 4. market stalls at ground level -------------------------------
    # A sealed district still trades. Stalls cluster near the gates, because
    # that is where the traffic is.
    gates = ((CRUCIBLE[0], CRUCIBLE_CENTRE[1]), (CRUCIBLE[1], CRUCIBLE_CENTRE[1]),
             (CRUCIBLE_CENTRE[0], CRUCIBLE[0]))
    for _ in range(150):
        gx, gz = gates[rng.randrange(3)]
        x = gx + rng.uniform(-260.0, 260.0)
        z = gz + rng.uniform(-260.0, 260.0)
        if not (CRUCIBLE[0] + 100.0 <= x <= CRUCIBLE[1] - 100.0
                and CRUCIBLE[2] + 100.0 <= z <= CRUCIBLE[3] - 100.0):
            continue
        if in_clearing(x, z):
            continue
        w = rng.uniform(3.0, 6.0)
        d = rng.uniform(3.0, 6.0)
        h = rng.uniform(2.2, 3.4)
        box(new_bid(), "MarketStall", (x, h / 2.0, z), (w, h, d), wood,
            metallic=0.1, roughness=0.9)
        # The canopy: a tilted slab on four posts.
        box(new_bid(), "MarketStallCanopy",
            (x, h + 0.9, z), (w * 1.3, 0.22, d * 1.3), rust,
            metallic=0.05, roughness=0.9, rot=(0.12, 0.0, 0.0))

    # --- 5. chimneys: cooking and burning -------------------------------
    for i, j, x, z in cells:
        if rng.random() > 0.18:
            continue
        y = tops[(i, j)]
        h = rng.uniform(3.0, 8.0)
        cylinder(new_bid(), "Chimney",
                 (x + rng.uniform(-6.0, 6.0), y + h / 2.0, z + rng.uniform(-6.0, 6.0)),
                 rng.uniform(0.5, 1.2), h, rust, metallic=0.3, roughness=0.85,
                 segments=8)

    # --- 6. stolen cables strung between the shacks ---------------------
    # Power is taken from the corp grid and strung wherever it will reach. These
    # are the cables that make the district read as wired-in rather than as a camp.
    for _ in range(120):
        if len(cells) < 2:
            break
        a = cells[rng.randrange(len(cells))]
        b = cells[rng.randrange(len(cells))]
        if a == b:
            continue
        ya = tops[(a[0], a[1])] * rng.uniform(0.5, 0.9)
        yb = tops[(b[0], b[1])] * rng.uniform(0.5, 0.9)
        cable(a[2], a[3], b[2], b[3], ya, yb)

    # --- 7. external stair towers --------------------------------------
    # The only way up in a district that cannot widen its alleys.
    for i, j, x, z in cells:
        if rng.random() > 0.14:
            continue
        y = tops[(i, j)]
        steps = max(3, int(y / 1.8))
        for k in range(steps):
            # The first tread rests ON the ground (centre 0.175 = half the 0.35
            # tread thickness), so the stair starts at grade rather than hovering.
            box(new_bid(), "CrucibleStair",
                (x + 6.5, 0.175 + k * 1.8, z + (k % 2) * 1.6),
                (3.0, 0.35, 2.4), wood, metallic=0.2, roughness=0.85)

# ===========================================================================
# THE CRUCIBLE AS A SELF-CONTAINED CITY
# ===========================================================================
#
# The wall exists because this is not a district OF the city -- it is a city
# next to the city, that the corporations chose to enclose rather than demolish.
# Three things carry that read, and they are the whole point of the blockout:

CRUCIBLE_GRID_ANGLE = 14.0     # degrees off the outer city's grid
CRUCIBLE_GRID_PITCH = 45.0     # denser than the outer 100 m cells
CRUCIBLE_LOWER_Y = -14.0       # the sunken quarter


def _rot(x: float, z: float, cx: float, cz: float, ang: float):
    """Rotate (x,z) about (cx,cz) by ang radians. Used for the Crucible's own grid."""
    import math
    dx, dz = x - cx, z - cz
    ca, sa = math.cos(ang), math.sin(ang)
    return cx + dx * ca - dz * sa, cz + dx * sa + dz * ca


def generate_crucible_city():
    """The Crucible: its own grid, its own towers, its own lower level.

    1. ITS OWN STREET GRID, at a different angle and pitch to the outer city's.
       A district that shares the city's grid is part of the city; one whose
       streets do not line up is somewhere else entirely. This is the single
       strongest read in the whole blockout and it costs nothing but a rotation.

    2. ITS OWN TOWERS. Shacks alone read as a shantytown. Towers read as housing
       that was BUILT and is still inhabited -- which is what makes this a city
       rather than a ruin, and why it needed a wall around it.

    3. ITS OWN LOWER AREA. A sunken quarter cut down into the ground and reached
       by ramps: a place that has grown downward because it cannot grow outward,
       since it is walled in.
    """
    import math
    x0, x1, z0, z1 = CRUCIBLE
    cx, cz = CRUCIBLE_CENTRE
    ang = math.radians(CRUCIBLE_GRID_ANGLE)
    ca, sa = math.cos(ang), math.sin(ang)
    scrap = (0.148, 0.128, 0.102, 1.0)

    # --- 1. the internal grid, rotated and denser -------------------------
    half = (x1 - x0) / 2.0
    n = int(2.0 * half / CRUCIBLE_GRID_PITCH)
    for i in range(n + 1):
        for j in range(n + 1):
            # Grid coordinates in the Crucible's OWN frame, then rotated back
            # into world space. The rotation is what breaks the alignment with
            # the outer city's streets.
            lx = -half + i * CRUCIBLE_GRID_PITCH
            lz = -half + j * CRUCIBLE_GRID_PITCH
            x = cx + lx * ca - lz * sa
            z = cz + lx * sa + lz * ca
            # Inset by 90 m, not 20: the outer 90 m is the firebreak ring road,
            # so no building butts onto the wall. At a 20 m inset the district's
            # 28-42 m footprints reached straight past the boundary and hid the
            # wall entirely.
            if not (x0 + 90.0 <= x <= x1 - 90.0
                    and z0 + 90.0 <= z <= z1 - 90.0):
                continue
            if in_clearing(x, z):
                continue
            # EVERY cell is built. The first pass skipped 18% as "empty lots",
            # which is right for a city with planning permission and wrong for a
            # sealed district: a shantytown fills every square metre it is allowed
            # to, because the alternative is sleeping in the street. The grid is
            # therefore fully occupied, with no gaps except the clearings.
            #
            # The footprints are deliberately close to the grid pitch, so adjacent
            # buildings ABUT rather than stand apart. That is what makes the
            # district read as a continuous mass of construction rather than as a
            # scatter of huts -- a slum is dense before it is tall.
            w = rng.uniform(24.0, 36.0)
            d = rng.uniform(24.0, 36.0)
            layers = rng.randint(3, 5)
            y = 0.0
            cw, cd = w, d
            for k in range(layers):
                h = rng.uniform(2.2, 4.6)
                ox = rng.uniform(-0.9, 0.9)
                oz = rng.uniform(-0.9, 0.9)
                lean = rng.uniform(-0.05, 0.05)
                hot = (x - cx) ** 2 + (z - cz) ** 2 < ENTERTAINMENT_R ** 2
                alb = pick(SCRAP)
                glow = pick(NEON_HOT if hot else NEON_DEAD)
                inten = rng.uniform(1.5, 4.5) if hot else rng.uniform(0.08, 0.40)
                box(new_bid(), "Shack",
                    (x + ox, y + h / 2.0, z + oz), (cw, h, cd), alb,
                    metallic=0.15, roughness=0.9,
                    emission=glow, emission_intensity=inten,
                    rot=(lean, rng.uniform(0.0, 3.14), lean * 0.7))
                y += h
                cw *= rng.uniform(0.82, 1.0)
                cd *= rng.uniform(0.82, 1.0)
            # ROOFTOP SHACKS. The ground grid is full, so the district grows
            # upward: every stack gets one to three more shacks built on its
            # roof, on stilts, because there is nowhere else to put them. This
            # is the single most recognisable feature of a mature slum and it is
            # what makes the skyline read as inhabited rather than as a camp.
            for _ in range(rng.randint(1, 3)):
                rw = rng.uniform(4.0, 10.0)
                rd = rng.uniform(4.0, 10.0)
                rh = rng.uniform(2.0, 4.0)
                box(new_bid(), "Shack",
                    (x + rng.uniform(-9.0, 9.0), y + rh / 2.0,
                     z + rng.uniform(-9.0, 9.0)),
                    (rw, rh, rd), pick(SCRAP), metallic=0.15, roughness=0.9,
                    rot=(rng.uniform(-0.05, 0.05), rng.uniform(0.0, 3.14),
                         rng.uniform(-0.05, 0.05)))
            # A lean-to or a scrap lean-to against the shack.
            if rng.random() < 0.30:
                box(new_bid(), "ShackLean",
                    (x + rng.uniform(-6.0, 6.0), 1.4, z + rng.uniform(-6.0, 6.0)),
                    (rng.uniform(3.0, 6.0), 2.8, rng.uniform(0.3, 0.8)),
                    pick(SCRAP), metallic=0.1, roughness=0.92,
                    rot=(rng.uniform(-0.35, 0.35), rng.uniform(0.0, 3.14), 0.0))

    # --- 2. its own towers: the housing blocks ----------------------------
    # Clustered off-centre so the entertainment strip stays legible. These are
    # the reason the place is a city and not a ruin.
    tower_pts = []
    for i in range(18):
        a = i / 11.0 * 2.0 * math.pi + 0.4
        r = rng.uniform(180.0, 620.0)
        tx = cx + math.cos(a) * r
        tz = cz + math.sin(a) * r * 0.85
        if not (x0 + 120.0 <= tx <= x1 - 120.0
                and z0 + 120.0 <= tz <= z1 - 120.0):
            continue
        if in_clearing(tx, tz):
            continue
        tower_pts.append((tx, tz))
        h = rng.uniform(60.0, 150.0)
        slab_building("CrucibleTower", tx, tz, 0.0, rng.uniform(34.0, 52.0),
                      rng.uniform(34.0, 52.0), h, (0.132, 0.116, 0.096, 1.0),
                      0.35, 0.85, pick(NEON_DEAD), rng.uniform(0.10, 0.55),
                      force=True)
        # A rooftop shack or two: the blocks are still being built on top of.
        for _ in range(rng.randint(1, 3)):
            box(new_bid(), "Shack",
                (tx + rng.uniform(-14.0, 14.0), h + 1.6,
                 tz + rng.uniform(-14.0, 14.0)),
                (rng.uniform(4.0, 8.0), 3.2, rng.uniform(4.0, 8.0)),
                pick(SCRAP), metallic=0.15, roughness=0.9,
                rot=(0.0, rng.uniform(0.0, 3.14), 0.0))

    # --- 3. the lower area: the sunken quarter ---------------------------
    # The south-west quadrant, cut down 14 m. Retaining walls close it, ramps
    # are the only way in, and the shacks down there are the poorest of the
    # district -- which is exactly why they are at the bottom.
    lx0, lx1 = x0 + 40.0, x0 + 1250.0
    lz0, lz1 = z0 + 40.0, z0 + 1250.0
    box(new_bid(), "CrucibleLowerFloor",
        ((lx0 + lx1) / 2.0, CRUCIBLE_LOWER_Y - 0.3, (lz0 + lz1) / 2.0),
        (lx1 - lx0, 0.6, lz1 - lz0), (0.078, 0.072, 0.066, 1.0),
        metallic=0.05, roughness=0.95)
    # Retaining walls on all four sides.
    for (wx0, wz0, wx1, wz1) in ((lx0, lz0, lx1, lz0), (lx0, lz1, lx1, lz1),
                                 (lx0, lz0, lx0, lz1), (lx1, lz0, lx1, lz1)):
        mx, mz = (wx0 + wx1) / 2.0, (wz0 + wz1) / 2.0
        dx, dz = wx1 - wx0, wz1 - wz0
        ln = math.hypot(dx, dz)
        box(new_bid(), "CrucibleRetainingWall",
            (mx, CRUCIBLE_LOWER_Y / 2.0, mz), (ln, -CRUCIBLE_LOWER_Y, 3.0),
            (0.108, 0.100, 0.090, 1.0), metallic=0.2, roughness=0.9,
            rot=(0.0, -math.atan2(dz, dx), 0.0))
    # Ramps down: three of them, one per side, so the lower area is reachable
    # but only at points -- which is the point of a sunken quarter.
    for (rx, rz, axis) in ((lx1, (lz0 + lz1) / 2.0, "x"),
                           ((lx0 + lx1) / 2.0, lz1, "z")):
        for k in range(6):
            u = k / 6.0
            ry = CRUCIBLE_LOWER_Y * u
            if axis == "x":
                box(new_bid(), "CrucibleRamp",
                    (rx + 40.0 - u * 80.0, ry / 2.0, rz),
                    (80.0, 0.5, 26.0), (0.098, 0.092, 0.084, 1.0),
                    metallic=0.2, roughness=0.9)
            else:
                box(new_bid(), "CrucibleRamp",
                    (rx, ry / 2.0, rz + 40.0 - u * 80.0),
                    (26.0, 0.5, 80.0), (0.098, 0.092, 0.084, 1.0),
                    metallic=0.2, roughness=0.9)
    # Shacks down in the lower area -- the poorest of the district.
    for _ in range(140):
        sx = rng.uniform(lx0 + 30.0, lx1 - 30.0)
        sz = rng.uniform(lz0 + 30.0, lz1 - 30.0)
        w = rng.uniform(3.5, 8.0)
        d = rng.uniform(3.5, 8.0)
        h = rng.uniform(2.0, 4.0)
        box(new_bid(), "Shack",
            (sx, CRUCIBLE_LOWER_Y + h / 2.0, sz), (w, h, d),
            pick(SCRAP), metallic=0.1, roughness=0.92,
            rot=(rng.uniform(-0.06, 0.06), rng.uniform(0.0, 3.14), 0.0))

    # --- 4. hellscape dressing ------------------------------------------
    # Fires down here are not decoration: in a sealed district with no
    # infrastructure, burning is how things get disposed of.
    for _ in range(70):
        fx = rng.uniform(x0 + 60.0, x1 - 60.0)
        fz = rng.uniform(z0 + 60.0, z1 - 60.0)
        fy = CRUCIBLE_LOWER_Y if (fx < lx1 and fz < lz1) else 0.0
        box(new_bid(), "Fire", (fx, fy + 1.2, fz),
            (rng.uniform(0.8, 1.6), 2.4, rng.uniform(0.8, 1.6)),
            (0.92, 0.34, 0.08, 1.0), metallic=0.0, roughness=0.5,
            emission=(0.92, 0.34, 0.08, 1.0), emission_intensity=rng.uniform(2.0, 5.0))
    # Scorch marks.
    for _ in range(90):
        fx = rng.uniform(x0 + 60.0, x1 - 60.0)
        fz = rng.uniform(z0 + 60.0, z1 - 60.0)
        box(new_bid(), "Scorch", (fx, 0.06, fz),
            (rng.uniform(6.0, 18.0), 0.12, rng.uniform(6.0, 18.0)),
            (0.030, 0.026, 0.022, 1.0), metallic=0.0, roughness=1.0)
    # Barricades: a sealed district is also a defended one.
    for _ in range(90):
        bx = rng.uniform(x0 + 80.0, x1 - 80.0)
        bz = rng.uniform(z0 + 80.0, z1 - 80.0)
        box(new_bid(), "Barricade", (bx, 1.6, bz),
            (rng.uniform(4.0, 12.0), 3.2, 1.4), (0.176, 0.152, 0.116, 1.0),
            metallic=0.45, roughness=0.8, rot=(0.0, rng.uniform(0.0, 3.14), 0.0))

# ===========================================================================
# DISTRICT SIGNATURES
#
# Each district gets ONE thing that makes it unmistakable from any other part
# of the city, the way a real district is identified by what it is fenced with
# or built around. Everything here is greybox: masses, barriers, towers and
# cables, no material work -- enough to read and to be refined later.
# ===========================================================================


def _wall_run(x0, z0, x1, z1, gaps, tag, height, thick, pitch, albedo,
              parapet=True):
    global SET_ID
    """Emit wall segments along a straight run, skipping the given gaps.

    `gaps` is a list of (centre, half_width) distances measured along the run
    from (x0,z0). A gap is where a gate goes, so the wall stops short of it and
    the gate structure closes the hole.

    The segment is a box whose local X axis runs along the wall, so it needs the
    same yaw convention as everything else here: yaw = -atan2(dz, dx).
    """
    import math
    dx, dz = x1 - x0, z1 - z0
    length = math.hypot(dx, dz)
    if length < 1.0:
        return
    ux, uz = dx / length, dz / length
    yaw = -math.atan2(uz, ux)
    n = max(1, int(length / pitch))
    seg = length / n
    # Axis-swap instead of rotate. For a run along Z the length goes in the Z
    # slot and the thickness in X, so the box is axis-aligned and yaw stays 0.
    # Measured against the render: the two rotated runs (east and west walls)
    # produced 0 and 16k bright pixels where the unrotated runs (north and south)
    # produced 123k and 521k. Whatever the rotation path does with a 90-degree
    # yaw on this geometry, it loses the wall -- so the wall does not use it.
    along_z = abs(uz) > abs(ux)
    scale = (thick, height, seg * 0.97) if along_z else (seg * 0.97, height, thick)
    for i in range(n):
        c = (i + 0.5) * seg
        if any(abs(c - gc) <= gw for gc, gw in gaps):
            continue
        box(SET_ID, tag, (x0 + ux * c, height / 2.0, z0 + uz * c),
            scale, albedo, metallic=0.55, roughness=0.75)
        SET_ID += 1
        if parapet:
            # A lit strip along the top of the wall, on BOTH faces. A wall is
            # lit at night -- that is how you know where the boundary is -- and
            # it is the single cheapest way to make an enclosure legible from a
            # distance. The strip runs the full length, so it reads as a
            # continuous line rather than as separate lamps.
            for face in (-1.0, 1.0):
                px = x0 + ux * c - uz * face * (thick / 2.0 + 0.35)
                pz = z0 + uz * c + ux * face * (thick / 2.0 + 0.35)
                lamp_scale = ((0.6, 1.0, seg * 0.9) if along_z
                              else (seg * 0.9, 1.0, 0.6))
                box(SET_ID, "CrucibleWallLamp", (px, height + 1.2, pz),
                    lamp_scale, (0.90, 0.42, 0.14, 1.0),
                    metallic=0.05, roughness=0.45,
                    emission=(0.90, 0.42, 0.14, 1.0),
                    emission_intensity=rng.uniform(2.5, 5.5))
                SET_ID += 1
            # A buttress, so the wall has a silhouette and not just a flat face.
            bx = x0 + ux * c
            bz = z0 + uz * c
            box(SET_ID, "CrucibleButtress",
                (bx + uz * (thick / 2.0 + 2.4), height * 0.35,
                 bz - ux * (thick / 2.0 + 2.4)),
                ((5.0, height * 0.7, seg * 0.30) if along_z
                 else (seg * 0.30, height * 0.7, 5.0)),
                albedo, metallic=0.5, roughness=0.8)
            SET_ID += 1


def _guard_gate(x, z, axis, tag_prefix, albedo):
    global SET_ID
    """A guarded choke point: two towers, a beam, a barrier line, a watchtower.

    The towers are what make a gap in a wall read as a CONTROL POINT rather than
    as a hole. The barrier line is the kill-zone -- it is deliberately offset
    from the gate so the approach is not straight, which is how a real checkpoint
    forces a vehicle to slow and present itself.
    """
    import math
    if axis == "x":          # the gate faces along X, so the opening runs in Z
        for sz in (-1.0, 1.0):
            box(SET_ID, tag_prefix + "Tower", (x, 15.0, z + sz * 26.0),
                (14.0, 30.0, 14.0), albedo, metallic=0.55, roughness=0.72)
            SET_ID += 1
        box(SET_ID, tag_prefix + "Beam", (x, 31.0, z), (9.0, 6.0, 66.0),
            albedo, metallic=0.55, roughness=0.72)
        SET_ID += 1
        # The zig-zag: two staggered barrier lines on each approach.
        for side in (-1.0, 1.0):
            for i in range(6):
                bz = z - 70.0 + i * 26.0
                box(SET_ID, tag_prefix + "Barrier",
                    (x + side * (14.0 + (i % 2) * 16.0), 1.6, bz),
                    (14.0, 3.2, 1.4), (0.205, 0.176, 0.128, 1.0),
                    metallic=0.5, roughness=0.7, rot=(0.0, side * 0.35, 0.0))
                SET_ID += 1
    else:                    # the gate faces along Z, so the opening runs in X
        for sx in (-1.0, 1.0):
            box(SET_ID, tag_prefix + "Tower", (x + sx * 26.0, 15.0, z),
                (14.0, 30.0, 14.0), albedo, metallic=0.55, roughness=0.72)
            SET_ID += 1
        box(SET_ID, tag_prefix + "Beam", (x, 31.0, z), (66.0, 6.0, 9.0),
            albedo, metallic=0.55, roughness=0.72)
        SET_ID += 1
        for side in (-1.0, 1.0):
            for i in range(6):
                bx = x - 70.0 + i * 26.0
                box(SET_ID, tag_prefix + "Barrier",
                    (bx, 1.6, z + side * (14.0 + (i % 2) * 16.0)),
                    (1.4, 3.2, 14.0), (0.205, 0.176, 0.128, 1.0),
                    metallic=0.5, roughness=0.7, rot=(0.0, side * 0.35, 0.0))
                SET_ID += 1
    # The watchtower, set back and off-axis so it can see both approaches.
    box(SET_ID, tag_prefix + "Watchtower", (x + 40.0, 26.0, z + 40.0),
        (16.0, 52.0, 16.0), albedo, metallic=0.55, roughness=0.72)
    SET_ID += 1
    box(SET_ID, tag_prefix + "WatchtowerCab",
        (x + 40.0, 54.0, z + 40.0), (20.0, 6.0, 20.0),
        (0.120, 0.116, 0.112, 1.0), metallic=0.6, roughness=0.6)
    SET_ID += 1


def set_piece_crucible_wall():
    global SET_ID
    """The Crucible is WALLED, with three guarded gates.

    This is the district's identity: a shantytown the corporations chose to
    enclose rather than clear. The wall is what makes it read as a container --
    without it the Crucible is just a poor district, and with it the same streets
    read as a holding pen.

    The wall follows the district rectangle with a gap per gate. Three gates, not
    four: the south edge backs onto the industrial belt, so the only way in is
    the north gate, the east gate off the financial ring, and the west gate off
    the old city. Every one of them is a choke point.
    """
    import math
    x0, x1, z0, z1 = CRUCIBLE
    # A poured-concrete wall, deliberately LIGHTER and warmer than the scrap the
    # district is built from. At the first attempt the wall used the same dark
    # grey as everything around it (0.148, 0.130, 0.104), so at any grazing angle
    # it was indistinguishable from the buildings and read as absent -- which is
    # exactly what was reported. A fortification has to read as a different
    # material from the slum inside it.
    albedo = (0.315, 0.288, 0.246, 1.0)
    # Gates sit on the north, east and west edges. Distances are measured along
    # each run from its start corner, matching _wall_run's convention.
    # 26 m tall and 12 m thick, not 14 and 7. At 14 m the wall was shorter than
    # the district's own buildings -- the shacks run 2-4 layers of 2.2-4.6 m, so
    # up to ~18 m, on footprints that reach the boundary -- and it was completely
    # hidden inside them. A wall you cannot see is not a wall. At 26 m it rises
    # above every roofline in the district, which is what makes the enclosure
    # legible from outside.
    _wall_run(x0, z0, x1, z0, [((x1 - x0) / 2.0, 42.0)],
              "CrucibleWall", 26.0, 12.0, 38.0, albedo)         # north
    _wall_run(x0, z1, x1, z1, [], "CrucibleWall", 26.0, 12.0, 38.0, albedo)  # south
    _wall_run(x0, z0, x0, z1, [((z1 - z0) / 2.0, 42.0)],
              "CrucibleWall", 26.0, 12.0, 38.0, albedo)         # west
    _wall_run(x1, z0, x1, z1, [((z1 - z0) / 2.0, 42.0)],
              "CrucibleWall", 26.0, 12.0, 38.0, albedo)         # east
    # The firebreak: a clear ring road just inside the wall. A wall needs clear
    # ground on its inner face -- without it the buildings butt straight onto the
    # masonry and the wall disappears behind them, which is exactly what happened
    # at the first attempt. The road is also how a sealed district gets supplies
    # in and out without opening a gate.
    fb = 46.0
    for (rx0, rz0, rx1, rz1) in (
            (x0 + fb, z0 + fb, x1 - fb, z0 + fb),
            (x0 + fb, z1 - fb, x1 - fb, z1 - fb),
            (x0 + fb, z0 + fb, x0 + fb, z1 - fb),
            (x1 - fb, z0 + fb, x1 - fb, z1 - fb)):
        mx, mz = (rx0 + rx1) / 2.0, (rz0 + rz1) / 2.0
        dx, dz = rx1 - rx0, rz1 - rz0
        ln = math.hypot(dx, dz)
        box(SET_ID, "CrucibleRingRoad", (mx, 0.16, mz),
            (ln, 0.32, 34.0), (0.115, 0.110, 0.104, 1.0),
            metallic=0.1, roughness=0.92, rot=(0.0, -math.atan2(dz, dx), 0.0))
        SET_ID += 1
    _guard_gate(x0, (z0 + z1) / 2.0, "x", "CrucibleGateW", albedo)
    _guard_gate(x1, (z0 + z1) / 2.0, "x", "CrucibleGateE", albedo)
    _guard_gate((x0 + x1) / 2.0, z0, "z", "CrucibleGateN", albedo)
    # Watchtowers along the wall, so the perimeter is patrolled rather than
    # merely closed. Every 400 m, alternating sides.
    per = 2.0 * ((x1 - x0) + (z1 - z0))
    for i in range(int(per / 400.0)):
        c = i * 400.0 + 200.0
        if c > x1 - x0:
            c -= (x1 - x0)
            if c > z1 - z0:
                c -= (z1 - z0)
                wx, wz = x0 + c, z1
            else:
                wx, wz = x1, z0 + c
        else:
            wx, wz = x0 + c, z0
        box(SET_ID, "CrucibleWatchtower", (wx, 22.0, wz),
            (12.0, 44.0, 12.0), albedo, metallic=0.55, roughness=0.72)
        SET_ID += 1


def set_piece_core_cordon():
    global SET_ID
    """The Core is a fortress with the Spire at its centre.

    The cordon is a ring of barriers and guard posts on the Core boundary, with
    four checkpoints aligned to the arterials. Unlike the Crucible wall this is
    not a wall -- the Core is where the corporations WANT you to go, so it is
    controlled rather than sealed. The sigil is the one piece of saturated colour
    in the district: the corporate mark, huge, unmissable, and lit.
    """
    import math
    r = 700.0
    albedo = (0.132, 0.126, 0.118, 1.0)
    # Barrier ring, with a gap at each of the four arterials.
    pitch = 34.0
    for i in range(int(4.0 * r / pitch)):
        for side, (cx, cz, ax) in enumerate(
                ((-r, 0.0, "x"), (r, 0.0, "x"), (0.0, -r, "z"), (0.0, r, "z"))):
            c = -r + (i + 0.5) * pitch
            if abs(c) < 70.0:            # the arterial gap
                continue
            if ax == "x":
                bx, bz, bs = cx, cz + c, (1.6, 3.4, 30.0)
            else:
                bx, bz, bs = cx + c, cz, (30.0, 3.4, 1.6)
            box(SET_ID, "CoreBarrier", (bx, 1.7, bz), bs,
                (0.208, 0.182, 0.138, 1.0), metallic=0.5, roughness=0.7)
            SET_ID += 1
    # Four checkpoints, one per arterial.
    for cx, cz, axis in ((-r, 0.0, "x"), (r, 0.0, "x"),
                         (0.0, -r, "z"), (0.0, r, "z")):
        _guard_gate(cx, cz, axis, "CoreGate", albedo)
    # The sigil: a huge emissive ring floating over the Spire plaza. It is the
    # corporate mark and the only saturated light in the Core.
    for i in range(48):
        a = i / 48.0 * 2.0 * math.pi
        box(SET_ID, "CorpSigil",
            (math.cos(a) * 150.0, 130.0, math.sin(a) * 150.0),
            (14.0, 2.0, 14.0), (0.90, 0.16, 0.08, 1.0),
            metallic=0.2, roughness=0.4,
            emission=(0.90, 0.16, 0.08, 1.0), emission_intensity=rng.uniform(4.0, 9.0),
            rot=(0.0, -a, 0.0))
        SET_ID += 1


def set_piece_arcology():
    global SET_ID
    """The Financial Arcology: a self-contained megastructure.

    Eight towers in a ring, joined by skybridges, around a void. The Financial
    district is where the corporations live as well as work, so its signature is
    a building that does not need the street at all -- the point being that the
    people inside it never have to come down.
    """
    import math
    cx, cz = 1200.0, -1200.0
    ring_r = 210.0
    albedo = (0.168, 0.160, 0.152, 1.0)
    glass = (0.120, 0.150, 0.176, 1.0)
    for i in range(8):
        a = i / 8.0 * 2.0 * math.pi
        tx, tz = cx + math.cos(a) * ring_r, cz + math.sin(a) * ring_r
        h = rng.uniform(150.0, 230.0)
        # Each tower is a tiered mass, so the arcology reads as a cluster of
        # distinct buildings rather than as one ring wall.
        slab_building("ArcologyTower", tx, tz, 0.0, 150.0, 150.0, h, glass,
                      0.85, 0.22, VOID_PURPLE, rng.uniform(0.10, 0.35),
                      force=True)
        # Skybridges to the two neighbours, at two heights.
        for dh, br_h in ((60.0, 4.0), (120.0, 4.0)):
            if dh > h:
                continue
            na = (i + 1) / 8.0 * 2.0 * math.pi
            nx, nz = cx + math.cos(na) * ring_r, cz + math.sin(na) * ring_r
            mx, mz = (tx + nx) / 2.0, (tz + nz) / 2.0
            dx, dz = nx - tx, nz - tz
            ln = math.hypot(dx, dz)
            box(SET_ID, "ArcologyBridge", (mx, br_h, mz),
                (ln, 8.0, 18.0), albedo, metallic=0.6, roughness=0.5,
                rot=(0.0, -math.atan2(dz, dx), 0.0))
            SET_ID += 1
    # The void in the middle: a sunken court, so the ring has something to be
    # a ring around.
    box(SET_ID, "ArcologyCourt", (cx, -0.4, cz), (ring_r * 2.0 - 60.0, 0.8,
        ring_r * 2.0 - 60.0), (0.075, 0.088, 0.104, 1.0),
        metallic=0.2, roughness=0.85)
    SET_ID += 1


def set_piece_collapsed_arcology():
    global SET_ID
    """The Old City's collapsed arcology: the same idea, half-buried.

    The Old City is what the Financial district used to be. So the signature is
    the Financial arcology's ruin -- the same ring of towers, but sheared off at
    a common height and tilted, with the whole thing sunk into the ground. The
    tilt is what makes it read as collapsed rather than as a low ring of towers.
    """
    import math
    cx, cz = -2200.0, 300.0
    ring_r = 240.0
    albedo = (0.152, 0.140, 0.124, 1.0)
    for i in range(8):
        a = i / 8.0 * 2.0 * math.pi
        tx, tz = cx + math.cos(a) * ring_r, cz + math.sin(a) * ring_r
        # Shear line: every tower is cut to roughly this height, with variation.
        h = rng.uniform(45.0, 95.0)
        slab_building("CollapsedArcology", tx, tz, -14.0, 170.0, 170.0, h,
                      albedo, 0.15, 0.9, (0.0, 0.0, 0.0, 1.0), 0.0,
                      force=True)
        # The shear: a tilted cap that suggests the rest of the tower came down.
        box(SET_ID, "CollapsedShear", (tx, h - 14.0 + 6.0, tz),
            (150.0, 12.0, 150.0), (0.138, 0.128, 0.114, 1.0),
            metallic=0.1, roughness=0.92,
            rot=(rng.uniform(0.10, 0.30), rng.uniform(0.0, 3.14),
                 rng.uniform(-0.18, 0.18)))
        SET_ID += 1


def set_piece_funicular():
    global SET_ID
    """The Hills funicular: a cable-car line climbing the terraces.

    The Hills district is terraced, so its signature is the thing that makes the
    terraces traversable -- a cable-car line running up the fall line, on towers,
    with cars. It reads as transport infrastructure rather than as architecture,
    which is the right note for a district of row houses.
    """
    import math
    x0, x1, z0, z1 = HILLS
    # The fall line runs up the hill: from the low corner to the high one.
    ax, az = x0 + 260.0, z1 - 260.0
    bx, bz = x1 - 260.0, z0 + 260.0
    dx, dz = bx - ax, bz - az
    ln = math.hypot(dx, dz)
    ux, uz = dx / ln, dz / ln
    albedo = (0.160, 0.152, 0.140, 1.0)
    n = 16
    prev_top = None
    for i in range(n + 1):
        c = i / n
        px, pz = ax + dx * c, az + dz * c
        base = hill_base_y(px, pz)
        top = base + 26.0
        # The tower.
        box(SET_ID, "FunicularTower", (px, base + 13.0, pz),
            (8.0, 26.0, 8.0), albedo, metallic=0.55, roughness=0.7)
        SET_ID += 1
        # The cable run to the previous tower, as a sagging cylinder chain.
        if prev_top is not None:
            qx, qz = prev_top[0], prev_top[1]
            qy = prev_top[2]
            seg_n = max(4, min(12, int(math.hypot(px - qx, pz - qz) / 8.0)))
            sag = rng.uniform(2.0, 4.0)
            pts = []
            for k in range(seg_n + 1):
                u = k / seg_n
                pts.append((qx + (px - qx) * u,
                            qy + (top - qy) * u - sag * 4.0 * u * (1.0 - u),
                            qz + (pz - qz) * u))
            for k in range(seg_n):
                ax2, ay2, az2 = pts[k]
                bx2, by2, bz2 = pts[k + 1]
                sx, sy, sz2 = bx2 - ax2, by2 - ay2, bz2 - az2
                sl = math.sqrt(sx * sx + sy * sy + sz2 * sz2)
                if sl < 0.05:
                    continue
                dy = max(-1.0, min(1.0, sy / sl))
                box(SET_ID, "FunicularCable",
                    ((ax2 + bx2) / 2.0, (ay2 + by2) / 2.0, (az2 + bz2) / 2.0),
                    (sl * 1.06, 0.22, 0.22), (0.045, 0.042, 0.040, 1.0),
                    metallic=0.4, roughness=0.7,
                    rot=(math.acos(dy), math.atan2(sx, sz2), 0.0))
                SET_ID += 1
            # A car, hanging partway along the run.
            cu = rng.uniform(0.3, 0.7)
            carx = qx + (px - qx) * cu
            cary = qy + (top - qy) * cu - sag * 4.0 * cu * (1.0 - cu) - 3.0
            box(SET_ID, "FunicularCar", (carx, cary, qz + (pz - qz) * cu),
                (9.0, 7.0, 11.0), (0.196, 0.158, 0.112, 1.0),
                metallic=0.6, roughness=0.5)
            SET_ID += 1
        prev_top = (px, pz, top)
    # Terminus stations at both ends.
    for sx, sz, e in ((ax, az, 1.0), (bx, bz, -1.0)):
        b = hill_base_y(sx, sz)
        box(SET_ID, "FunicularStation", (sx, b + 7.0, sz),
            (46.0, 14.0, 26.0), (0.176, 0.166, 0.152, 1.0),
            metallic=0.5, roughness=0.6)
        SET_ID += 1


def set_piece_beacon():
    global SET_ID
    """The Waterfront beacon: a light tower on the seawall, and a bay gate.

    The Waterfront is the city's edge, so its signature is the thing that marks
    the edge -- a beacon on the seawall, visible from the whole bay. The bay gate
    is the checkpoint where the waterfront road meets the water, which is what
    makes the district read as a controlled port rather than as a shoreline.
    """
    import math
    albedo = (0.172, 0.164, 0.152, 1.0)
    bx, bz = BAY_X - 14.0, 0.0
    # The beacon: a tapering stack of boxes, so it reads as a tower rather than
    # as a column.
    y = 0.0
    for i in range(7):
        h = rng.uniform(18.0, 26.0)
        w = 26.0 * (1.0 - i * 0.11)
        box(SET_ID, "BeaconTower", (bx, y + h / 2.0, bz), (w, h, w),
            albedo, metallic=0.6, roughness=0.5)
        SET_ID += 1
        y += h
    # The lamp head: the one saturated light on the whole waterfront.
    box(SET_ID, "BeaconLamp", (bx, y + 9.0, bz), (34.0, 18.0, 34.0),
        (0.96, 0.62, 0.20, 1.0), metallic=0.1, roughness=0.35,
        emission=(0.96, 0.62, 0.20, 1.0), emission_intensity=rng.uniform(8.0, 16.0))
    SET_ID += 1
    box(SET_ID, "BeaconLampGlow", (bx, y + 9.0, bz), (1.0, 1.0, 1.0),
        (0.96, 0.62, 0.20, 1.0), metallic=0.0, roughness=0.3,
        emission=(0.96, 0.62, 0.20, 1.0), emission_intensity=0.0,
        extra=light_extra((0.96, 0.62, 0.20, 1.0), rng.uniform(180.0, 260.0),
                          420.0))
    SET_ID += 1
    # The bay gate: where the waterfront road meets the water.
    gx = 2700.0
    _guard_gate(gx, 0.0, "x", "BayGate", albedo)


def set_piece_processional():
    global SET_ID
    """The Lower Zones processional: the road from the spawn plaza to the Core.

    The Lower Zones are where the player starts, so their signature is the route
    in -- a straight avenue from the spawn plaza to the Core cordon, lined with
    light pylons and banners. It gives the starting district a direction and a
    destination, which is the one thing a spawn area otherwise lacks.
    """
    import math
    albedo = (0.158, 0.150, 0.138, 1.0)
    z0, z1 = 1250.0, 760.0
    # The avenue deck, slightly raised, running due north to the Core.
    box(SET_ID, "ProcessionalDeck", (0.0, 0.35, (z0 + z1) / 2.0),
        (120.0, 0.7, z0 - z1), (0.208, 0.198, 0.186, 1.0),
        metallic=0.2, roughness=0.8)
    SET_ID += 1
    # Light pylons down both edges, and a banner between each pair.
    n = 14
    for i in range(n):
        z = z0 - 40.0 - i * (z0 - z1 - 80.0) / (n - 1)
        for sx in (-1.0, 1.0):
            box(SET_ID, "ProcessionalPylon", (sx * 56.0, 9.0, z),
                (3.0, 18.0, 3.0), albedo, metallic=0.6, roughness=0.5)
            SET_ID += 1
            box(SET_ID, "ProcessionalPylonLamp", (sx * 56.0, 19.0, z),
                (5.0, 3.0, 5.0), (0.90, 0.55, 0.22, 1.0),
                metallic=0.1, roughness=0.4,
                emission=(0.90, 0.55, 0.22, 1.0),
                emission_intensity=rng.uniform(3.0, 6.0))
            SET_ID += 1
        if i < n - 1:
            z2 = z0 - 40.0 - (i + 1) * (z0 - z1 - 80.0) / (n - 1)
            box(SET_ID, "ProcessionalBanner", (0.0, 15.0, (z + z2) / 2.0),
                (64.0, 9.0, 1.0), (0.24, 0.14, 0.10, 1.0),
                metallic=0.1, roughness=0.7,
                rot=(0.0, math.pi / 2.0, 0.0))
            SET_ID += 1


def set_piece_smokestacks():
    global SET_ID
    """The Industrial belt: a smokestack forest and a rail yard.

    The Industrial district is where the city's power comes from, so its
    signature is the stack line -- a row of tall chimneys on the district's
    southern edge, plus the rail yard that feeds them. The stacks are the
    tallest things in the district by a wide margin, which is the point: you can
    see where the power is from anywhere in the city.
    """
    import math
    albedo = (0.156, 0.146, 0.132, 1.0)
    # The stack line along the industrial belt's southern edge.
    for i in range(11):
        x = -1200.0 + i * 240.0
        z = -2900.0
        h = rng.uniform(120.0, 210.0)
        # Tapered: a stack that reads as a chimney rather than as a column.
        for k in range(6):
            seg = h / 6.0
            w = 26.0 * (1.0 - k * 0.10)
            box(SET_ID, "Smokestack", (x, seg * (k + 0.5), z), (w, seg, w),
                albedo, metallic=0.45, roughness=0.8)
            SET_ID += 1
        # The warning light on top.
        box(SET_ID, "SmokestackBeacon", (x, h + 3.0, z), (5.0, 6.0, 5.0),
            (0.92, 0.20, 0.12, 1.0), metallic=0.1, roughness=0.4,
            emission=(0.92, 0.20, 0.12, 1.0),
            emission_intensity=rng.uniform(4.0, 8.0))
        SET_ID += 1
    # The rail yard: two long parallel beds with sleepers.
    for rx in (-400.0, 0.0, 400.0):
        box(SET_ID, "RailBed", (rx, 0.15, -2300.0), (8.0, 0.3, 1400.0),
            (0.088, 0.084, 0.080, 1.0), metallic=0.3, roughness=0.9)
        SET_ID += 1
        for i in range(40):
            box(SET_ID, "RailSleeper", (rx, 0.45, -2900.0 + i * 32.0),
                (14.0, 0.3, 2.4), (0.120, 0.108, 0.096, 1.0),
                metallic=0.05, roughness=0.95)
            SET_ID += 1

def set_piece_spire():
    """The Spire analogue: a 200-storey tower converted to raw Element Zero.

    Built as a stack of offset, twisting segments so it reads as something
    that was converted rather than built. The void-purple emission is the
    only saturated light in the core.
    """
    global SET_ID
    x, z = 0.0, 0.0
    segs = 7
    y = 0.0
    w = 110.0
    d = 110.0
    # 560 m, not 340. The city is 2.7x wider, so a tower that anchored the old
    # skyline is a mid-rise here; the Spire has to stay the tallest thing in
    # the Ash District or the core reads as flat.
    total = 560.0
    for i in range(segs):
        h = total / segs * rng.uniform(0.9, 1.1)
        off = (i * 4.5)
        box(SET_ID, "SpireRuins", (x + off * 0.45, y + h / 2.0, z - off * 0.30),
            (w, h, d), pick(ELEMENT_ZERO_GLASS),
            metallic=0.92, roughness=0.10,
            emission=VOID_PURPLE, emission_intensity=rng.uniform(1.2, 3.6),
            rot=(0.0, i * 0.10, 0.0))
        SET_ID += 1
        # Conversion glow banding: the Element Zero showing through.
        neon_strip("VoidGlow", x + off * 0.45, y + h * 0.5,
                   z - off * 0.30 + d / 2.0 + 0.3, w * rng.uniform(0.5, 0.9),
                   "x", VOID_PURPLE, rng.uniform(3.0, 8.0))
        y += h
        w *= 0.90
        d *= 0.90
    # The statue at its foot: Voss, converted, still conscious.
    box(SET_ID, "VoidStatue", (x + 44.0, 5.5, z + 34.0), (4.2, 11.0, 4.2),
        (0.30, 0.06, 0.36, 1.0), metallic=0.9, roughness=0.15,
        emission=VOID_PURPLE, emission_intensity=2.5)
    SET_ID += 1


def set_piece_last_note():
    """The Last Note: Kael Voss's underground venue, in the Crucible."""
    global SET_ID
    x, z = CRUCIBLE_CENTRE
    # A long, low, fortified hall.
    box(SET_ID, "TheLastNote", (x, 8.0, z), (72.0, 16.0, 38.0),
        (0.180, 0.146, 0.112, 1.0), metallic=0.5, roughness=0.75)
    SET_ID += 1
    # Roofline signage, hot pink -- the one bright thing in the district.
    for i in range(7):
        neon_strip("NeonSign", x - 26.0 + i * 8.7, 17.2, z + 19.4,
                   6.0, "x", (0.95, 0.10, 0.38, 1.0),
                   rng.uniform(6.0, 12.0))
    neon_strip("NeonSign", x, 10.0, z + 19.6, 54.0, "x",
               (0.95, 0.10, 0.38, 1.0), rng.uniform(8.0, 16.0))
    box(SET_ID, "TheLastNoteGlow", (x, 12.0, z + 25.0), (1.0, 1.0, 1.0),
        (0.95, 0.10, 0.38, 1.0), metallic=0.0, roughness=0.4,
        emission=(0.95, 0.10, 0.38, 1.0), emission_intensity=0.0,
        extra=light_extra((0.95, 0.10, 0.38, 1.0),
                          rng.uniform(30.0, 50.0), 140.0))
    SET_ID += 1
    # The gate into the Crucible on the east road.
    gx = CRUCIBLE[1]
    gz = CRUCIBLE_CENTRE[1]
    box(SET_ID, "CrucibleGate", (gx, 11.0, gz - 46.0), (4.0, 22.0, 4.0),
        (0.140, 0.122, 0.098, 1.0), metallic=0.5, roughness=0.8)
    SET_ID += 1
    box(SET_ID, "CrucibleGate", (gx, 11.0, gz + 46.0), (4.0, 22.0, 4.0),
        (0.140, 0.122, 0.098, 1.0), metallic=0.5, roughness=0.8)
    SET_ID += 1
    box(SET_ID, "CrucibleGateBeam", (gx, 23.0, gz), (3.0, 3.0, 96.0),
        (0.160, 0.134, 0.104, 1.0), metallic=0.5, roughness=0.8)
    SET_ID += 1
    neon_strip("NeonSign", gx + 2.2, 19.0, gz, 60.0, "z",
               (0.92, 0.24, 0.08, 1.0), rng.uniform(4.0, 9.0))


def set_piece_spawn_plaza():
    """The spawn plaza in the Lower Zones, with the corp monument."""
    global SET_ID
    x, z = 0.0, 1250.0
    box(SET_ID, "PlazaPad", (x, 0.12, z), (90.0, 0.24, 70.0),
        (0.185, 0.178, 0.170, 1.0), metallic=0.15, roughness=0.85)
    SET_ID += 1
    box(SET_ID, "Monument", (x, 12.0, z - 18.0), (6.0, 24.0, 6.0),
        (0.100, 0.110, 0.130, 1.0), metallic=0.85, roughness=0.25,
        emission=(0.10, 0.45, 0.72, 1.0), emission_intensity=1.4)
    SET_ID += 1
    box(SET_ID, "MonumentCap", (x, 25.0, z - 18.0), (9.0, 2.5, 9.0),
        (0.100, 0.110, 0.130, 1.0), metallic=0.85, roughness=0.25,
        emission=(0.10, 0.45, 0.72, 1.0), emission_intensity=2.6)
    SET_ID += 1
    # Bollards around the showcase lot.
    for sx in (-1.0, 1.0):
        for sz in (-1.0, 1.0):
            box(SET_ID, "Bollard",
                (360.0 + sx * 66.0, 0.75, 1095.0 + sz * 26.0),
                (0.6, 1.5, 0.6), (0.180, 0.172, 0.164, 1.0),
                metallic=0.6, roughness=0.5)
            SET_ID += 1


def set_piece_waterfront():
    """Seawall, piers, cranes and the bay itself."""
    global SET_ID
    half = GROUND_HALF
    # Seawall: fills the cliff between the city slab and the bay floor.
    box(SET_ID, "Seawall", (BAY_X, -2.25, 0.0), (2.0, 9.5, 2.0 * half),
        (0.185, 0.178, 0.168, 1.0), metallic=0.1, roughness=0.9)
    SET_ID += 1
    box(SET_ID, "BayFloor", (BAY_X + 900.0, -7.5, 0.0),
        (1800.0, 1.0, 2.0 * half),
        (0.055, 0.058, 0.062, 1.0), metallic=0.05, roughness=0.95)
    SET_ID += 1
    # Promenade along the top of the seawall.
    box(SET_ID, "Promenade", (BAY_X - 8.0, 0.10, 0.0), (16.0, 0.20, 2.0 * half),
        (0.215, 0.206, 0.196, 1.0), metallic=0.1, roughness=0.88)
    SET_ID += 1
    # Piers reaching into the bay.
    for z in (-2700.0, -900.0, 900.0, 2700.0):
        box(SET_ID, "Pier", (BAY_X + 40.0, -0.6, z), (80.0, 1.2, 14.0),
            (0.170, 0.160, 0.148, 1.0), metallic=0.15, roughness=0.85)
        SET_ID += 1
        for dx in (10.0, 40.0, 70.0):
            cylinder(SET_ID, "PierPile", (BAY_X + dx, -4.0, z + 6.0), 0.6, 8.0,
                     (0.120, 0.112, 0.104, 1.0), metallic=0.4, roughness=0.8,
                     segments=8)
            SET_ID += 1
            cylinder(SET_ID, "PierPile", (BAY_X + dx, -4.0, z - 6.0), 0.6, 8.0,
                     (0.120, 0.112, 0.104, 1.0), metallic=0.4, roughness=0.8,
                     segments=8)
            SET_ID += 1
    # Container cranes.
    for z in (-1800.0, 0.0, 1800.0):
        box(SET_ID, "CraneMast", (BAY_X + 30.0, 22.0, z), (3.0, 44.0, 3.0),
            (0.196, 0.152, 0.108, 1.0), metallic=0.6, roughness=0.6)
        SET_ID += 1
        box(SET_ID, "CraneJib", (BAY_X + 75.0, 42.0, z), (90.0, 2.5, 2.5),
            (0.196, 0.152, 0.108, 1.0), metallic=0.6, roughness=0.6)
        SET_ID += 1
        box(SET_ID, "CraneCounter", (BAY_X - 12.0, 40.0, z), (14.0, 5.0, 4.0),
            (0.176, 0.136, 0.098, 1.0), metallic=0.6, roughness=0.6)
        SET_ID += 1
    # Warehouses on the quay.
    for z in (-3000.0, -3120.0, 3000.0, 3120.0):
        box(SET_ID, "QuayShed", (BAY_X - 60.0, 8.0, z), (60.0, 16.0, 40.0),
            pick(INDUSTRIAL), metallic=0.6, roughness=0.65)
        SET_ID += 1


def generate_hill_terraces() -> None:
    """Terraced ground in the hill district, on the same 100 m grid the
    buildings use, so nothing floats."""
    global SET_ID
    x0, x1, z0, z1 = HILLS
    ix = 0
    while x0 + ix * HILL_CELL < x1:
        iz = 0
        while z0 + iz * HILL_CELL < z1:
            cx = x0 + (ix + 0.5) * HILL_CELL
            cz = z0 + (iz + 0.5) * HILL_CELL
            e = hill_elev(cx, cz)
            if e > 0.0:
                box(SET_ID, "Terrace", (cx, e / 2.0 - 1.0, cz),
                    (HILL_CELL, e + 1.0, HILL_CELL),
                    (0.150, 0.142, 0.132, 1.0), metallic=0.05,
                    roughness=0.92)
                SET_ID += 1
            iz += 1
        ix += 1


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

FIXTURE_TAGS = {
    "Sun", "Ground", "Player", "Enemy1", "Enemy2", "Enemy3",
    "HealthPotion", "AmmoBox", "SwordPickup", "GoldBar", "Bandage",
    "GameManager",
}


def extract_fixtures(text: str) -> dict[str, str]:
    """Pull the fixture entities out of the existing scene, verbatim.

    The showcase row's coordinates are asserted by
    tests/headless/test_avb_demogame_scene.py, and every fixture's presence is
    what test_feature_coverage.py measures, so these must not be regenerated.
    """
    out: dict[str, str] = {}
    for block in re.split(r"(?m)^  - Entity: ", text)[1:]:
        m = re.search(r"(?m)^      Tag: (.+?)\s*$", block)
        if not m:
            continue
        tag = m.group(1)
        if tag in FIXTURE_TAGS or tag.startswith("Showcase"):
            body = "  - Entity: " + block
            if not body.endswith("\n"):
                body += "\n"
            out[tag] = body
    return out


def patch_ground(block: str) -> str:
    """Split the ground slab so the east edge can drop into the bay.

    DERIVED from GROUND_HALF and BAY_X, never written as literals. The first
    pass hardcoded the numbers for a 2.6 km city, and when the city was scaled
    to 7.2 km the slab was not: three quarters of the district stood on
    nothing, and the bay stayed where it used to be. Anything that describes
    the world extent has to come from the same constant the world does.

    Land spans x in [-GROUND_HALF, BAY_X], z in [-GROUND_HALF, GROUND_HALF];
    a separate BayFloor slab carries the seabed east of the seawall.
    """
    x0, x1 = -GROUND_HALF, BAY_X
    block = set_field(block, "TransformComponent", "Translation",
                      vec([(x0 + x1) / 2.0, -0.5, 0.0]))
    block = set_field(block, "TransformComponent", "Scale",
                      vec([x1 - x0, 1.0, 2.0 * GROUND_HALF]))
    return block


def patch_water(block: str) -> str:
    """Move the showcase water plane from 'covers the whole world' to the bay.

    Also derived: the surface spans x in [BAY_X, GROUND_HALF], z across the
    full ground slab, and sits 1.5 m below the land so it cannot z-fight the
    ground top at y=0.
    """
    x0, x1 = BAY_X, GROUND_HALF
    block = set_field(block, "TransformComponent", "Translation",
                      vec([(x0 + x1) / 2.0, -1.5, 0.0]))
    block = set_field(block, "TransformComponent", "Scale",
                      vec([x1 - x0, 1.0, 2.0 * GROUND_HALF]))
    return block


def set_field(block: str, component: str, field: str, value: str,
              indent: int = 6) -> str:
    """Set one field inside one component block. Idempotent.

    This generator READS the file it is about to rewrite, so any edit expressed
    as "replace the old value with the new one" is not idempotent: the second
    run finds nothing to replace and the field silently keeps whatever it had.
    That is exactly how the sky ended up with the first-pass cloud values while
    the script claimed the second-pass ones. Matching the field by name instead
    of by its previous value makes re-runs converge.
    """
    lines = block.split("\n")
    out = []
    in_comp = False
    done = False
    for ln in lines:
        # A component header is the only 4-space-indented line ending in ':'.
        if ln.startswith("    ") and not ln.startswith("      ") \
                and ln.rstrip().endswith(":"):
            in_comp = ln.strip()[:-1] == component
        pad = " " * indent
        if in_comp and not done and ln.startswith(pad + field + ":"):
            out.append(pad + field + ": " + value)
            done = True
            continue
        out.append(ln)
    if not done:
        raise AssertionError(
            f"{component}.{field} not found -- fixture shape changed")
    return "\n".join(out)


def patch_atmosphere(sun: str, fog: str, clouds: str):
    """Blood-red sky: the VolumetricClouds and VolumetricFog volumes carry it.

    These three entities are fixtures, so they are edited in place rather than
    regenerated -- the coverage test only needs them present, and their values
    are what makes the Ash District read as the Ash District.

    Tuned for legibility as much as for mood. The first pass at this used
    cloud coverage 0.88 / density 0.0042 over a 2000 m volume, which is
    correctly apocalyptic and completely opaque: a camera anywhere near the
    cloud layer (y 500-2500) renders a flat frame. These values keep the
    permanent overcast and still let the blockout be read.
    """
    sun = set_field(sun, "DirectionalLightComponent", "Color",
                    "[0.85, 0.32, 0.17, 1]")
    sun = set_field(sun, "DirectionalLightComponent", "Intensity", "1.35")

    fog = set_field(fog, "VolumetricFogComponent", "Color",
                    "[0.34, 0.17, 0.14, 1]")
    fog = set_field(fog, "VolumetricFogComponent", "Density", "0.0018")
    fog = set_field(fog, "VolumetricFogComponent", "Anisotropy", "0.55")
    fog = set_field(fog, "VolumetricFogComponent", "HeightFalloff", "0.02")
    fog = set_field(fog, "VolumetricFogComponent", "NoiseStrength", "0.65")
    fog = set_field(fog, "VolumetricFogComponent", "NoiseScale", "0.28")
    fog = set_field(fog, "VolumetricFogComponent", "WindSpeed", "1.6")

    clouds = set_field(clouds, "VolumetricCloudsComponent", "Color",
                       "[0.48, 0.09, 0.065, 1]")
    clouds = set_field(clouds, "VolumetricCloudsComponent", "AmbientTint",
                       "[0.24, 0.05, 0.04, 1]")
    clouds = set_field(clouds, "VolumetricCloudsComponent", "Coverage", "0.72")
    clouds = set_field(clouds, "VolumetricCloudsComponent", "Density", "0.0022")
    clouds = set_field(clouds, "VolumetricCloudsComponent", "SilverLining",
                       "0.10")
    clouds = set_field(clouds, "VolumetricCloudsComponent", "ShadowStrength",
                       "0.65")
    clouds = set_field(clouds, "VolumetricCloudsComponent", "WindDirection",
                       "[1, 0.25]")
    clouds = set_field(clouds, "VolumetricCloudsComponent", "WindSpeed", "2.2")
    return sun, fog, clouds


# ---------------------------------------------------------------------------
# Assembly
# ---------------------------------------------------------------------------


def build() -> str:
    with open(SCENE_PATH, encoding="utf-8") as f:
        old = f.read()

    fixtures = extract_fixtures(old)
    missing = [t for t in FIXTURE_TAGS if t not in fixtures]
    if missing:
        raise SystemExit(f"fixtures missing from the shipped scene: {missing}")
    sun, fog, clouds = patch_atmosphere(fixtures["Sun"],
                                        fixtures["ShowcaseFogVolume"],
                                        fixtures["ShowcaseCloudVolume"])
    # The scene camera's far plane has to clear the world diagonal. The shipped
    # 10000 was fine for a 2.6 km city; a 7.2 km one has a 10.2 km diagonal, so
    # standing in one corner and looking at the other clipped the far districts.
    for tag, block in fixtures.items():
        # Not every fixture is a camera -- GameManager carries none -- so the
        # guard has to be the component's presence, not the tag's name.
        if "CameraComponent" not in block:
            continue
        # PerspectiveFar is nested one level deeper, under
        # CameraComponent.Camera, so it needs the 8-space indent.
        fixtures[tag] = set_field(block, "CameraComponent",
                                  "PerspectiveFar", "20000", indent=8)

    # Derived AFTER the in-place patches above. Taking these snapshots before
    # the camera far-plane fix meant the write path used the un-patched copies
    # and the scene silently kept the old value.
    showcase = {t: b for t, b in fixtures.items() if t.startswith("Showcase")}
    core_fixtures = {t: b for t, b in fixtures.items()
                     if t not in showcase and t != "Ground"}

    _ENTITIES.clear()
    _USED_IDS.clear()
    _COUNTERS.clear()
    global BID, STREET_ID, LIGHT_ID, PROP_ID, SET_ID
    BID = 9600000000000000101
    STREET_ID = 96000000000000020001
    LIGHT_ID = 98000000000000006001
    PROP_ID = 96000000000000010001
    SET_ID = 96000000000000030001

    # Register the fixture ids so the allocator cannot collide with them.
    for block in fixtures.values():
        m = re.match(r"  - Entity: (\d+)", block)
        if m:
            _USED_IDS.add(int(m.group(1)))

    generate_streets()
    generate_hill_terraces()
    generate_city()
    generate_streetlights()
    generate_props()
    set_piece_spire()
    set_piece_last_note()
    set_piece_spawn_plaza()
    set_piece_waterfront()
    generate_crucible_city()
    crucible_structures()
    set_piece_crucible_wall()
    set_piece_core_cordon()
    set_piece_arcology()
    set_piece_collapsed_arcology()
    set_piece_funicular()
    set_piece_beacon()
    set_piece_processional()
    set_piece_smokestacks()

    out = ["Scene: Demo Game\n", "Entities:\n"]
    out.append(sun)
    out.append(patch_ground(fixtures["Ground"]))
    out.extend(_ENTITIES)
    out.append(fog)
    out.append(clouds)
    out.append(patch_water(showcase["ShowcaseWater"]))
    # Fog and clouds are already appended above, patched; water is patched too.
    # Emitting them again would duplicate their ids.
    out.extend(showcase[t] for t in sorted(showcase)
               if t not in ("ShowcaseWater", "ShowcaseFogVolume",
                            "ShowcaseCloudVolume"))
    out.extend(core_fixtures[t] for t in
               ["Player", "Enemy1", "Enemy2", "Enemy3", "HealthPotion",
                "AmmoBox", "SwordPickup", "GoldBar", "Bandage", "GameManager"]
               if t in core_fixtures)
    return "".join(b if b.endswith("\n") else b + "\n" for b in out)


# ---------------------------------------------------------------------------
# Self-check
# ---------------------------------------------------------------------------

# Tags whose geometry must rest on a base plane. Anything here whose lowest
# face ends up above its base is a floating object, and "floating black cubes
# dotted around" is exactly what a bad base offset looks like from the street.
# Deliberately NOT here: Shack, Ruin and Silhouette. Those are emitted as
# STACKS, so every segment above the first is legitimately off the ground and
# this check cannot tell a stacked segment from a floating one. Their
# contiguity is asserted where the stack is built instead.
GROUNDED_TAGS = {
    "Crater": 0.0, "Building": 0.0, "HabBlock": 0.0,
    "Warehouse": 0.0, "Tank": 0.0, "Stack": 0.0,
    "Barrel": 0.0, "WreckWheel": 0.0, "Barricade": 0.0,
    "Watchtower": 0.0, "Bollard": 0.0, "Rubble": 0.0,
    # NOT here: RowHouse. It goes through slab_building, so it is a tiered stack
    # and every tier above the first is legitimately off the terrace; the flat
    # check cannot tell a stacked tier from a floating one. slab_building asserts
    # the stack reaches exactly the requested height instead.
}

_SCALE_RE = re.compile(r"(?m)^      Scale: \[([^\]]+)\]$")
_POS_RE = re.compile(r"(?m)^      Translation: \[([^\]]+)\]$")


def _vec3(s):
    return [float(x) for x in s.split(",")]


def check_grounded(text: str, tolerance: float = 0.05) -> list[str]:
    """Report grounded entities whose lowest face is above their base plane.

    Returns a list of human-readable findings; empty means clean. Deliberately
    checks only tags that are supposed to touch the ground -- balconies,
    signage, walkways, crowns, antennas and lamps are meant to be in the air.
    """
    bad = []
    for block in re.split(r"(?m)^  - Entity: ", text)[1:]:
        m = re.search(r"(?m)^      Tag: (.+?)\s*$", block)
        if not m:
            continue
        tag = m.group(1)
        if tag not in GROUNDED_TAGS:
            continue
        eid = re.match(r"(\d+)", block.strip())
        if eid and int(eid.group(1)) in _STACKED_TIERS:
            continue
        pos = _POS_RE.search(block)
        scale = _SCALE_RE.search(block)
        if not pos or not scale:
            continue
        px, y, pz = _vec3(pos.group(1))
        # A cylinder's vertical extent comes from its Height field, NOT from
        # TransformComponent.Scale (which stays 1,1,1). Reading Scale here
        # reported every tank and smokestack as floating when they were
        # sitting exactly on the ground.
        sx, sy, sz = _vec3(scale.group(1))
        cyl = re.search(r"(?m)^      Height: ([0-9.eE+-]+)$", block)
        if cyl and "CylinderRendererComponent" in block:
            sy = float(cyl.group(1))
        # ROTATION-AWARE. Scale is the LOCAL extent; the world AABB of a rotated
        # box is wider, so bottom = y - sy/2 is only correct for an unrotated
        # box. Rubble is emitted tilted on purpose (a slab resting on an edge
        # reads as wreckage, one sitting flat reads as a paving stone), and
        # computing the bottom from the unrotated scale reported all of it as
        # floating. The half-extent of the rotated box is sum(|R_ij| * s_j / 2).
        rot = re.search(r"(?m)^      Rotation: \[([^\]]+)\]$", block)
        if rot:
            import math
            rx, ry, rz = [math.radians(float(v)) for v in _vec3(rot.group(1))]
            cx, sxn, cz = math.cos(rx), math.sin(rx), math.cos(rz)
            szn, cy, syn = math.sin(rz), math.cos(ry), math.sin(ry)
            # glm::quat(vec3(pitch,yaw,roll)) is Rz * Ry * Rx.
            r00 = cz * cy
            r01 = cz * syn * sxn - szn * cx
            r02 = cz * syn * cx + szn * sxn
            r10 = szn * cy
            r11 = szn * syn * sxn + cz * cx
            r12 = szn * syn * cx - cz * sxn
            r20 = -syn
            r21 = cy * sxn
            r22 = cy * cx
            half_y = (abs(r10) * sx + abs(r11) * sy + abs(r12) * sz) / 2.0
        else:
            half_y = sy / 2.0
        bottom = y - half_y
        base = GROUNDED_TAGS[tag]
        if base is None:
            base = hill_base_y(px, pz)
        if bottom > base + tolerance:
            bad.append(f"{tag}: bottom {bottom:.2f} floats above base {base:.2f}")
    return bad


def main() -> int:
    text = build()
    tmp = SCENE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, SCENE_PATH)

    floats = check_grounded(text)
    if floats:
        print(f"FLOATING GEOMETRY: {len(floats)} grounded entities above their base")
        for line in floats[:15]:
            print("  " + line)

    ids = re.findall(r"(?m)^  - Entity: (\d+)$", text)
    tags = re.findall(r"(?m)^      Tag: (.+?)\s*$", text)
    from collections import Counter
    counts = Counter(tags)
    print(f"wrote {SCENE_PATH}")
    print(f"entities: {len(ids)}  (unique ids: {len(set(ids))})")
    print(f"size: {len(text.encode('utf-8')) / 1e6:.2f} MB, {text.count(chr(10))} lines")
    for tag, n in counts.most_common():
        print(f"  {n:5d}  {tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
