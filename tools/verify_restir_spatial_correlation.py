#!/usr/bin/env python3
"""Exact finite-model diagnostic for ReSTIR spatial reuse.

This is a CPU/source-contract diagnostic, not a GPU or visual correctness proof.
It separates two issues that are easy to conflate:

* Correlation between proposal streams changes variance, but does not by itself
  bias the weighted estimator when every stream has valid support and the
  conditional reservoir-selection law is respected.
* Reusing reservoirs across receivers with different target support can bias the
  estimate if every input M is included in the denominator. The current shader
  twins use that total-M normalization; ReSTIR's unbiased combination instead
  uses the count of source PDFs that support the selected sample (Alg. 6).

The finite support example is intentionally tiny and exhaustive. Its target is
also its scalar integrand, and all proposals use a uniform two-light PMF. It is
not a model of the renderer's full RGB/shadow-ray pipeline.
"""

from fractions import Fraction as F
from itertools import product
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
HLSL = ROOT / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.hlsl"
GLSL = ROOT / "DemonCore-Editor" / "assets" / "shaders" / "Renderer3D_NovaRenderer.glsl"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def coupled_pair_outcomes(rho):
    """Joint PMF for A~Uniform{0,1}, B=A with prob rho, else independent."""
    return tuple(
        (a, b, rho * (F(1, 2) if a == b else F(0)) + (1 - rho) * F(1, 4))
        for a in (0, 1)
        for b in (0, 1)
    )


def correlated_full_support_stats(rho, candidate_count=4):
    """Exhaustively evaluate the merged estimator for two fully-supported RIS streams.

    With f == target, the final selected-light factor f/target is one. The
    merge's conditional estimator is therefore (S_A + S_B)/(2*M), independent
    of the final categorical draw. Enumerating the joint proposal arrays gives
    exact mean and variance, including cross-stream covariance.
    """
    target = (F(1), F(3))
    pair_outcomes = coupled_pair_outcomes(rho)
    mean = F(0)
    second_moment = F(0)
    probability_sum = F(0)
    for slots in product(pair_outcomes, repeat=candidate_count):
        probability = F(1)
        sum_a = F(0)
        sum_b = F(0)
        for a, b, slot_probability in slots:
            probability *= slot_probability
            sum_a += target[a] / F(1, 2)
            sum_b += target[b] / F(1, 2)
        estimate = (sum_a + sum_b) / (2 * candidate_count)
        probability_sum += probability
        mean += probability * estimate
        second_moment += probability * estimate * estimate
    variance = second_moment - mean * mean
    return probability_sum, mean, variance


def make_single_candidate_record(light, source_target):
    """Return (selected light or None, M, W) for one uniform proposal."""
    m = 1
    proposal_pdf = F(1, 2)
    target = source_target[light]
    if target == 0:
        return None, m, F(0)
    candidate_weight = target / proposal_pdf
    return light, m, candidate_weight / (m * target)


def support_mismatch_means():
    """Exhaustively compare total-M and source-support normalization."""
    center_target = (F(1), F(0))
    neighbor_target = (F(0), F(1))
    source_targets = (center_target, neighbor_target)
    true_integral = sum(center_target)
    naive_mean = F(0)
    support_mean = F(0)
    probability_sum = F(0)

    # Independent uniform proposal from each source; each pair has mass 1/4.
    for center_light, neighbor_light in product((0, 1), repeat=2):
        probability = F(1, 4)
        records = (
            make_single_candidate_record(center_light, center_target),
            make_single_candidate_record(neighbor_light, neighbor_target),
        )
        total_m = sum(record[1] for record in records)
        weighted_candidates = []
        total_weight = F(0)
        for selected, m, weight in records:
            if selected is None:
                continue
            center_p_hat = center_target[selected]
            merge_weight = center_p_hat * weight * m
            if merge_weight > 0:
                weighted_candidates.append((selected, merge_weight))
                total_weight += merge_weight

        conditional_naive = F(0)
        conditional_support = F(0)
        if total_weight > 0:
            for selected, merge_weight in weighted_candidates:
                selection_probability = merge_weight / total_weight
                center_target_value = center_target[selected]
                support_count = sum(
                    record[1]
                    for record, source_target in zip(records, source_targets)
                    if source_target[selected] > 0
                )
                require(support_count > 0, "selected sample has no supporting source")
                integrand = center_target[selected]
                conditional_naive += selection_probability * (
                    integrand * total_weight / (center_target_value * total_m)
                )
                conditional_support += selection_probability * (
                    integrand * total_weight / (center_target_value * support_count)
                )
        probability_sum += probability
        naive_mean += probability * conditional_naive
        support_mean += probability * conditional_support

    return probability_sum, true_integral, naive_mean, support_mean


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def function_body(source, name):
    match = re.search(r"\bvoid\s+" + re.escape(name) + r"\s*\(", source)
    if not match:
        return ""
    start = source.find("{", match.end())
    if start < 0:
        return ""
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    return ""


def current_shader_uses_total_m(body):
    clean = re.sub(r"\s+", "", strip_comments(body))
    return (
        "totalM+=neighborCount" in clean
        and "r.W=totalWeight/(selectedTarget*float(totalM))" in clean
    )


def main():
    truth = F(4)
    expected_variances = {
        F(0): F(1, 2),
        F(1, 2): F(3, 4),
        F(1): F(1),
    }
    for rho, expected_variance in expected_variances.items():
        probability_sum, mean, variance = correlated_full_support_stats(rho)
        require(probability_sum == 1, f"rho={rho}: joint PMF sums to {probability_sum}")
        require(mean == truth, f"rho={rho}: biased mean {mean}, expected {truth}")
        require(
            variance == expected_variance,
            f"rho={rho}: variance {variance}, expected {expected_variance}",
        )
    print("PASS: exact full-support enumeration: correlation preserves mean 4")
    print("      variance rho=0 / 1/2 / 1: 1/2 / 3/4 / 1")

    probability_sum, truth, naive_mean, support_mean = support_mismatch_means()
    require(probability_sum == 1, f"support fixture PMF sums to {probability_sum}")
    require(naive_mean == F(1, 2), f"total-M witness mean {naive_mean}, expected 1/2")
    require(support_mean == truth, f"support-aware witness mean {support_mean}, expected {truth}")
    print("PASS: support-mismatch witness: total-M mean 1/2; support-aware mean 1")

    source_results = []
    for path in (HLSL, GLSL):
        source = path.read_text(encoding="utf-8", errors="replace")
        body = function_body(source, "RunRestirSpatial")
        source_results.append((path.name, bool(body) and current_shader_uses_total_m(body)))
    for filename, matches in source_results:
        require(matches, f"{filename}: pass 14 no longer matches the total-M model; re-audit it")
    print("PASS: HLSL/GLSL pass 14 currently use unconditional neighbor M / total-M normalization")
    print("LIMIT: this identifies a source-contract risk; it does not prove GPU behavior or solve support tracking")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
