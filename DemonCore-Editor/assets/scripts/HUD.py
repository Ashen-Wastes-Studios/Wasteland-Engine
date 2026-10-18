# Wasteland Engine Script — HUD
# Renders the demo game HUD: health bar, ammo counter, crosshair,
# interaction prompt, score, and win/lose overlay.
import Wasteland


class HUD:
    def __init__(self, entity):
        self.entity = entity
        self.player = None
        self.gameManager = None

    def OnUpdateEntity(self, dt):
        if not self.entity.IsValid():
            return

        # Find player and game manager
        if self.player is None or self.gameManager is None:
            scene = Wasteland.Scene.GetActiveScene()
            if scene is None:
                return
            for e in scene.GetEntities():
                if e.HasTag():
                    tag = e.GetTag().Tag
                    if tag == "Player" and self.player is None:
                        self.player = e
                    elif tag == "GameManager" and self.gameManager is None:
                        self.gameManager = e

        # Get player state
        health = None
        weapon = None
        view = None
        if self.player is not None and self.player.IsValid():
            health = self.player.TryGetHealthComponent()
            weapon = self.player.TryGetWeaponComponent()
            view = self.player.TryGetViewStateComponent()

        # Get interaction prompt from player's InteractableComponent
        prompt = ""
        if self.player is not None and self.player.IsValid():
            interactor = self.player.TryGetInteractableComponent()
            if interactor is not None:
                prompt = interactor.Label

        # Get game state
        score = 0
        kills = 0
        items = 0
        totalItems = 5
        gameOver = False
        won = False
        if self.gameManager is not None and self.gameManager.IsValid():
            score = self.gameManager.TryGetGameManagerComponent().Score
            kills = self.gameManager.TryGetGameManagerComponent().Kills
            items = self.gameManager.TryGetGameManagerComponent().ItemsCollected
            totalItems = self.gameManager.TryGetGameManagerComponent().TotalItems
            gameOver = self.gameManager.TryGetGameManagerComponent().GameOver
            won = self.gameManager.TryGetGameManagerComponent().Won

        # Render HUD
        self.RenderHUD(health, weapon, view, prompt, score, kills, items, totalItems, gameOver, won)

    def RenderHUD(self, health, weapon, view, prompt, score, kills, items, totalItems, gameOver, won):
        # This would use Wasteland's ImGui bindings to render the HUD
        # For now, the actual rendering is handled by the engine's HUD system
        pass

    def OnRuntimeStart(self):
        pass

    def OnRuntimeStop(self):
        pass
