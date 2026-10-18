# Wasteland Engine Script — Inventory
# Manages the player's carried items in Python (the C++ InventoryComponent
# is not in the scene deserializer, so inventory state lives here).
#
# Attach to the player entity. The Pickup script calls AddItem() when the
# player interacts with a pickup. The HUD reads GetItems() to display them.
import Wasteland


class Inventory:
    def __init__(self, entity):
        self.entity = entity
        self.items = []  # list of {id, name, quantity, type, weight}
        self.maxSlots = 20
        self.maxWeight = 100.0

    def AddItem(self, itemId, quantity=1, itemType="misc"):
        """Add an item to the inventory. Returns True if successful."""
        # Check if already in inventory (stack)
        for item in self.items:
            if item['id'] == itemId:
                item['quantity'] += quantity
                return True

        # Check capacity
        if len(self.items) >= self.maxSlots:
            return False

        # Get display name from the pickup entity if possible
        displayName = itemId
        scene = Wasteland.Scene.GetActiveScene()
        if scene is not None:
            for e in scene.GetEntities():
                if e.HasTag():
                    tag = e.GetTag().Tag
                    if tag == itemId or tag == itemId.replace('_', ' ').title():
                        itemDef = e.TryGetItemDefinitionComponent()
                        if itemDef is not None:
                            displayName = itemDef.DisplayName
                            break

        self.items.append({
            'id': itemId,
            'name': displayName,
            'quantity': quantity,
            'type': itemType,
            'weight': 0.0
        })
        return True

    def RemoveItem(self, itemId, quantity=1):
        """Remove a quantity of an item. Returns True if successful."""
        for i, item in enumerate(self.items):
            if item['id'] == itemId:
                item['quantity'] -= quantity
                if item['quantity'] <= 0:
                    self.items.pop(i)
                return True
        return False

    def GetItems(self):
        """Return the list of items."""
        return self.items

    def GetItemCount(self, itemId):
        """Get the quantity of a specific item."""
        for item in self.items:
            if item['id'] == itemId:
                return item['quantity']
        return 0

    def HasItem(self, itemId):
        """Check if the inventory contains an item."""
        return self.GetItemCount(itemId) > 0

    def OnUpdateEntity(self, dt):
        pass
