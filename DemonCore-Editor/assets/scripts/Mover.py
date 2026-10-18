# Wasteland Engine Script — Mover
# Moves an entity along a straight line, looping back when it hits a boundary.
import Wasteland

class Mover:
    def __init__(self, entity):
        self.entity = entity
        self.speed = 5.0
        self.range = 20.0  # how far from origin before turning
        self.startPos = None

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return
        if self.startPos is None:
            self.startPos = self.entity.GetTransform().Translation

        t = self.entity.GetTransform()
        seconds = dt.GetSeconds()
        t.Translation.x += self.speed * seconds

        # bounce between -range and +range
        if t.Translation.x > self.range:
            t.Translation.x = self.range
            self.speed = -abs(self.speed)
        elif t.Translation.x < -self.range:
            t.Translation.x = -self.range
            self.speed = abs(self.speed)

        self.entity.SetTransform(t.Translation, t.Rotation, t.Scale)
