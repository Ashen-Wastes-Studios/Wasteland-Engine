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
print(f"Launched PID={proc.pid}, waiting 20s...")
time.sleep(20)

proc.terminate()
try:
    out, _ = proc.communicate(timeout=5)
except:
    proc.kill()
    out = proc.stdout.read() if proc.stdout else ""

print(f"Exit code: {proc.returncode}")
print("=== Editor output (last 10000 chars) ===")
print(out[-10000:] if out else "(empty)")

print("\n=== Screenshots ===")
for s in sorted(glob.glob(os.path.join(WORK, "screenshot_*.png"))):
    print(f"  {os.path.basename(s)}: {os.path.getsize(s):,} bytes")

# Check log
log_path = os.path.join(WORK, "Wasteland.log")
if os.path.exists(log_path):
    sz = os.path.getsize(log_path)
    print(f"\nWasteland.log: {sz} bytes")
    if sz > 0:
        with open(log_path) as f:
            content = f.read()
        print(content[-5000:] if len(content) > 5000 else content)
