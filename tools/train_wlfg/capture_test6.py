import os
import subprocess

EDITOR_EXE = "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe"

# Try WITH a minimal env (just PATH, WINDIR)
env_minimal = {
    "PATH": os.environ.get("PATH", ""),
    "WINDIR": os.environ.get("WINDIR", "C:\\WINDOWS"),
}

print("launching", EDITOR_EXE)
p = subprocess.Popen([EDITOR_EXE], env=env_minimal, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
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
