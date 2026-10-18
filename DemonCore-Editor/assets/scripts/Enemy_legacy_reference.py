# Wasteland Engine Script — Enemy
# Minimal chaser: finds the nearest entity with a HealthComponent (the
# player), walks toward it, and applies contact damage on its attack timer.
#
# Requirements on the entity this is attached to:
#   - HealthComponent      (so weapons can damage it)
#   - RigidBody3DComponent (Kinematic, so the script moves it directly)
#   - BoxCollider3DComponent or SphereCollider3DComponent
#
# Movement is kinematic: the script sets the transform and Box3D carries the
# body along, so the enemy collides with the level rather than floating
# through it. It does not path around obstacles -- use a navmesh/waypoint
# layer for that (see the roadmap); this is the "walk straight at you" tier.
import math

import Wasteland as WastelandMod


class Enemy:
    def __init__(self, entity):
        self.entity = entity

        # Movement
        self.speed = 3.0
        self.turnSpeed = 6.0       # radians per second of facing rotation
        self.attackRange = 1.6     # distance at which contact damage lands
        self.loseRange = 30.0      # beyond this, go idle instead of chasing

        # Damage
        self.damage = 12.0
        self.attackInterval = 1.0  # seconds between hits
        self.attackTimer = 0.0

        # How often to look for a target. Scanning every entity every frame
        # is wasted work when the roster is static, so cache and refresh.
        self.retargetInterval = 0.5
        self.retargetTimer = 0.0
        self.target = None

        # Ground probe: same downward-raycast trick Player.py uses, so the
        # enemy follows floors and steps instead of hovering or sinking.
        self.footOffset = 1.0
        self.groundProbeDistance = 0.4
        self.groundSnapTolerance = 0.2

        # Facing state, radians. Kept across frames so rotation is smooth
        # rather than snapping to the target's bearing every tick.
        self.yaw = 0.0

        self.isDead = False

    # ------------------------------------------------------------------ utils

    def FindTarget(self):
        """Nearest live entity that carries a HealthComponent, minus self."""
        best = None
        bestDistSq = self.loseRange * self.loseRange

        me = self.entity.GetTransform()
        if me is None:
            return None
        myPos = me.Translation

        for other in WastelandMod.Scene.GetEntities():
            if not other.IsValid() or other == self.entity:
                continue
            health = other.TryGetHealthComponent()
            if health is None:
                continue
            if health.IsDead:
                continue

            otherT = other.GetTransform()
            if otherT is None:
                continue

            dx = otherT.Translation.x - myPos.x
            dy = otherT.Translation.y - myPos.y
            dz = otherT.Translation.z - myPos.z
            distSq = dx * dx + dy * dy + dz * dz
            if distSq < bestDistSq:
                bestDistSq = distSq
                best = other

        return best

    def GroundHeightAt(self, x, y, z):
        """World-space ground height under a point, or None if no surface.

        Probes from a little above the requested foot level so a body resting
        exactly on a floor still generates an intersect.
        """
        scene = WastelandMod.Scene.GetActiveScene()
        if scene is None:
            return None
        origin = WastelandMod.Vec3(x, y - self.footOffset + self.groundSnapTolerance, z)
        hit, ent, point, normal, dist, hittable = scene.Raycast(
            origin, WastelandMod.Vec3(0.0, -1.0, 0.0), self.groundProbeDistance)
        if not hit:
            return None
        return point.y

    # ------------------------------------------------------------------ tick

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return

        seconds = dt.GetSeconds()
        if seconds <= 0.0:
            return

        health = self.entity.TryGetHealthComponent()
        if health is None:
            return
        if health.IsDead:
            if not self.isDead:
                self.isDead = True
                self.target = None
            return

        transform = self.entity.GetTransform()
        if transform is None:
            return

        # --- Target acquisition ---
        self.retargetTimer -= seconds
        if self.target is None or self.retargetTimer <= 0.0:
            self.target = self.FindTarget()
            self.retargetTimer = self.retargetInterval

        if self.target is not None and not self.target.IsValid():
            self.target = None
        if self.target is not None:
            th = self.target.TryGetHealthComponent()
            if th is not None and th.IsDead:
                self.target = None

        if self.target is None:
            return

        targetT = self.target.GetTransform()
        if targetT is None:
            self.target = None
            return

        pos = transform.Translation
        tgt = targetT.Translation

        dx = tgt.x - pos.x
        dz = tgt.z - pos.z
        horizontal = math.sqrt(dx * dx + dz * dz)

        # --- Move toward the target ---
        # Stop at attack range so the enemy does not grind into the player.
        if horizontal > self.attackRange:
            inv = 1.0 / horizontal if horizontal > 1e-6 else 0.0
            step = self.speed * seconds
            newX = pos.x + dx * inv * step
            newZ = pos.z + dz * inv * step

            # Follow the ground under the new position so the enemy tracks
            # slopes and steps rather than drifting off them.
            ground = self.GroundHeightAt(newX, pos.y, newZ)
            newY = pos.y
            if ground is not None:
                newY = ground + self.footOffset

            transform.Translation = WastelandMod.Vec3(newX, newY, newZ)

        # --- Face the target, smoothly ---
        # Angle to target in the XZ plane. atan2(dz, dx) with the engine's
        # convention (forward = local -Z at yaw 0) needs the +90 degree
        # offset applied here.
        if horizontal > 1e-4:
            desired = math.atan2(dx, dz) - math.pi * 0.5
            # Shortest-arc interpolation, so wrapping past +/-pi does not
            # spin the enemy the long way round.
            delta = (desired - self.yaw + math.pi) % (2.0 * math.pi) - math.pi
            maxTurn = self.turnSpeed * seconds
            if delta > maxTurn:
                delta = maxTurn
            elif delta < -maxTurn:
                delta = -maxTurn
            self.yaw += delta

        transform.Rotation = WastelandMod.Vec3(0.0, self.yaw, 0.0)
        self.entity.SetTransform(transform.Translation, transform.Rotation, transform.Scale)

        # --- Contact damage ---
        self.attackTimer -= seconds
        if horizontal <= self.attackRange and self.attackTimer <= 0.0:
            self.attackTimer = self.attackInterval
            victim = self.target.TryGetHealthComponent()
            if victim is not None:
                # Reset the cooldown explicitly: ApplyDamage only sets one
                # if DamageCooldown is non-zero, and a target configured with
                # 0 would otherwise be damaged every single frame.
                victim.DamageCooldown = max(victim.DamageCooldown, 0.1)
                victim.ApplyDamage(self.damage)
