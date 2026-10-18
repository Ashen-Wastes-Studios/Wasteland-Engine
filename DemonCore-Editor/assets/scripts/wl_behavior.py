# Wasteland Engine — wl_behavior
# Behavior tree evaluator. Hand-written and versioned: every compiled tree
# shares this file. It is NOT generated, and it must not be — it is the one
# place the tree semantics live, and duplicating them per-tree would mean
# fixing a bug in every tree instead of once.
#
# -----------------------------------------------------------------------------
# WHY AN EVALUATOR AND NOT GENERATED NESTED if/else
# -----------------------------------------------------------------------------
# The obvious compiler output is a nest of if/else, one branch per node. It is
# simpler and it is wrong, because it cannot express the single thing behavior
# trees exist for: SUSPEND at a latent task, resume THAT node next tick, then
# re-evaluate from the root to see whether a higher-priority branch now wants
# to fire.
#
# Straight-line code has no place to put "where was I". Every attempt to add one
# grows a hand-rolled state machine in the generated output, which is a worse
# evaluator than this one and much worse to debug. So the tree TOPOLOGY is data
# (a dict of nodes and child ids) and this module walks it.
#
# The leaves are generated Python functions in the tree's own module. There is
# no VM and no second binding surface: every engine API a leaf needs is already
# bound in Bindings.cpp.
#
# -----------------------------------------------------------------------------
# PUBLIC API
# -----------------------------------------------------------------------------
#   Result          SUCCESS | FAILURE | RUNNING  (the enum a leaf returns)
#   Task            base class for latent tasks; use ctx.success()/ctx.failure()
#   Context         per-tick ctx handed to a latent leaf (blackboard, seconds)
#   Blackboard      typed-ish dict with VALIDITY distinct from truthiness
#   Tree            the evaluator: tick(seconds), trace hook, running record
#   run_tick(tree, seconds)    convenience wrapper
#
# Topology dict shape (this is what the compiler emits; see NB.3):
#
#   {
#     "root": {"t": "Selector", "kids": ["a", "b"], "abort": "lower"},
#     "a":    {"t": "IsDead", "leaf": <callable>, "latent": False},
#     "b":    {"t": "MoveTo", "kids": [], "leaf": <callable>,
#              "latent": True, "abort": "lower", ...pins},
#   }
#
# "t" is the node type. "kids" are child NODE IDS in PRIORITY ORDER — first is
# highest priority. Children are referred to by id and never by index, so
# inserting a node cannot silently rewire the tree. "leaf" is the generated
# function for leaf nodes. "latent" marks a leaf that may return RUNNING.
# "abort" is "none" | "self" | "lower" and says what re-evaluation may
# interrupt.
#
# A condition/task leaf returns SUCCESS or FAILURE. It receives (ctx).
# A latent task leaf receives (ctx) and may return RUNNING.
#
# -----------------------------------------------------------------------------
# SEMANTICS — these are the rules the whole system rests on
# -----------------------------------------------------------------------------
# Traversal
#   Composite children run in declaration order. A composite returns as soon
#   as a child decides its outcome: Sequence stops at the first FAILURE,
#   Selector stops at the first SUCCESS.
#
# Latent resume
#   A latent leaf returning RUNNING has its id recorded in tree.running. Next
#   tick the search finds that id and resumes AT it — the ancestors' earlier
#   children do NOT re-run. Completing clears the record.
#
# Abort
#   A node whose subtree changes outcome may interrupt the currently running
#   node according to that running node's `abort` mode:
#     "none"  — nothing interrupts it
#     "self"  — its own subtree may preempt it
#     "lower" — any lower-priority branch may preempt it
#   Aborting re-searches from the ROOT, which is the only place a higher-
#   priority branch can be discovered.
#
# Blackboard validity
#   A key is VALID once written, and stays valid even when the value is falsy.
#   `attackAt = 0.0` IS set. Only ClearBlackboard unsets. `BlackboardIsSet`
#   therefore tests validity, never truthiness — a tree that treats 0.0 as
#   "unset" re-fires its cooldown branch forever.
#
# Re-entrancy
#   A tick is never re-entered. A latent task that can never finish is a bug,
#   and Timeout exists to catch it.

import time

SUCCESS = "SUCCESS"
FAILURE = "FAILURE"
RUNNING = "RUNNING"

# Tolerance for every "has this deadline passed?" test.
#
# `self.now` is a running sum of per-tick deltas, so it drifts: after ten
# 0.1s ticks it is 1.0000000000000007, not 1.0, and after five it is
# 0.9999999999999999. A raw `now < deadline` therefore stays True for one
# tick PAST the deadline whenever the accumulated drift lands short of it.
#
# The symptom is a cadence that is one tick long on every interval -- a 1.0s
# cooldown firing every 1.2s at a 0.1s tick -- which looks exactly like a
# wrong timer arithmetic until you print `now` and see 1.2000000000000002
# against a `readyAt` of 2.2. One tick is not a rounding curiosity here: it
# is 20% of the interval, and it compounds across every Cooldown, Timeout
# and Wait in the tree.
#
# Generous relative to the drift (which is ~1e-15 per tick) and far below any
# interval a designer would author, so it cannot swallow a real deadline.
_EPS = 1e-6


def deadline_passed(now, deadline):
    """True once `deadline` has been reached, tolerating accumulated drift."""
    return now >= deadline - _EPS


class Result(object):
    """The three outcomes. Constants on the class so leaves can return either
    `Result.SUCCESS` or the module-level `SUCCESS` — both are the same string,
    and supporting both keeps the compiler's output short."""

    SUCCESS = SUCCESS
    FAILURE = FAILURE
    RUNNING = RUNNING


# Node types. Strings rather than a class hierarchy because the compiler emits
# them as literals, and a dict lookup is the whole dispatch cost.
COMPOSITES = ("Sequence", "Selector", "Parallel")
DECORATORS = ("Invert", "ForceSuccess", "ForceFailure", "Repeat", "Cooldown",
              "Timeout", "SetTimer", "TickEvery")

# Which composites have children.
_HAS_CHILDREN = COMPOSITES + DECORATORS

# Decorators that take exactly one child. Repeat/Cooldown/Timeout/Invert/
# ForceSuccess/ForceFailure wrap a single subtree; SetTimer and TickEvery are
# leaves that also set a pin, so they have none.
_SINGLE_CHILD_DECORATORS = ("Invert", "ForceSuccess", "ForceFailure", "Repeat",
                            "Cooldown", "Timeout")


class Blackboard(object):
    """A dict that distinguishes "never written" from "written as falsy".

    Backed by two dicts: `_values` and `_valid`. Using a sentinel or a separate
    presence test on the values dict would work too, but two dicts make the
    validity question unmissable in the debugger — `bb.valid` is the whole
    reason this class exists."""

    def __init__(self, initial=None, types=None):
        self._values = {}
        self._valid = set()
        self._types = dict(types or {})
        if initial:
            for key, value in initial.items():
                # A declared default counts as WRITTEN. Otherwise a tree
                # declaring {"attackAt": 0.0} would read as unset on tick one
                # and its first branch would fire a frame late.
                self.set(key, value)

    def set(self, key, value):
        self._values[key] = value
        self._valid.add(key)
        return value

    def get(self, key, default=None):
        if key not in self._valid:
            return default
        return self._values.get(key, default)

    def is_set(self, key):
        return key in self._valid

    def clear(self, key):
        self._valid.discard(key)
        self._values.pop(key, None)

    def clear_all(self):
        self._valid.clear()
        self._values.clear()

    def keys(self):
        return list(self._valid)

    def snapshot(self):
        """A flat copy for the trace hook. Values are not deep-copied: an
        Entity in the blackboard must stay the reference it is, and a trace
        that deep-copied entities would be both slow and wrong."""
        return dict((k, self._values.get(k)) for k in self._valid)

    def __len__(self):
        return len(self._valid)

    def __contains__(self, key):
        return key in self._valid


class Context(object):
    """Handed to every leaf. Carries the blackboard, the tick length, the tree
    (for latent bookkeeping) and the agent entity."""

    __slots__ = ("bb", "seconds", "tree", "entity", "now")

    def __init__(self, tree, bb, seconds, entity, now):
        self.tree = tree
        self.bb = bb
        self.seconds = seconds
        self.entity = entity
        self.now = now

    # Leaves return these rather than the bare constants, so generated code
    # reads as intent ("return ctx.failure()") rather than as plumbing.
    def success(self):
        return SUCCESS

    def failure(self):
        return FAILURE

    def running(self):
        return RUNNING


class Tree(object):
    """The evaluator.

    One instance per agent. `running` holds the id of the latent node currently
    holding the tree open, and `tick()` is the only way to advance it.
    """

    def __init__(self, entity, topology, blackboard=None, blackboard_types=None,
                 clock=None):
        self.entity = entity
        self.topology = topology
        self.bb = Blackboard(blackboard, blackboard_types)
        # A monotonic clock, injectable so tests are deterministic. Wall time
        # would make a cooldown test depend on how fast the machine runs.
        self.clock = clock if clock is not None else time.monotonic
        self.now = 0.0
        self.running = None          # node id of the suspended latent node
        self.running_path = []       # ids from root to `running`
        self.ticking = False         # re-entrancy guard
        self.trace = None            # optional callable(node_id, event, tree)
        self.node_states = {}        # per-node runtime scratch (cooldowns etc.)
        self.last_result = None
        # Per-tick scratch, so a trace hook can read it without re-entering.
        self._entered_this_tick = []

    # ---------------------------------------------------------------- ticking

    def tick(self, seconds=0.0):
        """Advance one tick. Returns the root's result.

        Re-entrant calls are ignored rather than allowed to interleave: a leaf
        that ticks its own tree would corrupt `running`, and the failure would
        look like an inexplicable teleport. Ignoring is the safe choice — the
        outer tick still completes and the tree stays coherent.
        """
        if self.ticking:
            if self.trace:
                self.trace("__tree__", "reentrant-tick-ignored", self)
            return self.last_result

        self.ticking = True
        self._entered_this_tick = []
        try:
            if seconds is None or seconds < 0.0:
                seconds = 0.0
            self.now += seconds
            self.last_result = self._evaluate("root", seconds)
            return self.last_result
        finally:
            self.ticking = False
            self._entered_this_tick = []

    def _next_tick_seconds(self):
        return self.node_states.get("__dt__", 0.0)

    # ------------------------------------------------------------- evaluation

    def _is_suspended_under(self, node_id):
        """True if `node_id` is the suspended latent node or one of its
        ancestors.

        Cooldown and Timeout need this to tell "my child is mid-move" from "my
        child is not running at all". The two cases want opposite handling, and
        conflating them is what makes a cooldown cancel the very attack it was
        meant to rate-limit.
        """
        if self.running is None:
            return False
        return node_id == self.running or node_id in self.running_path

    def _evaluate(self, node_id, seconds, ctx=None, path=()):
        """Evaluate one node. Returns SUCCESS / FAILURE / RUNNING.

        `ctx` is built lazily: a tree that short-circuits on its first
        condition never needs one, and allocating a Context per node per tick
        would be the dominant cost at 60fps with many agents.

        `path` is the ancestor id chain, recorded when a latent leaf suspends so
        `_is_suspended_under` can find it again next tick.
        """
        node = self.topology.get(node_id)
        if node is None:
            # A topology naming a node that does not exist is a compile-time
            # bug. Failing the branch is survivable and visible; raising here
            # would take down every NPC in the level over one bad id.
            if self.trace:
                self.trace(node_id, "missing-node", self)
            return FAILURE

        if ctx is None:
            ctx = Context(self, self.bb, seconds, self.entity, self.now)

        ntype = node.get("t", "")
        self._enter(node_id, node)

        child_path = path + (node_id,)

        if ntype == "Sequence":
            result = self._eval_sequence(node, node_id, seconds, ctx, child_path)
        elif ntype == "Selector":
            result = self._eval_selector(node, node_id, seconds, ctx, child_path)
        elif ntype == "Parallel":
            result = self._eval_parallel(node, node_id, seconds, ctx, child_path)
        elif ntype == "Invert":
            result = self._eval_invert(node, node_id, seconds, ctx, child_path)
        elif ntype in ("ForceSuccess", "ForceFailure"):
            result = self._eval_force(node, node_id, seconds, ctx, child_path)
        elif ntype == "Repeat":
            result = self._eval_repeat(node, node_id, seconds, ctx, child_path)
        elif ntype == "Cooldown":
            result = self._eval_cooldown(node, node_id, seconds, ctx, child_path)
        elif ntype == "Timeout":
            result = self._eval_timeout(node, node_id, seconds, ctx, child_path)
        elif ntype == "TickEvery":
            result = self._eval_tick_every(node, node_id, seconds, ctx, child_path)
        elif ntype == "SetTimer":
            result = self._eval_set_timer(node, node_id, ctx)
        elif ntype == "Wait":
            result = self._eval_wait(node, node_id, ctx)
        else:
            result = self._eval_leaf(node, node_id, ctx, child_path)

        # A node that decided must not leave a stale record pointing at itself,
        # or a completed latent node still looks suspended and the next tick
        # resumes something that already finished.
        if result != RUNNING and self.running == node_id:
            self.running = None
            self.running_path = []

        # A node that DECIDED has abandoned any suspension below it that this
        # tick did not travel through. Without this, a higher-priority branch
        # going inactive -- an NPC dying, a target leaving range, a cooldown
        # arming -- leaves a stale latent record behind, and that record then
        # corrupts Cooldown and Timeout for the rest of the run because both
        # consult `_is_suspended_under`.
        if result != RUNNING:
            self._abort_path(path)

        self._exit(node_id, node, result)
        return result

    def _abort_path(self, path):
        """Release a suspension the current tick no longer reaches.

        Re-entry happens from the ROOT every tick, so a higher-priority branch
        gets its chance while a lower one is suspended. If that branch now
        DECIDES, the suspended node underneath is abandoned -- which is exactly
        what a node marked `abort="lower"` promises.

        The tell is `_is_suspended_under` continuing to answer yes for a node
        the tree walked past. That is not a no-op: a Cooldown around it then
        believes its own child is mid-flight and lets it run again, and a
        Timeout measures a deadline from a suspension that ended long ago.
        """
        if self.running is None:
            return
        if self.running in path:
            # Still on the route down to the suspended node: it is being
            # resumed, not abandoned.
            return
        if self.trace:
            self.trace(self.running, "abort", self)
        self.running = None
        self.running_path = []

    # ---- composites

    def _eval_sequence(self, node, node_id, seconds, ctx, path):
        """Succeed on first child success; fail on first child failure.

        A RUNNING child stops the sequence immediately. The later children must
        NOT run while an earlier one is unfinished -- continuing past it would
        run "attack" while "move into range" is still in progress.
        """
        kids = node.get("kids") or []
        for kid in kids:
            result = self._evaluate(kid, seconds, ctx, path)
            if result == RUNNING:
                return RUNNING
            if result == FAILURE:
                return FAILURE
        return SUCCESS

    def _eval_selector(self, node, node_id, seconds, ctx, path):
        """Fail on first child failure; succeed on first child success."""
        kids = node.get("kids") or []
        for kid in kids:
            result = self._evaluate(kid, seconds, ctx, path)
            if result == RUNNING:
                return RUNNING
            if result == SUCCESS:
                return SUCCESS
        return FAILURE

    def _eval_parallel(self, node, node_id, seconds, ctx, path):
        """Run all children; decide by policy.

        success_policy / fail_policy are "one" or "all", both defaulting to
        "all" -- a Parallel that succeeded on the first child to finish would
        just be a Selector with extra steps.
        """
        kids = node.get("kids") or []
        succ_policy = node.get("success", node.get("successPolicy", "all"))
        fail_policy = node.get("fail", node.get("failPolicy", "all"))

        if not kids:
            # An empty Parallel succeeding makes a half-built tree silently
            # "work". Failing is the honest answer.
            return FAILURE

        successes = 0
        failures = 0
        runnings = 0
        for kid in kids:
            result = self._evaluate(kid, seconds, ctx, path)
            if result == SUCCESS:
                successes += 1
                if succ_policy == "one":
                    return SUCCESS
            elif result == FAILURE:
                failures += 1
                if fail_policy == "one":
                    return FAILURE
            else:
                runnings += 1

        total = len(kids)
        if succ_policy == "all" and successes == total:
            return SUCCESS
        if fail_policy == "all" and failures == total:
            return FAILURE
        # Mixed results with "all" policies. If anything is still running, the
        # subtree is unfinished: reporting a decision here would let a parent
        # Sequence walk past a branch that has not run yet.
        if runnings > 0:
            return RUNNING
        return FAILURE

    # ---- decorators

    def _eval_invert(self, node, node_id, seconds, ctx, path):
        """Swap success and failure -- but pass RUNNING through UNCHANGED.

        Inverting RUNNING is the classic bug here. RUNNING is not a failure, it
        is "not finished", and turning it into SUCCESS makes an Invert parent
        report done while its child is still mid-move.
        """
        kids = node.get("kids") or []
        if not kids:
            return FAILURE
        result = self._evaluate(kids[0], seconds, ctx, path)
        if result == RUNNING:
            return RUNNING
        return FAILURE if result == SUCCESS else SUCCESS

    def _eval_force(self, node, node_id, seconds, ctx, path):
        kids = node.get("kids") or []
        if not kids:
            return FAILURE
        result = self._evaluate(kids[0], seconds, ctx, path)
        if result == RUNNING:
            return RUNNING
        return SUCCESS if node.get("t") == "ForceSuccess" else FAILURE

    def _eval_repeat(self, node, node_id, seconds, ctx, path):
        """Re-run the child `count` times, then succeed.

        The counter is per-node runtime state, not a local: a local restarts at
        zero every tick and the loop never finishes for any count above one.
        """
        kids = node.get("kids") or []
        if not kids:
            return FAILURE
        count = int(node.get("count", 1))
        state = self.node_states.setdefault(node_id, {})
        done = state.get("done", 0)
        break_key = node.get("breakKey")

        while done < count:
            result = self._evaluate(kids[0], seconds, ctx, path)
            if result == RUNNING:
                return RUNNING
            if result == FAILURE:
                # A failed iteration ends the repeat. Failing rather than
                # succeeding is what lets an enclosing Sequence see it.
                state["done"] = 0
                return FAILURE
            done += 1
            # Per-iteration break condition, checked AFTER the increment so the
            # final iteration can still satisfy it.
            if break_key is not None and self.bb.is_set(break_key):
                if not self.bb.get(break_key):
                    break

        state["done"] = 0
        return SUCCESS

    def _eval_cooldown(self, node, node_id, seconds, ctx, path):
        """Block re-entry for `seconds` after the child COMPLETES.

        While blocked this returns FAILURE, not RUNNING. RUNNING would park the
        tree here for the whole cooldown and block every sibling below it; the
        node's job is to say "not now", which is failure, so the parent falls
        through to the next branch.

        An already-suspended child is allowed to CONTINUE. The cooldown gates
        entry, not progress.
        """
        kids = node.get("kids") or []
        if not kids:
            return FAILURE
        wait = float(node.get("seconds", 0.0))
        state = self.node_states.setdefault(node_id, {})
        ready_at = state.get("readyAt")

        child_running = self._is_suspended_under(kids[0])

        if ready_at is not None and not deadline_passed(self.now, ready_at) \
                and not child_running:
            return FAILURE

        result = self._evaluate(kids[0], seconds, ctx, path)
        if result == RUNNING:
            return RUNNING

        # Only a SUCCESSFUL child arms the cooldown. Failing means "did not
        # happen", and arming on failure means a target that is out of range
        # silences the attack for a full interval after every miss.
        if result == SUCCESS:
            state["readyAt"] = self.now + wait
        state["lastResult"] = result
        return result

    def _eval_timeout(self, node, node_id, seconds, ctx, path):
        """Fail the subtree if the child runs longer than `seconds`.

        The deadline starts the first tick the child is found suspended, and is
        cleared when the child finishes. That way a Timeout around a long Wait
        works, while a Timeout around a repeatedly-visited condition does not
        accumulate across unrelated visits.
        """
        kids = node.get("kids") or []
        if not kids:
            return FAILURE
        limit = float(node.get("seconds", 0.0))
        state = self.node_states.setdefault(node_id, {})

        if self._is_suspended_under(kids[0]):
            # `startedAt` must be stamped on the tick the child FIRST suspends,
            # not the tick after it. The child returns RUNNING during this same
            # evaluation, so arming only on the way back in skipped the first
            # frame and, worse, re-stamped `startedAt` on every subsequent tick
            # -- which made the deadline reset forever and the Timeout could
            # never fire. A latent task that blocks forever is exactly what this
            # node exists to catch.
            # `None` means "not armed". Clearing must therefore DELETE the
            # key, not set it to None -- a `"startedAt" in state` test would
            # then see a stale armed state forever.
            if state.get("startedAt") is None:
                state["startedAt"] = self.now
            # Same drift tolerance as Cooldown/Wait: `now` is a running sum,
            # so a raw >= fires a tick late whenever the accumulation lands
            # short of the boundary.
            if deadline_passed(self.now, state["startedAt"] + limit):
                # Abandon the child. The latent record is cleared, or the tree
                # would resume a node its parent already gave up on.
                self.running = None
                self.running_path = []
                state.pop("startedAt", None)
                state["timedOut"] = True
                return FAILURE
        else:
            state.pop("startedAt", None)

        result = self._evaluate(kids[0], seconds, ctx, path)
        if result == RUNNING:
            # The child suspended during THIS evaluation; stamp the deadline
            # now, at the moment of suspension.
            if state.get("startedAt") is None:
                state["startedAt"] = self.now
            return RUNNING
        state.pop("startedAt", None)
        return result

    def _eval_tick_every(self, node, node_id, seconds, ctx, path):
        """Skip this subtree unless `seconds` have elapsed. The LOD node.

        Returns FAILURE when skipped so the parent falls through to a cheaper
        sibling, rather than RUNNING -- RUNNING would park the whole tree for
        the off-frames and stop the low-priority branches entirely.
        """
        kids = node.get("kids") or []
        interval = float(node.get("seconds", 0.0))
        state = self.node_states.setdefault(node_id, {})
        last = state.get("lastTick")

        if last is not None and (self.now - last) < interval:
            return FAILURE

        state["lastTick"] = self.now
        if not kids:
            return SUCCESS
        return self._evaluate(kids[0], seconds, ctx, path)

    def _eval_set_timer(self, node, node_id, ctx):
        """Set a blackboard float to `now + seconds`, stamped ONCE.

        A leaf rather than a decorator: it has no child and needs no traversal.

        The deadline is stamped on the FIRST visit and then HELD, and that is the
        whole point of the node. The tree is re-entered from the ROOT every tick,
        so a SetTimer sitting in a Sequence is visited on EVERY tick; re-deriving
        `self.now + delay` on each visit slides the deadline forward forever and
        the timer NEVER expires. That is the same bug shape as `Wait`'s deadline
        (`_eval_wait`, below) and `Timeout`'s (`_eval_timeout`), and it is
        invisible in a trace: every visit reports a clean SUCCESS while the key it
        writes is always `seconds` away, so the branch gated on it never fires.

        Holding rather than self-re-arming is also what makes the node USEFUL. A
        downstream `Compare(now >= attackAt)` needs the value to become a PAST
        timestamp; a timer that re-armed the instant it fired would leave the key
        permanently in the future and that comparison could never be true. An
        author who wants a REPEATING timer puts this node inside a `Cooldown` or
        `Repeat` -- which is where repetition belongs, and where it is visible in
        the tree rather than hidden inside a leaf.

        The blackboard write happens on every visit, so the key stays VALID even if
        something cleared it. It writes the same number each time, so it is
        idempotent -- no tick-order dependence.
        """
        key = node.get("pin") or node.get("key")
        if key is None:
            return FAILURE
        state = self.node_states.setdefault(node_id, {})
        due = state.get("due")
        # `is None` and not a falsy test: a zero delay at now=0 gives due=0.0,
        # which is a REAL deadline and must not be re-stamped on the next tick.
        if due is None:
            due = self.now + float(node.get("seconds", 0.0))
            state["due"] = due
        self.bb.set(key, due)
        return SUCCESS

    def _eval_wait(self, node, node_id, ctx):
        """Suspend for `seconds`, then succeed.

        A LEAF, not a decorator: it has no child and needs no traversal. It is
        latent by construction -- a wait that returned immediately would let the
        parent run its next sibling in the same tick, and a wait that blocked
        without recording itself would park the whole tree.

        The deadline is stored PER NODE, not read fresh each tick: re-deriving
        it from `now` on every visit restarts the timer, so a Wait reached on
        consecutive ticks never finishes. This is the same bug shape as the
        `Timeout` deadline, and for the same reason.

        A Wait with no `seconds` is treated as zero -- succeeding immediately --
        rather than never finishing, because a hang is the worse failure.
        """
        seconds = float(node.get("seconds", 0.0))
        state = self.node_states.setdefault(node_id, {})
        deadline = state.get("deadline")
        if deadline is None:
            deadline = self.now + seconds
            state["deadline"] = deadline
        if not deadline_passed(self.now, deadline):
            return RUNNING
        # Finished: clear the record so the next visit starts a fresh wait
        # instead of succeeding forever on a deadline in the past.
        del state["deadline"]
        return SUCCESS

    # ---- leaves

    def _eval_leaf(self, node, node_id, ctx, path):
        """Run a generated condition/task leaf.

        Latency is DECLARED by the node, not inferred from the result. A task
        returning RUNNING without being declared latent re-enters its own body
        every tick and duplicates partial work; a task declared latent that
        never returns RUNNING simply never suspends. Both are authoring
        mistakes the trace should expose, not things to guess at.

        On resume: the tree is re-entered from the ROOT every tick, and that is
        precisely how abort works -- a higher-priority branch gets its chance
        while a lower one is suspended. "Resume" therefore means the latent
        leaf's own STATE is preserved so it continues rather than restarts. The
        leaf holds that state on the tree (`node_states`); the evaluator's job
        is only to keep calling it and to keep `running` accurate.
        """
        leaf = node.get("leaf")
        if leaf is None:
            if self.trace:
                self.trace(node_id, "no-leaf-body", self)
            return FAILURE

        result = leaf(ctx)

        if result == RUNNING:
            if self.running != node_id:
                self.running = node_id
                self.running_path = list(path)
        elif self.running == node_id:
            self.running = None
            self.running_path = []

        return result

    # ---------------------------------------------------------------- tracing

    def _enter(self, node_id, node):
        if self.trace is None:
            return
        self._entered_this_tick.append(node_id)
        self.trace(node_id, "enter", self)

    def _exit(self, node_id, node, result):
        if self.trace is None:
            return
        self.trace(node_id, "exit:%s" % result, self)

    def set_trace(self, fn):
        self.trace = fn


def run_tick(tree, seconds=0.0):
    """Convenience wrapper: advance a tree one tick."""
    return tree.tick(seconds)


def trace_to_list(tree, sink=None):
    """Install a trace hook that appends to a list.

    Ships WITH the compiler rather than after it: a visual language with no
    per-node trace is unfixable by the people who have to fix it, and the trace
    is the only window into which branch fired.
    """
    sink = sink if sink is not None else []
    tree.set_trace(lambda node_id, event, t: sink.append((node_id, event)))
    return sink


def validate(topology):
    """Structural check of a topology dict.

    Returns a list of error strings naming the offending node — never raises,
    and never silently drops. A tree that quietly loses a task is a designer
    debugging an NPC that behaves like last week's build.
    """
    errors = []
    if not topology:
        return ["topology is empty"]

    if "root" not in topology:
        errors.append("topology has no 'root' node")

    for node_id, node in topology.items():
        if not isinstance(node, dict):
            errors.append("node '%s' is not a mapping" % node_id)
            continue
        ntype = node.get("t")
        if not ntype:
            errors.append("node '%s' has no type" % node_id)
            continue
        if ntype not in _HAS_CHILDREN and ntype not in DECORATORS:
            # A leaf must carry a callable body.
            if node.get("leaf") is None:
                errors.append("leaf node '%s' (%s) has no body" % (node_id, ntype))
        kids = node.get("kids") or []
        if ntype in _SINGLE_CHILD_DECORATORS and len(kids) > 1:
            errors.append("decorator '%s' (%s) takes one child, found %d"
                          % (node_id, ntype, len(kids)))
        if ntype in COMPOSITES and not kids:
            errors.append("composite '%s' (%s) has no children" % (node_id, ntype))
        for kid in kids:
            if kid not in topology:
                errors.append("node '%s' references unknown child '%s'"
                              % (node_id, kid))

    # Cycle check over the child graph.
    def walk(node_id, seen):
        if node_id in seen:
            errors.append("cycle in topology at '%s'" % node_id)
            return
        seen.add(node_id)
        node = topology.get(node_id) or {}
        for kid in node.get("kids") or []:
            if kid in topology:
                walk(kid, seen)
        seen.discard(node_id)

    if "root" in topology:
        walk("root", set())

    return errors
