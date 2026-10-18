# Wasteland Engine Script — Weapon
# Hitscan weapon driven by WeaponComponent (C++ owns fire gating + reload).
#
# Attach to the same entity as the player camera. On trigger:
#   1. WeaponComponent.Fire() consumes a round if the weapon is ready
#   2. a Raycast from the camera along its view direction finds the target
#   3. a hit on something with HealthComponent applies Weapon.Damage
#
# The shooting entity needs a RaycastIgnoreComponent so the trace does not
# terminate on the shooter's own capsule at distance 0.
import Wasteland
import math

class Weapon:
    def __init__(self, entity):
        self.entity = entity

        # Recoil state. Applied to the camera basis, so the view kicks and
        # then settles back toward the pre-shot aim point.
        self.recoilPitch = 0.0   # current upward kick, degrees
        self.recoilRecovered = 0.0
        self.recoilPerShot = 0.55
        self.recoilRecoverySpeed = 6.0
        self.recoilMax = 6.0

        # Camera-forward eye offset in world units, so the trace does not
        # start inside geometry when the player is up against a wall.
        self.eyeOffset = 0.6

        # Edge-trigger state for reload, so holding R does not spam
        # StartReload (which is already guarded, but keeps intent explicit).
        self.wasReload = False

        # --- Gamepad ---
        # Matches Player.py's slot and threshold so both read the same pad
        # with the same pull point; a player switching from mouse to pad
        # should not find aim and fire disagreeing about how far the trigger
        # has to go.
        self.gamepadSlot = 0
        self.gamepadEnabled = True
        self.gamepadFireThreshold = 0.5

        # Last shot feedback for HUD/debug: world point of the last hit.
        self.lastHitPoint = None
        self.lastHitWasEntity = False

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return

        seconds = dt.GetSeconds()
        if seconds <= 0.0:
            return

        weapon = self.entity.TryGetWeaponComponent()
        if weapon is None:
            return

        # Feedback timers run every frame, whether or not a shot happened, so
        # the effects expire. Ticking here rather than only on fire is what
        # makes a single-frame flash actually last one frame instead of
        # sticking on until the next shot.
        feedback = self.entity.TryGetWeaponFeedbackComponent()
        if feedback is not None:
            feedback.Tick(seconds)

        # Tick first: this advances the fire cooldown and completes any
        # reload in progress, so a reload that finished this frame frees the
        # weapon before we test the trigger below.
        weapon.Tick(seconds)

        transform = self.entity.GetTransform()
        if transform is None:
            return

        # --- Trigger ---
        # LMB is WL_MOUSE_BUTTON_1 (the code is 0; the "_1" is the first
        # button, not a 1-based index into GLFW's 0..7 range).
        firing = Wasteland.IsMouseButtonPressed(Wasteland.WL_MOUSE_BUTTON_1)

        # Gamepad fire on the RIGHT trigger, above a threshold. This is a
        # LEVEL test, not edge-triggered: an analog trigger has no clean
        # "pressed" edge, and treating it as one would make a gradual pull
        # fire once at the threshold and then never again.
        #
        # The threshold matters as much as the level test. A resting trigger
        # reports a small non-zero value, so `> 0` would fire constantly.
        gamepadFiring = False
        if (self.gamepadEnabled
                and Wasteland.Gamepad.IsConnected(self.gamepadSlot)):
            try:
                rightTrigger = Wasteland.Gamepad.GetTrigger(
                    Wasteland.Gamepad.AXIS_RIGHT_TRIGGER, self.gamepadSlot)
                gamepadFiring = rightTrigger > self.gamepadFireThreshold
            except Exception:
                # A missing gamepad must not stop the weapon working; the
                # mouse path above is unaffected.
                gamepadFiring = False

        if (firing or gamepadFiring) and weapon.CanFire():
            if weapon.Fire():
                self.FireHitscan(weapon, transform)
                self.recoilPitch = min(self.recoilPitch + self.recoilPerShot, self.recoilMax)
                # The flash is driven off the confirmed shot, not the trigger,
                # so a held trigger during a cooldown does not strobe the
                # flash while the gun is not actually firing.
                if feedback is not None:
                    feedback.TriggerMuzzleFlash()

        # Reload progress for the HUD. Read from the weapon rather than
        # tracked here, so the HUD cannot disagree with the weapon about
        # whether a reload is running.
        if feedback is not None:
            if weapon.IsReloading:
                feedback.ReloadProgress = weapon.ReloadTimer
                feedback.ReloadTotal = weapon.ReloadTime
            else:
                feedback.ReloadProgress = 0.0
                feedback.ReloadTotal = 0.0

        # --- Recoil recovery ---
        # Ease the view back down toward the original aim rather than
        # snapping, which is what separates a kick from a permanent tilt.
        if self.recoilPitch > 0.0:
            recovered = self.recoilRecoverySpeed * seconds
            self.recoilPitch -= recovered
            if self.recoilPitch < 0.0:
                self.recoilPitch = 0.0

        # --- Reload (edge-triggered) ---
        reload = Wasteland.IsKeyPressed(Wasteland.WL_KEY_R)
        if reload and not self.wasReload and weapon.CanReload():
            weapon.StartReload()
        self.wasReload = reload

        # Write the recoil-modified pitch back. The player script owns yaw
        # and the base pitch; this only layers the kick on top, so both
        # scripts touching Rotation is safe as long as the player applies
        # its aim first (it does — it runs before this in the same frame
        # when the entity's script list is ordered Player, Weapon).
        rotation = transform.Rotation
        transform.Rotation.x = rotation.x + math.radians(self.recoilPitch)
        self.entity.SetTransform(transform.Translation, transform.Rotation, transform.Scale)

    def FireHitscan(self, weapon, transform):
        """Trace from the camera along its forward axis and apply damage."""
        # Camera forward is the basis's -Z (see Player.py for the same
        # derivation): the scene camera builds its view as
        # inverse(transform), so forward is local -Z and right is +X.
        pitch = transform.Rotation.x
        yaw = transform.Rotation.y
        forward = (
            -math.sin(yaw) * math.cos(pitch),
            -math.sin(pitch),
            -math.cos(yaw) * math.cos(pitch),
        )

        # Start slightly ahead of the muzzle so a player flush against a
        # wall does not trace from inside it.
        origin = (
            transform.Translation.x,
            transform.Translation.y,
            transform.Translation.z,
        )

        result = Wasteland.Scene.GetActiveScene()
        if result is None:
            return

        hit, hitEntity, point, normal, distance, hittable = result.Raycast(
            Wasteland.Vec3(*origin), Wasteland.Vec3(*forward), weapon.Range)

        if not hit:
            self.lastHitPoint = None
            self.lastHitWasEntity = False
            return

        self.lastHitPoint = point
        self.lastHitWasEntity = bool(hittable)

        # Only a hittable entity takes damage. A wall hit is still a hit (a
        # script can spawn an impact decal at `point`), it just is not a
        # target.
        if not hittable or not hitEntity.IsValid():
            return

        health = hitEntity.TryGetHealthComponent()
        if health is None:
            return

        applied = health.ApplyDamage(weapon.Damage)
        if applied > 0.0:
            # Brief invulnerability so a weapon whose trigger is held (or a
            # multi-projectile shot) cannot drain a target in one frame.
            if health.DamageCooldown <= 0.0:
                health.DamageCooldown = 0.05

            # Hit marker, only for damage that actually landed. Applying the
            # marker from the trace instead would flash on every wall the
            # player shoots, which trains them to ignore it.
            #
            # A kill is distinguished by health.IsDead, which ApplyDamage has
            # just set, so the HUD can style it differently without a second
            # lookup.
            feedback = self.entity.TryGetWeaponFeedbackComponent()
            if feedback is not None:
                feedback.TriggerHitMarker(health.IsDead)
