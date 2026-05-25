import Wasteland

class Camera:
    def __init__(self, entity):
        self.entity = entity
        self.speed = 10.0

    def OnUpdateEntity(self, dt):
        # 1. Get the current transform reference from the C++ side
        transform = self.entity.GetTransform()
        
        # 2. Logic for movement
        # dt (delta time) ensures movement is consistent regardless of FPS
        if Wasteland.IsKeyPressed("W"):
            transform.Translation.z -= self.speed * dt
        if Wasteland.IsKeyPressed("S"):
            transform.Translation.z += self.speed * dt
        if Wasteland.IsKeyPressed("A"):
            transform.Translation.x -= self.speed * dt
        if Wasteland.IsKeyPressed("D"):
            transform.Translation.x += self.speed * dt

        # The 'transform' is a reference, so updating it here 
        # instantly updates the Engine's C++ data.