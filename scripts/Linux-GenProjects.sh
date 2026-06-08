#!/usr/bin/env bash
set -euo pipefail

# GenProjects.sh - Linux equivalent of Win-GenProjects.bat
# Runs premake (vendor/bin/premake/premake5 or premake5.exe via wine) to
# generate Visual Studio project files and the gmake2 build files.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${SCRIPT_DIR}/.."
cd "${REPO_ROOT}"

echo "Generating Visual Studio Community solutions..."

PREMAKE_BIN="${REPO_ROOT}/vendor/bin/premake/premake5"
PREMAKE_EXE="${REPO_ROOT}/vendor/bin/premake/premake5.exe"

run_premake() {
    local action="$1"
    if [[ -x "${PREMAKE_BIN}" ]]; then
        "${PREMAKE_BIN}" "${action}"
        return $?
    elif command -v wine >/dev/null 2>&1 && [[ -f "${PREMAKE_EXE}" ]]; then
        wine "${PREMAKE_EXE}" "${action}"
        return $?
    else
        echo "premake executable not found (checked: ${PREMAKE_BIN} and ${PREMAKE_EXE})."
        return 2
    fi
}

# Try VS2026, fall back to VS2022 if the action fails
if ! run_premake vs2026; then
    echo "VS2026 action failed or not available, trying VS2022..."
    if ! run_premake vs2022; then
        echo "Both vs2026 and vs2022 actions failed."
    fi
fi

# Generate VS Code / Makefile (gmake2)
echo
echo "Generating VS Code / gmake2 project files..."
if ! run_premake gmake2; then
    echo "gmake2 generation failed."
fi

echo "Done."

# Optional pause similar to Windows PAUSE (uncomment to enable)
# read -rp "Press Enter to continue..." _
