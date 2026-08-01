# Wasteland Engine Script
import sys
import os
import math
sys.path.append(os.path.dirname(__file__))
import Wasteland

class Player:
    def __init__(self, entity):
        try:
            self.entity = entity
            print(f"DEBUG: Initializing Entity: {entity}")

            # Movement
            self.speed = 10.0
            self.sprintMultiplier = 1.25

            # Mouse look
            self.mouseSensitivity = 0.01
            self.pitch = 0.0
            self.yaw = 0.0
            self.targetPitch = 0.0
            self.targetYaw = 0.0
            self.maxPitch = 89.0
            self.minPitch = -89.0
            self.maxTurnSpeed = 0.35
            self.rotationSmoothing = 0.25
            self.initializedRotation = False

            # Jump
            self.verticalVelocity = 0.0
            self.gravity = 25.0
            self.jumpForce = 9.0
            self.isGrounded = True
            self.groundLevel = 0.0

            # Stance (0 = standing, 1 = crouching, 2 = prone)
            self.stance = 0
            self.crouchSpeedMultiplier = 0.5
            self.proneSpeedMultiplier = 0.25
            self.wasCrouchPressed = False
            self.wasPronePressed = False

            # Head bob
            self.bobPhase = 0.0
            self.bobFrequency = 8.0
            self.bobAmplitude = 0.02
            self.bobSwayAmplitude = 0.01
            self.bobCurrentAmount = 0.0
            self.bobSmoothSpeed = 8.0
        except Exception as e:
            print(f"DEBUG: Error caught in __init__: {e}")
            raise e

    def OnUpdateEntity(self, dt):
        try:
            if self.entity is None:
                return
        except:
            return

        if not hasattr(self.entity, 'GetTransform'):
            return

        transform = self.entity.GetTransform()

        if transform is not None:
            pos = transform.Translation
            rot = transform.Rotation

            if not self.initializedRotation:
                self.targetPitch = rot.x
                self.targetYaw = rot.y
                self.pitch = rot.x
                self.yaw = rot.y
                self.initializedRotation = True

            seconds = dt.GetSeconds()

            # --- Gravity & Jump ---
            if not self.isGrounded:
                self.verticalVelocity -= self.gravity * seconds
                pos.y += self.verticalVelocity * seconds
                if pos.y <= self.groundLevel:
                    pos.y = self.groundLevel
                    self.verticalVelocity = 0.0
                    self.isGrounded = True

            # --- Stance Toggle (edge-triggered) ---
            crouchPressed = Wasteland.IsKeyPressed(Wasteland.WL_KEY_C)
            pronePressed = Wasteland.IsKeyPressed(Wasteland.WL_KEY_Z)

            if self.isGrounded:
                if crouchPressed and not self.wasCrouchPressed:
                    if self.stance == 1:
                        self.stance = 0  # crouch -> stand
                    elif self.stance == 2:
                        self.stance = 1  # prone -> crouch
                    else:
                        self.stance = 1  # stand -> crouch

                if pronePressed and not self.wasPronePressed:
                    if self.stance == 2:
                        self.stance = 0  # prone -> stand
                    else:
                        self.stance = 2  # any -> prone

            self.wasCrouchPressed = crouchPressed
            self.wasPronePressed = pronePressed

            # Jump (only when grounded and standing)
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_SPACE) and self.isGrounded and self.stance == 0:
                self.verticalVelocity = self.jumpForce
                self.isGrounded = False

            # --- Movement ---
            forwardX = -math.sin(self.yaw)
            forwardZ = -math.cos(self.yaw)
            rightX = math.cos(self.yaw)
            rightZ = -math.sin(self.yaw)

            moveSpeed = self.speed * seconds

            # Sprint (standing only, grounded only)
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_LEFT_SHIFT) and self.stance == 0 and self.isGrounded:
                moveSpeed *= self.sprintMultiplier

            # Stance speed reduction
            if self.stance == 1:
                moveSpeed *= self.crouchSpeedMultiplier
            elif self.stance == 2:
                moveSpeed *= self.proneSpeedMultiplier

            moveX = 0.0
            moveZ = 0.0

            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_W):
                moveX += forwardX
                moveZ += forwardZ
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_S):
                moveX -= forwardX
                moveZ -= forwardZ
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_D):
                moveX += rightX
                moveZ += rightZ
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_A):
                moveX -= rightX
                moveZ -= rightZ

            length = math.sqrt(moveX * moveX + moveZ * moveZ)
            if length > 0.0:
                moveX /= length
                moveZ /= length

            pos.x += moveX * moveSpeed
            pos.z += moveZ * moveSpeed

            # --- Head Bob ---
            isMoving = length > 0.0 and self.isGrounded
            bobTarget = 1.0 if isMoving else 0.0
            self.bobCurrentAmount += (bobTarget - self.bobCurrentAmount) * min(1.0, self.bobSmoothSpeed * seconds)

            if isMoving:
                stanceScale = 1.0 if self.stance == 0 else (0.5 if self.stance == 1 else 0.15)
                self.bobPhase += self.bobFrequency * seconds * stanceScale

            bobOffsetY = math.sin(self.bobPhase) * self.bobAmplitude * self.bobCurrentAmount
            bobSway = math.cos(self.bobPhase * 0.5) * self.bobSwayAmplitude * self.bobCurrentAmount

            # Apply bob in camera-aligned local space
            pos.y += bobOffsetY
            pos.x += rightX * bobSway
            pos.z += rightZ * bobSway

            # --- Mouse Look ---
            mouseDelta = Wasteland.GetMouseDelta()
            if mouseDelta is not None and (abs(mouseDelta[0]) > 0.0 or abs(mouseDelta[1]) > 0.0):
                deltaX = max(min(mouseDelta[0] * self.mouseSensitivity, self.maxTurnSpeed), -self.maxTurnSpeed)
                deltaY = max(min(mouseDelta[1] * self.mouseSensitivity, self.maxTurnSpeed), -self.maxTurnSpeed)
                self.targetYaw -= deltaX
                self.targetPitch -= deltaY
                self.targetPitch = max(min(self.targetPitch, self.maxPitch), self.minPitch)

            self.yaw += (self.targetYaw - self.yaw) * self.rotationSmoothing
            self.pitch += (self.targetPitch - self.pitch) * self.rotationSmoothing

            rot.x = self.pitch
            rot.y = self.yaw
            rot.z = 0.0

            transform.SetTransform(pos, rot)

