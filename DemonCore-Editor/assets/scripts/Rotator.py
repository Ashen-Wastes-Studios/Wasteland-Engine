# Wasteland Engine Script — Rotator
# Spins an entity around its Y axis at a constant rate.
import Wasteland

class Rotator:
    def __init__(self, entity):
        self.entity = entity
        self.rotationSpeed = 90.0  # degrees per second

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return
        t = self.entity.GetTransform()
        seconds = dt.GetSeconds()
        t.Rotation.y += self.rotationSpeed * seconds
        self.entity.SetTransform(t.Translation, t.Rotation, t.Scale)
