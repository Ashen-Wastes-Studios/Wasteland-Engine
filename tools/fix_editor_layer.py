import os

path = "DemonCore-Editor/src/EditorLayer.cpp"
with open(path, 'r') as f:
    lines = f.readlines()

# Find the start of the section (++m_FrameCounter)
start = None
for i, line in enumerate(lines):
    if '\t\t++m_FrameCounter;' in line or ('++m_FrameCounter;' in line and '++m_FrameCounter++' not in line):
        start = i
        break

if start is None:
    print("ERROR: Could not find the section to fix")
    exit(1)

# Find the end of the section
for i in range(start, min(start + 30, len(lines))):
    if 'Wasteland::Renderer3D::NotifyFrameTime' in lines[i]:
        end = i
        break

if end is None:
    print("ERROR: Could not find the end of the section")
    exit(1)

# Build the correct section
new_section = [
    '\t\t// temporal history needs a number of frames to converge, so an early shot\n',
    '\t\t// is a black or near-black image that looks like a broken shader. Retrying\n',
    '\t\t// makes the same frame index deterministic (the camera is pinned) while\n',
    '\t\t// giving the tracer time to settle.\n',
    '\t\t++m_FrameCounter;\n',
    '\t\t{\n',
    '\t\t\t// TODO: no-op until the mask readback hook is re-wired.\n',
    '\t\t}\n',
    '\n',
    '\t\tWasteland::Renderer3D::NotifyFrameTime((double)ts.GetMilliseconds());\n',
]

lines[start:end+1] = new_section

with open(path, 'w') as f:
    f.writelines(lines)

print(f"Fixed section from line {start+1} to {end+1}")
