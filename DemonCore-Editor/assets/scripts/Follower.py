# Wasteland Engine Script — Follower
# An entity that follows another entity by name, maintaining an offset.
import Wasteland

class Follower:
    def __init__(self, entity):
        self.entity = entity
        self.targetName = "Target"  # entity tag to follow
        self.offset = Wasteland.Vec3(0, 2, 5)
        self.speed = 8.0

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return
        seconds = dt.GetSeconds()

        # find target by tag
        target = None
        for e in Wasteland.Scene.GetEntities():
            if e.HasTag() and e.GetTag().Tag == self.targetName:
                target = e
                break

        if target is None or not target.IsValid():
            return

        targetT = target.GetTransform()
        myT = self.entity.GetTransform()

        desired = Wasteland.Vec3(
            targetT.Translation.x + self.offset.x,
            targetT.Translation.y + self.offset.y,
            targetT.Translation.z + self.offset.z,
        )

        # smooth follow
        myT.Translation.x += (desired.x - myT.Translation.x) * min(1.0, self.speed * seconds)
        myT.Translation.y += (desired.y - myT.Translation.y) * min(1.0, self.speed * seconds)
        myT.Translation.z += (desired.z - myT.Translation.z) * min(1.0, self.speed * seconds)

        # look at target
        dir = Wasteland.Vec3(
            desired.x - myT.Translation.x,
            desired.y - myT.Translation.y,
            desired.z - myT.Translation.z,
        )
        if dir.Length() > 0.01:
            yaw = math.degrees(math.atan2(dir.x, dir.z))
            myT.Rotation.y = yaw

        self.entity.SetTransform(myT.Translation, myT.Rotation, myT.Scale)

import math
