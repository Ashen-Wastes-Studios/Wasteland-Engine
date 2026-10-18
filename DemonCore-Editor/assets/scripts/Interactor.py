# Wasteland Engine Script — Interactor
# Attach alongside Player.py on the player entity. Each frame it traces out
# along the camera's forward direction and, if it hits an entity within
# interactRange, calls that entity's `OnInteract(interactor)` method if one
# exists. Pickups, doors, levers and switches are all implemented by
# defining OnInteract on their own script -- this script needs no changes
# to support them.
#
# Also handles the interaction prompt: the HUD cannot know whether a target
# is in range (that is gameplay state, not UI state), so this script writes
# the prompt text into the InteractableComponent's Label field, and the HUD
# reads it from there. One source of truth, two consumers.
import Wasteland
import math


class Interactor:
    def __init__(self, entity):
        self.entity = entity

        self.interactRange = 3.0
        self.interactKey = Wasteland.WL_KEY_E

        # Seconds between interaction attempts. Prevents a held key from
        # firing a pickup that is meant to be one-shot (a health pack would
        # otherwise be consumed once per frame while E is down).
        self.interactCooldown = 0.35
        self.cooldownTimer = 0.0

        # The entity currently under the crosshair, and the text shown for
        # it. The HUD reads currentTargetLabel.
        self.currentTarget = None
        self.currentTargetLabel = ""
        # Last frame's target, so its prompt can be cleared when the player
        # looks away.
        self.previousTarget = None

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return

        seconds = dt.GetSeconds()
        if seconds <= 0.0:
            return

        self.cooldownTimer = max(0.0, self.cooldownTimer - seconds)

        scene = Wasteland.Scene.GetActiveScene()
        if scene is None:
            return

        transform = self.entity.GetTransform()
        if transform is None:
            return

        # Forward is local -Z at yaw 0, matching Player.py / Weapon.py.
        rot = transform.Rotation
        pitch = rot.x
        yaw = rot.y
        cp = math.cos(pitch)
        forward = Wasteland.Vec3(
            -math.sin(yaw) * cp,
            -math.sin(pitch),
            -math.cos(yaw) * cp)

        hit, hitEntity, point, normal, distance, hittable = scene.Raycast(
            transform.Translation, forward, self.interactRange)

        # Start from a clean state each frame: a target that left the ray
        # must not leave its prompt behind.
        self.currentTarget = None
        self.currentTargetLabel = ""

        if not hit or not hitEntity.IsValid():
            return

        # Only entities that opted into being interactive are candidates.
        # Level geometry must not absorb the keypress.
        target = hitEntity.TryGetInteractableComponent()
        if target is None or not target.Enabled:
            return

        # Clear the stale label on the outgoing target so it does not keep
        # displaying once the player looks away.
        if self.previousTarget is not None and self.previousTarget != hitEntity:
            prev = self.previousTarget.TryGetInteractableComponent()
            if prev is not None:
                prev.Label = ""
        self.previousTarget = hitEntity

        self.currentTarget = hitEntity
        self.currentTargetLabel = target.Label

        if self.cooldownTimer <= 0.0 and Wasteland.IsKeyPressed(self.interactKey):
            self.cooldownTimer = self.interactCooldown
            # Dispatch through the entity's live script instance. The entity
            # from a raycast is a C++ wrapper, so hitEntity.OnInteract(...)
            # would not resolve -- CallScriptMethod looks the method up on the
            # Python object the ScriptEngine built for that entity. Pickups,
            # doors and levers are implemented by defining OnInteract in
            # their own script; no change here is needed to support them.
            hitEntity.CallScriptMethod("OnInteract", self.entity)
