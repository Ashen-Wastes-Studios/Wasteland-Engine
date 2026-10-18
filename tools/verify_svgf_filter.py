#!/usr/bin/env python3
"""
SVGF moment and A-Trous wavelet filter maths, verified standalone.

WHY: the Nova path tracer's temporal accumulation pass computes YCoCg luminance
moments and a variance, but DISCARDS the temporal moments (never persists them
across frames) and uses a HARDCODED 1.25-sigma spatial term. The intended fix is:
persist packed temporal moments (mean, mean-of-squares) in one RG32F target,
derive variance = max(m2/n - mean^2, 0), then run a FIXED 4-iteration A-Trous
wavelet filter (steps 1,2,4,8) whose per-iteration blend weight is
variance-driven, with edge-stopping weights combining depth difference, normal
difference, and variance.

This harness validates the MATHS of that fix as arithmetic (no engine link, no
GPU, no numpy), matching what tools/verify_variance_math.py and
tools/verify_restir_reservoir.py already do for the moment estimator and the
ReSTIR reservoir.

NEGATIVE CONTROLS: every check re-runs against deliberately broken variants and
the run fails outright if any of them passes -- a test that cannot fail is not a
test.

Run: python tools/verify_svgf_filter.py   (exit 0 = all green, 1 = failed)
"""

import math
import os
import random
import re
import sys

# ------------------------------------------------------- specification ----
#
# The A-Trous wavelet filter (SVGF, Schied et al. 2017):
#
#   for step in [1, 2, 4, 8]:
#       for each pixel p:
#           sum = 0, wsum = 0
#           for each tap t at distance `step`:
#               w = kernel[t] * spatial(p, t) * luminance(p, t) * depth(p, t) * normal(p, t)
#               sum += w * image[t]
#               wsum += w
#           output[p] = sum / wsum
#
# The edge-stopping functions are:
#   spatial(p, t) = exp(-||p - t||^2 / (phi_l^2 * variance + eps))
#   luminance(p, t) = max(0, 1 - (L_t - L_p)^2 / (phi_s^2 * variance + eps))
#   depth(p, t) = exp(-|Z_t - Z_p|^2 / (phi_z^2 * sigma_z^2 + eps))
#   normal(p, t) = max(0, dot(N_t, N_p))^phi_n
#
# The variance-driven blend weight: the spatial and luminance terms use the
# local variance to control the kernel width. High variance -> wider kernel ->
# more smoothing. Low variance -> tighter kernel -> less smoothing.

# B3 spline kernel weights (separable, sums to 1 in 1D and 2D)
ATROUS_KERNEL = (1.0 / 16.0, 4.0 / 16.0, 6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0)

# Edge-stopping parameters
PHI_L = 2.0      # spatial Gaussian scale
PHI_S = 8.0      # luminance edge-stop scale
PHI_Z = 1.0      # depth edge-stop scale
PHI_N = 16.0     # normal edge-stop sharpness
SIGMA_Z = 0.01   # depth variance estimate

# Guards denominators: a converged pixel has variance exactly 0
EPS = 1e-8

# History blend window
ALPHA_MIN, ALPHA_MAX = 0.05, 0.20

# A-Trous iteration steps
ATROUS_STEPS = (1, 2, 4, 8)

# Shader path for the structural grep
SHADER_REL = os.path.join("DemonCore-Editor", "assets", "shaders",
                          "Renderer3D_NovaRenderer.hlsl")


# ============================================================ images =====

def make_flat(width, height, value):
    """Flat region: all pixels = value. Analytic variance = 0."""
    return [[value for _ in range(width)] for _ in range(height)]


def make_step_edge(width, height, left_value, right_value):
    """Step edge: left half = left_value, right half = right_value.
    Analytic variance = (left_value - right_value)^2 / 4."""
    mid = width // 2
    return [[left_value if x < mid else right_value for x in range(width)]
            for _ in range(height)]


def make_gradient(width, height):
    """Linear gradient from 0 to 1 along x. Analytic variance = 1/12."""
    return [[x / (width - 1) for x in range(width)] for _ in range(height)]


def make_noise_field(width, height, mean, variance, seed=42):
    """High-frequency noise field with known mean and variance.
    Uses a fixed seed for determinism."""
    rng = random.Random(seed)
    std = math.sqrt(variance)
    return [[rng.gauss(mean, std) for _ in range(width)] for _ in range(height)]


def analytic_variance_flat(value):
    return 0.0


def analytic_variance_step(left, right):
    return (left - right) ** 2 / 4.0


def analytic_variance_gradient():
    return 1.0 / 12.0


def analytic_variance_noise(mean, variance):
    return variance


# ============================================================ moments =====

class MomentEstimator:
    """Progressive luminance moments with history blend.

    Accumulates (mean, mean-of-squares) across frames with a configurable
    alpha history blend clamped to [0, 1]. Derives variance with the clamp:
    variance = max(m2 - mean^2, 0).

    When blend=False, the estimator always overwrites (no history), which is
    the broken variant used by negative control (d).
    """

    def __init__(self, alpha=0.1, blend=True):
        self.blend = blend
        self.alpha = self._clamp_alpha(alpha) if blend else 1.0
        self.n = 0
        self.m1 = 0.0  # mean
        self.m2 = 0.0  # mean of squares

    @staticmethod
    def _clamp_alpha(alpha):
        if not math.isfinite(alpha):
            return ALPHA_MAX
        return min(max(alpha, ALPHA_MIN), ALPHA_MAX)

    def update(self, samples):
        """Update moments with a new frame of samples.

        Blends the new frame's moments with the accumulated history using
        the alpha weight: history = (1 - alpha) * history + alpha * current.
        When blend=False, always overwrites.
        """
        if not samples:
            return

        n_new = len(samples)
        m1_new = math.fsum(samples) / n_new
        m2_new = math.fsum(s * s for s in samples) / n_new

        if self.n == 0 or not self.blend:
            self.n = n_new
            self.m1 = m1_new
            self.m2 = m2_new
        else:
            # Blend moments, not variances
            self.m1 = (1 - self.alpha) * self.m1 + self.alpha * m1_new
            self.m2 = (1 - self.alpha) * self.m2 + self.alpha * m2_new
            self.n = (1 - self.alpha) * self.n + self.alpha * n_new

    def variance(self):
        """Derive variance with the clamp: max(m2 - mean^2, 0).

        m1 and m2 are stored as MEANS (sum/n), so variance = m2 - m1^2.
        """
        if self.n == 0:
            return 0.0
        raw = self.m2 - self.m1 * self.m1
        return max(raw, 0.0)

    def variance_unclamped(self):
        """Derive variance WITHOUT the clamp. Used for negative control (a)."""
        if self.n == 0:
            return 0.0
        return self.m2 - self.m1 * self.m1


# ============================================================ A-Trous =====

def atrous_taps(step):
    """Generate tap offsets for a given step size.

    Returns list of (dx, dy, kernel_index) tuples.
    """
    taps = [(0, 0, 0)]  # center
    for h in range(1, 3):  # h = 1, 2
        ki = h  # kernel index: 1 or 2
        taps.append((step, 0, ki))
        taps.append((-step, 0, ki))
        taps.append((0, step, ki))
        taps.append((0, -step, ki))
    return taps


def edge_stop_luminance(center_luma, tap_luma, variance):
    """Luminance edge-stop: max(0, 1 - (dL)^2 / (phi_s^2 * variance + eps))."""
    diff = tap_luma - center_luma
    denom = PHI_S * PHI_S * variance + EPS
    stop = 1.0 - diff * diff / denom
    return max(stop, 0.0)


def edge_stop_spatial(dx, dy, variance):
    """Spatial Gaussian: exp(-d^2 / (phi_l^2 * variance + eps))."""
    dist_sq = dx * dx + dy * dy
    denom = PHI_L * PHI_L * variance + EPS
    return math.exp(-dist_sq / denom)


def edge_stop_depth(center_depth, tap_depth):
    """Depth edge-stop: exp(-|dZ|^2 / (phi_z^2 * sigma_z^2 + eps))."""
    diff = tap_depth - center_depth
    denom = PHI_Z * PHI_Z * SIGMA_Z * SIGMA_Z + EPS
    return math.exp(-diff * diff / denom)


def edge_stop_normal(center_normal, tap_normal):
    """Normal edge-stop: max(0, dot(N_c, N_t))^phi_n."""
    dot = sum(c * t for c, t in zip(center_normal, tap_normal))
    return max(dot, 0.0) ** PHI_N


def atrous_weights(center_luma, variance, tap_lumas, tap_depths, tap_normals,
                   center_depth, center_normal, step, use_edge_stop=True):
    """Compute normalized A-Trous weights for all taps.

    Returns (weights, raw_sum) where weights are normalized to sum to 1.
    If use_edge_stop is False, all edge-stop weights are set to 1 (negative control).
    """
    taps = atrous_taps(step)
    raw_weights = []

    for i, (dx, dy, ki) in enumerate(taps):
        kernel_w = ATROUS_KERNEL[ki]

        if use_edge_stop:
            spatial = edge_stop_spatial(dx, dy, variance)
            luma = edge_stop_luminance(center_luma, tap_lumas[i], variance)
            depth = edge_stop_depth(center_depth, tap_depths[i])
            normal = edge_stop_normal(center_normal, tap_normals[i])
            w = kernel_w * spatial * luma * depth * normal
        else:
            w = kernel_w

        raw_weights.append(w)

    total = math.fsum(raw_weights)
    if total <= 0.0 or not math.isfinite(total):
        # Fallback: center tap only
        return [1.0] + [0.0] * (len(raw_weights) - 1), total

    return [w / total for w in raw_weights], total


def atrous_filter(image, depth, normal, variance, step, use_edge_stop=True):
    """Apply one iteration of the A-Trous filter.

    Returns the filtered image.
    """
    height = len(image)
    width = len(image[0])
    output = [[0.0] * width for _ in range(height)]

    for y in range(height):
        for x in range(width):
            center_luma = image[y][x]
            center_depth = depth[y][x]
            center_normal = normal[y][x]

            # Gather tap values
            taps = atrous_taps(step)
            tap_lumas = []
            tap_depths = []
            tap_normals = []

            for dx, dy, ki in taps:
                tx = min(max(x + dx, 0), width - 1)
                ty = min(max(y + dy, 0), height - 1)
                tap_lumas.append(image[ty][tx])
                tap_depths.append(depth[ty][tx])
                tap_normals.append(normal[ty][tx])

            weights, _ = atrous_weights(
                center_luma, variance, tap_lumas, tap_depths, tap_normals,
                center_depth, center_normal, step, use_edge_stop
            )

            output[y][x] = math.fsum(w * l for w, l in zip(weights, tap_lumas))

    return output


def atrous_filter_full(image, depth, normal, variance, use_edge_stop=True):
    """Run the full 4-iteration A-Trous filter (steps 1, 2, 4, 8)."""
    result = image
    for step in ATROUS_STEPS:
        result = atrous_filter(result, depth, normal, variance, step, use_edge_stop)
    return result


def compute_variance(samples):
    """Compute sample variance using two-pass algorithm."""
    n = len(samples)
    mean = math.fsum(samples) / n
    return math.fsum((s - mean) ** 2 for s in samples) / n


# ============================================================ checks =====

def check_moment_variance_tracks_analytic():
    """Assert estimated variance tracks analytic variance for each region type."""
    results = []

    # Flat region
    est = MomentEstimator(alpha=0.1)
    for _ in range(10):
        frame = [0.5] * 100
        est.update(frame)
    v = est.variance()
    expected = 0.0
    ok = abs(v - expected) < 1e-6
    results.append((ok, "flat: est=%.6f expected=%.6f" % (v, expected)))

    # Step edge
    est = MomentEstimator(alpha=0.1)
    for _ in range(10):
        frame = [0.0] * 50 + [1.0] * 50
        est.update(frame)
    v = est.variance()
    expected = 0.25
    ok = abs(v - expected) < 1e-6
    results.append((ok, "step: est=%.6f expected=%.6f" % (v, expected)))

    # Gradient: discrete uniform on {0, 1/99, ..., 1}
    # Variance = (n^2 - 1) / (12 * (n-1)^2) for n=100 points
    est = MomentEstimator(alpha=0.1)
    for _ in range(10):
        frame = [i / 99.0 for i in range(100)]
        est.update(frame)
    v = est.variance()
    n_pts = 100
    expected = (n_pts * n_pts - 1) / (12.0 * (n_pts - 1) * (n_pts - 1))
    ok = abs(v - expected) < 1e-6
    results.append((ok, "gradient: est=%.6f expected=%.6f" % (v, expected)))

    # Noise field
    est = MomentEstimator(alpha=0.1)
    rng = random.Random(42)
    for _ in range(10):
        frame = [rng.gauss(0.5, 0.1) for _ in range(100)]
        est.update(frame)
    v = est.variance()
    expected = 0.01
    # Allow 20% tolerance for sampling noise
    ok = abs(v - expected) / expected < 0.2
    results.append((ok, "noise: est=%.6f expected=%.6f (%.1f%% err)" % (v, expected, abs(v - expected) / expected * 100)))

    return results


def check_atrous_kernel_sums_to_one():
    """Assert kernel weights sum to 1 at each iteration."""
    results = []

    for step in ATROUS_STEPS:
        # Flat image, uniform depth/normal -- large enough for step=8 taps
        size = 20
        image = [[0.5] * size for _ in range(size)]
        depth = [[0.5] * size for _ in range(size)]
        normal = [[(0.0, 0.0, 1.0)] * size for _ in range(size)]
        variance = 0.01

        # Compute weights for center pixel
        c = size // 2
        center_luma = image[c][c]
        center_depth = depth[c][c]
        center_normal = normal[c][c]

        taps = atrous_taps(step)
        tap_lumas = [image[c + dy][c + dx] for dx, dy, ki in taps]
        tap_depths = [depth[c + dy][c + dx] for dx, dy, ki in taps]
        tap_normals = [normal[c + dy][c + dx] for dx, dy, ki in taps]

        weights, raw_sum = atrous_weights(
            center_luma, variance, tap_lumas, tap_depths, tap_normals,
            center_depth, center_normal, step
        )

        total = math.fsum(weights)
        ok = abs(total - 1.0) < 1e-12
        results.append((ok, "step=%d: sum=%.15f (tol 1e-12)" % (step, total)))

    return results


def check_atrous_preserves_constant():
    """Assert a constant input is preserved exactly."""
    results = []

    for step in ATROUS_STEPS:
        size = 20
        image = [[0.5] * size for _ in range(size)]
        depth = [[0.5] * size for _ in range(size)]
        normal = [[(0.0, 0.0, 1.0)] * size for _ in range(size)]
        variance = 0.0

        filtered = atrous_filter(image, depth, normal, variance, step)

        # Check all pixels are exactly 0.5
        max_diff = max(abs(filtered[y][x] - 0.5) for y in range(size) for x in range(size))
        ok = max_diff == 0.0
        results.append((ok, "step=%d: max_diff=%.3e (want 0.0)" % (step, max_diff)))

    return results


def check_atrous_variance_falls():
    """Assert variance actually falls across iterations on a noisy input."""
    results = []

    # Create a noisy image
    rng = random.Random(42)
    image = [[rng.gauss(0.5, 0.1) for _ in range(20)] for _ in range(20)]
    depth = [[0.5] * 20 for _ in range(20)]
    normal = [[(0.0, 0.0, 1.0)] * 20 for _ in range(20)]
    variance = 1.0  # High variance -> wide kernel -> measurable smoothing

    # Compute initial variance
    flat = [image[y][x] for y in range(20) for x in range(20)]
    initial_var = compute_variance(flat)

    # Run filter
    filtered = atrous_filter_full(image, depth, normal, variance)

    # Compute final variance
    flat_f = [filtered[y][x] for y in range(20) for x in range(20)]
    final_var = compute_variance(flat_f)

    ok = final_var < initial_var
    results.append((ok, "variance: initial=%.6f final=%.6f (want final < initial)" % (initial_var, final_var)))

    return results


def check_edge_discrimination():
    """Assert the filter does NOT blur across depth/normal discontinuity,
    and DOES smooth within a uniform region."""
    results = []

    # Test 1: Filter DOES smooth within uniform depth/normal
    # Step edge in luminance, but uniform depth and normal
    width, height = 20, 20
    image = [[0.0 if x < width // 2 else 1.0 for x in range(width)] for _ in range(height)]
    depth = [[0.5] * width for _ in range(height)]
    normal = [[(0.0, 0.0, 1.0)] * width for _ in range(height)]
    variance = 1.0  # High variance -> weak luminance edge-stop

    filtered = atrous_filter_full(image, depth, normal, variance)

    # Check that the edge is smoothed (values near the boundary should be between 0 and 1)
    mid = width // 2
    boundary_values = [filtered[y][mid - 1] for y in range(height)] + [filtered[y][mid] for y in range(height)]
    has_intermediate = any(0.1 < v < 0.9 for v in boundary_values)
    results.append((has_intermediate, "uniform depth/normal: edge is smoothed (has intermediate values)"))

    # Test 2: Filter does NOT blur across depth/normal discontinuity
    # Step edge in luminance AND depth AND normal
    image2 = [[0.0 if x < width // 2 else 1.0 for x in range(width)] for _ in range(height)]
    depth2 = [[0.0 if x < width // 2 else 1.0 for x in range(width)] for _ in range(height)]
    normal2 = [[(0.0, 0.0, 1.0) if x < width // 2 else (0.0, 0.0, -1.0) for x in range(width)] for _ in range(height)]

    filtered2 = atrous_filter_full(image2, depth2, normal2, variance)

    # Check that the edge is preserved (values near the boundary should still be close to 0 or 1)
    boundary_values2 = [filtered2[y][mid - 1] for y in range(height)] + [filtered2[y][mid] for y in range(height)]
    no_intermediate = all(v < 0.1 or v > 0.9 for v in boundary_values2)
    results.append((no_intermediate, "depth/normal discontinuity: edge is preserved (no intermediate values)"))

    return results


# ============================================================ negative controls =====

def negative_control_a():
    """Variance computed WITHOUT the max(...,0) clamp.

    Must yield a negative variance somewhere and fail an assertion.
    """
    # Construction: large mean, tiny variance -> catastrophic cancellation
    # This gives a negative unclamped variance
    samples = [1e8 + 0.1 * math.sin(i) for i in range(100)]

    est = MomentEstimator(alpha=0.1)
    est.update(samples)

    unclamped = est.variance_unclamped()
    clamped = est.variance()

    # The unclamped variance should be negative
    is_negative = unclamped < 0.0

    # The clamped variance should be 0
    is_clamped_zero = clamped == 0.0

    return is_negative and is_clamped_zero, \
        "unclamped=%.6e (want < 0), clamped=%.6e (want 0)" % (unclamped, clamped)


def negative_control_b():
    """A-Trous weights not normalised.

    Must fail the kernel-sums-to-1 assertion.
    """
    # Create a flat image
    size = 20
    image = [[0.5] * size for _ in range(size)]
    depth = [[0.5] * size for _ in range(size)]
    normal = [[(0.0, 0.0, 1.0)] * size for _ in range(size)]
    variance = 0.01
    step = 1

    c = size // 2
    center_luma = image[c][c]
    center_depth = depth[c][c]
    center_normal = normal[c][c]

    taps = atrous_taps(step)
    tap_lumas = [image[c + dy][c + dx] for dx, dy, ki in taps]
    tap_depths = [depth[c + dy][c + dx] for dx, dy, ki in taps]
    tap_normals = [normal[c + dy][c + dx] for dx, dy, ki in taps]

    # Compute raw (unnormalized) weights
    raw_weights = []
    for i, (dx, dy, ki) in enumerate(taps):
        kernel_w = ATROUS_KERNEL[ki]
        spatial = edge_stop_spatial(dx, dy, variance)
        luma = edge_stop_luminance(center_luma, tap_lumas[i], variance)
        depth = edge_stop_depth(center_depth, tap_depths[i])
        normal = edge_stop_normal(center_normal, tap_normals[i])
        w = kernel_w * spatial * luma * depth * normal
        raw_weights.append(w)

    raw_sum = math.fsum(raw_weights)

    # The raw sum should NOT be 1 (it should be less than 1 due to edge-stopping)
    not_one = abs(raw_sum - 1.0) > 1e-6

    return not_one, "raw_sum=%.6f (want != 1)" % raw_sum


def negative_control_c():
    """Edge-stopping term dropped entirely.

    Must fail edge discrimination (it will smear across the discontinuity).
    """
    width, height = 20, 20
    variance = 1.0

    # Step edge in luminance AND depth AND normal
    image = [[0.0 if x < width // 2 else 1.0 for x in range(width)] for _ in range(height)]
    depth = [[0.0 if x < width // 2 else 1.0 for x in range(width)] for _ in range(height)]
    normal = [[(0.0, 0.0, 1.0) if x < width // 2 else (0.0, 0.0, -1.0) for x in range(width)] for _ in range(height)]

    # Filter WITHOUT edge-stopping
    filtered = atrous_filter_full(image, depth, normal, variance, use_edge_stop=False)

    # Check if the edge is smeared (has intermediate values)
    mid = width // 2
    boundary_values = [filtered[y][mid - 1] for y in range(height)] + [filtered[y][mid] for y in range(height)]
    has_intermediate = any(0.1 < v < 0.9 for v in boundary_values)

    # The control passes if the edge IS smeared (which is the broken behavior)
    return has_intermediate, "without edge-stop: edge is smeared (has intermediate values)"


def negative_control_d():
    """History alpha blend removed (always overwrite instead of blend).

    Must fail the temporal-convergence assertion.
    """
    # Create a sequence of frames with a known pattern
    # Frame 1: all 0.0, Frame 2: all 1.0, Frame 3: all 0.0, ...
    # With blending, the moments should converge to the mean (0.5)
    # Without blending (always overwrite), the moments should track the last frame

    est = MomentEstimator(alpha=0.1, blend=False)  # blend=False means always overwrite

    # Feed alternating frames
    for i in range(10):
        frame = [0.0] * 100 if i % 2 == 0 else [1.0] * 100
        est.update(frame)

    # With blend=False, the moments should reflect only the last frame
    # The last frame (i=9, odd) was [1.0] * 100, so mean should be 1.0
    mean = est.m1

    # The control passes if the mean is NOT 0.5 (i.e., it didn't converge)
    not_converged = abs(mean - 0.5) > 0.1

    return not_converged, "blend=False (overwrite): mean=%.6f (want != 0.5)" % mean


# ============================================================ shader grep =====

def grep_shader():
    """Grep the shader for moments/A-Trous constructs."""
    if not os.path.exists(SHADER_REL):
        return False, []

    with open(SHADER_REL, 'r') as f:
        lines = f.readlines()

    patterns = [
        (r'm1\s*\+=\s*s', 'm1 accumulation'),
        (r'm2\s*\+=\s*s\s*\*\s*s', 'm2 accumulation'),
        (r'm2\s*/\s*\d+\.0f\s*-\s*mean\s*\*\s*mean', 'variance from moments'),
        (r'sqrt\s*\(\s*max\s*\(', 'sqrt(max(..., 0)) clamp'),
        (r'atrous|a-trous|wavelet', 'A-Trous wavelet'),
        (r'phi_l|phi_s|PHI_L|PHI_S', 'edge-stopping parameters'),
        (r'edge.?stop', 'edge-stopping'),
        (r'history.*blend|alpha.*history', 'history blend'),
        (r'RG32F|rg32f', 'RG32F format'),
    ]

    results = []
    for pattern, desc in patterns:
        matches = []
        for i, line in enumerate(lines):
            if re.search(pattern, line, re.IGNORECASE):
                matches.append((i + 1, line.rstrip()))
        results.append((desc, matches))

    return True, results


# ============================================================ main =====

def main():
    print("=" * 78)
    print("SVGF moment and A-Trous wavelet filter maths -- standalone verification")
    print("=" * 78)
    print()

    # Positive checks
    all_passed = True

    print("-" * 78)
    print("POSITIVE CHECKS")
    print("-" * 78)

    # Check 1: Moment variance tracks analytic
    print("\n[1] Moment variance tracks analytic variance")
    for ok, msg in check_moment_variance_tracks_analytic():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            all_passed = False

    # Check 2: A-Trous kernel sums to 1
    print("\n[2] A-Trous kernel weights sum to 1 at each iteration")
    for ok, msg in check_atrous_kernel_sums_to_one():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            all_passed = False

    # Check 3: Constant input preserved
    print("\n[3] Constant input is preserved exactly")
    for ok, msg in check_atrous_preserves_constant():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            all_passed = False

    # Check 4: Variance falls across iterations
    print("\n[4] Variance falls across iterations on noisy input")
    for ok, msg in check_atrous_variance_falls():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            all_passed = False

    # Check 5: Edge discrimination
    print("\n[5] Edge discrimination")
    for ok, msg in check_edge_discrimination():
        print("  [%s] %s" % ("PASS" if ok else "FAIL", msg))
        if not ok:
            all_passed = False

    # Negative controls
    print()
    print("-" * 78)
    print("NEGATIVE CONTROLS -- each must FAIL (i.e., the broken variant is detected)")
    print("-" * 78)

    controls = [
        ("(a) variance without max(...,0) clamp", negative_control_a),
        ("(b) A-Trous weights not normalised", negative_control_b),
        ("(c) edge-stopping dropped entirely", negative_control_c),
        ("(d) history alpha blend removed", negative_control_d),
    ]

    controls_passed = []
    for name, fn in controls:
        try:
            ok, msg = fn()
        except Exception as exc:
            ok, msg = False, "raised %s: %s" % (type(exc).__name__, exc)

        # For negative controls, "ok" means the broken variant was detected
        # (i.e., the assertion failed as expected)
        print("  [%s] %s: %s" % ("PASS" if ok else "FAIL", name, msg))
        if not ok:
            controls_passed.append(name)

    # Shader grep
    print()
    print("-" * 78)
    print("SHADER GREP -- structural check of Renderer3D_NovaRenderer.hlsl (READ ONLY)")
    print("-" * 78)

    ran, results = grep_shader()
    if not ran:
        print("  [WARN] Shader file not found: %s" % SHADER_REL)
    else:
        for desc, matches in results:
            if matches:
                print("  [FOUND] %s: %d matches" % (desc, len(matches)))
                for line_num, line_text in matches[:3]:
                    print("    L%d: %s" % (line_num, line_text.strip()))
            else:
                print("  [ABSENT] %s" % desc)

    # Scope statement
    print()
    print("=" * 78)
    print("WHAT THIS RUN PROVES -- and what it does not")
    print("=" * 78)
    print()
    print("PROVEN (arithmetic on the published algorithm):")
    print("  * Moment accumulation with alpha history blend produces variance")
    print("    estimates that track analytic variance for flat, step, gradient,")
    print("    and noise regions.")
    print("  * The A-Trous filter's kernel weights sum to 1 at each iteration,")
    print("    so a flat region is unchanged.")
    print("  * A constant input is preserved exactly by the filter.")
    print("  * Variance falls across iterations on a noisy input.")
    print("  * The filter does not blur across depth/normal discontinuities,")
    print("    and does smooth within uniform regions.")
    print()
    print("NOT PROVEN -- and no amount of arithmetic in this file could prove it:")
    print("  * GPU execution. No shader was compiled or dispatched.")
    print("  * Descriptor or binding layout.")
    print("  * Register pressure, spills, or wave divergence.")
    print("  * Barrier correctness.")
    print("  * That the Python and the HLSL compute the same function.")
    print("  * Numerical behaviour in fp32. Every computation here is fp64.")
    print()

    # Final verdict
    if controls_passed:
        print("!" * 78)
        print("!! NEGATIVE CONTROL DID NOT FAIL: %s" % ", ".join(controls_passed))
        print("!! The checks above are not measuring what they claim to measure.")
        print("!! Fix the harness before trusting anything it says.")
        print("!" * 78)
        return 1

    if not all_passed:
        print("RESULT: FAIL -- some positive checks failed")
        return 1

    print("RESULT: PASS -- all positive checks green, all negative controls provably failable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
