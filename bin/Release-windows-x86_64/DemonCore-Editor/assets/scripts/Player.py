import sys
import os
sys.path.append(os.path.dirname(__file__))
import Wasteland

class Player:
    def __init__(self, entity):
        try:
            self.entity = entity
            print(f"DEBUG: Initializing Entity: {entity}")
            self.speed = 10.0
        except Exception as e:
            print(f"DEBUG: Error caught in __init__: {e}")
            raise e

    def OnUpdateEntity(self, dt):
        if self.entity is None:
            return
        
        if not hasattr(self.entity, 'GetTransform'):
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