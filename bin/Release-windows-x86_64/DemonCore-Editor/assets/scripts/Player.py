import Wasteland

class Player:
    def __init__(self, entity):
        self.entity = entity

    class Camera:
        def __init__(self, entity):
            self.entity = entity
            self.speed = 10.0

        def OnUpdateEntity(self, ts):
            transform = self.entity.GetTransform()
            
            # Using the exposed C++ enum values via Python
            if Wasteland.IsKeyPressed(Wasteland.Key.W):
                transform.Translation.z -= self.speed * ts
            if Wasteland.IsKeyPressed(Wasteland.Key.S):
                transform.Translation.z += self.speed * ts
            if Wasteland.IsKeyPressed(Wasteland.Key.A):
                transform.Translation.x -= self.speed * ts
            if Wasteland.IsKeyPressed(Wasteland.Key.D):
                transform.Translation.x += self.speed * ts