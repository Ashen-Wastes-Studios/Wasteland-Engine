import os
import subprocess
import sys
from pathlib import Path

_ROOT = Path(__file__).resolve().parents[1]
_EDITOR_EXE = "C:/dev/Wasteland/bin/Release-windows-x86_64/DemonCore-Editor/DemonCore-Editor.exe"

def run_capture(api_name, api_value, output_path):
    env = dict(os.environ)
    env.update({
        "WASTELAND_PIN_CAMERA": "0,0,0,9,-85,0",
        "WL_DRS_OFF": "1",
        "WL_AUTO_SWITCH_TO": str(api_value),
        "WL_SCREENSHOT": output_path,
        "WL_SCREENSHOT_FRAME": "300",
        "WL_SCREENSHOT_LINGER": "6",
        "WL_NOVA_DIAGNOSTICS": "1",
    })
    print(f"launching {_EDITOR_EXE} for {api_name}")
    p = subprocess.Popen([_EDITOR_EXE], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        out, _ = p.communicate(timeout=180)
        rc = p.returncode
        print(f"{api_name} exit code: {rc}")
        log_path = f"C:/Users/rtoue/AppData/Local/hermes/cache/scratch/nova_material_capture/capture_{api_name.lower()}.log"
        with open(log_path, "w", encoding="utf-8", errors="replace") as f:
            f.write(out.decode("utf-8", errors="replace"))
        print(f"log written to {log_path}")
        return rc, log_path
    except subprocess.TimeoutExpired:
        p.kill()
        try:
            out, _ = p.communicate(timeout=10)
            log_path = f"C:/Users/rtoue/AppData/Local/hermes/cache/scratch/nova_material_capture/capture_{api_name.lower()}.log"
            with open(log_path, "w", encoding="utf-8", errors="replace") as f:
                f.write(out.decode("utf-8", errors="replace"))
            print(f"log written on timeout to {log_path}")
        except Exception as e:
            print(f"failed to read stdout on timeout: {e}")
        print(f"{api_name} TIMEOUT")
        return -1, None

# DX11 capture
rc11, log11 = run_capture("DX11", 2, "C:/dev/Wasteland/dataset_wlfg/target/test_dx11.png")

# DX12 capture
rc12, log12 = run_capture("DX12", 3, "C:/dev/Wasteland/dataset_wlfg/target/test_dx12.png")

# Check outputs
for api, path in [("DX11", "C:/dev/Wasteland/dataset_wlfg/target/test_dx11.png"),
                  ("DX12", "C:/dev/Wasteland/dataset_wlfg/target/test_dx12.png")]:
    if os.path.exists(path):
        from PIL import Image
        import numpy as np
        img = Image.open(path).convert('RGB')
        arr = np.array(img, dtype=np.float32)
        print(f"{api} frame: mean {arr.mean():.1f}, shape {arr.shape}, min {arr.min():.1f}, max {arr.max():.1f}")
    else:
        print(f"{api}: NO OUTPUT FILE")

print(f"DX11 rc={rc11}, DX12 rc={rc12}")
