# Wasteland Engine Script — Player (rewritten for testing)
# Clean first-person controller: WASD movement, mouse look, jump, crouch/prone stances.
# Also owns the player's damage/death/respawn state via HealthComponent.
import Wasteland
import math

class Player:
    def __init__(self, entity):
        self.entity = entity

        # Where respawn puts the player. Captured from the entity's authored
        # position on the first frame, so spawning the player somewhere
        # specific in the editor is enough to define the spawn point.
        self.spawnPoint = None

        # Death / respawn. RespawnDelay is the seconds to wait after dying
        # before control returns; 0 respawns immediately on the next frame.
        self.respawnDelay = 2.0
        self.deadTimer = 0.0
        self.isDead = False

        # Movement
        self.speed = 8.0
        self.sprintMultiplier = 2.0
        # Aiming slows the player down so holding the right mouse is a real
        # trade rather than a free zoom. 0.5 is a common tactical-shooter
        # value: precise, but still mobile.
        self.aimSpeedMultiplier = 0.5

        # --- View feedback (FOV) ---
        # All angles in RADIANS, because SetPerspectiveVerticalFOV takes
        # radians (60 deg = 1.0472). Getting this wrong is the classic
        # "my FOV is a sliver" bug.
        self.baseFOV = math.radians(60.0)
        # Sprinting pulls the view out to convey speed. Kept modest: too much
        # reads as fisheye rather than momentum.
        self.sprintFOV = math.radians(70.0)
        # Aiming pulls it in, which is the visual "focus" cue.
        self.aimFOV = math.radians(45.0)
        # How fast the FOV chases its target, in FOV units per second. A
        # direct assignment would snap, which reads as a teleport rather than
        # a push-in.
        self.fovLerpSpeed = 8.0

        # Mouse look
        # Sensitivity is DEGREES PER PIXEL of mouse movement (0.15 ~= 6.7 px/deg,
        # a typical default; raise for faster flicks, lower for finer aim).
        self.mouseSensitivity = 0.15
        self.pitch = 0.0
        self.yaw = 0.0
        self.maxPitch = 89.0
        self.minPitch = -89.0
        # Spike guard only, in degrees per frame — NOT a turn-rate cap. Mouse delta
        # is already in pixels, so sensitivity*delta is frame-rate independent and
        # needs no per-frame limiting. The old value of 0.3 clamped every frame to
        # 0.3 deg, capping rotation at ~18 deg/sec at 60fps no matter how far the
        # mouse moved — that, not the sensitivity, is what made looking feel stuck.
        # This only rejects nonsense deltas (e.g. a cursor jump on window focus).
        self.maxTurnSpeed = 180.0

        # --- Gamepad ---
        # Which joystick slot to read. 0 is the first controller the OS
        # enumerates, the right default for a single-player game.
        self.gamepadSlot = 0
        # Master switch. An absent pad reports as disconnected, so this stays
        # on by default and simply does nothing when none is plugged in.
        self.gamepadEnabled = True
        # Right stick look speed in DEGREES PER SECOND at full deflection.
        # 180 deg/s is a deliberate starting point: fast enough to turn around
        # in a fight, slow enough to land a shot on a mid-sized target
        # without sub-pixel precision.
        self.gamepadLookSpeed = 180.0
        # Left stick movement passes through as an analog magnitude, so the
        # player can walk slowly by nudging the stick. World units per second
        # at full deflection.
        self.gamepadMoveSpeed = 8.0
        # Trigger threshold for firing, on the trigger's 0..1 scale. Not 0.0,
        # because a trigger resting slightly off zero would otherwise fire
        # constantly.
        self.gamepadFireThreshold = 0.5

        # Jump / gravity
        self.verticalVelocity = 0.0
        self.gravity = 20.0
        self.jumpForce = 8.0
        self.isGrounded = True
        self.groundLevel = 0.0
        # Height of the entity's origin above the ground surface. Matches
        # a capsule collider with radius 0.5 / height 2.0, so the capsule's
        # lower cap sits exactly on the floor instead of sinking into it.
        self.footOffset = 1.0
        # How far below the feet to probe for ground, and the smallest gap
        # that still counts as standing on a surface.
        #
        # groundProbeDistance must cover at least groundSnapTolerance, because
        # the probe starts that far ABOVE the feet (see OnUpdateEntity). A
        # shorter distance than the raise silently never reaches the floor for
        # a player resting exactly on a surface, so the player is treated as
        # airborne and starts falling through it. The +0.05 keeps a little
        # slack over the bare minimum.
        self.groundSnapTolerance = 0.15
        self.groundProbeDistance = 0.35

        # --- Collision ---
        # The swept body. radius is the capsule thickness; halfHeight is the
        # distance between the two hemisphere centers, so the full height is
        # 2 * halfHeight + 2 * radius. footOffset must equal halfHeight +
        # radius for the capsule's lower cap to rest exactly on the floor
        # instead of sinking into it or hovering above it.
        self.hasCollision = True
        self.colliderRadius = 0.4
        self.colliderHalfHeight = 0.6

        # Stances: 0=stand, 1=crouch, 2=prone
        self.stance = 0
        self.crouchSpeed = 0.5
        self.proneSpeed = 0.25

        # edge-trigger state
        self.wasCrouch = False
        self.wasProne = False
        self.wasSprint = False
        self.wasJump = False

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return

        t = self.entity.GetTransform()
        if t is None:
            return

        seconds = dt.GetSeconds()

        # Remember the authored position once, so respawn has a spawn point
        # even if the player has since walked (or fallen) somewhere else.
        if self.spawnPoint is None:
            self.spawnPoint = Wasteland.Vec3(t.Translation.x, t.Translation.y, t.Translation.z)

        # --- Death / respawn ---
        # Checked before any movement so a dead player does not keep walking
        # or shooting. Returns early until the respawn timer expires.
        health = self.entity.TryGetHealthComponent()
        if health is not None:
            if health.IsDead:
                if not self.isDead:
                    self.isDead = True
                    self.deadTimer = self.respawnDelay
                self.deadTimer -= seconds
                if self.deadTimer <= 0.0:
                    self.Respawn(health)
                return
            # Revived externally (a pickup, a script) while we thought we
            # were dead: drop the dead state so control resumes.
            if self.isDead:
                self.isDead = False
                self.deadTimer = 0.0

        # ==================================================================
        # Order matters here: horizontal movement FIRST, then the ground
        # probe against the resulting position.
        #
        # Probing before the move (and then snapping to that height) is what
        # let a player walk up a wall: the probe at the old position found
        # the floor, the player moved into the wall, and the rest-snap then
        # applied the floor height to the new position -- or, once the old
        # position was beside the wall, the wall's top height. Probing after
        # the move means the ground height always belongs to where the player
        # actually ended up this frame.
        # ==================================================================

        scene = Wasteland.Scene.GetActiveScene()

        # --- Stance (edge-triggered) ---
        crouch = Wasteland.IsKeyPressed(Wasteland.WL_KEY_C)
        prone = Wasteland.IsKeyPressed(Wasteland.WL_KEY_Z)

        if self.isGrounded:
            if crouch and not self.wasCrouch:
                self.stance = 0 if self.stance == 1 else (1 if self.stance != 2 else 2)
            if prone and not self.wasProne:
                self.stance = 0 if self.stance == 2 else 2

        self.wasCrouch = crouch
        self.wasProne = prone

        # --- Jump (edge-triggered, grounded + standing only) ---
        jump = Wasteland.IsKeyPressed(Wasteland.WL_KEY_SPACE)
        if jump and not self.wasJump and self.isGrounded and self.stance == 0:
            self.verticalVelocity = self.jumpForce
            self.isGrounded = False
        self.wasJump = jump

        # --- Mouse look ---
        # Guarded: an exception here used to abort OnUpdateEntity *before* SetTransform
        # ran, silently discarding the frame's movement every tick. Look must never be
        # able to take translation down with it.
        #
        # Applied BEFORE movement is computed below, so this frame's movement uses the
        # aim the player just gave the camera rather than last frame's.
        try:
            delta = Wasteland.GetMouseDelta()
            if delta is not None:
                dx = max(min(delta[0] * self.mouseSensitivity, self.maxTurnSpeed), -self.maxTurnSpeed)
                dy = max(min(delta[1] * self.mouseSensitivity, self.maxTurnSpeed), -self.maxTurnSpeed)
                self.yaw -= dx
                self.pitch -= dy
                self.pitch = max(min(self.pitch, self.maxPitch), self.minPitch)
        except Exception:
            pass

        # --- Gamepad look ---
        # The right stick drives aim when it is actually deflected. The
        # deadzone is applied by the engine's ApplyDeadzone, so a resting
        # stick contributes nothing and does not fight the mouse for control
        # of the view.
        #
        # Sensitivity is DEGREES PER SECOND, not per frame, so it has to be
        # scaled by elapsed time. Without that, a gamepad turns the camera
        # twice as fast at 120fps as at 60fps, which makes the sensitivity
        # setting meaningless.
        if self.gamepadEnabled and Wasteland.Gamepad.IsConnected(self.gamepadSlot):
            try:
                stickX = Wasteland.Gamepad.GetAxisDeadzone(
                    Wasteland.Gamepad.AXIS_RIGHT_X, self.gamepadSlot)
                stickY = Wasteland.Gamepad.GetAxisDeadzone(
                    Wasteland.Gamepad.AXIS_RIGHT_Y, self.gamepadSlot)

                if stickX != 0.0 or stickY != 0.0:
                    # GetAxisDeadzone preserves the raw stick direction, and
                    # GLFW reports up as positive, so invert Y to match the
                    # mouse convention used just above.
                    gdx = stickX * self.gamepadLookSpeed * seconds
                    gdy = -stickY * self.gamepadLookSpeed * seconds
                    # Same clamp as the mouse: a stick slam should not spin
                    # the camera through a full turn in one frame.
                    gdx = max(min(gdx, self.maxTurnSpeed), -self.maxTurnSpeed)
                    gdy = max(min(gdy, self.maxTurnSpeed), -self.maxTurnSpeed)
                    self.yaw -= gdx
                    self.pitch -= gdy
                    self.pitch = max(min(self.pitch, self.maxPitch), self.minPitch)
            except Exception:
                # A gamepad reading must never take the player script down;
                # losing aim control is recoverable, a crash is not.
                pass

        # --- Movement direction (derived from the camera's own transform) ---
        # Forward/right are read off the entity's rotation, so "forward" is always
        # exactly where the camera looks. The scene camera builds its view as
        # inverse(transform) (Renderer3D::BeginScene), so with a right-handed
        # basis forward is the local -Z axis and right is the local +X axis.
        #
        # self.yaw/self.pitch stay in DEGREES (clamped, mouse-friendly) and are
        # converted to radians here: TransformComponent feeds Rotation straight into
        # glm::quat(), and the editor round-trips it through
        # glm::radians/glm::degrees (SceneHierarchyPanel), so the stored value is
        # radians. Writing degrees made forward disagree with the camera.
        pitch_rad = math.radians(self.pitch)
        yaw_rad = math.radians(self.yaw)

        cy, sy = math.cos(yaw_rad), math.sin(yaw_rad)
        cp, sp = math.cos(pitch_rad), math.sin(pitch_rad)

        # forward = -Z of the camera basis; right = +X (kept level, no pitch).
        # A pitched camera still moves along the ground plane, so the vertical
        # component is dropped when movement is applied below.
        forward_x = -sy * cp
        forward_z = -cy * cp
        right_x = cy
        right_z = -sy

        move_x = 0.0
        move_z = 0.0

        if Wasteland.IsKeyPressed(Wasteland.WL_KEY_W):
            move_x += forward_x
            move_z += forward_z
        if Wasteland.IsKeyPressed(Wasteland.WL_KEY_S):
            move_x -= forward_x
            move_z -= forward_z
        if Wasteland.IsKeyPressed(Wasteland.WL_KEY_D):
            move_x += right_x
            move_z += right_z
        if Wasteland.IsKeyPressed(Wasteland.WL_KEY_A):
            move_x -= right_x
            move_z -= right_z

        # Gamepad left stick, added to the same vector the keyboard writes to.
        #
        # The stick is ANALOG: its magnitude is how far the player pushed, so
        # it must survive to the speed calculation as a 0..1 scale factor --
        # that is what lets a small nudge walk slowly instead of at full
        # speed.
        analogScale = 1.0
        stickScale = 0.0

        # The keyboard's own intent, measured BEFORE the stick is added --
        # after that point `move_x`/`move_z` no longer say anything about the
        # keyboard alone. The keyboard is binary, so any nonzero direction is
        # full intent.
        key_length = math.sqrt(move_x * move_x + move_z * move_z)
        keyScale = 1.0 if key_length > 0.001 else 0.0

        if self.gamepadEnabled and Wasteland.Gamepad.IsConnected(self.gamepadSlot):
            try:
                stickX, stickY = Wasteland.Gamepad.GetLeftStick(self.gamepadSlot)
                if stickX != 0.0 or stickY != 0.0:
                    # GetLeftStick returns Y as-is (up positive), and forward
                    # is -Z, so stickY maps onto forward directly.
                    move_x += stickX * right_x + stickY * forward_x
                    move_z += stickX * right_z + stickY * forward_z
                    stickScale = min(1.0, math.sqrt(
                        stickX * stickX + stickY * stickY))
            except Exception:
                pass

        # normalize
        #
        # Normalizing AFTER the stick is added means the keyboard and the
        # stick share one direction, so pushing them together steers rather
        # than doubling the velocity.
        #
        # The SPEED comes from max(keyboard, stick), never from the magnitude
        # of their sum. The two express the same wish in different ways, so
        # the stronger one wins. Summing them instead means holding W while
        # nudging the stick walks faster than either alone, which is the
        # classic "gamepad feels broken next to the keyboard" bug.
        length = math.sqrt(move_x * move_x + move_z * move_z)
        if length > 0.001:
            move_x /= length
            move_z /= length
        analogScale = min(1.0, max(keyScale, stickScale))

        # sprint (standing + grounded)
        speed = self.speed * seconds * analogScale
        sprint = Wasteland.IsKeyPressed(Wasteland.WL_KEY_LEFT_SHIFT)
        if sprint and not self.wasSprint and self.stance == 0 and self.isGrounded:
            speed *= self.sprintMultiplier
        self.wasSprint = sprint

        # --- Aim (right mouse held) ---
        # Aiming trades speed for a tighter view. Level-triggered, unlike the
        # sprint multiplier above, because holding the button is the whole
        # meaning -- edge-triggering it would apply the FOV change for a
        # single frame.
        aiming = Wasteland.IsMouseButtonPressed(Wasteland.WL_MOUSE_BUTTON_2)

        # Gamepad aim on the LEFT trigger, above a threshold. Held, not
        # edge-triggered, for the same reason the mouse button is.
        if not aiming and self.gamepadEnabled and Wasteland.Gamepad.IsConnected(self.gamepadSlot):
            try:
                leftTrigger = Wasteland.Gamepad.GetTrigger(
                    Wasteland.Gamepad.AXIS_LEFT_TRIGGER, self.gamepadSlot)
                # Threshold rather than > 0: a resting trigger reports a
                # small non-zero value, and testing against zero would make
                # the player permanently aim.
                if leftTrigger > self.gamepadFireThreshold:
                    aiming = True
            except Exception:
                pass

        if aiming:
            speed *= self.aimSpeedMultiplier

        # --- Sprint / aim view state ---
        # "Actually sprinting" is narrower than "shift is down": the speed
        # bonus above only applies standing and grounded, so the FOV push
        # must use the same condition or the view widens while the player
        # walks at normal speed.
        isSprinting = (sprint and self.stance == 0 and self.isGrounded
                       and not aiming)
        self.UpdateViewState(isSprinting, aiming, seconds)

        # stance speed penalty
        if self.stance == 1:
            speed *= self.crouchSpeed
        elif self.stance == 2:
            speed *= self.proneSpeed

        # --- Horizontal movement (with capsule collision) ---
        # Must happen AFTER the ground probe above, and before the rotation
        # commit below.
        #
        # Writing Translation directly teleports the character through walls.
        # MoveCapsule sweeps the body and returns the move that was actually
        # allowed, sliding along whatever blocked it, so the player walks into
        # a wall instead of through it.
        move_x *= speed
        move_z *= speed

        pos = t.Translation
        if self.hasCollision and scene is not None and (move_x != 0.0 or move_z != 0.0):
            blocked, hitEntity, normal, fraction, allowed = scene.MoveCapsule(
                pos, self.colliderRadius, self.colliderHalfHeight,
                Wasteland.Vec3(move_x, 0.0, move_z))
            pos = Wasteland.Vec3(pos.x + allowed.x, pos.y + allowed.y, pos.z + allowed.z)
        else:
            pos = Wasteland.Vec3(pos.x + move_x, pos.y, pos.z + move_z)

        t.Translation = pos

        # --- Ground detection (real raycast, not a hardcoded y=0 plane) ---
        # Runs AFTER horizontal movement, probing at the position the player
        # just moved to. This ordering is the whole point: probing first and
        # snapping afterwards applies a stale ground height to the new
        # position, which is how a player ends up teleporting up a wall.
        footY = t.Translation.y - self.footOffset
        groundFound = False
        groundY = 0.0
        if scene is not None:
            # Start a hair above the feet so a player resting exactly on a
            # floor still generates an intersect rather than starting inside
            # the surface.
            probeOrigin = Wasteland.Vec3(
                t.Translation.x, footY + self.groundSnapTolerance, t.Translation.z)
            hit, hitEntity, point, normal, distance, hittable = scene.Raycast(
                probeOrigin, Wasteland.Vec3(0.0, -1.0, 0.0), self.groundProbeDistance)
            if hit:
                groundFound = True
                groundY = point.y

        if not self.isGrounded and groundFound and self.verticalVelocity <= 0.0:
            self.isGrounded = True

        if self.isGrounded and not groundFound:
            # Walked off an edge: start falling.
            self.isGrounded = False

        # --- Gravity & vertical movement ---
        if not self.isGrounded:
            self.verticalVelocity -= self.gravity * seconds
            t.Translation.y += self.verticalVelocity * seconds
        elif groundFound:
            # Sit exactly on the surface, so slopes/steps do not accumulate
            # drift as a gap between the feet and the floor.
            restY = groundY + self.footOffset
            if abs(t.Translation.y - restY) > 0.0001:
                t.Translation.y = restY
            self.verticalVelocity = 0.0

        # Landing test: crossing below the surface while descending snaps the
        # player up rather than letting them sink through.
        if groundFound and not self.isGrounded and self.verticalVelocity <= 0.0:
            if t.Translation.y - self.footOffset <= groundY + self.groundSnapTolerance:
                t.Translation.y = groundY + self.footOffset
                self.verticalVelocity = 0.0
                self.isGrounded = True
        elif not groundFound and self.verticalVelocity < 0.0:
            if t.Translation.y - self.footOffset <= self.groundLevel:
                # Fell back to the fallback plane (y=0) with no collider
                # underfoot. Keeps the old behaviour for bare scenes.
                t.Translation.y = self.groundLevel + self.footOffset
                self.verticalVelocity = 0.0
                self.isGrounded = True

        # Rotation is stored in RADIANS (see note above). self.pitch/self.yaw are
        # degrees, so convert on the way out — writing them raw was the reason
        # forward did not line up with where the camera was actually looking.
        t.Rotation.x = pitch_rad
        t.Rotation.y = yaw_rad
        t.Rotation.z = 0.0

        # Commit movement + rotation. Everything above only mutates the local copy `t`.
        self.entity.SetTransform(t.Translation, t.Rotation, t.Scale)

    def Respawn(self, health):
        """Restore the player to the spawn point with full health and ammo.

        `health` is the live HealthComponent reference from this entity (not a
        copy) -- passing it in keeps the reset visible to the rest of the
        engine, which reads the component out of the registry.
        """
        health.Reset()

        # Refill any weapons this entity carries, so respawning does not
        # start the player with an empty magazine.
        weapon = self.entity.TryGetWeaponComponent()
        if weapon is not None:
            weapon.Reset()

        if self.spawnPoint is not None:
            self.entity.SetTransform(
                Wasteland.Vec3(self.spawnPoint.x, self.spawnPoint.y, self.spawnPoint.z),
                Wasteland.Vec3(0.0, 0.0, 0.0),
                Wasteland.Vec3(1.0, 1.0, 1.0))

        # Clear movement/aim state so the player does not resume mid-stance
        # or still drifting from the velocity that killed them.
        self.verticalVelocity = 0.0
        self.isGrounded = True
        self.stance = 0
        self.pitch = 0.0
        self.yaw = 0.0
        self.isDead = False
        self.deadTimer = 0.0
        self.wasCrouch = False
        self.wasProne = False
        self.wasJump = False
        self.wasSprint = False

    def UpdateViewState(self, isSprinting, isAiming, seconds):
        """Move the camera FOV toward whatever the player's state calls for.

        Aiming wins over sprinting when both are true: aim is the more
        deliberate action, and letting a sprint FOV win while the player holds
        the aim button makes aiming feel like it did nothing.
        """
        if isAiming:
            target = self.aimFOV
        elif isSprinting:
            target = self.sprintFOV
        else:
            target = self.baseFOV

        view = self.entity.TryGetViewStateComponent()
        if view is not None:
            # Publish the intent. The HUD reads IsSprinting/IsAiming to tint
            # the screen edge, so it needs the resolved flags rather than
            # re-deriving them from key state (which it cannot see).
            view.TargetFOV = target
            view.FOVLerpSpeed = self.fovLerpSpeed
            view.IsSprinting = bool(isSprinting)
            view.IsAiming = bool(isAiming)

        camera = self.entity.TryGetCameraComponent()
        if camera is None:
            return

        # CameraComponent wraps a SceneCamera in a "Camera" member -- the
        # wrapper carries the primary flag and aspect settings alongside the
        # actual camera. The FOV lives on the inner SceneCamera.
        sceneCamera = camera.Camera

        # Ease toward the target instead of snapping. Frame-rate independent:
        # moving a fixed fraction per frame makes the transition twice as fast
        # at 120fps as at 60fps, so the step scales with elapsed time.
        current = sceneCamera.GetPerspectiveVerticalFOV()
        maxStep = self.fovLerpSpeed * seconds
        delta = target - current
        if abs(delta) <= maxStep:
            sceneCamera.SetPerspectiveVerticalFOV(target)
        else:
            # Sign-preserving step: move toward the target, never past it.
            sceneCamera.SetPerspectiveVerticalFOV(
                current + maxStep * (1.0 if delta > 0.0 else -1.0))

    def OnRuntimeStart(self):
        """Grab the cursor and snap the FOV to its starting value."""
        self.SetMouseCapture(True)

        camera = self.entity.TryGetCameraComponent()
        if camera is not None:
            camera.Camera.SetPerspectiveVerticalFOV(self.baseFOV)

        view = self.entity.TryGetViewStateComponent()
        if view is not None:
            view.TargetFOV = self.baseFOV
            view.FOVLerpSpeed = self.fovLerpSpeed
            view.IsSprinting = False
            view.IsAiming = False

    def OnRuntimeStop(self):
        """Release the cursor.

        This is the important half of mouse capture. Grabbing the cursor and
        exiting without releasing it leaves the user with an invisible pointer
        and no way to click anything -- they have to kill the process. Always
        paired with the capture in OnRuntimeStart.
        """
        self.SetMouseCapture(False)

    def SetMouseCapture(self, captured):
        try:
            if not Wasteland.Application.IsInitialized():
                return
            window = Wasteland.Application.Get().GetWindow()
            if window is None:
                return
            window.SetMouseCaptured(bool(captured))
        except Exception:
            # Mouse capture is a convenience, never a correctness
            # requirement. A headless test host has no window, and a failure
            # here must not take the whole player script down with it.
            pass


