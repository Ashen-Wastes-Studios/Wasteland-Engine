#!/usr/bin/env python3
"""
Analytic-light admission: priority quota + overflow reporting, verified standalone.

WHY THIS EXISTS: `Renderer3D::PushAnalyticLight` used to be

    if (s_Data.AnalyticLights.size() >= (size_t)Renderer3D::MaxAnalyticLights)
        return;
    s_Data.AnalyticLights.push_back(v);

which is first-come admission into a cap of 8 (`Renderer3D.h`,
`MaxAnalyticLights`). `Scene::SubmitSceneLights` submits lights in TYPE order
(directional, then point, then spot, then area), each in its own entt view
loop, so a scene with 8 or more point lights filled all 8 slots before a single
spot or area light was ever offered. Every spot and area light in the scene was
dropped -- no log, no counter, no error. That is a correctness bug, not a
performance trade: the lights exist in the scene and simply do not appear, and
the symptom ("my spotlight vanished") is indistinguishable from a typo'd
component or a bad transform.

The fix has two halves, and this checks both:

  1. ADMISSION IS PRIORITY-BASED. A light type with no seat gets one, by
     evicting the least important light from a type that holds a surplus.
     Remaining ties are broken by a camera-distance importance score
     (`AnalyticLightImportance`: intensity / (1 + squared distance to camera),
     with directional scoring 0 because it has no position).

  2. THE DROP IS OBSERVABLE. Submitted / admitted / dropped are counted per
     frame and reported through `WL_CORE_WARN` at most once per frame, and only
     when the dropped count changes (`ReportAnalyticLightOverflow`).

The engine cap is fixed at 8 because the shader's light arrays are fixed size
(`UploadAnalyticLights` clamps into `glm::vec3 pos[MaxAnalyticLights]`), so this
test asserts the cap is unchanged rather than asking for it to be raised.

NEGATIVE CONTROL: the same assertions are re-run against four deliberately
broken admissions, including a verbatim port of the ORIGINAL first-come code.
The run FAILS if any broken variant passes -- a test that cannot fail is not a
test. Same guard `verify_variance_math.py` and `generate_blue_noise.py` use.

The admission logic is SIMULATED here, not imported: no engine link, no GPU, no
numpy, standard library only. `PushAnalyticLight` is transcribed statement for
statement from `Renderer3D.cpp` so the simulation cannot drift into describing
behaviour the engine does not have -- see the `PushAnalyticLight` docstring for
the line-by-line correspondence.

Run: python3 tools/verify_analytic_light_admission.py  (0 = all green, 1 = failed)
"""

import math
import sys

# The engine cap. Fixed by the shader's fixed-size light arrays
# (Renderer3D.h: MaxAnalyticLights; UploadAnalyticLights clamps to it). This
# test asserts it is NOT raised -- widening the CPU list without widening the
# upload is the literal-vs-named-constant hazard the engine comment warns about.
MAX_ANALYTIC_LIGHTS = 8

DIRECTIONAL, POINT, SPOT, AREA = 0, 1, 2, 3

TYPE_NAMES = {DIRECTIONAL: "directional", POINT: "point", SPOT: "spot", AREA: "area"}

_INF = float("inf")


class Light:
    """The subset of AnalyticLightData the admission logic reads.

    Mirrors Renderer3D.cpp's struct: Type is a float in the engine (it is packed
    into a vec4 for the upload) but an int here, because the admission code only
    ever compares whole type ids and the float would obscure that.
    """

    __slots__ = ("type", "pos", "intensity", "name")

    def __init__(self, type_, pos=(0.0, 0.0, 0.0), intensity=1.0, name=""):
        self.type = type_
        self.pos = tuple(pos)
        self.intensity = intensity
        self.name = name

    def __repr__(self):
        tag = self.name or TYPE_NAMES.get(self.type, str(self.type))
        return "%s@%s" % (tag, self.pos)


class Admission:
    """The per-frame analytic-light list, exactly as Renderer3D keeps it.

    Holds what `s_Data` holds for this purpose: the admitted vector plus the
    three counters (`AnalyticLightsSubmitted`, `AnalyticLightsDropped`, and
    `AnalyticLightsLastReportedDropped`). `camera_pos` stands in for
    `s_Data.CurrentCameraPosition`, which `BeginScene` writes before the Scene
    submits any light -- so it is genuinely available to the admission path with
    no new plumbing. That is what licenses a real distance metric instead of the
    type-quota fallback.
    """

    def __init__(self, camera_pos=(0.0, 0.0, 0.0), cap=MAX_ANALYTIC_LIGHTS):
        self.cap = cap
        self.camera_pos = tuple(camera_pos)
        self.lights = []
        self.submitted = 0
        self.dropped = 0
        self.last_reported_dropped = -1
        self.warnings = []  # every line WL_CORE_WARN would have emitted

    # -- AnalyticLightImportance ------------------------------------------
    def importance(self, light):
        """Transcribed from `AnalyticLightImportance` in Renderer3D.cpp.

        Directional scores 0: it has no position, so distance is meaningless,
        and the sun must not lose a slot to a nearby torch. Squared distance,
        no sqrt, no inverse square -- the ranking only needs to be monotone.
        """
        if light.type == DIRECTIONAL:
            return 0.0
        dx = light.pos[0] - self.camera_pos[0]
        dy = light.pos[1] - self.camera_pos[1]
        dz = light.pos[2] - self.camera_pos[2]
        return max(light.intensity, 0.0) / (1.0 + dx * dx + dy * dy + dz * dz)

    # -- PushAnalyticLight -------------------------------------------------
    def push(self, light):
        """Transcribed from `PushAnalyticLight` in Renderer3D.cpp.

        Rule 1 (type quota): if the list is full and this light's type has no
        seat, evict the least important light belonging to a type that holds
        more than one -- never a type's last member, which would only move the
        starvation. This admission is UNCONDITIONAL: the type has no seat, so it
        gets one, and comparing importance first is exactly what reintroduced the
        bug (a directional scores 0 by construction and so lost every comparison).
        Rule 2 (importance tiebreak): otherwise contend with the least important
        admitted light of the SAME type, and drop the loser if the incumbent is
        at least as important.

        The evicted light counts as a drop too, so submitted == admitted + dropped
        holds exactly -- including the eviction case, which is precisely when the
        count matters most.
        """
        self.submitted += 1
        if len(self.lights) < self.cap:
            self.lights.append(light)
            return

        count = [0, 0, 0, 0]
        for e in self.lights:
            count[e.type & 3] += 1
        t = light.type & 3
        victim = -1
        quota_applies = False

        if count[t] == 0:
            quota_applies = True
            worst = _INF
            for i, e in enumerate(self.lights):
                if count[e.type & 3] < 2:
                    continue
                imp = self.importance(e)
                if imp < worst:
                    worst, victim = imp, i
            # Unreachable at cap 8 with 4 types; falls back to the tiebreak.
            if victim == -1:
                quota_applies = False

        if victim == -1:
            worst = _INF
            for i, e in enumerate(self.lights):
                if (e.type & 3) != t:
                    continue
                imp = self.importance(e)
                if imp < worst:
                    worst, victim = imp, i

        if victim == -1:
            self.dropped += 1
            return

        if not quota_applies and self.importance(light) <= self.importance(self.lights[victim]):
            self.dropped += 1
            return

        self.lights[victim] = light
        self.dropped += 1

    # -- ReportAnalyticLightOverflow --------------------------------------
    def report(self):
        """Transcribed from `ReportAnalyticLightOverflow` in Renderer3D.cpp.

        Called from BeginScene BEFORE the list is cleared, so it reports the
        frame that just ended. Emits at most one line per frame and only when
        the dropped count differs from the last one reported: a permanently
        over-budget scene would otherwise print one line per frame forever.
        Silent while the scene fits.
        """
        if self.dropped != self.last_reported_dropped:
            self.last_reported_dropped = self.dropped
            if self.dropped > 0:
                self.warnings.append(
                    "Analytic lights over budget: %d submitted, %d admitted, "
                    "%d dropped (cap %d)."
                    % (self.submitted, len(self.lights), self.dropped, self.cap))
        self.submitted = 0
        self.dropped = 0

    def type_counts(self):
        c = [0, 0, 0, 0]
        for e in self.lights:
            c[e.type & 3] += 1
        return c

    def has(self, name):
        return any(e.name == name for e in self.lights)


class FirstComeAdmission(Admission):
    """NEGATIVE CONTROL 1: the ORIGINAL shipped code, transcribed verbatim.

        if (s_Data.AnalyticLights.size() >= (size_t)Renderer3D::MaxAnalyticLights)
            return;
        s_Data.AnalyticLights.push_back(v);

    This is the bug. It must FAIL the assertions below; if it passes, those
    assertions are not testing what they claim to test.
    """

    def push(self, light):
        self.submitted += 1
        if len(self.lights) >= self.cap:
            return  # silent drop -- no counter, no log
        self.lights.append(light)


class NoCountersAdmission(FirstComeAdmission):
    """NEGATIVE CONTROL 2: original admission, but with the new counters.

    Isolates requirement (a) from (b). This one still starves -- that is
    expected and is why it must fail. It exists to prove the starvation
    assertions are not accidentally satisfied by the counters alone.
    """

    def push(self, light):
        self.submitted += 1
        if len(self.lights) >= self.cap:
            self.dropped += 1
            return
        self.lights.append(light)


class FirstAdmissionAdmission(Admission):
    """NEGATIVE CONTROL 3: quota removed, pure importance across ALL types.

    The plausible-but-wrong "fix": rank every light by distance and keep the
    best 8. This is what a reviewer would write if they read the bug as "drop
    unimportant lights" instead of "drop a whole light TYPE". It still starves,
    because a cluster of 8 point lights next to the camera outscores a distant
    spot light -- which is exactly the argument the engine comment makes for
    checking the quota BEFORE the score. Must fail.
    """

    def push(self, light):
        self.submitted += 1
        if len(self.lights) < self.cap:
            self.lights.append(light)
            return
        victim = -1
        worst = _INF
        for i, e in enumerate(self.lights):
            imp = self.importance(e)
            if imp < worst:
                worst, victim = imp, i
        if victim == -1 or self.importance(light) <= self.importance(self.lights[victim]):
            self.dropped += 1
            return
        self.lights[victim] = light


class NoDirectionalGuardAdmission(Admission):
    """NEGATIVE CONTROL 4: quota present, but directional scored like the rest.

    The `Type == 0 -> 0` special case in AnalyticLightImportance is not about
    giving the sun a seat -- the unconditional quota already guarantees that. It
    is about WHICH surplus light is sacrificed when a new light type needs a
    slot: a directional scores 0, so a REDUNDANT sun is the cheapest victim and
    gives up its slot first. Score the sun by distance instead and a distant sun
    looks valuable, so the engine sacrifices a distant torch instead and keeps
    the sun -- the opposite of the documented intent. Must fail.
    """

    def importance(self, light):
        dx = light.pos[0] - self.camera_pos[0]
        dy = light.pos[1] - self.camera_pos[1]
        dz = light.pos[2] - self.camera_pos[2]
        return max(light.intensity, 0.0) / (1.0 + dx * dx + dy * dy + dz * dz)


# ---------------------------------------------------------------------------
# Assertion harness
# ---------------------------------------------------------------------------

FAILURES = []
CHECKS = [0]


def check(cond, label, detail=""):
    CHECKS[0] += 1
    if cond:
        print("  PASS  %s" % label)
    else:
        print("  FAIL  %s%s" % (label, (" -- " + detail) if detail else ""))
        FAILURES.append(label)
    return bool(cond)


def scene_point_lights(n, near=True):
    """n point lights submitted in Scene order (point pass runs second)."""
    if near:
        return [Light(POINT, (0.5 * i, 1.0, 0.0), 1.0, "point%d" % i) for i in range(n)]
    return [Light(POINT, (0.5 * i, 1.0, 0.0), 1.0, "farpoint%d" % i) for i in range(n)]


def run_scenario(cls, camera=(0.0, 0.0, 0.0), cap=MAX_ANALYTIC_LIGHTS):
    """Submits in Scene::SubmitSceneLights order: directional, point, spot, area.

    The ordering IS the bug's mechanism, so the scenario has to reproduce it
    rather than submit in some neutral order.
    """
    a = cls(camera_pos=camera, cap=cap)
    a.push(Light(DIRECTIONAL, (0.0, 10.0, 0.0), 1.0, "sun"))       # directional pass
    for l in scene_point_lights(8):                                 # point pass
        a.push(l)
    a.push(Light(SPOT, (2.0, 3.0, 1.0), 5.0, "heroSpot"))           # spot pass
    a.push(Light(AREA, (4.0, 3.0, 1.0), 5.0, "heroArea"))           # area pass
    return a


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_cap_unchanged():
    print("\n[1] The engine cap is unchanged (shader arrays are fixed size)")
    check(MAX_ANALYTIC_LIGHTS == 8, "MaxAnalyticLights is still 8",
          "found %d" % MAX_ANALYTIC_LIGHTS)


def test_old_behaviour_starves():
    print("\n[2] OLD behaviour (first-come) really did starve -- the bug is real")
    a = run_scenario(FirstComeAdmission)
    counts = a.type_counts()
    check(counts[SPOT] == 0, "old code admits 0 spot lights", "got %d" % counts[SPOT])
    check(counts[AREA] == 0, "old code admits 0 area lights", "got %d" % counts[AREA])
    # The directional is submitted first and takes a slot, so 7 of the 8 point
    # lights fit -- enough to fill every remaining slot before the spot pass
    # runs. That is the whole mechanism: the point pass ends with zero free
    # slots, so spot and area never get a look in.
    check(counts[POINT] == MAX_ANALYTIC_LIGHTS - 1,
          "the point pass fills every remaining slot",
          "point count %d" % counts[POINT])
    check(len(a.lights) == MAX_ANALYTIC_LIGHTS, "the old list is exactly full")
    check(a.dropped == 0, "old code drops silently (no counter at all)")


def test_new_behaviour_fixes_starvation():
    print("\n[3] NEW behaviour admits spot and area -- the starvation is fixed")
    a = run_scenario(Admission)
    counts = a.type_counts()
    check(a.has("sun"), "the directional light keeps a slot")
    check(a.has("heroSpot"), "the spot light is admitted",
          "admitted: %r" % (a.lights,))
    check(a.has("heroArea"), "the area light is admitted",
          "admitted: %r" % (a.lights,))
    check(counts[POINT] < 8, "point lights yield slots to the starved types",
          "point count %d" % counts[POINT])
    check(len(a.lights) == MAX_ANALYTIC_LIGHTS, "the list stays exactly at the cap",
          "len %d" % len(a.lights))


def test_overflow_is_counted_and_reported():
    print("\n[4] The drop is counted and reported (was silent)")
    a = run_scenario(Admission)
    admitted = len(a.lights)
    submitted = a.submitted
    dropped = a.dropped
    check(submitted == 11, "submitted counts every candidate", "got %d" % submitted)
    check(dropped == submitted - admitted,
          "dropped == submitted - admitted", "%d != %d - %d" % (dropped, submitted, admitted))
    check(dropped > 0, "this scenario really does overflow", "dropped %d" % dropped)

    a.report()
    check(len(a.warnings) == 1, "exactly one warning line for the frame",
          "got %d" % len(a.warnings))
    line = a.warnings[0] if a.warnings else ""
    for field in (str(submitted), str(admitted), str(dropped)):
        check(field in line, "the warning names the %s count" % field, "line: %r" % line)

    # A second frame with the same overflow must NOT re-log.
    a2 = run_scenario(Admission)
    a2.report()
    a2.report()
    check(len(a2.warnings) == 1, "an unchanged overflow is not re-reported every frame",
          "got %d" % len(a2.warnings))


def test_silent_when_within_budget():
    print("\n[5] A scene that fits stays completely silent")
    a = Admission()
    a.push(Light(DIRECTIONAL, (0.0, 10.0, 0.0), 1.0, "sun"))
    a.push(Light(POINT, (1.0, 1.0, 0.0), 1.0, "p0"))
    a.push(Light(SPOT, (2.0, 1.0, 0.0), 1.0, "s0"))
    a.report()
    check(a.dropped == 0, "nothing is dropped")
    check(len(a.warnings) == 0, "no warning while the scene fits",
          "got %r" % (a.warnings,))
    check(len(a.lights) == 3, "all three admitted")


def test_importance_breaks_ties_within_a_type():
    print("\n[6] Within one type, the nearer/more important light wins")
    # 8 spots, then 9 more. The camera sits at the origin, so importance falls
    # off with distance and the FARTHEST spots are the ones that must lose.
    a = Admission(camera_pos=(0.0, 0.0, 0.0))
    a.push(Light(DIRECTIONAL, (0.0, 10.0, 0.0), 1.0, "sun"))
    for i in range(8):
        a.push(Light(SPOT, (100.0 * (i + 1), 1.0, 0.0), 1.0, "far%d" % i))
    # The nearest spot arrives last and must win a slot.
    a.push(Light(SPOT, (0.5, 1.0, 0.0), 1.0, "nearest"))
    check(a.has("nearest"), "the last-submitted but nearest spot is admitted",
          "admitted: %r" % (a.lights,))
    check(not a.has("far7"), "the farthest spot is the one evicted")
    # Two drops, not one: far7 is rejected outright on arrival, and far6 is
    # admitted and then EVICTED to make room for the near spot. Counting only
    # the rejection would report 1 and under-state the frame by exactly the
    # number of evictions.
    check(a.dropped == 2, "both the rejection and the eviction are counted",
          "dropped %d" % a.dropped)
    check(a.submitted - a.dropped == len(a.lights),
          "submitted - dropped == admitted in the eviction case")

    # And the reverse: a distant late arrival must NOT displace a near one.
    b = Admission(camera_pos=(0.0, 0.0, 0.0))
    for i in range(8):
        b.push(Light(SPOT, (0.5 + i, 1.0, 0.0), 1.0, "near%d" % i))
    b.push(Light(SPOT, (9000.0, 1.0, 0.0), 1.0, "distant"))
    check(not b.has("distant"), "a distant late spot does not steal a slot")
    check(b.has("near7"), "the nearer incumbent survives")
    check(b.dropped == 1, "the distant arrival is counted as dropped")


def test_directional_is_never_starved_or_stolen():
    print("\n[7] The key light is protected")
    # Directional submitted LAST, after 8 point lights already hold every slot:
    # it must still get in by evicting a point light.
    a = Admission(camera_pos=(0.0, 0.0, 0.0))
    for l in scene_point_lights(8):
        a.push(l)
    a.push(Light(DIRECTIONAL, (0.0, 10.0, 0.0), 1.0, "lateSun"))
    check(a.has("lateSun"), "a directional light gets a slot even when submitted last")
    check(a.type_counts()[DIRECTIONAL] == 1, "exactly one directional is admitted")

    # Two directionals and 6 points: the surplus directional is the better
    # victim than a point light, but the first one must survive.
    b = Admission(camera_pos=(0.0, 0.0, 0.0))
    b.push(Light(DIRECTIONAL, (0.0, 10.0, 0.0), 1.0, "sunA"))
    for l in scene_point_lights(6):
        b.push(l)
    b.push(Light(DIRECTIONAL, (0.0, 10.0, 0.0), 1.0, "sunB"))
    check(b.has("sunA") or b.has("sunB"), "a directional survives the second sun")
    check(b.type_counts()[DIRECTIONAL] >= 1, "the sun is never fully evicted")


def test_every_type_gets_a_seat():
    print("\n[8] All four types present in the scene means all four are admitted")
    a = Admission(camera_pos=(0.0, 0.0, 0.0))
    n_points = 20
    for l in scene_point_lights(n_points):         # 20 point lights, all near
        a.push(l)
    a.push(Light(DIRECTIONAL, (0.0, 10.0, 0.0), 1.0, "sun"))
    a.push(Light(SPOT, (0.1, 1.0, 0.0), 1.0, "spot"))    # even nearer than any point
    a.push(Light(AREA, (0.1, 1.0, 0.0), 1.0, "area"))
    counts = a.type_counts()
    check(all(c >= 1 for c in counts), "every type has at least one seat", "%r" % counts)
    check(len(a.lights) == MAX_ANALYTIC_LIGHTS, "still exactly at the cap")
    # Every light that is not in the final list is a drop, including the ones
    # evicted to make room. This is the invariant that the naive counter -- which
    # only counted rejects -- silently violated.
    check(a.submitted - a.dropped == len(a.lights),
          "submitted - dropped == admitted",
          "%d - %d != %d" % (a.submitted, a.dropped, len(a.lights)))
    check(a.submitted == n_points + 3, "every candidate was counted as submitted",
          "got %d" % a.submitted)


def test_determinism_and_invariants():
    print("\n[9] Invariants hold across randomised scenes")
    import random

    rng = random.Random(20261004)  # fixed seed: a failure must be reproducible
    bad_cap = bad_count = bad_accounting = 0
    starved_runs = 0
    for _ in range(400):
        a = Admission(camera_pos=(rng.uniform(-5, 5), 1.0, rng.uniform(-5, 5)))
        present = set()
        order = [DIRECTIONAL, POINT, SPOT, AREA]  # Scene submission order
        for t in order:
            for _ in range(rng.randint(0, 12)):
                a.push(Light(t, (rng.uniform(-50, 50), rng.uniform(-50, 50),
                                 rng.uniform(-50, 50)),
                             rng.choice([0.5, 1.0, 5.0, 20.0]), ""))
                present.add(t)
        if len(a.lights) > MAX_ANALYTIC_LIGHTS:
            bad_cap += 1
        if any(e.type & 3 > 3 for e in a.lights):
            bad_count += 1
        if a.submitted - a.dropped != len(a.lights):
            bad_accounting += 1
        # Quota invariant: every type present in the scene got a seat, PROVIDED
        # the cap allows one seat per type (8 >= 4, which it does).
        for t in present:
            if a.type_counts()[t] == 0:
                starved_runs += 1
                break

    check(bad_cap == 0, "the list never exceeds the cap", "%d violations" % bad_cap)
    check(bad_count == 0, "no corrupted type index ever lands in the list")
    check(bad_accounting == 0, "submitted - dropped == admitted, every run",
          "%d violations" % bad_accounting)
    check(starved_runs == 0, "no scene ever starves a light type that exists in it",
          "%d/400 runs starved a present type" % starved_runs)


# ---------------------------------------------------------------------------
# Negative control
# ---------------------------------------------------------------------------

def test_negative_control():
    """Every broken admission must FAIL the property it was supposed to break.

    Each control gets a scenario chosen to expose ITS OWN defect. An earlier
    version ran all four against the shared starvation scenario, and two of them
    passed it -- the quota in the real fix was strong enough to carry them, so
    the scenario was not discriminating and the "control" proved nothing. A
    control that passes is a hole in the test, so the harness fails loudly below
    if any of them survives.
    """
    print("\n[10] NEGATIVE CONTROL -- each broken admission must fail its own check")

    # (a) The starvation scenario: the exact bug as reported.
    def scenario_starvation(cls):
        return run_scenario(cls)

    def prop_starvation(a):
        c = a.type_counts()
        return c[SPOT] >= 1 and c[AREA] >= 1

    # (b) Pure importance must not be able to rescue a starved type: 8 torches
    # clustered ON the camera, then a spot light right next to it. Every torch
    # outscores the spot on distance, so only the quota can admit it.
    def scenario_clustered(cls):
        a = cls(camera_pos=(0.0, 0.0, 0.0))
        for i in range(8):
            a.push(Light(POINT, (0.01 * i, 0.0, 0.0), 1.0, "torch%d" % i))
        a.push(Light(SPOT, (50.0, 0.0, 0.0), 1.0, "distantSpot"))
        return a

    def prop_clustered(a):
        return a.has("distantSpot")

    # (c) The directional guard: a redundant sun must be sacrificed before a
    # torch when a new type needs a slot. The geometry is what makes this
    # discriminating -- the sun sits NEARER the camera than the torches, so if
    # the sun were scored by distance it would look like the most valuable light
    # in the scene and a torch would be dropped instead. With the Type==0 -> 0
    # special case the sun scores 0 and is the cheapest victim, so sunB goes and
    # the torches survive. Only the guard produces that outcome.
    def scenario_redundant_sun(cls):
        a = cls(camera_pos=(0.0, 0.0, 0.0))
        a.push(Light(DIRECTIONAL, (0.0, 5.0, 0.0), 1.0, "sunA"))
        a.push(Light(DIRECTIONAL, (0.0, 5.0, 0.0), 1.0, "sunB"))
        for i in range(6):
            a.push(Light(POINT, (0.0, 900.0, 0.0), 1.0, "torch%d" % i))
        a.push(Light(SPOT, (0.0, 1.0, 0.0), 1.0, "newSpot"))
        return a

    def prop_redundant_sun(a):
        # The signature of the guard: a DIRECTIONAL is sacrificed and the far
        # torches survive. Score the sun by distance and it becomes the most
        # valuable light here, so torch0 is evicted instead and both suns remain
        # -- so requiring a torch to survive is what discriminates.
        #
        # It is sunA rather than sunB specifically: both suns score 0, and the
        # scan keeps the first minimum. The test asserts on the TYPE, not the
        # index, because which of two identical suns goes is arbitrary by design.
        return a.has("torch0") and a.type_counts()[DIRECTIONAL] == 1 and a.has("newSpot")

    controls = [
        ("FirstComeAdmission (original shipped code)", FirstComeAdmission,
         scenario_starvation, prop_starvation,
         "8 point lights admitted, spot and area dropped, and no counter at all"),
        ("NoCountersAdmission (original admission + counters only)", NoCountersAdmission,
         scenario_starvation, prop_starvation,
         "counters exist but admission is still first-come, so spot/area starve"),
        ("FirstAdmissionAdmission (quota removed, pure importance)", FirstAdmissionAdmission,
         scenario_clustered, prop_clustered,
         "a cluster of near torches outscores a distant spot, so distance alone starves"),
        ("NoDirectionalGuardAdmission (directional scored like the rest)",
         NoDirectionalGuardAdmission, scenario_redundant_sun, prop_redundant_sun,
         "the Type==0 importance special case is load-bearing"),
    ]

    # The real fix must PASS every one of these scenarios.
    for label, _cls, scenario, prop, _why in controls:
        if label.startswith(("FirstCome", "NoCounters")):
            continue  # these are expected to fail; that is their whole job
        check(prop(scenario(Admission)),
              "the real fix holds this property (%s)" % label.split(" (")[0])

    passed_a_control = []
    for label, cls, scenario, prop, why in controls:
        holds = prop(scenario(cls))
        print("  %-9s %s" % ("PASSED" if holds else "BROKEN", label))
        print("            (must not hold: %s)" % why)
        if holds:
            passed_a_control.append(label)
        check(not holds, "control fails as required: %s" % label)

    if passed_a_control:
        print("\n" + "!" * 72)
        print("!! NEGATIVE CONTROL LEAKED: the following broken admission(s) PASSED")
        print("!! the assertions this file exists to enforce. The test is not")
        print("!! proving anything -- it cannot detect the bug it was written for.")
        for label in passed_a_control:
            print("!!   - %s" % label)
        print("!" * 72 + "\n")
        return False
    return True


def main():
    print("=" * 72)
    print("Analytic-light admission: quota, importance, and overflow reporting")
    print("cap = %d (fixed by the shader's fixed-size light arrays)" % MAX_ANALYTIC_LIGHTS)
    print("=" * 72)

    test_cap_unchanged()
    test_old_behaviour_starves()
    test_new_behaviour_fixes_starvation()
    test_overflow_is_counted_and_reported()
    test_silent_when_within_budget()
    test_importance_breaks_ties_within_a_type()
    test_directional_is_never_starved_or_stolen()
    test_every_type_gets_a_seat()
    test_determinism_and_invariants()
    control_ok = test_negative_control()

    print("\n" + "=" * 72)
    if FAILURES:
        print("FAILED: %d of %d assertions" % (len(FAILURES), CHECKS[0]))
        for f in FAILURES:
            print("  - %s" % f)
        print("=" * 72)
        return 1
    if not control_ok:
        print("FAILED: negative control did not fail -- the test is vacuous")
        print("=" * 72)
        return 1
    print("ALL GREEN: %d assertions passed, and every negative control failed"
          % CHECKS[0])
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())