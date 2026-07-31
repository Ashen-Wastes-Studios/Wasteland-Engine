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
            self.speed = 10.0
            self.mouseSensitivity = 0.04
            self.pitch = 0.0
            self.yaw = 0.0
            self.targetPitch = 0.0
            self.targetYaw = 0.0
            self.maxPitch = 89.0
            self.minPitch = -89.0
            self.maxTurnSpeed = 0.35
            self.rotationSmoothing = 0.25
            self.initializedRotation = False
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

            forwardX = -math.sin(self.yaw)
            forwardZ = -math.cos(self.yaw)
            rightX = math.cos(self.yaw)
            rightZ = -math.sin(self.yaw)

            moveSpeed = self.speed * seconds
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_LEFT_SHIFT):
                moveSpeed *= 1.25

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

            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_SPACE):
                pos.y += self.speed * seconds
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_LEFT_CONTROL):
                pos.y -= self.speed * seconds

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