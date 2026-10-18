# Wasteland Engine Script — Bouncer
# A sphere that falls under gravity and bounces off the ground plane (y=0).
import Wasteland
import math

class Bouncer:
    def __init__(self, entity):
        self.entity = entity
        self.gravity = 20.0
        self.velocity = 0.0
        self.bounceFactor = 0.7
        self.groundY = 0.0

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return
        seconds = dt.GetSeconds()
        t = self.entity.GetTransform()

        # gravity
        self.velocity -= self.gravity * seconds
        t.Translation.y += self.velocity * seconds

        # ground collision
        if t.Translation.y <= self.groundY:
            t.Translation.y = self.groundY
            self.velocity = -self.velocity * self.bounceFactor
            if abs(self.velocity) < 0.5:
                self.velocity = 0.0

        self.entity.SetTransform(t.Translation, t.Rotation, t.Scale)
