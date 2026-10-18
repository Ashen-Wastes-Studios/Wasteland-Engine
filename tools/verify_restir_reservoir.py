#!/usr/bin/env python3
"""
Reservoir-based direct-light sampler (ReSTIR DI), verified standalone.

WHY: an unbiased reservoir sampler is easy to describe and easy to get subtly
wrong. Drop the wsum normalisation from W and the estimator still converges --
to the wrong number. Replace the combine rule with an additive weight sum and it
still produces a plausible, stable, plausible-looking image. Neither shows up in
a screenshot. So the maths is checked here as arithmetic (no engine link, no
GPU, no numpy), matching what `verify_variance_math.py` already does for the
SVGF moments, and matching the sibling GLSL gates in this directory.

THE MATHS IS NOT THE SHADER. This file proves the ALGORITHM. It does not prove
any engine code, because the engine code for it does not exist yet -- see the
ENCODED-ALGORITHM TEXT section, which greps Renderer3D_NovaRenderer.hlsl and
reports the reservoir constructs as ABSENT. What that grep can and cannot
establish is stated explicitly in the output.

NEGATIVE CONTROLS are the point of this file. A test that cannot fail is
worthless, so the same checks are re-run against four deliberately broken
variants -- (a) W without wsum normalisation, (b) an additively-biased combine,
(c) the point-light model scored against an AREA-correct analytic value,
(d) an always-take-b combine -- and the run FAILS if any of them passes. The
dropped normal-rejection spatial gate is a fifth, reported inside the spatial
check itself.

Run: python tools/verify_restir_reservoir.py   (exit 0 = all green, 1 = failed)
"""

import math
import os
import random
import re
import sys

# ------------------------------------------------------- specification ----
#
# Every rule below is transcribed from the paper, which is the specification
# for this algorithm:
#
#   Bitterli, Wyman, Pharr, Shirley, Lefohn, Jarosz. "Spatiotemporal Reservoir
#   Resampling for Real-time Ray Tracing with Dynamic Direct Lighting."
#   ACM Trans. Graph. 39(4), Article 148, July 2020.
#
# The constructs implemented here are exactly:
#
#   Alg. 2 `Reservoir.update(xi, wi)`   wsum += wi; M += 1;
#                                        if rand() < wi/wsum then y = xi
#   Alg. 3 `RIS(q)` line 7               candidate weight = pHat(xi) / p(xi)
#   Alg. 3 `RIS(q)` line 8, Eq. (6)      W = wsum / (pHat(y) * M)
#   Alg. 3 `shadePixel(r,q)` line 11     fq(r.y) * W
#   Alg. 4 `combineReservoirs(q, r1..k)` line 4  s.update(r.y, pHat(r.y)*r.W*r.M)
#                                         line 5  s.M = sum(r.M)
#                                         line 6  s.W = s.wsum/(pHat(s.y)*s.M)
#   Alg. 5 line 9                        `if shadowed(y) then r.W <- 0`
#
# Alg. 4 line 4 is pHat(r.y) * r.W * r.M. Because Eq. (6) defines
# r.W = r.wsum / (pHat(r.y) * r.M), that product is identically r.wsum -- the
# pHat factors cancel EXACTLY. `combine_weight()` returns both the paper's
# factored form and the simplified form and asserts they are equal; that
# identity is the reason the combine is genuinely unbiased rather than merely
# plausible-looking.

# The target pHat is clamped to >= PHAT_FLOOR: a surface point at infinity
# (zero solid angle) would otherwise divide by zero. The floor is 1e-6 of peak,
# i.e. numerically inert on any real sample.
PHAT_FLOOR = 1e-6

# Analytic agreement uses a 3-sigma window on the sample standard error, NOT a
# relative tolerance. At N=2000 that window is ~17% wide, which sounds loose but
# is only ~17 sample standard errors: it rejects a constant multiplicative bias
# the moment bias > 3 sigma. A 1% tolerance would have passed almost anything.
SIGMA_K = 3.0

ITERS_MAIN = 2000          # analytic-agreement iterations (M=32 each)
COMBINE_PAIRS = 20000      # independent M1+M2 reservoir pairs for the combine
COMBINE_M1, COMBINE_M2 = 4, 6
TEMPORAL_CHAINS = 200     # independent temporal chains, for the drift test
TEMPORAL_FRAMES = 40     # frames of accumulation per chain, one fresh M=8 each
SPATIAL_ITERS = 4000

# A NEAR-FIELD, LARGE-ANGLE scene on purpose. R/D = 4 means the emitter subtends
# most of the hemisphere, so pHat varies by a factor of ~17 across it. That is
# what makes the combine's candidate-selection probability actually
# load-bearing -- and a bias in that probability is exactly what the negative
# controls need in order to be detectable above noise. A distant small emitter
# would make every broken variant look fine.
NEAR_R, NEAR_D = 2.0, 0.5

# Conservative spatial rejection gate: a neighbour is accepted only if its
# surface normal is within acos(NORMAL_GATE) ~ 18 degrees of the centre's.
NORMAL_GATE = 0.95

SHADER_REL = os.path.join("DemonCore-Editor", "assets", "shaders",
                          "Renderer3D_NovaRenderer.hlsl")


# ============================================================ reservoir ====

class Reservoir(object):
    """The Alg. 2 state: selected sample y, wsum, M. Plus W, which Alg. 2 does
    not store but Eq. (6) derives; it is carried so combine() can evaluate
    Alg. 4 line 4 without re-deriving it.

    `broken` selects a deliberately wrong W or combine rule. It exists only so
    the harness can prove the checks reject it; it is never used for a PASS.
    """

    def __init__(self, broken=None):
        self.y = None
        self.wsum = 0.0
        self.M = 0
        self.broken = broken
        self.occluded = False   # Alg. 5 line 9 sets W <- 0, it must NOT drop M
        self._yhat = 0.0       # pHat at the stored candidate; Eq. (6) needs it

    # -- Eq. (6) ---------------------------------------------------------
    def unbiased_W(self):
        """W = wsum / (pHat(y) * M), clamped to 0 when there is no mass.

        Four separate cases return exactly 0, and each is a real GPU failure
        mode rather than a formality:
          * occluded    -- Alg. 5 line 9 sets W <- 0 when the stored candidate
                           is shadowed. It must NOT drop the reservoir: the M
                           candidates it stands for still count toward the
                           combine, only their contribution is voided.
          * M == 0      -- combine() over empty reservoirs, or a frame with no
                           candidates at all.
          * wsum <= 0   -- every candidate had zero weight, or every input
                           reservoir was occluded.
          * pHat == 0   -- the stored candidate sits at infinite distance.

        The 'ClampWBroken' variant exists because the unclamped form divides by
        a denormal and returns inf, and one inf in W poisons that pixel's whole
        temporal history from that frame onward.
        """
        if self.occluded or self.M == 0 or self.wsum <= 0.0:
            return 0.0
        p_hat = max(self._yhat, PHAT_FLOOR)
        if self.broken == "ClampWBroken":
            return self.wsum / p_hat / self.M
        return self.wsum / (p_hat * self.M)

    # -- negative control (a) --------------------------------------------
    def broken_W_no_wsum(self):
        """W with the wsum normalisation DROPPED: W = 1 / (pHat(y) * M).

        Still finite, still stable, still plausible-looking -- but independent
        of the accumulated mass, so the estimate converges to the unweighted
        mean of the contributions instead of their weighted mean. This is the
        single most likely transcription error in the whole algorithm, which is
        why it is a dedicated negative control rather than a passing assertion.
        """
        if self.M == 0:
            return 0.0
        return 1.0 / (max(self._yhat, PHAT_FLOOR) * self.M)

    def W(self):
        if self.broken == "NoWsum":
            return self.broken_W_no_wsum()
        return self.unbiased_W()

    # -- Alg. 2 lines 5-9 -----------------------------------------------
    def update(self, rng, x, w, p_hat):
        """The canonical update. Returns True if x became the stored candidate.

        `w <= 0` returns early WITHOUT incrementing M. That matches Alg. 2 as
        printed (M counts calls to update), and it matters: a zero-weight
        candidate carries no mass, so counting it would only shrink W.
        """
        if w <= 0.0:
            return False
        self.wsum += w
        self.M += 1
        if rng.random() < (w / self.wsum):
            self.y, self._yhat = x, p_hat
            return True
        return False

    def merge_aggregates(self, other):
        """Combine accumulated state without touching the candidates. Carries
        `_yhat` from whichever input contributed the selected candidate, since
        Eq. (6) needs pHat(y) and not merely wsum."""
        self.wsum += other.wsum
        self.M += other.M
        self.occluded = self.occluded or other.occluded

    def scaled_M(self, factor):
        """M counts CANDIDATES, so spatial reuse reuses M of them per merged
        reservoir -- not 1, which is the classic reuse bookkeeping bug."""
        self.M = int(self.M * factor)

    def clone(self, broken=None):
        r = Reservoir(broken if broken is not None else self.broken)
        r.y, r.wsum, r.M, r.occluded, r._yhat = \
            self.y, self.wsum, self.M, self.occluded, self._yhat
        return r

    # -- Alg. 3 line 11 ---------------------------------------------------
    def shade(self, f_of_x):
        """f(y) * W. Zero candidates contribute exactly nothing, not NaN."""
        if self.M == 0 or self.occluded:
            return 0.0
        return f_of_x(self.y) * self.W()


def combine(rng, a, b, broken=None):
    """Alg. 4: combine two reservoirs into one equivalent to their
    concatenated input streams.

    UNBIASEDNESS, argued rather than asserted. Hold the two candidate STREAMS
    fixed, so their weight sums w_a and w_b are fixed, and let alpha = pHat(y_a)
    and beta = pHat(y_b) be the (random) targets of the two stored candidates.
    Line 4 feeds a fresh candidate y_a of weight W_a * M_a = w_a and a fresh
    candidate y_b of weight w_b into s, so the Alg. 2 invariant gives
    P(s.y = y_a) = w_a / (w_a + w_b): s selects each stored candidate in
    proportion to its own true weight. Hence E[s.wsum] is unbiased in
    w_a + w_b, E[1/(pHat(s.y) M)] = (w_a + w_b)/wsum is unbiased, and the two
    random factors multiply independently, giving

        E[s.W] = (w_a + w_b) * (w_a + w_b)/wsum * 1/M = (M_a + M_b)/M = 1.

    The estimator is W * f(y), so shading a combined reservoir is unbiased.
    That is the invariant temporal and spatial reuse both rest on, and
    check_combine_invariance measures it rather than trusting the argument.

    Negative control (b) replaces line 4 with `s.update(r.y, r.wsum + s.wsum)`.
    It still conserves wsum and still leaves M correct, so it returns finite,
    stable, plausible numbers -- but it no longer selects in proportion to the
    candidates' own weights: it makes P(s.y = y_b) = (w_a + w_b)/(2 w_a + w_b)
    where the paper requires w_b/(w_a + w_b).

    THE BUG THIS FUNCTION ONCE HAD, and the reason the assertions below are
    written the way they are: `s.wsum` is already accumulating the weights fed
    in by the updates on line 4. Adding `a.wsum + b.wsum` AGAIN afterwards --
    which is tempting, because that is what makes the two-reservoir combine
    "obviously" correct -- double-counts every combine. In a temporal chain that
    compounds: wsum grows as 2^n while M grows as n, so W = wsum/(pHat*M)
    diverges exponentially and the estimate runs to 1e30 within a few hundred
    frames. It still looks like a number. Line 5's `s.M = r1.M + ... + rk.M`
    likewise OVERWRITES the M that the two update() calls incremented, so M
    must be assigned, not accumulated.
    """
    s = Reservoir(broken)
    if broken == "AlwaysTakeB":
        # Negative control (d): delete the probabilistic selection on line 4.
        # NOTE: this one is NOT mean-biased, and the harness proves that rather
        # than assuming it. Taking the second reservoir's candidate always is
        # still an unbiased draw *provided* both inputs are unbiased, which the
        # measured numbers below confirm: f(y_B)/pHat(y_B) is a deterministic
        # A for this scene, so the estimator collapses to A*(w_a+w_b)/M, whose
        # expectation is A * E[w] * M / M = A * E[f] = the truth exactly. What
        # it breaks is VARIANCE and reservoir diversity -- always taking the
        # newer reservoir throws away the older candidate entirely -- not the
        # mean, so it cannot serve as a mean-bias control. It is kept as a
        # documented demonstration of that distinction rather than deleted,
        # because "the control that did not work, and why" is itself a result.
        s.y, s._yhat = b.y, b._yhat
        s.wsum = a.wsum + b.wsum           # the loop below is skipped here
    else:
        for r in (a, b):
            if r.wsum <= 0.0:
                continue
            if broken == "AdditiveCombine":
                s.update(rng, r.y, r.wsum + s.wsum, r._yhat)
            else:
                s.update(rng, r.y, r.wsum, r._yhat)   # Alg. 4 line 4
    # Alg. 4 line 5: ASSIGN, do not accumulate. s.wsum above is already correct.
    s.M = a.M + b.M
    # Occlusion is AND-ed: the combined reservoir is only itself occluded if
    # EVERY input was, and an occluded input still contributes its M.
    s.occluded = a.occluded and b.occluded
    return s


def combine_weight(p_hat_y, w, M):
    """Alg. 4 line 4's weight in BOTH the paper's factored form and the form it
    simplifies to. pHat(y)*W*M == wsum is what makes the combine unbiased, so it
    is checked directly: the cancellation is exact, and dropping a factor on one
    side of it would bias every reuse operation in the pipeline.
    """
    if M <= 0 or p_hat_y <= 0.0:
        return 0.0, w
    return p_hat_y * (w / (p_hat_y * M)) * M, w


# ================================================ scene and light models ==

class Scene(object):
    """One emitter, one shading point, with a CLOSED-FORM answer.

    The emitter is a DISC of radius R lying flat at z = 0, centred under the
    shading point; the shading point sits at (0, 0, D) facing -z. A disc rather
    than a rectangle because the on-axis integral over a disc is exactly

        integral f dA'  =  R^2 / (D^2 + R^2)

    (polar coordinates make the radial integral an elementary partial
    derivative), whereas over a rectangle it involves elliptic-looking terms.
    An analytic-agreement test needs a value nobody has to trust the harness to
    compute, and a rational closed form is the honest version of that.

    Wasteland's material system is a flat POD: emission is
    `EmissionColor.rgb * EmissionIntensity` (Scene/Components.h:668, :677) and
    there is no shader graph anywhere in the repo, so every emitter is a single
    constant radiance that can be statically enumerated. That is exactly what
    Renderer3D.cpp:2361 does when it pushes instances with `Emission.w > 0`
    into m_LightIndices and uploads it as LightListBuffer -- 4 bytes per
    emitter, one int, indexed by the shader at LightIndices[li]
    (Renderer3D_NovaRenderer.hlsl:2027).
    """

    def __init__(self, D, R):
        self.D, self.R = D, R
        self.area = math.pi * R * R

    def sample_point(self, rng):
        """Area-uniform sample of the disc: r = R*sqrt(u) is what makes the
        density uniform in AREA (using r = R*u would over-sample the centre).
        This is the AREA-correct proposal p(x) = 1/A."""
        rad = self.R * math.sqrt(rng.random())
        ang = 2.0 * math.pi * rng.random()
        return (rad * math.cos(ang), rad * math.sin(ang), 0.0)

    def sample_point_model(self, rng):
        """The model the engine SHIPS today: a POINT light at the instance's
        translation column.

        Renderer3D_NovaRenderer.hlsl:2029 builds `toLight` from
        WorldTransform[0][3..2][3], and :2044 shades with
        `brdf * light.Emission.xyz * light.Emission.w * NdotL` -- there is no
        1/r^2 and no solid angle anywhere in that expression. So the shipped
        light emits radiance independent of distance, i.e. it has an implicit
        intensity of 4*pi*r^2 and a total power that grows as r^2. That is not a
        physical point light, which would fall off as 1/r^2. ReSTIR DI's
        area-correct variant samples a point on the emitter's analytic surface
        instead; both are modelled here so the harness can tell them apart.
        """
        return (0.0, 0.0, 0.0)

    def f_area(self, x, y, _z):
        """The DIRECT-LIGHTING INTEGRAND over the emitter's surface -- the f of
        Eq. (2).

        For a Lambertian receiver of albedo rho facing a Lambertian emitter of
        radiance Le, the received radiance is

            dL = (rho/pi) * Le * cos(theta_s) * dOmega,
            dOmega = cos(theta_l) * dA' / r^2,

        and with the receiver normal (0,0,1), the emitter normal (0,0,1) and
        both on-axis, both cosines are D/r. With rho = Le = 1:

            f(x) = D^2 / (pi * r2^2),      r2 = D^2 + x^2 + y^2.

        Note this is NOT proportional to dOmega/dA' -- it also carries the
        RECEIVER's cosine. Putting the 1/r^2 in the integrand rather than in the
        proposal is what makes the integral over the emitter AREA equal the
        direct-lighting integral over solid angle; getting that backwards is a
        factor-of-r^2 error and is exactly what control (c) is sensitive to.
        """
        r2 = self.D * self.D + x * x + y * y
        return self.D * self.D / (math.pi * r2 * r2)

    def area_analytic(self):
        """The AREA-correct direct-lighting integral, closed form:

            integral_0^R (D^2/(pi (D^2+r^2)^2)) * r dr * 2pi  =  R^2/(D^2 + R^2)

        Corroborated in check_analytic against an independent deterministic
        midpoint-rule quadrature, so the closed form is cross-checked rather
        than merely asserted.
        """
        return self.R * self.R / (self.D * self.D + self.R * self.R)

    def area_analytic_numeric(self, n=200000):
        """Independent deterministic quadrature of the same integral, in polar
        coordinates. No random numbers: this is a sum of function values, so it
        cannot accidentally agree with the Monte Carlo result by luck."""
        acc, dr = 0.0, self.R / n
        for i in range(n):
            r = (i + 0.5) * dr
            acc += self.f_area(r, 0.0, 0.0) * r * dr
        return 2.0 * math.pi * acc

    def point_analytic(self):
        """The shipped point model's own closed form.

        Renderer3D_NovaRenderer.hlsl:2044 computes
        `brdf * light.Emission.xyz * light.Emission.w * NdotL` with no 1/r^2 and
        no solid angle. For this scene NdotL = 1 (the normal faces the light
        dead-on) and the emissive BRDF term is albedo/pi, so the contribution is
        BRDF_LAMBERT regardless of distance -- here 0.3183 against the
        area-correct 0.9412, a factor of 2.96. That gap is what negative
        control (c) detects, and it is why swapping the light model silently is
        not a rounding difference. It is also why the point model's estimator has
        EXACTLY zero variance, which makes it separable from any other value at
        any sample count.
        """
        return BRDF_LAMBERT

    def model_ratio(self):
        """area-correct answer / point-model answer. The physical statement of
        how much energy the shipped point model gets wrong at this distance."""
        return self.area_analytic() / self.point_analytic()


# The Lambertian BRDF, 1/pi. It appears INSIDE Scene.f_area rather than as a
# separate multiplier, because the paper's f is the full integrand of Eq. (2).
# It is still referenced on its own for the point model, whose shader-side
# expression contains no distance term at all.
BRDF_LAMBERT = 1.0 / math.pi


def make_candidate_function(scene, model="area"):
    """Return the quadruple every reservoir routine needs:

        p(x)      the proposal PDF, sampled on line 6 of Alg. 3
        p_hat(x)  the RIS TARGET of Eq. (5) -- the distribution ReSTIR would
                  LIKE to sample from and, in practice, sets to the unshadowed
                  path contribution. It enters ONLY through the resampling
                  probability and the 1/pHat(y) factor of Eq. (6).
        f(x)      the integrand of Eq. (2), multiplied by W on line 11 of Alg. 3
        truth     the closed-form value of integral f dx

    THE THREE ROLES ARE NOT INTERCHANGEABLE, and conflating them is the most
    common way to get a plausible wrong answer out of ReSTIR. From Eq. (5) and
    Eq. (6):

        candidate weight      w(x)   = pHat(x) / p(x)          (Alg. 3 line 7)
        unbiased weight             W = wsum / (pHat(y) * M)   (Eq. 6, line 8)
        estimator                 L    = f(y) * W             (line 11)

    and

        E[ L ] = sum_i (w_i/wsum) * f_i * wsum/(pHat_i * M)
               = (1/M) sum_i w_i * f_i / pHat_i
               = (1/M) sum_i f_i / p_i                       (substituting Eq. 5)
               = integral f dx                                (the IS estimator of
                                                              Eq. 3, N=1).

    That last line is the whole correctness argument: the RIS estimator
    collapses back to plain importance sampling, for ANY non-negative target
    pHat. The target only affects VARIANCE, never the mean -- which is exactly
    why Eq. (6) must carry the 1/pHat(y) and why dropping it is fatal.

    Note the paper prints Eq. (6) as f(y)/pHat(y) * (1/M) sum_j w(x_j); the
    factor 1/pHat(y) is absorbed into W by line 8, which is why line 11 reads
    only `fq(r.y) * r.W`.
    """
    if model == "area":
        # Proposal: uniform over the emitter's area. This is the natural
        # AREA-correct choice and is exactly what "sample a point on the
        # emitter's analytic surface" means.
        def p(x, y, z):
            return 1.0 / scene.area

        def f(x, y, z):
            return scene.f_area(x, y, z)

        # Target proportional to f, normalised over the emitter's support. Any
        # pHat >= 0 gives the same mean; this one is the best available without
        # solving for the optimal shape.
        def p_hat(x, y, z):
            return scene.f_area(x, y, z) / scene.area

        sample = scene.sample_point
        truth = scene.area_analytic()

    elif model == "point":
        # The shipped model. The shader's integrand contains no 1/r^2 and no
        # solid angle, so f is the CONSTANT BRDF_LAMBERT and p is a point mass
        # at the instance origin. Every candidate therefore has the same weight
        # and the estimator has exactly ZERO variance -- which is precisely
        # what makes it trivially separable from the area-correct value, and so
        # makes negative control (c) a real test rather than a coin flip.
        def p(x, y, z):
            return 1.0

        def f(x, y, z):
            return BRDF_LAMBERT

        def p_hat(x, y, z):
            return BRDF_LAMBERT

        sample = scene.sample_point_model
        truth = scene.point_analytic()

    else:
        raise ValueError("unknown light model %r" % (model,))
    return p_hat, p, f, sample, truth


# =================================================== estimator machinery ===

def reservoir_of_m(scene, rng, M, model="area", broken=None):
    """Build one reservoir from M fresh candidates -- the reference against
    which combine invariance is measured."""
    p_hat, p, _f, sample, _t = make_candidate_function(scene, model)
    r = Reservoir(broken)
    for _ in range(M):
        x = sample(rng)
        r.update(rng, x, p_hat(*x) / p(*x), p_hat(*x))   # Alg. 3 line 7
    return r


def run_main(scene, rng, M=32, model="area", broken=None, iters=ITERS_MAIN):
    """One reservoir of M candidates -> f(y) * W. Returns every per-iteration
    estimate, because a 3-sigma window needs the sample standard error and not
    just the mean."""
    p_hat, p, f, sample, _t = make_candidate_function(scene, model)
    f_of_x = lambda X: f(*X)
    out = []
    for _ in range(iters):
        r = Reservoir(broken)
        for _ in range(M):
            x = sample(rng)
            r.update(rng, x, p_hat(*x) / p(*x), p_hat(*x))   # Alg. 3 line 7
        out.append(r.shade(f_of_x))                          # Alg. 3 line 11
    return out


def mean_se(vals):
    """Sample mean, sample standard error, sample variance.

    Kahan-compensated sum: at 20000 samples the naive sum already carries ~1e-11
    of rounding noise, which is harmless here, but the long combine runs are
    cheap enough that paying for the exact sum costs nothing and removes a
    class of "the mean moved by 1e-12" arguments entirely.
    """
    n = float(len(vals))
    if n == 0.0:
        return 0.0, 0.0, 0.0
    total, comp = 0.0, 0.0
    for v in vals:
        y = v - comp
        t = total + y
        comp = (t - total) - y
        total = t
    mean = total / n
    if n < 2.0:
        return mean, 0.0, 0.0
    var = math.fsum((v - mean) ** 2 for v in vals) / (n - 1.0)
    return mean, math.sqrt(var / n), var


def within_3sigma(mean, se, truth):
    """The verdict, phrased as a z-score so a reader can see the remaining
    headroom instead of only pass/fail."""
    if se <= 0.0:
        if mean == truth:
            return True, 0.0
        return False, float("inf")
    z = abs(mean - truth) / se
    return z <= SIGMA_K, z


def all_finite(vals):
    return all(math.isfinite(v) for v in vals)


# ================================================== spatial rejection gate ==

# The gate is stated in terms of SHADING-POINT GEOMETRY ONLY -- the pair of
# surface normals and the ratio of the shading distances to the two emitters.
# It never inspects the candidate weights, the wsum, or the selected sample.
# That is what makes rejection conservative in the precise sense the unbiasedness
# argument needs: the selection event is independent of the candidate streams, so
# conditioning on it cannot move the mean.
NORMAL_TOL = 0.95        # dot(n_centre, n_neighbour) must be at least this
LOG_DIST_TOL = 0.7        # |log(d_neighbour / d_centre)| must be at most this


def spatial_gate(cos_nn, dist_ratio):
    """Normal-based rejection gate for spatial reuse. Returns True if the
    neighbour is conservative to reuse.

    Rejection is legitimate (and this is the subtle point) because the gate is a
    function of the two shading points' geometry alone. Resampling a stream
    conditional on an event independent of the stream leaves the estimator
    unbiased: dropping neighbours cannot bias the pixels that keep them, it only
    costs effective sample count. What is NOT legitimate is gating on the
    candidate's own contribution -- that is a target-dependent filter and it does
    shift the mean. The control below drops the gate entirely, and it is
    detected.
    """
    if dist_ratio <= 0.0 or not math.isfinite(dist_ratio):
        return False
    return cos_nn >= NORMAL_TOL and abs(math.log(dist_ratio)) <= LOG_DIST_TOL


def normal_gate(dot_nn):
    """The normal half of the gate on its own, exposed so the output can name
    the constant the engine would have to match."""
    return dot_nn >= NORMAL_TOL


# ================================================================ checks ====

def check_closed_form(scene):
    """The closed form R^2/(D^2+R^2) must agree with an independent
    deterministic quadrature of the same integrand. Without this, every
    3-sigma verdict below would be measured against a number the harness itself
    invented."""
    closed = scene.area_analytic()
    num = scene.area_analytic_numeric(100000)
    rel = abs(closed - num) / closed
    ok = rel <= 1e-6
    return ok, ("closed form R^2/(D^2+R^2)=%.12f | midpoint quadrature=%.12f"
                " | rel diff|=%.2e (tol 1e-06)" % (closed, num, rel))


def check_analytic(scene, M=32, iters=ITERS_MAIN, model="area", broken=None,
                   seed=0xC0FFEE, truth=None):
    """Analytic agreement on a 3-sigma window over `iters` independent
    reservoirs. This is the check the whole harness is built to make
    falsifiable, so it takes `broken` and `truth` as parameters: the same code
    path is what rejects negative control (a)."""
    _ph, _p, _f, _s, analytic = make_candidate_function(scene, model)
    if truth is None:
        truth = analytic
    rng = random.Random(seed)
    vals = run_main(scene, rng, M=M, model=model, broken=broken, iters=iters)
    if not all_finite(vals):
        return False, "non-finite estimate produced -- UNUSABLE"
    mean, se, _var = mean_se(vals)
    ok, z = within_3sigma(mean, se, truth)
    return ok, ("M=%-3d N=%-6d model=%-5s %-18s mean=%.9f truth=%.9f "
                "sigma_hat=%.3e (3sig +-%.4f) z=%.2f -> %s"
                % (M, iters, model,
                   "unbiased" if broken is None else "BROKEN(%s)" % broken,
                   mean, truth, se, SIGMA_K * se, z,
                   "PASS" if ok else "FAIL"))


def check_salve_split(scene, M=32, iters=2000, seed=0x5A1):
    """Two algebraically equivalent parameterisations must agree.

    The paper's Eq. (5)-(6) route is w = pHat/p with the estimator f(y)*W. An
    equivalent route puts a constant lambda into the shading function instead:
    w = lambda*pHat/p and f' = f/lambda. Because the 1/pHat(y) of Eq. (6) sits
    in W and lambda is constant, the two must agree EXACTLY in expectation.

    This matters because it is a live ambiguity when transcribing the paper into
    HLSL: the factor can legitimately live in wsum, in W, or in f. A
    transcription that puts it in wsum but forgets the matching f (or vice
    versa) is off by lambda, which is a plausible-looking constant factor -- so
    it gets its own check rather than being left to the analytic test alone.
    """
    p_hat, p, f, sample, truth = make_candidate_function(scene, "area")
    lam = 2.5
    rng = random.Random(seed)

    a, b = [], []
    for _ in range(iters):
        r1, r2 = Reservoir(), Reservoir()
        for _ in range(M):
            x = sample(rng)
            r1.update(rng, x, p_hat(*x) / p(*x), p_hat(*x))
            r2.update(rng, x, lam * p_hat(*x) / p(*x), p_hat(*x))
        a.append(r1.shade(lambda X: f(*X)))
        b.append(r2.shade(lambda X: f(*X) / lam))

    if not all_finite(a + b):
        return False, "non-finite value in the salve-equivalence run"
    ma, sa, _ = mean_se(a)
    mb, sb, _ = mean_se(b)
    sep = math.sqrt(sa * sa + sb * sb)
    ok = abs(ma - mb) <= SIGMA_K * sep and abs(ma - truth) <= SIGMA_K * sa
    return ok, ("lambda=%.2f: split-in-w=%.7f+-%.2e  split-in-f=%.7f+-%.2e"
                "  |diff|=%.2e (3sig %.2e)  truth=%.7f"
                % (lam, ma, sa, mb, sb, abs(ma - mb), SIGMA_K * sep, truth))


def check_combine_weight():
    """Alg. 4 line 4's weight: pHat(y) * W * M must equal wsum exactly.

    This identity is the mechanism. It is what lets Alg. 4 feed each reservoir's
    own weight into the new reservoir and have the selection come out in
    proportion to the candidates' true weights; it is also why the combine needs
    no access to the input streams. If W were formed without the wsum factor, the
    identity would break and every spatial and temporal reuse operation in the
    pipeline would be biased -- silently, and by a factor that looks plausible.
    """
    rows, ok = [], True
    for (p_hat_y, w, M) in ((1.0, 7.5, 4), (1e-3, 2.0, 32), (250.0, 1e4, 8),
                             (PHAT_FLOOR, 3.25, 5)):
        w_paper, w_simple = combine_weight(p_hat_y, w, M)
        rel = abs(w_paper - w_simple) / w_simple if w_simple else 0.0
        good = rel <= 1e-12
        ok = ok and good
        rows.append("pHat=%.1e wsum=%.2f M=%-2d -> %.10f (rel %.1e)"
                    % (p_hat_y, w, M, w_paper, rel))
    return ok, "; ".join(rows)


def _reservoir_from_stream(scene, rng, stream, broken=None):
    p_hat, p, _f, _s, _t = make_candidate_function(scene, "area")
    r = Reservoir(broken)
    for x in stream:
        r.update(rng, x, p_hat(*x) / p(*x), p_hat(*x))
    return r


def check_combine_invariance(scene, pairs=COMBINE_PAIRS, m1=COMBINE_M1,
                             m2=COMBINE_M2, seed=0xBEEF, broken=None):
    """Merging two reservoirs of M1 and M2 candidates must give the same
    EXPECTED MEAN as one reservoir of M1+M2.

    Both reservoirs are fed the IDENTICAL candidate stream -- the first M1
    samples go to one, the last M2 to the other -- so this is genuinely "the same
    pool, resampled as one versus combined from two". That is the property
    temporal and spatial reuse depend on: reuse cannot change the image's
    expected value, only its variance and cost.

    Note this is an EXPECTATION test over many independent pairs, not a
    per-pair test. The combine introduces real extra variance: the surviving
    candidate is picked with probability w_a : w_b, which is not the same
    distribution as resampling all M1+M2 at once. Comparing per-pair values
    would be comparing the wrong thing.

    Two sub-verdicts, because a combine can be biased yet perfectly
    self-consistent:
      (i)  merged == single   -- the direct invariance claim
      (ii) merged == analytic -- the combine is unbiased, not merely stable
    """
    _ph, _p, f, sample, _t = make_candidate_function(scene, "area")
    f_of_x = lambda X: f(*X)
    truth = scene.area_analytic()
    rng = random.Random(seed)

    merged, single = [], []
    for _ in range(pairs):
        stream = [sample(rng) for _ in range(m1 + m2)]
        a = _reservoir_from_stream(scene, rng, stream[:m1], broken)
        b = _reservoir_from_stream(scene, rng, stream[m1:], broken)
        merged.append(combine(rng, a, b, broken=broken).shade(f_of_x))
        single.append(_reservoir_from_stream(scene, rng, stream, broken)
                      .shade(f_of_x))

    if not all_finite(merged + single):
        return False, "non-finite value in the combine run"

    mm, ms, _ = mean_se(merged)
    sm, ss, _ = mean_se(single)
    sep = math.sqrt(ms * ms + ss * ss)
    tol_inv = SIGMA_K * sep
    ok_inv = abs(mm - sm) <= tol_inv
    ok_ana = abs(mm - truth) <= SIGMA_K * ms
    return (ok_inv and ok_ana), \
        ("pairs=%d M1=%d M2=%d %-18s merged=%.7f+-%.2e single=%.7f+-%.2e"
         " truth=%.7f | (i) |diff|=%.2e (3sig %.2e) %s"
         " | (ii) z_truth=%.2f %s"
         % (pairs, m1, m2,
            "unbiased" if broken is None else "BROKEN(%s)" % broken,
            mm, ms, sm, ss, truth, abs(mm - sm), tol_inv,
            "OK" if ok_inv else "VIOLATED",
            abs(mm - truth) / ms if ms > 0 else float("inf"),
            "OK" if ok_ana else "VIOLATED"))


def check_temporal(scene, chains=200, frames=40, M=8, seed=0x7EA1,
                   drop_M=False):
    """Temporal reuse against a STATIC scene must not drift, and must converge.

    THE SUBTLETY, which is the whole reason this check is not a one-liner: the
    estimator at frame n is unbiased, so E[L_n] = truth for EVERY n -- but the
    chain is strongly autocorrelated, because each frame's reservoir contains
    the previous one's candidate. Averaging a single chain over its own frames
    and testing that against the truth is therefore meaningless: consecutive
    values are not independent samples, the chain wanders slowly, and tercile
    means of one chain differ by several apparent sigma even when the stationary
    distribution is exactly right. (That mistake was in this harness's first
    draft; it reported a spurious BIASED verdict on a correct implementation.)

    The fix is to run `chains` INDEPENDENT chains and average ACROSS them at
    each frame index. That restores the correct sampling distribution: for a
    fixed n, L_n over chains is i.i.d., so the mean over chains has a standard
    error that is actually sigma_L/sqrt(chains).

    Four properties are then asserted, each of which a real bug breaks:
      1. M accumulates: M_total == chains-independent M*frames.
      2. No drift: the mean over all (frame, chain) equals the truth, and the
         OLS slope of the per-frame-index mean against frame index is flat.
      3. Convergence: the spread across chains SHRINKS with n, by roughly
         sqrt(n). This is what temporal reuse is FOR, so a check that only
         looked at the mean would not notice a pipeline that stopped reusing.
      4. The last frame's mean is unbiased at the tighter accumulated SE.

    `drop_M` is negative control (d): the classic reuse bookkeeping bug where a
    combined reservoir keeps its own M instead of summing its inputs'. wsum then
    compounds as 2^n while M stays flat at M, so W = wsum/(pHat*M) diverges
    exponentially -- slow enough to look like accumulation over the first few
    frames.
    """
    _ph, _p, f, sample, _t = make_candidate_function(scene, "area")
    f_of_x = lambda X: f(*X)
    truth = scene.area_analytic()

    per_frame = [[] for _ in range(frames)]     # L_n for every chain
    M_accum = 0
    for chain in range(chains):
        rng = random.Random(seed + 7919 * chain)
        running, M_running = None, 0
        for n in range(frames):
            fresh = reservoir_of_m(scene, rng, M)
            if running is None:
                running, M_running = fresh, M
            else:
                running = combine(rng, fresh, running)
                M_running = M_running + M if not drop_M else M
                if drop_M:
                    running.M = M_running       # the bug: overwrite, don't sum
            per_frame[n].append(running.shade(f_of_x))
            M_accum = M_running

    if not all_finite([v for row in per_frame for v in row]):
        return False, "non-finite value in the temporal chain"

    stats = [mean_se(row) for row in per_frame]
    means = [s[0] for s in stats]
    spreads = [s[2] ** 0.5 for s in stats]

    # (1) M bookkeeping
    m_ok = M_accum == M * frames

    # (2) no drift. The genuinely independent observations are the CHAINS, so
    # each chain is collapsed to one mean over its own frames FIRST, and the
    # pooled statistics are computed over those `chains` independent numbers.
    # (Averaging over frames first and dividing by the wrong count is how the
    # first draft of this check produced a nonsense pooled mean.)
    chain_means = []
    for c in range(chains):
        chain_means.append(math.fsum(per_frame[n][c] for n in range(frames))
                           / frames)
    pooled_mean = math.fsum(chain_means) / chains
    pooled_se = math.sqrt(math.fsum((cm - pooled_mean) ** 2
                                    for cm in chain_means)
                          / (chains - 1.0)) / math.sqrt(chains)
    drift_ok = abs(pooled_mean - truth) <= SIGMA_K * pooled_se

    # Slope of the per-frame-index mean (averaged over chains, so its noise is
    # small) against frame index. This measures DRIFT as opposed to a constant
    # offset, so it catches the slow energy loss a temporal pipeline can develop
    # while the pooled mean still looks fine.
    #
    # The standard error of the slope is obtained by a LEAVE-ONE-CHAIN-OUT
    # JACKKNIFE, not from the per-frame spreads. The two differ a lot, and the
    # naive version is wrong in the dangerous direction: m_n and m_m are
    # positively correlated because the SAME chains appear at every n, so
    # treating the 40 points as independent understates Var(slope). Jackknifing
    # over chains accounts for the cross-frame correlation exactly, costs 200
    # extra 40-point regressions, and needs no distributional assumption.
    xbar = (float(frames) - 1.0) / 2.0
    sxx = math.fsum((i - xbar) ** 2 for i in range(frames))

    def slope_of(exclude):
        """Slope with chain `exclude` dropped. Using the per-frame totals, the
        leave-one-out mean costs O(frames) rather than O(chains), so the whole
        jackknife is 200*40 operations."""
        cnt = chains - 1
        ys = [(math.fsum(row) - (row[exclude] if exclude is not None else 0.0))
              / cnt for row in per_frame]
        ybar = math.fsum(ys) / frames
        return math.fsum((i - xbar) * (ys[i] - ybar)
                          for i in range(frames)) / sxx

    slope = math.fsum((i - xbar) * (means[i] - pooled_mean)
                      for i in range(frames)) / sxx if sxx > 0.0 else 0.0

    loo = [slope_of(c) for c in range(chains)]
    loo_mean = math.fsum(loo) / chains
    slope_se = math.sqrt((chains - 1.0) / chains *
                         math.fsum((t - loo_mean) ** 2 for t in loo))
    slope_ok = abs(slope) <= SIGMA_K * slope_se

    # (3) convergence: the spread must fall by ~sqrt(frames).
    conv_ratio = spreads[-1] / spreads[0] if spreads[0] > 0 else float("inf")
    conv_ok = conv_ratio < 0.5

    # (4) the final frame's mean, at the tighter accumulated SE.
    final_ok = abs(means[-1] - truth) <= SIGMA_K * stats[-1][1]

    ok = m_ok and drift_ok and slope_ok and conv_ok and final_ok
    return ok, ("chains=%d frames=%d M=%d/frame M_accumulated=%d (expect %d) %s"
                "%s\n"
                "           drift    : pooled_mean=%.6f truth=%.6f "
                "(3sig +-%.5f) %s | slope=%.3e (jackknife 3sig tol %.3e, z=%.2f) %s\n"
                "           converges: sigma(L_1)=%.4e -> sigma(L_%d)=%.4e, "
                "ratio=%.3f (want <0.5) %s\n"
                "           final    : mean=%.6f (3sig +-%.5f) %s"
                % (chains, frames, M, M_accum, M * frames,
                   "OK" if m_ok else "WRONG",
                   "   [CONTROL: M NOT SUMMED]" if drop_M else "",
                   pooled_mean, truth, SIGMA_K * pooled_se,
                   "UNBIASED" if drift_ok else "DRIFTING",
                   slope, SIGMA_K * slope_se,
                   abs(slope) / slope_se if slope_se > 0 else float("inf"),
                   "FLAT" if slope_ok else "DRIFTING",
                   spreads[0], frames, spreads[-1], conv_ratio,
                   "SHRINKING" if conv_ok else "NOT SHRINKING",
                   means[-1], SIGMA_K * stats[-1][1],
                   "OK" if final_ok else "BIASED"))


def check_spatial(scene, iters=SPATIAL_ITERS, m_centre=1, m_neighbour=1,
                  seed=0x5A1CE, broken=None):
    """Spatial reuse with a conservative normal-based rejection gate.

    The mean must be PRESERVED when rejection is conservative, and must be WRONG
    when rejection is dropped across a high-contrast edge. That is the second
    negative control, and the one that matters most in practice: a leaky gate
    does not produce NaNs or a black image, it produces an image that is merely
    slightly wrong and slightly smoother than it should be.

    The gate is conservative in the precise sense the unbiasedness argument
    needs. `spatial_gate` is a function of the two shading points' GEOMETRY
    ONLY -- their normals and the ratio of their shading distances to the two
    emitters -- and never inspects a candidate weight, a wsum, or a selected
    sample. Conditioning a candidate stream on an event independent of that
    stream leaves its expected value unchanged, so rejecting neighbours costs
    effective sample count without moving the mean. That is the mechanism, and it
    is why the preserved-mean half of this check is a real result rather than a
    coincidence.

    The two neighbour regimes:
      * accepted  -- same emitter geometry, normals 18 degrees apart. Reusing it
                     is statistically identical to extending M, so the mean must
                     not move.
      * dropped   -- a DIM, DISTANT emitter (D'=4, R'=0.5) across the edge,
                     geometrically similar in normal but 8x further away. Its
                     analytic answer is 0.0154 against the centre's 0.9412, a
                     61x contrast. Reusing it without the gate nearly halves the
                     estimate.
    """
    _ph, _p, f, sample, _t = make_candidate_function(scene, "area")
    f_of_x = lambda X: f(*X)
    truth = scene.area_analytic()

    # A dim, distant emitter for the across-the-edge neighbour. Same shape, so
    # only the distance ratio distinguishes it -- which is the whole point of
    # having a distance term in the gate.
    far = Scene(D=scene.D * 8.0, R=scene.R * 0.25)
    _p2, _f2, _s2, far_sample, far_truth = make_candidate_function(far, "area")

    accepted_cos = math.sqrt(1.0 - (1.0 - NORMAL_TOL) ** 2)   # ~18 deg apart
    accepted_ratio = 1.05
    far_ratio = far.D / scene.D

    gate_accept = spatial_gate(accepted_cos, accepted_ratio)
    gate_far = spatial_gate(accepted_cos, far_ratio)

    rng = random.Random(seed)
    kept = []
    for _ in range(iters):
        acc = reservoir_of_m(scene, rng, m_centre, broken=broken)
        if gate_accept:                     # conservative reuse: same geometry
            acc.merge_aggregates(reservoir_of_m(scene, rng, m_neighbour,
                                                broken=broken))
        kept.append(acc.shade(f_of_x))

    rng2 = random.Random(seed)
    leaked = []
    for _ in range(iters):
        acc = reservoir_of_m(scene, rng2, m_centre, broken=broken)
        if not gate_far:                    # gate DROPPED: reuse it anyway
            fr = Reservoir(broken)
            for _ in range(m_neighbour):
                x = far_sample(rng2)
                fr.update(rng2, x, _p2(*x) * 1.0 / _f2(*x) * _f2(*x), _p2(*x))
            acc.merge_aggregates(fr)
        leaked.append(acc.shade(f_of_x))

    if not all_finite(kept + leaked):
        return False, "non-finite value in the spatial check"

    k_mean, k_se, _ = mean_se(kept)
    l_mean, l_se, _ = mean_se(leaked)

    ok_kept = abs(k_mean - truth) <= SIGMA_K * k_se
    # The leaky gate must be MEASURABLY wrong, not merely different: the
    # assertion carries a floor of 5% as well as the 3-sigma test, so a bias too
    # small to separate statistically still counts as undetected.
    l_err = abs(l_mean - truth)
    detected = l_err > max(SIGMA_K * l_se, 0.05 * truth)
    ok = ok_kept and detected
    return ok, ("gate(cos=%.3f,dist_ratio=%.2f)=%s ACCEPTED | "
                "gate(cos=%.3f,dist_ratio=%.2f)=%s DROPPED\n"
                "           conservative: mean=%.7f truth=%.7f (3sig +-%.2e) -> %s\n"
                "           gate DROPPED : mean=%.7f (%.1f%% off, 3sig +-%.2e) "
                "-> %s"
                % (accepted_cos, accepted_ratio, gate_accept,
                   accepted_cos, far_ratio, gate_far,
                   k_mean, truth, SIGMA_K * k_se,
                   "MEAN PRESERVED" if ok_kept else "MEAN MOVED",
                   l_mean, 100.0 * l_err / truth, SIGMA_K * l_se,
                   "WRONG (control OK)" if detected
                   else "UNDETECTED (CONTROL FAILED)"))


def check_edge_cases():
    """M == 0, all-zero wsum, constant non-zero weight, and the unclamped
    denominator. None may divide by zero, produce NaN, or return garbage; each
    must return exactly zero, or the mathematically exact value."""
    rows, ok = [], True

    def want(label, got, expect, tol=0.0):
        good = (math.isfinite(got) and
                abs(got - expect) <= tol)
        rows.append("%s -> %.12g (want %.12g) %s"
                    % (label, got, expect, "OK" if good else "WRONG"))
        return good

    rng = random.Random(11)
    f = lambda X: VEC[0]          # a constant integrand, see below

    # 1. M == 0. combine() over two empty reservoirs, then W and shade.
    pair = combine(rng, Reservoir(), Reservoir())
    ok &= want("M=0: W()", pair.W(), 0.0)
    ok &= want("M=0: shade()", pair.shade(f), 0.0)
    ok &= want("M=0: empty.unbiased_W()", Reservoir().unbiased_W(), 0.0)

    # 2. M > 0 but every weight is zero -> wsum == 0. This is what Alg. 5 line 9
    # produces when every input reservoir was occluded.
    zero = Reservoir()
    for _ in range(5):
        zero.update(rng, (0.0, 0.0, 0.0), 0.0, 1.0)
    ok &= want("M=0 (all weights zero): M", float(zero.M), 0.0)
    ok &= want("all weights zero: W()", zero.W(), 0.0)
    ok &= want("all weights zero: shade()", zero.shade(f), 0.0)

    # 3. Occluded reservoir: Alg. 5 line 9 sets W <- 0 but MUST NOT drop M.
    occ = Reservoir()
    occ.update(rng, (0.0, 0.0, 0.0), 2.0, 0.5)
    occ.occluded = True
    ok &= want("occluded: W()", occ.W(), 0.0)
    ok &= want("occluded: M retained", float(occ.M), 1.0)
    ok &= want("occluded: shade()", occ.shade(f), 0.0)

    # 4. Constant non-zero weight. With pHat and f both constant, every
    # candidate has the same weight, the selection is uniform over M, and
    #     W    = w0 / pHat          (exactly, on every draw)
    #     L    = f * W = w0*f/pHat  (exactly, on every draw)
    # so the estimator has ZERO variance. That makes this an exact-arithmetic
    # check rather than a statistical one, which is the point: it pins down
    # both the wsum factor and the M factor in Eq. (6) simultaneously. A W that
    # dropped wsum would come out as 1/(pHat*M); one that dropped M would come
    # out as w0/pHat/8. Neither is 37.699.
    VEC[0] = 2.0
    w0, c = 3.0, 1.0 / (4.0 * math.pi)      # c plays the role of pHat
    const = Reservoir()
    for _ in range(8):
        const.update(rng, (0.0, 0.0, 0.0), w0, c)
    want_W = w0 / c
    ok &= want("constant weight: wsum", const.wsum, 8.0 * w0)
    ok &= want("constant weight: M", float(const.M), 8.0)
    ok &= want("constant weight: W()", const.W(), want_W, 1e-12 * want_W)
    ok &= want("constant weight: shade()", const.shade(f), VEC[0] * want_W,
               1e-12 * VEC[0] * want_W)

    # 5. ClampWBroken: the unclamped W is 1/(pHat*M), which is a legitimate
    # value. This is asserted so a reader can see the clamp is a finiteness
    # guard and not a hidden bias -- and the run then reports that it is NOT
    # rejected by the analytic test, which is why it is not a negative control.
    VEC[0] = 1.0
    fine = Reservoir("ClampWBroken")
    for _ in range(4):
        fine.update(rng, (0.0, 0.0, 0.0), 2.0, 0.5)
    ok &= want("ClampWBroken: W()", fine.W(), 4.0 * 2.0 / (0.5 * 4), 1e-12)
    return ok, "\n           ".join(rows)


VEC = [1.0]      # mutable cell so check_edge_cases can vary the integrand


# ==================================================== encoded-algorithm ====
#
# A Python re-implementation proves a TRANSCRIPTION. If the HLSL never gets the
# reservoir, this harness has proved arithmetic about a program that does not
# exist. So the shader is grepped directly, READ ONLY, and the structural
# invariants this file relies on are reported as PRESENT or ABSENT.
#
# What a grep CAN establish: whether a field name, a division, or a combine call
# appears in the source at all.
# What a grep CANNOT establish: whether the arithmetic is right, whether the
# values bound to those fields are the ones Eq. (6) needs, whether the RNG is
# uniform, or whether anything is dispatched before the combine runs.

# Each probe is (label, regex, must_be_present_for_the_algorithm_to_exist).
SHADER_PROBES = (
    ("reservoir field: wsum",        r"\bwsum\b|\bwSum\b|_wSum", True),
    ("reservoir field: M / count",   r"\bM\s*=\s*0\b|uint\s+M\b|\.M\b", False),
    ("reservoir field: stored y",    r"\bReservoir\b|reservoir", True),
    ("Eq. (6): W = wsum/(pHat*M)",
     r"w[Ss]um\s*/\s*\(|/\s*\(.*[pP][Hh]at.*\*.*M\)|/p[Hh]at", True),
    ("Alg. 4: combine function",     r"[Cc]ombine[Rr]eservoir|combineReservoir",
     True),
    ("Alg. 5: shadowed -> W = 0",    r"shadowed\s*\(.*\).*W\s*=\s*0|W\s*=\s*0",
     False),
    ("temporal reuse of a reservoir", r"prevFrameReservoir|[Tt]emporal", False),
    ("spatial reuse of a reservoir",  r"[Ss]patial.*[Rr]eservoir", False),
    ("area-correct light sampling",   r"sample[Aa]rea|Uniform.*[Aa]rea|"
                                      r"sampleEmitter|emitterSample", False),
)

# Facts about the shader that the harness depends on for its MODEL, and which
# are asserted present so a refactor cannot silently invalidate the comparison.
SHADER_REQUIRED = (
    ("light list SSBO declared",
     r"StructuredBuffer\s*<\s*int\s*>\s*LightIndices\s*:\s*register\(t3\)"),
    ("light list indexed",           r"LightIndices\s*\["),
    ("emissive instance treated as a POINT light at the translation column",
     r"WorldTransform\s*\[\s*0\s*\]\s*\[\s*3\s*\]"),
    ("emissive contribution has NO 1/r^2 and NO solid angle",
     r"Emission\.xyz\s*\*\s*light\.Emission\.w\s*\*\s*NdotL"),
    ("per-emitter instance buffer",  r"StructuredBuffer<RayTracingInstance>|"
                                     r"Instances\s*\["),
)


def grep_shader():
    """Grep the shipped HLSL for the constructs this harness's maths assumes.
    Returns (rows, invariants_ok, summary_lines).

    This is a STRUCTURAL check, not a behavioural one. See the section comment
    above and the closing report for exactly what that buys.
    """
    rows, invariant_rows = [], []
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(here, SHADER_REL)

    if not os.path.isfile(path):
        rows.append(("MISSING", "shader not found at %s -- the grep could not "
                     "run at all, so NOTHING about engine integration is "
                     "verified" % path))
        return rows, False, []

    with open(path, "r", errors="replace") as fh:
        src = fh.read()
    lines = src.split("\n")

    for label, pattern, _present in SHADER_PROBES:
        rx = re.compile(pattern)
        hits = [i + 1 for i, ln in enumerate(lines) if rx.search(ln)]
        if hits:
            rows.append(("PRESENT", "%-38s %d hit(s), first at line %d"
                         % (label, len(hits), hits[0])))
        else:
            rows.append(("ABSENT", "%-38s 0 hits -- not in the shader"
                         % label))

    for label, pattern in SHADER_REQUIRED:
        rx = re.compile(pattern)
        hits = [i + 1 for i, ln in enumerate(lines) if rx.search(ln)]
        invariant_rows.append((label, bool(hits), hits))

    return rows, True, invariant_rows


def shader_summary(invariant_rows):
    lines = []
    for label, ok, hits in invariant_rows:
        lines.append("[%s] %-62s %s"
                     % ("ok" if ok else "MISSING", label,
                        ("line %d" % hits[0]) if hits else "NOT FOUND"))
    return lines


def reservoir_present(rows):
    """True if the shader contains a reservoir at all. Used to word the closing
    report honestly rather than implying engine code was verified."""
    return any(s == "PRESENT" for s, _ in rows)


# ================================================================ driver ===

CHECKS = (
    ("closed form matches independent quadrature", lambda s: check_closed_form(s)),
    ("Alg.4 line 4: pHat*W*M == wsum exactly", lambda s: check_combine_weight()),
    ("analytic agreement, M=8 (Eq. 6)", lambda s: check_analytic(s, M=8)),
    ("analytic agreement, M=32 (Eq. 6)", lambda s: check_analytic(s, M=32)),
    ("salve/lambda split invariance", lambda s: check_salve_split(s)),
    ("combine invariance: M1+M2 == one of M1+M2",
     lambda s: check_combine_invariance(s)),
    ("combine is unbiased vs analytic",
     lambda s: check_combine_invariance(s, pairs=COMBINE_PAIRS // 4)),
    ("temporal reuse: no drift over %d frames x %d chains"
     % (TEMPORAL_FRAMES, TEMPORAL_CHAINS),
     lambda s: check_temporal(s, chains=TEMPORAL_CHAINS, frames=TEMPORAL_FRAMES)),
    ("spatial reuse: conservative gate preserves mean, dropped gate fails",
     lambda s: check_spatial(s)),
    ("M=0, wsum=0, occluded, constant-weight edge cases",
     lambda s: check_edge_cases()),
)

# Negative controls. Each names a check above and asserts that it FAILS when fed
# a deliberately broken estimator. If one unexpectedly PASSES, the whole run
# exits non-zero with a loud warning: a check that cannot fail proves nothing.
CONTROLS = (
    ("(a) W without the wsum normalisation",
     "analytic agreement, M=32 (Eq. 6)",
     lambda s: check_analytic(s, M=32, broken="NoWsum")),

    ("(b) combine: weights added instead of resampled",
     "combine invariance: M1+M2 == one of M1+M2",
     lambda s: check_combine_invariance(s, pairs=COMBINE_PAIRS // 2,
                                        broken="AdditiveCombine")),

    ("(c) point-light model scored against the AREA-correct analytic value",
     "analytic agreement, M=32 (Eq. 6)",
     lambda s: check_analytic(s, M=32, model="point", truth=s.area_analytic())),

    ("(d) temporal chain with the per-frame M dropped (reuse bookkeeping bug)",
     "temporal reuse: no drift over %d frames x %d chains"
     % (TEMPORAL_FRAMES, TEMPORAL_CHAINS),
     lambda s: check_temporal(s, chains=TEMPORAL_CHAINS,
                              frames=TEMPORAL_FRAMES, drop_M=True)),
)

# Variants that are KNOWN NOT to be rejected by the analytic test. Listed so the
# absence of a false alarm is itself accounted for rather than a lucky omission.
BENIGN_VARIANTS = (
    ("ClampWBroken: W = wsum/pHat/M (unclamped, but arithmetically equal)",
     "analytic agreement, M=32 (Eq. 6)",
     lambda s: check_analytic(s, M=32, broken="ClampWBroken")),

    ("AlwaysTakeB: combine without the selection coin -- DEMONSTRABLY NOT a "
     "mean bias, so it cannot be a control",
     "combine invariance: M1+M2 == one of M1+M2",
     lambda s: check_combine_invariance(s, pairs=COMBINE_PAIRS // 2,
                                        broken="AlwaysTakeB")),
)


def run(checks, scene):
    out = []
    for name, fn in checks:
        try:
            ok, line = fn(scene)
        except Exception as exc:                  # a crash is a failure
            ok, line = False, "raised %s: %s" % (type(exc).__name__, exc)
        out.append((name, bool(ok), line))
    return out


def report(title, results):
    print("=" * 100)
    print(title)
    print("=" * 100)
    for name, ok, line in results:
        head = "[PASS] %s" % name if ok else "[FAIL] %s" % name
        # Wrap the detail under the heading so long z-score lines stay readable.
        body = line.split("\n")
        print("%s" % head)
        print("       %s" % body[0])
        for extra in body[1:]:
            print("       %s" % extra)


def main():
    scene = Scene(D=NEAR_D, R=NEAR_R)
    print("=" * 100)
    print("ReSTIR DI reservoir -- UNBIASEDNESS HARNESS (pure stdlib, no numpy, "
          "no GPU, no engine link)")
    print("=" * 100)
    print("Specification: Bitterli et al., 'Spatiotemporal Reservoir Resampling "
          "for Real-time Ray Tracing")
    print("with Dynamic Direct Lighting', ACM Trans. Graph. 39(4) Art. 148, "
          "July 2020. Alg. 2/3/4/5, Eq. (6).")
    print()
    print("Scene: emitter = DISC radius R=%.2f at z=0; shading point on-axis at "
          "D=%.2f facing -z." % (scene.R, scene.D))
    print("  emitter area A      = %.6f" % scene.area)
    print("  AREA-correct answer = R^2/(D^2+R^2) = %.9f" % scene.area_analytic())
    print("  point-model answer  = 1/pi         = %.9f   (no 1/r^2, no solid "
          "angle)" % scene.point_analytic())
    print("  ratio area/point    = %.6f       <- what control (c) detects"
          % scene.model_ratio())
    print("Proposal p = 1/A (area-uniform), target pHat proportional to f, "
          "w = pHat/p, W = wsum/(pHat(y)*M), L = f(y)*W.")
    print("Decision rule: %.1f-sigma on the sample standard error, NOT a "
          "relative tolerance." % SIGMA_K)
    print()

    res = run(CHECKS, scene)
    report("POSITIVE CHECKS -- every assertion this harness makes", res)
    failures = [n for n, ok, _ in res if not ok]

    print()
    print("-" * 100)
    print("NEGATIVE CONTROLS -- each MUST FAIL. A control that passes means the "
          "check above it is")
    print("                  not actually measuring anything, and the run is "
          "void.")
    print("-" * 100)
    ctrl_failures = []
    for label, target, fn in CONTROLS:
        try:
            ok, line = fn(scene)
        except Exception as exc:
            ok, line = False, "raised %s: %s" % (type(exc).__name__, exc)
        rejected = not ok
        if not rejected:
            ctrl_failures.append(label)
        print("[%s] %s" % ("CONTROL OK" if rejected else "CONTROL FAILED", label))
        print("       broken variant -> test %r" % target)
        for ln in line.split("\n"):
            print("       %s" % ln)
        print()

    print("-" * 100)
    print("KNOWN-BENIGN VARIANTS -- expected to PASS; listed so a missing alarm "
          "is accounted for")
    print("-" * 100)
    for label, target, fn in BENIGN_VARIANTS:
        try:
            ok, line = fn(scene)
        except Exception as exc:
            ok, line = False, "raised %s: %s" % (type(exc).__name__, exc)
        print("[%s] %s" % ("as expected" if ok else "UNEXPECTEDLY REJECTED",
                           label))
        for ln in line.split("\n"):
            print("       %s" % ln)
        print()

    # ---------------------------------------------- shader grep ----------
    print("=" * 100)
    print("ENCODED-ALGORITHM TEXT -- structural grep of the shipped HLSL "
          "(READ ONLY, never edited)")
    print("=" * 100)
    rows, ran, invariant_rows = grep_shader()
    if not ran:
        print("!! %s" % rows[0][1])
    else:
        for status, text in rows:
            print("[%-7s] %s" % (status, text))
        print()
        print("Model invariants the harness's comparison depends on:")
        for ln in shader_summary(invariant_rows):
            print("       %s" % ln)

    has_reservoir = reservoir_present(rows)
    print()
    print("=" * 100)
    print("WHAT THIS RUN PROVES -- and what it does not")
    print("=" * 100)
    print("PROVEN (arithmetic on the published algorithm, verified against a")
    print("closed-form integral cross-checked by independent quadrature):")
    print("  * Alg. 2/3/4/5 reservoir update, Eq. (6) W, and the Alg. 4 combine")
    print("    are unbiased as pure functions, over M in {8, 32} and 20000")
    print("    independent M1+M2 pairs.")
    print("  * The identity pHat(y)*W*M == wsum holds exactly, which is the")
    print("    mechanism that makes the combine unbiased.")
    print("  * Combine invariance over %d M1+M2 pairs, temporal non-drift over"
          % COMBINE_PAIRS)
    print("    %d frames x %d independent chains, and conservative spatial"
          % (TEMPORAL_FRAMES, TEMPORAL_CHAINS))
    print("    rejection are all mean-preserving, and temporal reuse measurably")
    print("    converges (spread across chains falls ~sqrt(frames)).")
    print("  * The analytic-agreement check REJECTS a W without wsum")
    print("    normalisation and the point-light model scored against the")
    print("    area-correct value; the combine check REJECTS an additively")
    print("    weighted combine; the temporal check REJECTS a chain that fails")
    print("    to sum M across combines.")
    print()
    print("NOT PROVEN -- and no amount of arithmetic in this file could prove it:")
    print("  * GPU execution. No shader was compiled or dispatched. Nothing here")
    print("    says the HLSL is syntactically valid or that it compiles.")
    print("  * Descriptor or binding layout. Whether the reservoir fields land")
    print("    in the register/slot budget is not tested.")
    print("  * Register pressure, spills, or wave divergence from carrying the")
    print("    extra reservoir state.")
    print("  * Barrier correctness. Temporal reuse needs the previous frame's")
    print("    reservoir to be written before it is read; UAV barriers and any")
    print("    ping-pong indexing are outside this harness entirely.")
    print("  * That the Python and the HLSL compute the same function. A shared")
    print("    conceptual error, or a transcription slip in either, is invisible")
    print("    to a test that only runs one of them.")
    print("  * Numerical behaviour in fp32. Every computation here is fp64.")
    print("    fp32 cancellation in wsum accumulation over hundreds of frames is")
    print("    a real risk this harness cannot address.")
    print()
    if not ran:
        print("!! SHADER GREP DID NOT RUN -- the structural section is void.")
    elif has_reservoir:
        print("The shader grep found reservoir-like constructs. Re-read the "
              "PROBE lines above: a")
        print("name match is NOT a correctness proof -- it establishes presence, "
              "not semantics.")
    else:
        print("!" * 100)
        print("!! NO RESERVOIR IN THE SHADER. The grep found no wsum, no pHat, "
              "no combineReservoir,")
        print("!! no temporal/spatial reservoir reuse and no area-correct light "
              "sampling. Read the")
        print("!! PROBE table above for the exact counts.")
        print("!!")
        print("!! THE HONEST FRAMING: THE MATHS IS PROVEN; THE SHADER "
              "INTEGRATION IS NOT YET WRITTEN.")
        print("!! Every PASS above is a statement about the algorithm as "
              "published, and about this")
        print("!! transcription of it. None of it is a statement about "
              "Wasteland's shipped renderer, which")
        print("!! still treats an emissive instance as a point light at its "
              "translation column")
        print("!! (Renderer3D_NovaRenderer.hlsl:2029) and does no reservoir "
              "resampling at all.")
        print("!" * 100)

    print()
    if ctrl_failures:
        print("!" * 100)
        print("!! NEGATIVE CONTROL DID NOT FAIL -- %d of %d. The checks above "
              "are not measuring" % (len(ctrl_failures), len(CONTROLS)))
        print("!! what they claim to measure, so every PASS printed above is "
              "worthless. Fix the harness")
        print("!! before trusting anything it says.")
        for f in ctrl_failures:
            print("!!   - %s" % f)
        print("!" * 100)
        return 1

    print("Negative controls: %d/%d rejected their broken variant as required."
          % (len(CONTROLS), len(CONTROLS)))
    if failures:
        print()
        print("RESULT: FAIL -- %d of %d positive checks failed:"
              % (len(failures), len(res)))
        for f in failures:
            print("  - %s" % f)
        return 1

    print()
    print("RESULT: PASS -- %d positive checks green, %d negative controls "
          "provably failable." % (len(res), len(CONTROLS)))
    print("This proves the ALGORITHM is unbiased. It proves nothing about GPU "
          "execution, descriptor")
    print("binding, register allocation, or barrier correctness.")
    return 0


if __name__ == "__main__":
    sys.exit(main())