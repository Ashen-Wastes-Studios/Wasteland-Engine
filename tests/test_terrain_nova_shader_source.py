from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHADER_PATH = "DemonCore-Editor/assets/shaders/Renderer3D_NovaRenderer.hlsl"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    index = opening
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
        index += 1
    raise AssertionError(f"unterminated function body for {signature}")


def assert_finite_positive_extent_contract(testcase, source: str) -> None:
    grid = function_body(source, "float HitTerrainGrid(")
    testcase.assertIn("!isfinite(extentX)", grid)
    testcase.assertIn("!isfinite(extentZ)", grid)
    testcase.assertIn("extentX <= 0.0f", grid)
    testcase.assertIn("extentZ <= 0.0f", grid)
    testcase.assertNotRegex(grid, r"extent[ XZ]+<=\s*1e-")


class TerrainNovaShaderSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / SHADER_PATH).read_text(encoding="utf-8")

    def test_grid_accepts_any_finite_positive_horizontal_extent(self):
        assert_finite_positive_extent_contract(self, self.source)

    def test_extent_regression_guard_rejects_the_old_absolute_cutoff(self):
        mutant = self.source.replace("extentX <= 0.0f", "extentX <= 1e-8f", 1)
        self.assertNotEqual(mutant, self.source, "negative-control mutation was not applied")

        with self.assertRaises(AssertionError):
            assert_finite_positive_extent_contract(self, mutant)

    def test_triangle_determinant_and_ray_tolerance_are_scale_aware(self):
        triangle = function_body(self.source, "bool HitTerrainTriangle(")
        triangle_signature = self.source[
            self.source.index("bool HitTerrainTriangle("):self.source.index("{", self.source.index("bool HitTerrainTriangle("))
        ]

        self.assertRegex(triangle_signature, r"float\s+tMin\b")
        self.assertIn("length(edge1) * length(edge2) * length(ray.Direction)", triangle)
        self.assertIn("determinantScale > 0.0f", triangle)
        self.assertIn("t <= tMin", triangle)
        self.assertNotIn("1e-30f", triangle)
        self.assertNotIn("t <= 1e-5f", triangle)

    def test_grid_uses_cell_scaled_tolerance_for_triangle_tests_and_dda_bounds(self):
        grid = function_body(self.source, "float HitTerrainGrid(")

        self.assertIn("float cellScale = min(cellWidthX, cellWidthZ)", grid)
        self.assertIn("float directionLength = length(localRay.Direction)", grid)
        self.assertIn("if (!(directionLength > 0.0f) || !isfinite(directionLength))", grid)
        self.assertRegex(grid, r"min\(1e-5f,\s*cellScale\s*\*\s*1e-4f\s*/\s*directionLength\)")
        self.assertIn("a, c, b, tMin, candidateT, candidateNormal", grid)
        self.assertIn("b, c, d, tMin, candidateT, candidateNormal", grid)
        self.assertIn("candidateT >= cellEnter - tMin", grid)
        self.assertIn("candidateT <= cellExit + tMin", grid)
        self.assertNotIn("cellEnter - 1e-5f", grid)
        self.assertNotIn("cellExit + 1e-5f", grid)

    def test_only_exactly_zero_direction_components_are_treated_as_parallel(self):
        clip = function_body(self.source, "bool ClipTerrainAxis(")
        grid = function_body(self.source, "float HitTerrainGrid(")

        self.assertIn("direction == 0.0f", clip)
        self.assertIn("localRay.Direction.x > 0.0f", grid)
        self.assertIn("localRay.Direction.x < 0.0f", grid)
        self.assertIn("localRay.Direction.z > 0.0f", grid)
        self.assertIn("localRay.Direction.z < 0.0f", grid)
        self.assertNotIn("directionEpsilon", grid)

    def test_terrain_primitive_shape_identifier_is_unchanged(self):
        primitive = function_body(self.source, "float HitPrimitive(")
        self.assertIn("if (shape == 8) return HitTerrainGrid(localRay, inst, outNormal);", primitive)


if __name__ == "__main__":
    unittest.main()
