import os
import subprocess

EDITOR_EXE = "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe"

# Try with minimal env vars (task-7-vetted set)
env = {
    "WASTELAND_PIN_CAMERA": "0,0,0,9,-85,0",
    "WL_DRS_OFF": "1",
}

print("launching", EDITOR_EXE)
p = subprocess.Popen([EDITOR_EXE], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    rc = p.wait(timeout=240)
    print("exit code:", rc)
except subprocess.TimeoutExpired:
    p.kill()
    print("timeout after 240s")

import numpy as np
from PIL import Image
p = "C:/dev/Wasteland/dataset_wlfg/target/test_min.png"
if os.path.exists(p):
    img = Image.open(p).convert('RGB')
    arr = np.array(img, dtype=np.float32)
    print("test_min frame: mean", round(arr.mean(), 3), "shape", arr.shape)
else:
    print("no output file")
