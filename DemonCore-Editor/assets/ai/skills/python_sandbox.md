---
name: python_sandbox
description: "Use when executing Python code via python_exec. Sandbox rules and safe imports."
version: 1.0.0
author: Wasteland
license: MIT
platforms: [windows, linux, macos]
metadata:
  wasteland:
    tags: [python, sandbox, safety]
    related_skills: [file_ops, scene_inspection]
---

# Python Sandbox Skill

## Rules
- Only use whitelisted imports: `math`, `json`, `re`, `datetime`, `wasteland`.
- Never import `os`, `subprocess`, `socket`, `ctypes`, or `sys` for path manipulation.
- Keep scripts short; long-running code is killed by timeout.
- All file paths must be project-relative and validated by the tool layer.
