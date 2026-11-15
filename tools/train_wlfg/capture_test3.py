import os
import subprocess
from pathlib import Path

EDITOR_EXE = "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe"

# Try with the task-7-vetted env vars (no WL_SCREENSHOT)
# Use CREATE_NO_WINDOW so the engine doesn't try to attach to a console that
# doesn't exist in this headless context.
env = {
    "WASTELAND_PIN_CAMERA": "0,0,0,9,-85,0",
    "WL_DRS_OFF": "1",
    "WL_CAPTURE_SCENE": "Example3D",
    "WL_CAPTURE_PRESET": "3",
    "WL_CAPTURE_SCALE": "1.0",
}

print("launching", EDITOR_EXE)
p = subprocess.Popen([EDITOR_EXE], env=env, stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE,
                     creationflags=subprocess.CREATE_NO_WINDOW)
try:
    rc = p.wait(timeout=240)
    print("exit code:", rc)
except subprocess.TimeoutExpired:
    p.kill()
    print("timeout")

import numpy as np
from PIL import Image
p = "C:/dev/Wasteland/dataset_wlfg/target/test2.png"
if os.path.exists(p):
    img = Image.open(p).convert('RGB')
    arr = np.array(img, dtype=np.float32)
    print("test2 frame: mean", round(arr.mean(), 3), "shape", arr.shape)
else:
    print("no output file")
