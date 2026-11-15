import subprocess, os, time, glob

REPO = r"C:\dev\Wasteland"
EXE = os.path.join(REPO, "bin", "Release-windows-x86_64", "DemonCore-Editor", "DemonCore-Editor.exe")
WORK = os.path.join(REPO, "bin", "Release-windows-x86_64", "DemonCore-Editor")

for f in glob.glob(os.path.join(WORK, "screenshot_*.png")):
    try: os.remove(f)
    except: pass

env = os.environ.copy()
env["WL_SCREENSHOT_FRAME"] = "50"
env["WL_FREEZE_TIME"] = "1"
env["WASTELAND_PIN_CAMERA"] = "1"

proc = subprocess.Popen([EXE], cwd=WORK, env=env,
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
print(f"Launched PID={proc.pid}, waiting 15s...")
time.sleep(15)

proc.terminate()
try:
    out, _ = proc.communicate(timeout=5)
except:
    proc.kill()
    out = proc.stdout.read() if proc.stdout else ""

print(f"Process exit code: {proc.returncode}")
print("=== Editor output (last 8000 chars) ===")
print(out[-8000:] if out else "(empty)")

print("\n=== Screenshots ===")
screenshots = glob.glob(os.path.join(WORK, "screenshot_*.png"))
for s in sorted(screenshots):
    sz = os.path.getsize(s)
    print(f"  {os.path.basename(s)}: {sz:,} bytes")
