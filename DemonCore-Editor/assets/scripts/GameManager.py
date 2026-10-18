# Wasteland Engine Script — GameManager
# Tracks game state: score, kills, items collected, win/lose conditions.
# Attach to a dedicated "GameManager" entity.
import Wasteland


class GameManager:
    def __init__(self, entity):
        self.entity = entity
        self.score = 0
        self.kills = 0
        self.itemsCollected = 0
        self.totalItems = 5
        self.totalEnemies = 3
        self.gameOver = False
        self.won = False

    def OnItemPickedUp(self, itemId, effectType):
        self.itemsCollected += 1
        self.score += 100
        if self.itemsCollected >= self.totalItems:
            self.won = True
            self.gameOver = True

    def OnEnemyKilled(self):
        self.kills += 1
        self.score += 250
        if self.kills >= self.totalEnemies:
            self.won = True
            self.gameOver = True

    def OnPlayerDied(self):
        self.gameOver = True
        self.won = False

    def OnUpdateEntity(self, dt):
        pass
