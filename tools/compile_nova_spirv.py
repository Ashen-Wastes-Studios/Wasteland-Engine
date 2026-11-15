#!/usr/bin/env python3
"""
DEPRECATED -- superseded by tools/compile_nova_slang.py.

This script compiled the HLSL twin to SPIR-V via DXC. The Slang single-source
migration replaces it with compile_nova_slang.py, which produces DXIL + SPIR-V
+ GLSL from one .slang source. DXC is no longer used: it produces a non-standard
DXBCu container that CreateComputePipelineState rejects on Windows 11 26200.

Kept only for git history; the build no longer invokes it.
"""
import sys
sys.exit("DEPRECATED: use tools/compile_nova_slang.py instead.")
