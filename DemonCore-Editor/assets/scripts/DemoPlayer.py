# Wasteland Engine Script — DemoPlayer
# Combined first-person controller + weapon + interactor for the demo game.
# Handles: WASD movement, mouse look, jump, crouch, sprint, aim, shoot,
# reload, interaction, death/respawn.
import Wasteland
import math


class DemoPlayer:
    def __init__(self, entity):
        self.entity = entity

        # Spawn point (captured on first frame)
        self.spawnPoint = None

        # Death / respawn
        self.respawnDelay = 2.0
        self.deadTimer = 0.0
        self.isDead = False

        # Movement
        self.speed = 8.0
        self.sprintMultiplier = 1.8
        self.aimSpeedMultiplier = 0.5

        # FOV
        self.baseFOV = math.radians(60.0)
        self.sprintFOV = math.radians(70.0)
        self.aimFOV = math.radians(45.0)
        self.fovLerpSpeed = 8.0

        # Mouse look
        self.mouseSensitivity = 0.15
        self.pitch = 0.0
        self.yaw = 0.0
        self.maxPitch = 89.0
        self.minPitch = -89.0
        self.maxTurnSpeed = 180.0

        # Jump / gravity
        self.verticalVelocity = 0.0
        self.gravity = 20.0
        self.jumpForce = 8.0
        self.isGrounded = True
        self.groundLevel = 0.0
        self.footOffset = 1.0
        self.groundSnapTolerance = 0.15
        self.groundProbeDistance = 0.35

        # Collision
        self.hasCollision = True
        self.colliderRadius = 0.4
        self.colliderHalfHeight = 0.6

        # Stances
        self.stance = 0
        self.crouchSpeed = 0.5
        self.proneSpeed = 0.25

        # Edge-trigger state
        self.wasCrouch = False
        self.wasProne = False
        self.wasSprint = False
        self.wasJump = False
        self.wasReload = False

        # Weapon state
        self.recoilPitch = 0.0
        self.recoilPerShot = 0.55
        self.recoilRecoverySpeed = 6.0
        self.recoilMax = 6.0

        # Interaction
        self.interactRange = 3.0
        self.interactKey = Wasteland.WL_KEY_E
        self.interactCooldown = 0.35
        self.cooldownTimer = 0.0
        self.currentTarget = None
        self.currentTargetLabel = ""
        self.previousTarget = None

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return

        t = self.entity.GetTransform()
        if t is None:
            return

        seconds = dt.GetSeconds()
        if seconds <= 0.0:
            return

        # Capture spawn point
        if self.spawnPoint is None:
            self.spawnPoint = Wasteland.Vec3(t.Translation.x, t.Translation.y, t.Translation.z)

        # --- Death / respawn ---
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
            if self.isDead:
                self.isDead = False
                self.deadTimer = 0.0

        scene = Wasteland.Scene.GetActiveScene()
        if scene is None:
            return

        # --- Stance ---
        crouch = Wasteland.IsKeyPressed(Wasteland.WL_KEY_C)
        prone = Wasteland.IsKeyPressed(Wasteland.WL_KEY_Z)
        if self.isGrounded:
            if crouch and not self.wasCrouch:
                self.stance = 0 if self.stance == 1 else (1 if self.stance != 2 else 2)
            if prone and not self.wasProne:
                self.stance = 0 if self.stance == 2 else 2
        self.wasCrouch = crouch
        self.wasProne = prone

        # --- Jump ---
        jump = Wasteland.IsKeyPressed(Wasteland.WL_KEY_SPACE)
        if jump and not self.wasJump and self.isGrounded and self.stance == 0:
            self.verticalVelocity = self.jumpForce
            self.isGrounded = False
        self.wasJump = jump

        # --- Mouse look ---
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

        # --- Movement direction ---
        pitch_rad = math.radians(self.pitch)
        yaw_rad = math.radians(self.yaw)
        cy, sy = math.cos(yaw_rad), math.sin(yaw_rad)
        cp, sp = math.cos(pitch_rad), math.sin(pitch_rad)
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

        length = math.sqrt(move_x * move_x + move_z * move_z)
        if length > 0.001:
            move_x /= length
            move_z /= length

        # Sprint
        speed = self.speed * seconds
        sprint = Wasteland.IsKeyPressed(Wasteland.WL_KEY_LEFT_SHIFT)
        if sprint and not self.wasSprint and self.stance == 0 and self.isGrounded:
            speed *= self.sprintMultiplier
        self.wasSprint = sprint

        # Aim
        aiming = Wasteland.IsMouseButtonPressed(Wasteland.WL_MOUSE_BUTTON_2)
        if aiming:
            speed *= self.aimSpeedMultiplier

        # FOV
        isSprinting = (sprint and self.stance == 0 and self.isGrounded and not aiming)
        self.UpdateViewState(isSprinting, aiming, seconds)

        # Stance speed
        if self.stance == 1:
            speed *= self.crouchSpeed
        elif self.stance == 2:
            speed *= self.proneSpeed

        # --- Horizontal movement ---
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

        # --- Ground detection ---
        footY = t.Translation.y - self.footOffset
        groundFound = False
        groundY = 0.0
        if scene is not None:
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
            self.isGrounded = False

        # --- Gravity ---
        if not self.isGrounded:
            self.verticalVelocity -= self.gravity * seconds
            t.Translation.y += self.verticalVelocity * seconds
        elif groundFound:
            restY = groundY + self.footOffset
            if abs(t.Translation.y - restY) > 0.0001:
                t.Translation.y = restY
            self.verticalVelocity = 0.0

        if groundFound and not self.isGrounded and self.verticalVelocity <= 0.0:
            if t.Translation.y - self.footOffset <= groundY + self.groundSnapTolerance:
                t.Translation.y = groundY + self.footOffset
                self.verticalVelocity = 0.0
                self.isGrounded = True
        elif not groundFound and self.verticalVelocity < 0.0:
            if t.Translation.y - self.footOffset <= self.groundLevel:
                t.Translation.y = self.groundLevel + self.footOffset
                self.verticalVelocity = 0.0
                self.isGrounded = True

        # --- Rotation ---
        t.Rotation.x = pitch_rad
        t.Rotation.y = yaw_rad
        t.Rotation.z = 0.0

        # --- Weapon ---
        self.UpdateWeapon(dt, t, aiming)

        # --- Interaction ---
        self.UpdateInteraction(dt, t)

        # Commit
        self.entity.SetTransform(t.Translation, t.Rotation, t.Scale)

    def UpdateWeapon(self, dt, transform, aiming):
        weapon = self.entity.TryGetWeaponComponent()
        if weapon is None:
            return

        seconds = dt.GetSeconds()
        feedback = self.entity.TryGetWeaponFeedbackComponent()
        if feedback is not None:
            feedback.Tick(seconds)

        weapon.Tick(seconds)

        # Fire
        firing = Wasteland.IsMouseButtonPressed(Wasteland.WL_MOUSE_BUTTON_1)
        if firing and weapon.CanFire():
            if weapon.Fire():
                self.FireHitscan(weapon, transform)
                self.recoilPitch = min(self.recoilPitch + self.recoilPerShot, self.recoilMax)
                if feedback is not None:
                    feedback.TriggerMuzzleFlash()

        # Reload progress
        if feedback is not None:
            if weapon.IsReloading:
                feedback.ReloadProgress = weapon.ReloadTimer
                feedback.ReloadTotal = weapon.ReloadTime
            else:
                feedback.ReloadProgress = 0.0
                feedback.ReloadTotal = 0.0

        # Recoil recovery
        if self.recoilPitch > 0.0:
            recovered = self.recoilRecoverySpeed * seconds
            self.recoilPitch -= recovered
            if self.recoilPitch < 0.0:
                self.recoilPitch = 0.0

        # Reload
        reload = Wasteland.IsKeyPressed(Wasteland.WL_KEY_R)
        if reload and not self.wasReload and weapon.CanReload():
            weapon.StartReload()
        self.wasReload = reload

        # Apply recoil
        rotation = transform.Rotation
        transform.Rotation.x = rotation.x + math.radians(self.recoilPitch)

    def FireHitscan(self, weapon, transform):
        pitch = transform.Rotation.x
        yaw = transform.Rotation.y
        forward = (
            -math.sin(yaw) * math.cos(pitch),
            -math.sin(pitch),
            -math.cos(yaw) * math.cos(pitch),
        )
        origin = (
            transform.Translation.x,
            transform.Translation.y,
            transform.Translation.z,
        )
        scene = Wasteland.Scene.GetActiveScene()
        if scene is None:
            return

        hit, hitEntity, point, normal, distance, hittable = scene.Raycast(
            Wasteland.Vec3(*origin), Wasteland.Vec3(*forward), weapon.Range)

        if not hit:
            return

        if not hittable or not hitEntity.IsValid():
            return

        health = hitEntity.TryGetHealthComponent()
        if health is None:
            return

        applied = health.ApplyDamage(weapon.Damage)
        if applied > 0.0:
            if health.DamageCooldown <= 0.0:
                health.DamageCooldown = 0.05
            feedback = self.entity.TryGetWeaponFeedbackComponent()
            if feedback is not None:
                feedback.TriggerHitMarker(health.IsDead)

    def UpdateInteraction(self, dt, transform):
        self.cooldownTimer = max(0.0, self.cooldownTimer - dt.GetSeconds())
        scene = Wasteland.Scene.GetActiveScene()
        if scene is None:
            return

        rot = transform.Rotation
        pitch = rot.x
        yaw = rot.y
        cp = math.cos(pitch)
        forward = Wasteland.Vec3(
            -math.sin(yaw) * cp,
            -math.sin(pitch),
            -math.cos(yaw) * cp)

        # Clear ALL interactable labels in the scene
        for e in scene.GetEntities():
            interactable = e.TryGetInteractableComponent()
            if interactable is not None:
                interactable.Label = ""

        # Clear previous target tracking
        self.previousTarget = None
        self.currentTarget = None
        self.currentTargetLabel = ""

        # Raycast for interaction
        hit, hitEntity, point, normal, distance, hittable = scene.Raycast(
            transform.Translation, forward, self.interactRange)

        if not hit or not hitEntity.IsValid():
            return

        # Check distance (proximity)
        if distance > self.interactRange:
            return

        target = hitEntity.TryGetInteractableComponent()
        if target is None or not target.Enabled:
            return

        # Get item name from ItemDefinitionComponent
        itemDef = hitEntity.TryGetItemDefinitionComponent()
        if itemDef is not None:
            labelText = "[E] Pick up " + itemDef.DisplayName
        else:
            labelText = "[E] Interact"

        # Set label on the LOOKED-AT entity (engine HUD reads this)
        target.Label = labelText

        self.previousTarget = hitEntity
        self.currentTarget = hitEntity
        self.currentTargetLabel = labelText

        if self.cooldownTimer <= 0.0 and Wasteland.IsKeyPressed(self.interactKey):
            self.cooldownTimer = self.interactCooldown
            hitEntity.CallScriptMethod("OnInteract", self.entity)

    def Respawn(self, health):
        health.Reset()
        weapon = self.entity.TryGetWeaponComponent()
        if weapon is not None:
            weapon.Reset()
        if self.spawnPoint is not None:
            self.entity.SetTransform(
                Wasteland.Vec3(self.spawnPoint.x, self.spawnPoint.y, self.spawnPoint.z),
                Wasteland.Vec3(0.0, 0.0, 0.0),
                Wasteland.Vec3(1.0, 1.0, 1.0))
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
        if isAiming:
            target = self.aimFOV
        elif isSprinting:
            target = self.sprintFOV
        else:
            target = self.baseFOV

        view = self.entity.TryGetViewStateComponent()
        if view is not None:
            view.TargetFOV = target
            view.FOVLerpSpeed = self.fovLerpSpeed
            view.IsSprinting = bool(isSprinting)
            view.IsAiming = bool(isAiming)

        camera = self.entity.TryGetCameraComponent()
        if camera is None:
            return
        sceneCamera = camera.Camera
        current = sceneCamera.GetPerspectiveVerticalFOV()
        maxStep = self.fovLerpSpeed * seconds
        delta = target - current
        if abs(delta) <= maxStep:
            sceneCamera.SetPerspectiveVerticalFOV(target)
        else:
            sceneCamera.SetPerspectiveVerticalFOV(
                current + maxStep * (1.0 if delta > 0.0 else -1.0))

    def OnRuntimeStart(self):
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
            pass
