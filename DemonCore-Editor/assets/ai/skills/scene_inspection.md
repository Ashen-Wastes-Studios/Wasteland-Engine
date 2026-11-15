---
name: scene_inspection
description: "Use when inspecting scene entities, components, or script state."
version: 1.0.0
author: Wasteland
license: MIT
platforms: [windows, linux, macos]
metadata:
  wasteland:
    tags: [scene, entities, components]
    related_skills: [file_ops, python_sandbox]
---

# Scene Inspection Skill

## Rules
- Use `list_entities` first to discover what exists.
- Use `get_entity_info` for detailed component data.
- Use `get_script_state` only when debugging scripted entities.
- Never attempt to mutate entities directly; suggest edits to scene files instead.
