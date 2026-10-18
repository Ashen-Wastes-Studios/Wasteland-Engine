# Wasteland Engine Script — Crate
# A trivial interactable used to exercise the interaction dispatch path.
# Interactor.py finds this entity, and pressing E calls OnInteract on it.


class Crate:
    def __init__(self, entity):
        self.entity = entity
        # Number of times the crate has been opened. The test reads this
        # back off the live script instance to prove dispatch really ran.
        self.timesOpened = 0

    def OnInteract(self, interactor):
        self.timesOpened += 1
