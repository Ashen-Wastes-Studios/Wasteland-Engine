import subprocess, os, time

REPO = r"C:\dev\Wasteland"
EXE = os.path.join(REPO, "bin", "Release-windows-x86_64", "DemonCore-Editor", "DemonCore-Editor.exe")
WORK = os.path.join(REPO, "bin", "Release-windows-x86_64", "DemonCore-Editor")

env = os.environ.copy()
env["WL_SCREENSHOT_FRAME"] = "50"
env["WL_FREEZE_TIME"] = "1"
env["WASTELAND_PIN_CAMERA"] = "1"

proc = subprocess.Popen([EXE], cwd=WORK, env=env,
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
print(f"Launched PID={proc.pid}, waiting 12s...")
time.sleep(12)
if proc.poll() is None:
    print("Editor still running, terminating...")
    proc.terminate()
    try:
        out, _ = proc.communicate(timeout=5)
    except:
        proc.kill()
        out = proc.stdout.read() if proc.stdout else ""
else:
    out = proc.stdout.read() if proc.stdout else ""

print("=== Editor output (last 6000 chars) ===")
print(out[-6000:] if out else "(empty)")
