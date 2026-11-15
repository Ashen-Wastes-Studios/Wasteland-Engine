import os
import subprocess
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
_EDITOR_EXE = "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe"

env = dict(os.environ)
env.update({
    "WASTELAND_PIN_CAMERA": "0,0,0,9,-85,0",
    "WL_DRS_OFF": "1",
    "WL_SCREENSHOT": "C:/dev/Wasteland/dataset_wlfg/target/test.png",
    "WL_SCREENSHOT_FRAME": "300",
    "WL_SCREENSHOT_LINGER": "6",
})
print("launching", _EDITOR_EXE)
p = subprocess.Popen([_EDITOR_EXE], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    rc = p.wait(timeout=120)
    out = p.stdout.read().decode('utf-8', errors='replace')
    with open('C:/Users/rtoue/AppData/Local/hermes/cache/scratch/nova_material_capture/capture_diag.log', 'w') as f:
        f.write(out)
    print("exit code:", rc)
except subprocess.TimeoutExpired:
    p.kill()
    print("timeout")

# Check output
if os.path.exists("C:/dev/Wasteland/dataset_wlfg/target/test.png"):
    from PIL import Image
    import numpy as np
    img = Image.open("C:/dev/Wasteland/dataset_wlfg/target/test.png").convert('RGB')
    arr = np.array(img, dtype=np.float32)
    print("test frame: mean", arr.mean(), "shape", arr.shape)

