# Wasteland Engine Script — Floaty
# An entity that bobs up and down sinusoidally, demonstrating sine-based motion.
import Wasteland
import math

class Floaty:
    def __init__(self, entity):
        self.entity = entity
        self.baseY = 0.0
        self.frequency = 1.5   # Hz
        self.amplitude = 1.5   # units

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return
        if self.baseY == 0.0:
            self.baseY = self.entity.GetTransform().Translation.y

        seconds = dt.GetSeconds()
        t = self.entity.GetTransform()
        t.Translation.y = self.baseY + math.sin(seconds * self.frequency * 2 * math.pi) * self.amplitude
        self.entity.SetTransform(t.Translation, t.Rotation, t.Scale)
