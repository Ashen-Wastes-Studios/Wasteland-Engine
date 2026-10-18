# Wasteland Engine Script — Pickup
# Attach to any interactable entity. When the player interacts, adds the
# item to the player's inventory and destroys the entity.
import Wasteland


class Pickup:
    def __init__(self, entity):
        self.entity = entity
        self.itemId = "item"
        self.quantity = 1
        self.effectType = "heal"
        self.effectAmount = 50

    def OnUpdateEntity(self, dt):
        pass

    def OnInteract(self, interactor):
        if not self.entity.IsValid():
            return

        # Read item definition from the entity
        itemDef = self.entity.TryGetItemDefinitionComponent()
        if itemDef is not None:
            self.itemId = itemDef.Name
            tags = itemDef.Tags if hasattr(itemDef, 'Tags') else ""
            if "heal" in tags or "consumable" in tags:
                self.effectType = "heal"
                self.effectAmount = 50
            elif "ammo" in tags:
                self.effectType = "ammo"
                self.effectAmount = 24
            elif "weapon" in tags:
                self.effectType = "weapon"
                self.effectAmount = 35
            elif "valuable" in tags:
                self.effectType = "valuables"
                self.effectAmount = 0

        # Add to player inventory
        inventory = interactor.TryGetInventoryComponent()
        if inventory is not None:
            inventory.AddItem(self.itemId, self.quantity, self.effectType)
        else:
            # Fallback: apply effect directly if no inventory component
            if self.effectType == "heal":
                health = interactor.TryGetHealthComponent()
                if health is not None:
                    health.Heal(self.effectAmount)
            elif self.effectType == "ammo":
                weapon = interactor.TryGetWeaponComponent()
                if weapon is not None:
                    weapon.ReserveAmmo += self.effectAmount
            elif self.effectType == "weapon":
                weapon = interactor.TryGetWeaponComponent()
                if weapon is not None:
                    weapon.Damage = self.effectAmount
                    weapon.MaxAmmo = 60
                    weapon.CurrentAmmo = 60
                    weapon.ReserveAmmo = 240

        # Notify game manager
        scene = Wasteland.Scene.GetActiveScene()
        if scene is not None:
            for e in scene.GetEntities():
                if e.HasTag() and e.GetTag().Tag == "GameManager":
                    e.CallScriptMethod("OnItemPickedUp", self.itemId, self.effectType)
                    break

        # Destroy the pickup
        Wasteland.Scene.DestroyEntity(self.entity)
