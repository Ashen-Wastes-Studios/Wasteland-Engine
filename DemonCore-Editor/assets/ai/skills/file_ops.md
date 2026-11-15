---
name: file_ops
description: "Use when reading or writing project files. Safe file patterns and path rules."
version: 1.0.0
author: Wasteland
license: MIT
platforms: [windows, linux, macos]
metadata:
  wasteland:
    tags: [files, safety, project]
    related_skills: [scene_inspection, python_sandbox]
---

# File Operations Skill

## Rules
- Always use project-relative paths.
- Never read or write files outside the project root.
- Prefer `read_file` before `write_file` to inspect existing content.
- Use `edit_file` for surgical changes; supply exact old text.
