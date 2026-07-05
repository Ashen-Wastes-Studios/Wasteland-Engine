# Running Wasteland Engine

## Step-by-Step Guide for All Operating Systems

### Prerequisites (All Platforms)

Before getting started, ensure you have the following installed:

#### Windows
- **Visual Studio 2022 or newer** (with C++ workload)
- **Python 3.14** (or compatible version)
- **CMake** (optional, for advanced builds)

#### Linux
- **GCC or Clang** compiler
- **Make** and **GNU Make tools**
- **Python 3.14** (or compatible version)
- **Development libraries**: `libglfw3-dev`, `libgl1-mesa-dev`
  ```bash
  sudo apt-get install build-essential libglfw3-dev libgl1-mesa-dev
  ```

#### macOS
- **Xcode Command Line Tools**
  ```bash
  xcode-select --install
  ```
- **Homebrew** (package manager)
- **Python 3.14** (or compatible version)
- **GLFW and OpenGL libraries**
  ```bash
  brew install glfw
  ```

---

### Step 1: Clone the Repository

```bash
git clone https://github.com/Ashen-Wastes-Studios/Wasteland-Engine.git
cd Wasteland-Engine
```

---

### Step 2: Generate Project Files

#### Windows (Visual Studio)

Run the Premake script to generate Visual Studio project files:

```bash
.\scripts\Win-GenProjects.bat
```

Or manually:

```bash
.\vendor\bin\premake\premake5.exe vs2026
```

#### Linux

Run the Premake script to generate Makefiles:

```bash
chmod +x ./scripts/Linux-GenProjects.sh
./scripts/Linux-GenProjects.sh
```

Or manually:

```bash
./vendor/bin/premake/premake5 gmake
```

#### macOS

Run the Premake script to generate Makefiles:

```bash
chmod +x ./scripts/Mac-GenProjects.sh
./scripts/Mac-GenProjects.sh
```

Or manually:

```bash
./vendor/bin/premake/premake5 gmake
```

---

### Step 3: Build the Project

#### Windows (Visual Studio - MSBuild)

**Debug Build:**
```bash
msbuild Wasteland.slnx -property:Configuration=Debug -property:Platform=x64
```

**Release Build:**
```bash
msbuild Wasteland.slnx -property:Configuration=Release -property:Platform=x64
```

Or use VS Code Tasks:
- Open the Command Palette (`Ctrl+Shift+P`)
- Select `Tasks: Run Task`
- Choose `Build: MSBuild (Debug)` or `Build: MSBuild (Release)`

#### Linux & macOS (Make)

**Debug Build:**
```bash
make config=debug_x64
```

**Release Build:**
```bash
make config=release_x64
```

---

### Step 4: Run the Application

After a successful build, the executables are in the `bin/` directory.

#### Windows

**DemonCore Editor (Debug):**
```bash
.\bin\Debug-windows-x86_64\DemonCore-Editor\DemonCore-Editor.exe
```

**DemonCore Editor (Release):**
```bash
.\bin\Release-windows-x86_64\DemonCore-Editor\DemonCore-Editor.exe
```

**Sandbox (Debug):**
```bash
.\bin\Debug-windows-x86_64\Sandbox\Sandbox.exe
```

**Sandbox (Release):**
```bash
.\bin\Release-windows-x86_64\Sandbox\Sandbox.exe
```

#### Linux & macOS

**DemonCore Editor (Debug):**
```bash
./bin/Debug-linux-x86_64/DemonCore-Editor/DemonCore-Editor
# or on macOS:
./bin/Debug-macosx-x86_64/DemonCore-Editor/DemonCore-Editor
```

**DemonCore Editor (Release):**
```bash
./bin/Release-linux-x86_64/DemonCore-Editor/DemonCore-Editor
# or on macOS:
./bin/Release-macosx-x86_64/DemonCore-Editor/DemonCore-Editor
```

**Sandbox (Debug):**
```bash
./bin/Debug-linux-x86_64/Sandbox/Sandbox
# or on macOS:
./bin/Debug-macosx-x86_64/Sandbox/Sandbox
```

**Sandbox (Release):**
```bash
./bin/Release-linux-x86_64/Sandbox/Sandbox
# or on macOS:
./bin/Release-macosx-x86_64/Sandbox/Sandbox
```

---

### Step 5: Verify Installation

Run the Sandbox application to verify everything is working correctly. You should see:
- The Wasteland Engine window opening
- The editor interface (if running DemonCore-Editor)
- No console errors

---

## Troubleshooting

### Python Paths Issue
**Note:** Python paths are currently hardcoded in the project configuration. You may need to update them in:
- `premake5.lua` - Adjust Python include and lib paths to match your installation
- `.vcxproj` files (Windows) - Update Python paths in project properties

### Missing Dependencies
- **Windows:** Ensure Visual Studio C++ workload is installed via Visual Studio Installer
- **Linux:** Run `sudo apt-get install build-essential libglfw3-dev libgl1-mesa-dev`
- **macOS:** Run `brew install glfw` and ensure Xcode Command Line Tools are installed

### Build Failures
- Clean the build: Remove `bin/`, `bin-int/`, and `Makefile` directories, then regenerate projects
- Regenerate project files using Premake (Step 2)
- Check that all vendor dependencies are present in the `vendor/` directory

---

## Additional Notes

- **Python Scripting Engine:** The Python scripting engine is a work in progress. Contributions via pull request are welcome!
- **Supported Configurations:** Debug and Release builds for x86_64 architecture
- **Editor:** DemonCore-Editor is the main development tool for creating and editing projects
- **Sandbox:** The Sandbox application is a testing environment for the engine

---