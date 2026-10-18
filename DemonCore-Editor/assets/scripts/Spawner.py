# Wasteland Engine Script — Spawner
# Periodically spawns and despawns entities to test Scene.CreateEntity / DestroyEntity.
import Wasteland

class Spawner:
    def __init__(self, entity):
        self.entity = entity
        self.elapsed = 0.0
        self.interval = 2.0  # seconds between spawns
        self.maxEntities = 5
        self.spawned = []

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return
        seconds = dt.GetSeconds()
        self.elapsed += seconds

        if self.elapsed >= self.interval:
            self.elapsed = 0.0

            # despawn oldest if over limit
            if len(self.spawned) >= self.maxEntities:
                oldest = self.spawned.pop(0)
                if oldest.IsValid():
                    Wasteland.Scene.DestroyEntity(oldest)

            # spawn a new sphere
            newEnt = Wasteland.Scene.CreateEntity("SpawnedSphere")
            if newEnt.IsValid():
                newEnt.AddTag("Spawned")
                t = newEnt.GetTransform()
                t.Translation = Wasteland.Vec3(
                    (len(self.spawned) - self.maxEntities / 2) * 3,
                    5,
                    0,
                )
                t.Rotation = Wasteland.Vec3(0, 0, 0)
                t.Scale = Wasteland.Vec3(1, 1, 1)
                newEnt.SetTransform(t.Translation, t.Rotation, t.Scale)
                self.spawned.append(newEnt)
