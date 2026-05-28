import Wasteland

class Player:
    def __init__(self, entity):
        self.entity = entity
        self.speed = 10.0

    def OnUpdateEntity(self, dt):
        if self.entity is None:
            return

        transform = self.entity.GetTransform()

        if transform is not None:
            pos = transform.Translation
            
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_W):
                pos.z -= self.speed * dt
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_S):
                pos.z += self.speed * dt
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_A):
                pos.x -= self.speed * dt
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_D):
                pos.x += self.speed * dt

            transform.Translation = pos