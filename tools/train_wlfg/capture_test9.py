import os
import subprocess
import sys

EDITOR_EXE = "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe"

# Try with task-7-vetted env vars
env = {
    "WASTELAND_PIN_CAMERA": "0,0,0,9,-85,0",
    "WL_DRS_OFF": "1",
    "WL_CAPTURE_SCENE": "Example3D",
    "WL_CAPTURE_PRESET": "3",
    "WL_CAPTURE_SCALE": "1.0",
}

print("launching", EDITOR_EXE)
print("env:", env)

# Use CREATE_NO_WINDOW so the engine doesn't try to attach to a console
p = subprocess.Popen([EDITOR_EXE], env=env,
                     stdout=subprocess.PIPE,
                     stderr=subprocess.PIPE,
                     creationflags=subprocess.CREATE_NO_WINDOW)

# Read stdout and stderr in a non-blocking way
import threading
stdout_data = []
stderr_data = []

def read_stdout():
    for line in iter(p.stdout.readline, b''):
        stdout_data.append(line)

def read_stderr():
    for line in iter(p.stderr.readline, b''):
        stderr_data.append(line)

t1 = threading.Thread(target=read_stdout)
t2 = threading.Thread(target=read_stderr)
t1.start()
t2.start()

try:
    rc = p.wait(timeout=240)
    print("exit code:", rc)
except subprocess.TimeoutExpired:
    p.kill()
    print("timeout after 240s")

t1.join()
t2.join()

print("stdout:", stdout_data[:5])
print("stderr:", stderr_data[:5])

import numpy as np
from PIL import Image
p2 = "C:/dev/Wasteland/dataset_wlfg/target/test_min.png"
if os.path.exists(p2):
    img = Image.open(p2).convert('RGB')
    arr = np.array(img, dtype=np.float32)
    print("test_min frame: mean", round(arr.mean(), 3), "shape", arr.shape)
else:
    print("no output file")
