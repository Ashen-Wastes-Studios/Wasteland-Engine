"""Generate assets/scenes/ExampleGame.wastescene -- the FPS blockout level.

A hand-authored-by-script arena so the layout is reproducible and reviewable
as code rather than as opaque YAML. Run from the repo root:

    python tools/make_example_game_scene.py

Design: an open courtyard roughly 44x44 with a central raised platform, four
cover blocks the player can fight around, a perimeter wall to keep the player
in, and three enemies that will converge on the player on play.

Geometry conventions (match Player.py's capsule):
  - Floor top surface is y=0, so the player entity sits at y=1.0.
  - Walls are boxes with a static RigidBody3DComponent and a
    BoxCollider3DComponent whose HalfExtents match their visual scale/2, so
    what you see is what you collide with.

Every solid is a Static body: blockout geometry should never move, and a
static body is also what the Raycast/MoveCapsule queries are cheapest
against.
"""
import os

# Entity ids are opaque 64-bit handles. These are fixed constants so
# regenerating the file produces a byte-identical result and git diffs stay
# readable instead of churning on every run.
IDS = {
    "DirectionalLight": 1100000000000000001,
    "Floor": 1100000000000000002,
    "PerimeterNorth": 1100000000000000003,
    "PerimeterSouth": 1100000000000000004,
    "PerimeterEast": 1100000000000000005,
    "PerimeterWest": 1100000000000000006,
    "CentralPlatform": 1100000000000000007,
    # Two steps per ramp side, so four distinct ids rather than deriving them
    # by arithmetic (which previously collided with the CoverA id).
    "PlatformStepN1": 1100000000000000008,
    "PlatformStepN2": 1100000000000000021,
    "PlatformStepS1": 1100000000000000009,
    "PlatformStepS2": 1100000000000000022,
    "CoverA": 1100000000000000010,
    "CoverB": 1100000000000000011,
    "CoverC": 1100000000000000012,
    "CoverD": 1100000000000000013,
    "PillarA": 1100000000000000014,
    "PillarB": 1100000000000000015,
    "CrateInteractable": 1100000000000000016,
    "Player": 1100000000000000017,
    "EnemyA": 1100000000000000018,
    "EnemyB": 1100000000000000019,
    "EnemyC": 1100000000000000020,
}

# Half-extents of the playable area. The perimeter walls sit on these bounds
# so the player is contained without needing invisible blockers.
HALF = 22.0
WALL_H = 4.0
WALL_T = 1.0


def solid(entity_id, tag, translation, half_extents, albedo=(0.45, 0.47, 0.5, 1.0),
          roughness=0.85, metallic=0.0):
    """A collidable, visible box.

    `translation` is the box center; `half_extents` is half its size, which is
    what both the visual Scale and the BoxCollider3D HalfExtents need. Using
    one source for both is what guarantees the collision matches the visuals.
    """
    sx = half_extents[0] * 2.0
    sy = half_extents[1] * 2.0
    sz = half_extents[2] * 2.0
    return f"""  - Entity: {entity_id}
    TagComponent:
      Tag: {tag}
    TransformComponent:
      Translation: [{translation[0]}, {translation[1]}, {translation[2]}]
      Rotation: [0, 0, 0]
      Scale: [{sx}, {sy}, {sz}]
    CubeRendererComponent:
      Color: [{albedo[0]}, {albedo[1]}, {albedo[2]}, {albedo[3]}]
      TextureWidth: 0
      TextureHeight: 0
      TextureIndex: 0
      TilingFactor: 1
    MaterialComponent:
      Albedo: [{albedo[0]}, {albedo[1]}, {albedo[2]}, {albedo[3]}]
      TextureWidth: 0
      TextureHeight: 0
      TextureIndex: 0
      TexturePath: ""
      HasGeneratedMaps: false
      NormalStrength: 1
      Metallic: {metallic}
      Roughness: {roughness}
      EmissionColor: [0, 0, 0, 1]
      EmissionIntensity: 0
      DisplacementScale: 0
    RigidBody3DComponent:
      BodyType: Static
      GravityScale: 1
      LinearDamping: 0
      AngularDamping: 0
      FixedRotationX: false
      FixedRotationY: false
      FixedRotationZ: false
    BoxCollider3DComponent:
      HalfExtents: [{half_extents[0]}, {half_extents[1]}, {half_extents[2]}]
      Offset: [0, 0, 0]
      Density: 1
      Friction: 0.5
      Restitution: 0
"""


def build():
    out = []
    out.append("Scene: ExampleGame\n")
    out.append("Entities:\n")

    # --- Lighting ---
    out.append(f"""  - Entity: {IDS['DirectionalLight']}
    TagComponent:
      Tag: Directional Light
    TransformComponent:
      Translation: [0, 10, 0]
      Rotation: [-0.488692194, 0.785398185, 0.0366519131]
      Scale: [1, 1, 1]
    DirectionalLightComponent:
      Color: [0.9, 0.92, 1, 1]
      Intensity: 2.0
""")

    floor_he = (HALF, 0.5, HALF)
    out.append(solid(IDS["Floor"], "Floor", (0.0, -0.5, 0.0), floor_he,
                     albedo=(0.3, 0.32, 0.35, 1.0), roughness=0.9))

    # --- Perimeter: four walls just outside the playable bounds ---
    out.append(solid(IDS["PerimeterNorth"], "Wall North",
                     (0.0, WALL_H / 2, -(HALF + WALL_T / 2)),
                     (HALF + WALL_T, WALL_H / 2, WALL_T / 2)))
    out.append(solid(IDS["PerimeterSouth"], "Wall South",
                     (0.0, WALL_H / 2, HALF + WALL_T / 2),
                     (HALF + WALL_T, WALL_H / 2, WALL_T / 2)))
    out.append(solid(IDS["PerimeterEast"], "Wall East",
                     (HALF + WALL_T / 2, WALL_H / 2, 0.0),
                     (WALL_T / 2, WALL_H / 2, HALF + WALL_T)))
    out.append(solid(IDS["PerimeterWest"], "Wall West",
                     (-(HALF + WALL_T / 2), WALL_H / 2, 0.0),
                     (WALL_T / 2, WALL_H / 2, HALF + WALL_T)))

    # --- Central raised platform with ramps on two sides ---
    # Ramps are stepped boxes: a staircase the player can actually walk up
    # under the current controller, which has no slope support.
    plat_h = 1.0
    plat = (5.0, plat_h / 2, 5.0)
    out.append(solid(IDS["CentralPlatform"], "Central Platform", (0.0, plat_h / 2, 0.0),
                     plat, albedo=(0.42, 0.44, 0.48, 1.0)))

    # Two steps per ramp side, each half the platform height and 1.0 deep,
    # so walking outward from the platform descends to the floor.
    step_h = plat_h / 2.0
    for key, zsign in (("N", -1.0), ("S", 1.0)):
        for i, z in enumerate((5.0, 6.0)):
            out.append(solid(
                IDS["PlatformStep{}1".format(key) if i == 0
                    else "PlatformStep{}2".format(key)],
                "Platform Step",
                (0.0, step_h / 2.0, zsign * z),
                (5.0, step_h / 2.0, 0.5),
                albedo=(0.4, 0.42, 0.45, 1.0)))

    # --- Cover blocks the player can fight around ---
    out.append(solid(IDS["CoverA"], "Cover A", (-9.0, 1.0, -9.0), (1.5, 1.0, 1.5),
                     albedo=(0.55, 0.35, 0.28, 1.0)))
    out.append(solid(IDS["CoverB"], "Cover B", (9.0, 1.0, -9.0), (1.5, 1.0, 1.5),
                     albedo=(0.55, 0.35, 0.28, 1.0)))
    out.append(solid(IDS["CoverC"], "Cover C", (-9.0, 1.0, 9.0), (1.5, 1.0, 1.5),
                     albedo=(0.55, 0.35, 0.28, 1.0)))
    out.append(solid(IDS["CoverD"], "Cover D", (9.0, 1.0, 9.0), (1.5, 1.0, 1.5),
                     albedo=(0.55, 0.35, 0.28, 1.0)))

    # --- Pillars: tall, thin, break up the sightlines ---
    out.append(solid(IDS["PillarA"], "Pillar A", (-14.0, 2.0, 0.0), (0.6, 2.0, 0.6),
                     albedo=(0.5, 0.5, 0.55, 1.0)))
    out.append(solid(IDS["PillarB"], "Pillar B", (14.0, 2.0, 0.0), (0.6, 2.0, 0.6),
                     albedo=(0.5, 0.5, 0.55, 1.0)))

    # --- Interactable crate, to exercise the Interactor ---
    out.append(f"""  - Entity: {IDS['CrateInteractable']}
    TagComponent:
      Tag: Supply Crate
    TransformComponent:
      Translation: [0, 0.75, 12.0]
      Rotation: [0, 0, 0]
      Scale: [1.5, 1.5, 1.5]
    CubeRendererComponent:
      Color: [0.9, 0.75, 0.2, 1]
      TextureWidth: 0
      TextureHeight: 0
      TextureIndex: 0
      TilingFactor: 1
    MaterialComponent:
      Albedo: [0.9, 0.75, 0.2, 1]
      TextureWidth: 0
      TextureHeight: 0
      TextureIndex: 0
      TexturePath: ""
      HasGeneratedMaps: false
      NormalStrength: 1
      Metallic: 0
      Roughness: 0.6
      EmissionColor: [0, 0, 0, 1]
      EmissionIntensity: 0
      DisplacementScale: 0
    RigidBody3DComponent:
      BodyType: Static
      GravityScale: 1
      LinearDamping: 0
      AngularDamping: 0
      FixedRotationX: false
      FixedRotationY: false
      FixedRotationZ: false
    BoxCollider3DComponent:
      HalfExtents: [0.75, 0.75, 0.75]
      Offset: [0, 0, 0]
      Density: 1
      Friction: 0.5
      Restitution: 0
    InteractableComponent:
      Enabled: true
      Label: "[E] Open Supply Crate"
    ScriptComponent:
      ScriptPath: assets/scripts/Crate.py
      ScriptName: Crate
""")

    # --- Player: camera + scripts + health/weapon/ignore ---
    # y=1.0 puts the capsule's lower cap exactly on the floor (see
    # Player.py footOffset == colliderHalfHeight + colliderRadius == 1.0).
    out.append(f"""  - Entity: {IDS['Player']}
    TagComponent:
      Tag: Player
    TransformComponent:
      Translation: [0, 1, 16]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    CameraComponent:
      Camera:
        ProjectionType: 0
        PerspectiveFOV: 1.04719755
        PerspectiveNear: 0.05
        PerspectiveFar: 500
        OrthographicSize: 10
        OrthographicNear: -1
        OrthographicFar: 1
      Primary: true
      FixedAspectRatio: false
    HealthComponent:
      Current: 100
      Max: 100
      Invulnerable: false
      DamageCooldown: 0
      CooldownTimer: 0
      IsDead: false
    WeaponComponent:
      Damage: 25
      Range: 200
      AmmoPerShot: 1
      MaxAmmo: 30
      CurrentAmmo: 30
      ReserveAmmo: 120
      ReloadTime: 1.5
      AutoReload: true
      FireRate: 10
      FireCooldown: 0
      ReloadTimer: 0
      IsReloading: false
      ShotsFired: 0
      ShotsHit: 0
    RaycastIgnoreComponent:
      Enabled: true
    ViewStateComponent:
      TargetFOV: 1.04719755
      FOVLerpSpeed: 8
      IsSprinting: false
      IsAiming: false
    WeaponFeedbackComponent:
      MuzzleFlashTimer: 0
      HitMarkerTimer: 0
      LastShotHit: false
      LastShotKilled: false
      MuzzleFlashDuration: 0.05
      HitMarkerDuration: 0.25
      ReloadProgress: 0
      ReloadTotal: 0
    ScriptComponent:
      ScriptPath: assets/scripts/Player.py
      ScriptName: Player
""")

    # --- Enemies: health + kinematic body + Enemy.py ---
    # Visual scale is derived from the collider half-extents so the enemy you
    # see is the enemy you shoot. The collider is 0.8 wide and 1.8 tall, and
    # the entity origin sits at the capsule center, so the scale is applied
    # about that center.
    enemy_he = (0.4, 0.9, 0.4)
    for key, pos in (("EnemyA", (-12.0, 1.0, -14.0)),
                     ("EnemyB", (12.0, 1.0, -14.0)),
                     ("EnemyC", (0.0, 1.0, -16.0))):
        out.append(f"""  - Entity: {IDS[key]}
    TagComponent:
      Tag: {key.replace('Enemy', 'Enemy ')}
    TransformComponent:
      Translation: [{pos[0]}, {pos[1]}, {pos[2]}]
      Rotation: [0, 0, 0]
      Scale: [{enemy_he[0] * 2}, {enemy_he[1] * 2}, {enemy_he[2] * 2}]
    CubeRendererComponent:
      Color: [0.75, 0.15, 0.2, 1]
      TextureWidth: 0
      TextureHeight: 0
      TextureIndex: 0
      TilingFactor: 1
    MaterialComponent:
      Albedo: [0.75, 0.15, 0.2, 1]
      TextureWidth: 0
      TextureHeight: 0
      TextureIndex: 0
      TexturePath: ""
      HasGeneratedMaps: false
      NormalStrength: 1
      Metallic: 0.1
      Roughness: 0.7
      EmissionColor: [0, 0, 0, 1]
      EmissionIntensity: 0
      DisplacementScale: 0
    HealthComponent:
      Current: 60
      Max: 60
      Invulnerable: false
      DamageCooldown: 0
      CooldownTimer: 0
      IsDead: false
    RigidBody3DComponent:
      BodyType: Kinematic
      GravityScale: 1
      LinearDamping: 0
      AngularDamping: 0
      FixedRotationX: true
      FixedRotationY: false
      FixedRotationZ: true
    BoxCollider3DComponent:
      HalfExtents: [0.4, 0.9, 0.4]
      Offset: [0, 0, 0]
      Density: 1
      Friction: 0.5
      Restitution: 0
    ScriptComponent:
      ScriptPath: assets/scripts/Enemy.py
      ScriptName: Enemy
""")

    # Normalize every block to end with exactly one newline. A literal block
    # that ends without one concatenates with whatever is appended next, which
    # silently produces a file the engine's yaml-cpp reader cannot parse (the
    # header collapses onto the first entity).
    return "".join(part if part.endswith("\n") else part + "\n" for part in out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, "DemonCore-Editor", "assets", "scenes", "ExampleGame.wastescene")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(build())
    print("wrote", path)


if __name__ == "__main__":
    main()
