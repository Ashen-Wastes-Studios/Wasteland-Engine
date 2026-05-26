import Wasteland

class Player:
    def __init__(self, entity):
        self.entity = entity
        self.speed = 10.0

    def OnUpdateEntity(self, ts):
        if self.entity is None:
            return

        transform = self.entity.GetTransform()

        if transform is not None:
            pos = transform.Translation
            
            if Wasteland.IsKeyPressed(Wasteland.Key.W):
                pos.z -= self.speed * ts
            if Wasteland.IsKeyPressed(Wasteland.Key.S):
                pos.z += self.speed * ts
            if Wasteland.IsKeyPressed(Wasteland.Key.A):
                pos.x -= self.speed * ts
            if Wasteland.IsKeyPressed(Wasteland.Key.D):
                pos.x += self.speed * ts

            transform.Translation = pos

Player(entity=Player)