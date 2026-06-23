# Wasteland Engine Script
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
        try:
            if self.entity is None:
                return
            
        except: 
            return
        
        if not hasattr(self.entity, 'GetTransform'):
            return

        transform = self.entity.GetTransform()

        if transform is not None:
            pos = transform.Translation
            
            seconds = dt.GetSeconds() 

            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_W):
                pos.z -= self.speed * seconds
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_S):
                pos.z += self.speed * seconds
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_A):
                pos.x -= self.speed * seconds
            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_D):
                pos.x += self.speed * seconds

            if Wasteland.IsKeyPressed(Wasteland.WL_KEY_SPACE):
                pos.y += self.speed * seconds
            if not Wasteland.IsKeyPressed(Wasteland.WL_KEY_SPACE):
                pos.y -= self.speed * seconds

            transform.Translation = pos