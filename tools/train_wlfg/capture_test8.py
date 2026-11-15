import os
import subprocess
import time

EDITOR_EXE = "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe"

# Try with creationflags for Windows GUI apps
print("launching", EDITOR_EXE)
p = subprocess.Popen([EDITOR_EXE],
                     stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE,
                     creationflags=subprocess.CREATE_NO_WINDOW)
try:
    rc = p.wait(timeout=240)
    print("exit code:", rc)
except subprocess.TimeoutExpired:
    p.kill()
    print("timeout after 240s")

import numpy as np
from PIL import Image
p2 = "C:/dev/Wasteland/dataset_wlfg/target/test_min.png"
if os.path.exists(p2):
    img = Image.open(p2).convert('RGB')
    arr = np.array(img, dtype=np.float32)
    print("test_min frame: mean", round(arr.mean(), 3), "shape", arr.shape)
else:
    print("no output file")
