# Wasteland Engine Script — ColorCycler
# Cycles the material emission color through a rainbow over time.
import Wasteland
import math

class ColorCycler:
    def __init__(self, entity):
        self.entity = entity
        self.hue = 0.0
        self.speed = 0.5  # hue cycles per second

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return
        seconds = dt.GetSeconds()
        self.hue = (self.hue + self.speed * seconds) % 1.0

        # HSV -> RGB (simple version)
        h = self.hue
        r = math.sin(h * 2 * math.pi + 0) * 0.5 + 0.5
        g = math.sin(h * 2 * math.pi + 2) * 0.5 + 0.5
        b = math.sin(h * 2 * math.pi + 4) * 0.5 + 0.5

        mat = self.entity.GetComponent("MaterialComponent")
        if mat is not None:
            mat.EmissionColor = Wasteland.Vec4(r, g, b, 1.0)
            mat.EmissionIntensity = 0.5 + 0.5 * math.sin(seconds * 3)
