// Precompiled header, first as always. Brings in <algorithm>, <functional>,
// <sstream> and <string>, so the explicit includes below are belt-and-braces
// rather than the only source of those names.
#include "wlpch.h"

// The compiler's own declaration. Pulled in for Graph, GraphNode, GraphLink,
// EmitContext, EmitOutputs, GraphError and the two Compile* signatures.
// Compiler.h includes Validator.h, which is where Validator and ParseLiteral
// come from -- neither is included directly here.
#include "Compiler.h"

// std::min / std::max, used in SanitizeIdentifier's callers and in the
// registry lookups below. Already in the PCH; repeated because the file uses
// it in a hot template.
#include <algorithm>
// std::function. Load-bearing rather than decorative: the node walker and the
// flow emitter are BOTH std::function, because each one needs to call itself
// recursively. A plain lambda cannot capture itself, so the std::function
// indirection is a requirement, not a style choice.
#include <functional>
// std::ostringstream / std::istringstream. The compiler builds the generated
// Python with << rather than string concatenation, then re-parses its OWN
// emitted shell text line by line (see ResolveBranchMarkers' caller).
#include <sstream>
// std::unordered_map. The dominant data structure in the file: child counts,
// the emitted set, and the per-node data-emission bookkeeping.
#include <unordered_map>

// The node table. NodeRegistry::Get() is a singleton, so the registry is
// shared by every compile -- it is read-only here, never mutated, which is
// what makes concurrent compiles safe (Validator.h makes the same point).
#include "NodeRegistry.h"

// The C++17 nested namespace form, so no closing brace is needed for
// Wasteland itself -- only the one at the bottom of the file.
namespace Wasteland::VisualBlocks
// File-scope helpers live in the anonymous namespace just below, so they get
// internal linkage: two translation units can each have their own
// SanitizeIdentifier without an ODR clash.
{

	// The anonymous namespace begins. Internal linkage for everything through
	// line 142 -- which is also why Unquote being unused below cannot produce a
	// link error.
	namespace
	{
		// A valid Python identifier, or a fallback. The generated class name
		// comes from the graph name, which is author-supplied text, so it is
		// sanitised rather than trusted.
		// Author-supplied text -> a valid Python identifier.
		// ---
		// Used for the generated CLASS name and for each generated variable
		// attribute. Compiler.h explains why the runtime needs the same rule: it
		// has to reconstruct the name to instantiate the class.
		// ---
		// DEFECT (documented, not fixed): this checks only the character set and the
		// leading digit. It does not reject Python KEYWORDS, so a graph named
		// "class" yields a generated file containing a `class class:` header -- a
		// SyntaxError reported against generated code with no pointer to the name.
		std::string SanitizeIdentifier(const std::string &text, const char *fallback)
		{
			// Built up character by character rather than with std::replace or a
			// regex: one pass, one allocation strategy, and no dependency on <regex>
			// (which wlpch.h deliberately does not pull in).
			std::string out;
			// Range-for over a std::string yields CHAR, not code point. A multi-byte
			// UTF-8 graph name therefore has every byte above 0x7F replaced by an
			// underscore -- 'Enemy Chase' with an accented a becomes Enemy_Ch_se.
			// Lossy but total, which is what a sanitiser needs to be.
			for (char c : text)
			{
				// The allowed set: ASCII letters, digits, underscore. No high bytes and no
				// '$', so the result is always a legal identifier body.
				if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
					// The digit half of the allowed set.
					(c >= '0' && c <= '9') || c == '_')
				{
					// The keep branch. Note digits are accepted HERE but not in first position
					// -- the check below is what makes that safe.
					out += c;
				}
				// Everything else collapses to a single underscore, so 'Enemy Chase (v2)'
				// becomes 'Enemy_Chase__v2_' exactly as Compiler.h documents. Two
				// different bad characters in a row produce two underscores, which is why
				// the sanitised form is not reversible.
				else
				{
					// The substitution. Fixed width, so the output length always equals the
					// input length before the fallback prefix is added.
					out += '_';
				}
			}
			// Guards the two cases Python rejects as an identifier: empty, and leading
			// digit. out.empty() short-circuits the ||, so out[0] is only read when the
			// string is non-empty -- the order of the two operands is load-bearing.
			// ---
			// The fallback is PREPENDED rather than replacing the bad first character,
			// so '1st' becomes 'var1st' and stays roughly unique.
			if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
				// Prepends the caller's fallback. The `fallback` parameter is what makes
				// the result predictable rather than dependent on the input: ClassNameFor
				// passes "GeneratedBehavior", the flow variable loop passes "var".
				out = std::string(fallback) + out;
			// Returned by value, so the caller cannot mutate the internal buffer. At
			// this point the return is a move, not a copy.
			return out;
		}

		// The Python attribute a node's output is assigned to, and read from.
		//
		// ONE leading underscore, not two. Python mangles any attribute whose
		// name begins with two underscores inside a class body: `self.__out_x`
		// written in a method becomes `self._Generated__out_x` at runtime, so
		// the assignment and the read agree only if BOTH are mangled -- which
		// they are not, because the read side is built by string
		// concatenation here and the mangling happens in the parser. The
		// symptom is an AttributeError naming a mangled name that appears
		// nowhere in the generated source.
		//
		// One underscore is also unambiguous against author-declared
		// variables: the compiler refuses to emit a GraphVariable starting
		// with an underscore, so `_vb_` cannot collide with one.
		// The attribute name a node's output is stored under, read back by every
		// successor through TempAttr.
		// ---
		// The single leading underscore is the load-bearing part of this function;
		// the header comment above it explains the name-mangling trap in full.
		std::string TempAttr(const std::string &nodeId, const std::string &pinId)
		{
			// nodeId and pinId are concatenated RAW -- no sanitiser runs here.
			// ---
			// DEFECT (documented, not fixed): node and pin ids are author-supplied, and
			// anything outside [A-Za-z0-9_] lands in a Python attribute name verbatim.
			// A pin id containing a space or a dot therefore produces generated source
			// that does not parse, reported as a SyntaxError in a file the author never
			// wrote. This is the one name-building path with no SanitizeIdentifier in
			// front of it.
			return "self._vb_" + nodeId + "_" + pinId;
		}

		// A control-flow node is one that OWNS bodies: more than one exec OUT
		// pin, each of which carries statements rather than a value.
		//
		// Distinguishing it structurally rather than with a flag is deliberate.
		// A hand-maintained "isControlFlow" list in the compiler is a second
		// registry that has to be updated whenever a node is added, and the
		// failure mode is silent -- a new Branch-shaped node emits an empty
		// body and the graph runs to no effect. Asking the node's own PINS
		// means the answer cannot drift from the definition.
		// The structural test for 'this node owns bodies'.
		// ---
		// Reads the node's own PINS rather than consulting a list of control-flow
		// types, which is what stops the compiler drifting from the registry when a
		// node is added. The comment above this function spells out the reasoning.
		bool HasBranchPins(const NodeDef &def)
		{
			// Counts exec OUT pins. The threshold is 2, so a single-exit node is
			// classified as NOT control flow and chains onward.
			int execOuts = 0;
			// Iterates the def's pins, which is data -- no registry mutation, so the
			// const NodeDef& here is safe even though the registry is a singleton.
			for (const PinDef &pin : def.Pins)
			{
				// Direction AND type must both match: an Exec IN pin is a flow edge, not an
				// exit, and counting those would make every ordinary node look like a
				// branch.
				if (pin.Dir == PinDir::Out && pin.Type == PinType::Exec)
					// Pre-increment inside the test, kept terse because execOuts is read exactly
					// once, on the return line below.
					++execOuts;
			}
			// Two or more exits: a single-exit node just chains onward.
			// Two or more. A node with exactly one exec out has nothing to branch on,
			// so classifying it as control flow would only cost an empty marker pass.
			return execOuts >= 2;
		}

		// The token a control-flow node's emitter writes where a body goes.
		//
		// Uses control characters that cannot appear in authored source, so
		// substitution can never corrupt real code and a marker left
		// unsubstituted is a loud compile error rather than a silent no-op.
		// Builds the placeholder a node's emitter writes where a body belongs.
		// ---
		// The sentinels are 0x01 control bytes, which cannot occur in authored
		// source. That is what makes the substitution below safe against real code
		// and a leftover marker loud rather than silent.
		const std::string BranchMarker(const std::string &pinId)
		{
			// Marker text is 0x01, the literal __VB_BODY_, the pin id, two underscores,
			// then 0x01 again. The pin id is interpolated raw -- the same unescaped
			// author-text exposure TempAttr has, though here the failure is at least a
			// loud SyntaxError.
			return "\x01__VB_BODY_" + pinId + "__\x01";
		}

		// Replace every branch marker in a control-flow node's emitted shell
		// with the statements chained off that exec pin.
		//
		// The node emits a SHELL ("if <cond>:", "while <cond>:") with markers
		// where its bodies belong, and the compiler -- which alone knows how to
		// walk the graph -- fills them in. Node authors therefore never need a
		// compiler reference, which is what keeps the registry a table of
		// self-contained functions.
		template <typename BodyFn>
		// The bodyIndent parameter is part of the SIGNATURE but unused by the body
		// below -- it is passed through to emitBody, which is what actually uses it.
		// ---
		// Templated on BodyFn rather than taking std::function so the caller's
		// lambda is inlined with no type erasure on this per-node path.
		std::string ResolveBranchMarkers(const std::string &shell, const GraphNode *node,
										 int bodyIndent, BodyFn emitBody)
		{
			// Re-resolves the node's def from the registry rather than taking it as an
			// argument, so the caller cannot pass a def that disagrees with node->TypeId.
			const NodeDef *def = NodeRegistry::Get().Find(node->TypeId);
			// Unknown type. Returning the shell UNCHANGED leaves the body markers
			// unsubstituted, which the comment above calls out as the loud-failure
			// choice rather than a silent no-op.
			if (def == nullptr)
				// Pass-through on the unknown-type path. The markers survive into the
				// generated file, where Python sees a stray control byte and reports a
				// SyntaxError, rather than accepting valid code that quietly does nothing.
				return shell;

			// A COPY of the shell, so the caller's string is untouched. The loop below
			// mutates `out` in place with replace().
			// ---
			// Size is the shell's size, not a reservation for the expansion: the
			// substituted bodies can be arbitrarily larger and the string reallocates.
			std::string out = shell;
			// Walks the PINS rather than searching the shell for markers, so a marker
			// whose pin no longer exists is simply never looked for.
			for (const PinDef &pin : def->Pins)
			{
				// Same direction+type filter as HasBranchPins. Non-exec out pins are data
				// wires and have no body to fill.
				if (pin.Dir != PinDir::Out || pin.Type != PinType::Exec)
					continue;

				// The marker for this specific pin. One per exec out pin, keyed by pin id.
				const std::string marker = BranchMarker(pin.Id);
				// First occurrence only; the loop at the bottom then drains any repeats, so
				// a node that emits the same marker twice gets both bodies.
				size_t at = out.find(marker);
				// Pin not referenced by this node's shell: skip it. Normal -- a Branch may
				// declare an exec out it chooses not to route to.
				if (at == std::string::npos)
					continue;

				// A body already carries its own absolute indentation, so it
				// is substituted verbatim rather than re-indented here.
				// The callback decides the indentation; this function only substitutes. The
				// argument's second parameter is the bodyIndent passed in above.
				// ---
				// The body is spliced VERBATIM, at absolute indentation, because the writer
				// below adds the base indent once to the shell's lines and not to these.
				std::string body = emitBody(pin.Id, 1);
				// An empty branch body would leave the generated `if:` with nothing after
				// it -- a Python IndentationError.
				if (body.empty())
					// `pass` substituted instead. This is why an unreachable Branch in the
					// editor still generates valid Python rather than a file that will not parse.
					body = "pass\n";

				// Drains every occurrence, advancing past the inserted text so a body
				// containing the marker cannot loop forever.
				while (at != std::string::npos)
				{
					// replace() at `at` for marker.size() bytes. Using the MARKER's length and
					// not a fixed constant is what keeps the splice correct when pin ids differ
					// in length.
					out.replace(at, marker.size(), body);
					// Resumes the search AFTER the body just written. Starting at `at` would
					// re-find a marker the body itself contains.
					at = out.find(marker, at + body.size());
				}
			}
			// The finished shell. Every marker the node emitted has either been
			// replaced by a body or left in place on the unknown-def path above.
			return out;
		}


		// Strips a surrounding pair of DOUBLE quotes from authored literal text.
		// ---
		// DEFECT (documented, not fixed): Unquote is DEAD CODE. This is the only
		// occurrence of the name in the file -- nothing calls it -- and it sits in
		// an anonymous namespace, so the linker cannot even warn about it. Literals
		// therefore reach the emitters with any author quotes still attached.
		std::string Unquote(const std::string &text)
		{
			// Requires BOTH ends to be a double quote and at least two characters, so
			// a bare quote character is returned untouched rather than sliced to nothing.
			if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
				// substr(1, size-2) drops exactly the two quote characters.
				// ---
				// Single-quoted literals are NOT handled -- text wrapped in apostrophes
				// comes back with the apostrophes intact.
				return text.substr(1, text.size() - 2);
			// Everything else is returned as-is, which is the case that makes this a
			// no-op rather than a parser.
			return text;
		}



	}

	// See Compiler.h for why the runtime needs this rather than re-deriving
	// the sanitised name itself.
	// The single public definition of the generated class name.
	// ---
	// Delegates to SanitizeIdentifier rather than repeating the rule, because
	// Compiler.h documents that the runtime re-derives this name and any second
	// implementation would silently drift out of step with this one.
	std::string Compiler::ClassNameFor(const Graph &graph)
	{
		// "GeneratedBehavior" is the fallback for an empty or digit-leading graph
		// name. Note the name is NOT otherwise namespaced: two graphs both called
		// "Enemy Chase" produce the SAME class name in two different files.
		return SanitizeIdentifier(graph.Name, "GeneratedBehavior");
	}


	// Escapes author text for interpolation into a generated Python string
	// literal.
	// ---
	// Called at all four sites that splice an id, a pin id or a key name into
	// generated source: child id lists, the topology table, and both
	// blackboard dicts.
	std::string EscapePy(const std::string &text)
	{
		// Accumulates into a local and returns by value; the return value
		// optimisation makes that a move rather than a copy.
		// ---
		// This is the file's injection barrier. The comment at CompileBehavior says
		// validation exists so this function is never handed a value it cannot make
		// safe.
		std::string out;
		// One char at a time. A whole-string pass would need the same case analysis
		// anyway, and this keeps the escape set in one readable place.
		for (char c : text)
		{
			// Escapes the two characters that can break out of or continue a Python
			// string literal: the double quote and the backslash.
			if (c == '"' || c == '\\')
				// Closes the escape branch; control falls to the newline test below.
				out += '\\';
			// A real newline becomes the two-character backslash-n escape, so it cannot
			// terminate the literal.
			// ---
			// DEFECT (minor, documented, not fixed): only the newline is handled. A TAB,
			// a carriage return or any other control character in an authored node name
			// is copied through verbatim into the generated source.
			if (c == '\n')
				// Note this branch is NOT the else of the line above -- it is a second,
				// independent if. A newline is handled once here, while a quote or a
				// backslash is escaped on the previous line AND then falls through to this
				// else. Reading the two as an if/else pair is the natural mistake.
				out += "\\n";
			else
				// Everything else is copied through. Combined with the line above this
				// yields a newline as backslash-n, a quote as backslash-quote, and a
				// backslash as two backslashes -- all correct, but only because the
				// double-append is intentional.
				out += c;
		}
		// The escaped text, ready to sit between the quotes the emitters write
		// around it. Those emitters choose the quote character, which is why only
		// the double-quoted case has to be handled here.
		return out;
	}

		// The runtime shim every generated file carries.
		//
		// It is emitted rather than imported so a generated tree has no import
		// beyond wl_behavior itself, and so the exact helper the tree relies on
		// is visible in the file when something misbehaves.
		// NOTE: Wasteland.Vec3 exposes x/y/z, Length() and LengthSquared() and
		// NOTHING else -- no __sub__, no __truediv__, no __mul__. Arithmetic on
		// it raises TypeError on the first tick, so the maths here is
		// component-wise. Verified against the pybind class, not assumed.
		const char *kRuntimeShim = R"WL_SHIM(
# ---------------------------------------------------------------- shim
#
# Every name below was verified against the BUILT module (dir(Wasteland.Entity),
# dir(Wasteland.HealthComponent)) rather than inferred from C++ or from how the
# type looks. Three of the first drafts were fiction:
#
#   MoveCapsule(delta)   -> does not exist. Movement is Get/SetTransform.
#   GetComponent(T)      -> does not exist. It is GetHealthComponent, etc.,
#                          per component type.
#   health.IsDead()      -> IsDead is a PROPERTY; calling it raises
#                          "TypeError: bool object is not callable".
#
# A shim that names a non-existent method still compiles and still walks, and
# raises only when an NPC enters that branch -- the silent-inert-NPC failure
# this whole layer exists to prevent.

# NB.7 added sight-cone maths, so the shim imports math itself. It is exec'd
# into a generated module's namespace: relying on the caller to have imported
# math is a NameError on the first tick of whichever tree happens to run first,
# and the shim has no other way to guarantee it.
import math


def _position(agent):
    """Read a position as a plain (x, y, z) tuple.

    GetTransform().Translation is a Vec3, and Vec3 has no arithmetic, so
    every distance and heading below unpacks to floats first.
    """
    t = agent.GetTransform()
    p = t.Translation
    return (p.x, p.y, p.z)


def _scene(ctx):
    """Resolve the active scene, or None when there is no world.

    Three sources, in order, because each covers a case the others miss:

      1. `ctx.tree.scene` -- an explicitly injected scene. Wins because it is
         the only one that can be a DIFFERENT scene from the ambient one
         (a test ticking two worlds, or a tree owned by a sub-scene).
      2. `Wasteland.Scene.GetActiveScene()` -- the engine's own accessor,
         set by Scene before it ticks scripts. This is what the hand-written
         scripts use, and it is the only source that works in a headless
         test or a dedicated server build.
      3. `Application.Get().GetScene()` -- the editor's live scene. Kept last
         because it is the one that returns None outside a running
         Application: a tree ticked from a test got no world at all and every
         NPC in it silently stood still, which reads as broken AI rather than
         as a missing application.

    Returns None rather than raising. A tree with no world is a silent no-op
    by design -- an early startup tick must not take the NPC down -- and every
    caller treats None as "perception unavailable", never as an error.
    """
    scene = getattr(ctx.tree, "scene", None)
    if scene is not None:
        return scene
    try:
        scene = Wasteland.Scene.GetActiveScene()
    except Exception:
        scene = None
    if scene is not None:
        return scene
    app = Wasteland.Application.Get()
    return app.GetScene() if app is not None else None


def _set_position(agent, x, y, z):
    """Write a position, PRESERVING rotation.

    Entity.SetTransform takes (translation, rotation[, scale]) as separate
    Vec3s -- not a Transform component -- so rotation must be read and passed
    back explicitly. Writing only the position zeroes rotation, and an NPC
    that walks to its target while facing the world axis looks broken in a way
    that reads like a facing bug rather than a movement bug.
    """
    t = agent.GetTransform()
    agent.SetTransform(Wasteland.Vec3(x, y, z), t.Rotation, t.Scale)


def _dist(agent, other):
    """Distance between two entities, or +inf when there is no target.

    +inf rather than 0.0: a missing target must make a `DistanceTo <= range`
    check FAIL so the tree picks another branch. Returning 0 would make an
    NPC with no target believe it is standing on top of it.
    """
    if agent is None or other is None:
        return float('inf')
    ax, ay, az = _position(agent)
    bx, by, bz = _position(other)
    dx = ax - bx
    dy = ay - by
    dz = az - bz
    return (dx * dx + dy * dy + dz * dz) ** 0.5


def _target(ctx, key):
    return ctx.bb.get(key)


# Ground-follow tuning, matching the values Enemy.py used. Module-level so a
# designer can override them once rather than per node, and so the numbers
# live in exactly one place.
_MOVE_FOOT_OFFSET = 1.0
_MOVE_PROBE_DISTANCE = 0.4
_MOVE_SNAP_TOLERANCE = 0.2

# Blackboard key each generated class uses to hold its own path. Per agent,
# per tree -- two NPCs must not share one route.
_PATH_KEY = "_wl_path"

# How far an agent may stray from the path segment it is following, squared, in
# metres. ~1.2 m: enough that ground-snapping and the step clamp never trip it,
# small enough that being shoved through a wall re-plans rather than grinding
# against geometry along a route that no longer makes sense.
_PATH_DRIFT_SQUARED = 1.5

# How long a shared perception snapshot stays valid, in seconds. 0.2 s at 60 Hz
# is 12 ticks: imperceptible on a human reaction time, and it cuts the O(R*N)
# scan to O(R) once per interval. Chosen against the cooldown lesson already
# learned here -- throttling a CHEAP scan with a LATENT node starves its
# siblings, but throttling it with a TIMESTAMP starves nothing, because the
# node still returns on the tick it runs.
_PERCEPTION_INTERVAL = 0.2


def _ground_height(ctx, agent, x, y, z, foot_offset, probe_distance, snap_tolerance):
    """World-space ground height under a point, or None if no surface.

    Probes from a little ABOVE the requested foot level, so a body already
    resting exactly on a floor still produces an intersect -- probing from the
    exact foot position can start inside the floor and miss.

    This exists because straight-line movement does not survive slopes and
    steps: an NPC walking toward a target on a ramp interpolates its own Y
    toward the target's Y, so it hovers above the ramp on the way up and sinks
    through it on the way down. Enemy.py solved this with an explicit probe;
    the tree has to, or migrating an NPC onto a tree is a regression.

    Returns None rather than a guess when nothing is hit, so the caller can
    keep its previous Y instead of snapping to y=0 over empty space.
    """
    scene = _scene(ctx)
    if scene is None:
        return None
    origin = Wasteland.Vec3(x, y - foot_offset + snap_tolerance, z)
    try:
        hit, ent, point, normal, dist, hittable = scene.Raycast(
            origin, Wasteland.Vec3(0.0, -1.0, 0.0), probe_distance)
    except Exception:
        # A scene without a physics world has nothing to hit. Degrade to
        # "no ground" rather than raising out of a tick.
        return None
    if not hit:
        return None
    return point.y


def _agent_key(agent):
    """A stable per-agent key for path state.

    NOT `id(agent)`: every query hands back a fresh pybind wrapper, so the same
    entity has a different id on every tick and the cached path would be
    discarded and re-planned 60 times a second -- which is exactly the cost
    this feature exists to remove.

    Uses the UUID when there is one, and falls back to the position otherwise.
    A UUID is stable for the entity's lifetime; a position is not, but it is
    still better than a fresh wrapper id.

    int(), not the UUID object itself: the binding exposes __int__ but no
    __str__, so stringifying the object would emit an address -- different every
    run and every process, which is precisely the instability being avoided.
    """
    try:
        return str(int(agent.GetUUID()))
    except Exception:
        return "%0.2f:%0.2f:%0.2f" % _position(agent)


def _navmesh(ctx):
    """The scene's baked navmesh, or None when there is not one.

    Reached through the scene rather than cached on the tree: baking is a
    level-setup step, not a per-tree one, and every tree sharing the scene's
    mesh is what makes pathfinding affordable with more than one NPC.
    """
    scene = _scene(ctx)
    if scene is None:
        return None
    try:
        return scene.GetNavMesh()
    except AttributeError:
        # A build without the Navigation module bound. Degrade to direct
        # movement rather than taking every tick down.
        return None


def _path_to(ctx, agent, target):
    """A path from the agent to the target, or None to walk straight.

    Re-planned only when the agent has no path left, has drifted off it, or the
    target has moved far enough to matter. Re-planning EVERY tick would be the
    obvious implementation and the wrong one: an A* over a few thousand cells
    per NPC per frame is the cost this feature exists to avoid.

    Returns the whole path including the current position, so the follower
    can index into it without special-casing the first step.
    """
    mesh = _navmesh(ctx)
    if mesh is None or target is None:
        return None
    ax, ay, az = _position(agent)
    bx, by, bz = _position(target)
    # ctx carries no writable attributes (__slots__), and the blackboard
    # is shared, so per-agent path state goes in the tree's node_states --
    # keyed on the AGENT, not just the node, or two NPCs following the same
    # tree would fight over one route.
    state = ctx.tree.node_states.setdefault("path:%s:%s" % (_PATH_KEY, _agent_key(agent)), {})
    path = state.get("path")
    index = state.get("index", 0)
    goal = state.get("goal")

    # Re-plan if there is no path, we have run off the end of it, or the target
    # has moved far enough to matter.
    stale = (
        not path
        or index >= len(path)
        or goal is None
        or (bx - goal[0]) ** 2 + (by - goal[1]) ** 2 + (bz - goal[2]) ** 2 > 4.0
    )
    if not stale and path:
        # "Drifted off the path" means the agent moved somewhere the route did
        # not take it -- e.g. physics shoved it, or another system teleported
        # it. The test is against the SEGMENT it is traversing (the previous
        # waypoint -> the current one), not against the waypoint itself:
        # an agent heading for a waypoint is *supposed* to be far from it, and
        # testing against the waypoint makes every agent look lost, so the route
        # gets re-planned every single tick and the cache never does anything.
        px, py, pz = path[index - 1] if index > 0 else (ax, ay, az)
        wx, wy, wz = path[index]
        # Perpendicular distance from the agent to the current segment.
        sx, sz = wx - px, wz - pz
        seg_len2 = sx * sx + sz * sz
        if seg_len2 <= 1e-9:
            drift = (ax - wx) ** 2 + (az - wz) ** 2
        else:
            tproj = ((ax - px) * sx + (az - pz) * sz) / seg_len2
            tproj = 0.0 if tproj < 0.0 else (1.0 if tproj > 1.0 else tproj)
            drift = (ax - (px + sx * tproj)) ** 2 + (az - (pz + sz * tproj)) ** 2
        if drift > _PATH_DRIFT_SQUARED:
            stale = True

    if stale:
        path = mesh.FindPath(Wasteland.Vec3(ax, ay, az), Wasteland.Vec3(bx, by, bz))
        if not path:
            # No route. Forget it so the next tick retries rather than being
            # stuck forever on a path that does not exist.
            state["path"] = None
            state["index"] = 0
            return None
        state["path"] = [(p.Position.x, p.Position.y, p.Position.z) for p in path]
        state["index"] = 1
        state["goal"] = (bx, by, bz)
        index = 1

    return state["path"], index


def _move(agent, ctx, key, speed, use_pathfinding=False):
    """Latent MoveTo: RUNS while travelling, SUCCEEDS on arrival.

    Returning SUCCESS immediately is the bug this comment exists to prevent:
    the parent `approach` then succeeds on tick one, the selector never tries
    its attack sibling, and the NPC stands in melee range doing nothing while
    the trace shows a clean success walk.

    `use_pathfinding` routes around geometry via the scene's navmesh. It is OFF
    by default, which keeps every existing tree behaving exactly as before and
    costs nothing. When it is on but no route exists -- no mesh baked, the
    target is unreachable, the module is not bound -- this falls back to direct
    movement. A missing navmesh must never stop an NPC dead.
    """
    target = _target(ctx, key)
    if target is None:
        return FAILURE
    ax, ay, az = _position(agent)
    bx, by, bz = _position(target)

    # --- pick the point to head for ---
    wx, wy, wz = bx, by, bz
    route = None
    if use_pathfinding:
        found = _path_to(ctx, agent, target)
        if found is not None:
            route, index = found
            # Walk to the furthest waypoint still within reach this tick, so a
            # long straight leg is not chopped into cell-sized steps.
            wx, wy, wz = route[index]
            while index + 1 < len(route):
                nxp, nyp, nzp = route[index + 1]
                dstep = ((nxp - ax) ** 2 + (nzp - az) ** 2) ** 0.5
                if dstep <= max(speed * ctx.seconds, 0.05):
                    index += 1
                    wx, wy, wz = nxp, nyp, nzp
                else:
                    break
            state = ctx.tree.node_states.get("path:%s:%s" % (_PATH_KEY, _agent_key(agent)))
            if state is not None:
                state["index"] = index

    dx = wx - ax
    dy = wy - ay
    dz = wz - az
    distance = (dx * dx + dy * dy + dz * dz) ** 0.5

    # Arrival is measured against the TARGET, not the current waypoint.
    # Otherwise the last waypoint reports SUCCESS the moment it is touched and
    # the agent stops short of the thing it was sent to reach.
    to_goal = ((bx - ax) ** 2 + (by - ay) ** 2 + (bz - az) ** 2) ** 0.5
    if to_goal <= 0.05:
        return SUCCESS

    if distance <= 1e-6:
        # Standing exactly on the current waypoint is the NORMAL case, not a
        # fault: the agent arrived and must now head for the next one.
        # Discarding the route here (the first version did) throws away every
        # remaining waypoint, re-plans from scratch, walks back to the same
        # corner, and freezes there forever -- which is what an NPC standing
        # motionless next to a wall looks like in game.
        if route is not None and index + 1 < len(route):
            while index + 1 < len(route):
                nxp, nyp, nzp = route[index + 1]
                dx, dy, dz = nxp - ax, nyp - ay, nzp - az
                distance = (dx * dx + dy * dy + dz * dz) ** 0.5
                index += 1
                if distance > 1e-6:
                    break
            wx, wy, wz = ax + dx, ay + dy, az + dz
            state = ctx.tree.node_states.get("path:%s:%s" % (_PATH_KEY, _agent_key(agent)))
            if state is not None:
                state["index"] = index
        else:
            # Genuinely the last waypoint, and the goal is still far away --
            # the route cannot help. Drop it and walk straight this tick.
            if route is not None:
                state = ctx.tree.node_states.get("path:%s:%s" % (_PATH_KEY, _agent_key(agent)))
                if state is not None:
                    state["path"] = None
            wx, wy, wz = bx, by, bz
            dx = wx - ax
            dy = wy - ay
            dz = wz - az
            distance = (dx * dx + dy * dy + dz * dz) ** 0.5
            if distance <= 1e-6:
                return SUCCESS

    # Clamp the step to the remaining distance: a long frame must not
    # overshoot past the waypoint, or the NPC oscillates around it forever and
    # never satisfies the arrival test.
    step = min(distance, speed * ctx.seconds)
    scale = step / distance
    nx = ax + dx * scale
    ny = ay + dy * scale
    nz = az + dz * scale
    # Follow the ground under the NEW position rather than interpolating
    # toward the target's Y. Interpolating Y toward a target on a ramp makes
    # the NPC hover above the ramp going up and sink through it coming down,
    # because straight-line Y is not ground height. Enemy.py solved this with
    # an explicit probe; the tree has to as well, or moving an NPC from a
    # script onto a tree is a silent regression on any level with slopes.
    #
    # The path's own Y is used when it has one -- the bake already knows the
    # ground height there, and re-probing it every tick is a raycast per frame.
    ground = _ground_height(ctx, agent, nx, ay, nz, _MOVE_FOOT_OFFSET,
                            _MOVE_PROBE_DISTANCE, _MOVE_SNAP_TOLERANCE)
    if ground is not None:
        ny = ground + _MOVE_FOOT_OFFSET
    elif route is not None and wy != by:
        ny = wy
    _set_position(agent, nx, ny, nz)
    return RUNNING



def _move_to_point(agent, ctx, position, speed, arrival):
    """Latent walk to an explicit POSITION -- flee, return-to-home.

    Exists so MoveTo does not have to pretend a point is an entity, which is
    what lets the same node serve both "chase that target" and "go back to
    the patrol post".
    """
    if agent is None:
        return FAILURE
    ax, ay, az = _position(agent)
    dx = position.x - ax
    dy = position.y - ay
    dz = position.z - az
    distance = (dx * dx + dy * dy + dz * dz) ** 0.5
    if distance <= arrival:
        return SUCCESS
    step = min(distance, speed * ctx.seconds)
    scale = step / distance
    nx = ax + dx * scale
    ny = ay + dy * scale
    nz = az + dz * scale
    # Follow the ground under the NEW position rather than interpolating
    # toward the target's Y. Interpolating Y toward a target on a ramp makes
    # the NPC hover above the ramp going up and sink through it coming down,
    # because straight-line Y is not ground height. Enemy.py solved this with
    # an explicit probe; the tree has to as well, or moving an NPC from a
    # script onto a tree is a silent regression on any level with slopes.
    ground = _ground_height(ctx, agent, nx, ay, nz, _MOVE_FOOT_OFFSET,
                            _MOVE_PROBE_DISTANCE, _MOVE_SNAP_TOLERANCE)
    if ground is not None:
        ny = ground + _MOVE_FOOT_OFFSET
    _set_position(agent, nx, ny, nz)
    return RUNNING


def _is_dead(agent):
    """True when the agent's OWN health says it is dead.

    An entity with no HealthComponent is NOT dead -- it is scenery, and
    calling it dead would let the top branch succeed for an NPC that was
    never given health, so nothing below it ever runs.
    """
    if agent is None or not agent.HasHealthComponent():
        return False
    return bool(agent.GetHealthComponent().IsDead)


def _health_fraction(agent):
    """Current health as a FRACTION of max, so one threshold reads correctly
    at any max health.

    Returns 0.0 with no health component, which makes IsLowHealth false: an
    entity with no health bar is not "nearly dead".
    """
    if agent is None or not agent.HasHealthComponent():
        return 0.0
    h = agent.GetHealthComponent()
    if h.Max <= 0.0:
        return 0.0
    return h.Current / h.Max


def _damage(agent, target, damage):
    """Damage a target through its HealthComponent.

    Goes through ApplyDamage so whatever bookkeeping the component owns
    (DamageCooldown, Invulnerable, death) is not bypassed. A read-modify-write
    on Current would silently skip all of it -- the silent-write trap the
    binding audit exists to catch.
    """
    if target is None or not target.HasHealthComponent():
        return FAILURE
    target.GetHealthComponent().ApplyDamage(damage)
    return SUCCESS


def _attack(agent, ctx, key, damage):
    """Legacy id, kept because saved trees reference nodes by string.

    Delegates to _damage so there is exactly ONE implementation of "hit
    something", and it is the one that goes through ApplyDamage.
    """
    return _damage(agent, _target(ctx, key), damage)


def _perception_snapshot(ctx, scene):
    """One shared list of live perceivable entities, rebuilt on an interval.

    NB.7. The first version of `SenseNearest` scanned every entity in the scene,
    per NPC, per tick. With R entities and N NPCs that is O(R*N) every frame --
    fine at 12 entities, and the first thing to fall over when a level has a few
    hundred. This is the fix: the SCAN is shared and throttled, so the cost is
    O(R) once per interval regardless of how many NPCs are asking.

    The snapshot is a list of (x, y, z, entity) tuples, computed once and read
    by everyone. It is deliberately NOT cached forever: entities die, spawn and
    move, and a stale roster makes NPCs chase corpses. The interval bounds both
    the cost and the staleness, and the staleness is stated here rather than
    discovered later as "the NPC attacked a body for ten seconds".

    Held on the SCENE, not the tree, because sharing is the entire point -- a
    per-tree cache would be one full scan per NPC, i.e. the thing being fixed.
    """
    if scene is None:
        return []
    cache = getattr(scene, "_wl_perception", None)
    now = ctx.now
    if cache is not None and (now - cache[0]) < _PERCEPTION_INTERVAL:
        return cache[1]
    live = []
    for other in scene.GetAllEntitiesWithTransform():
        if not other.HasHealthComponent():
            continue
        if other.GetHealthComponent().IsDead:
            continue
        ox, oy, oz = _position(other)
        live.append((ox, oy, oz, other))
    snapshot = live
    try:
        scene._wl_perception = (now, snapshot)
    except Exception:
        # Some scene objects refuse attribute assignment. Perception then
        # rebuilds every tick, which is the old behaviour and still correct --
        # slower, never wrong.
        pass
    return snapshot


def _los_blocked(ctx, ax, ay, az, bx, by, bz, ignore=None):
    """True when geometry sits between two points.

    Scene.Raycast returns SIX values and the two that identify WHAT was hit are
    easy to get backwards:

        (hit, entity, point, normal, distance, hittable)
                    ^^^^^^          ^^^^^^   ^^^^^^
                    index 1         index 3  index 5

    Two separate mistakes lived here, and they had to be fixed together:

    1. Reading `hit[3]` for the entity. Index 3 is the NORMAL -- a Vec3, never
       None. So `hit_entity is None` never fired, world geometry never counted
       as blocking, and `hit_entity.GetUUID()` raised into the bare except
       below so the ignore-the-target check never ran either.

    2. Testing `entity is None` for world geometry AT ALL. The binding returns a
       default-constructed Entity there, not None, so that test can never pass.
       Calling GetUUID() on that invalid handle crashes the interpreter outright.
       `hittable` (index 5) is the documented discriminator: False means the ray
       struck world geometry with no entity behind it.

    Net effect of both: HasLineOfSight reported a target directly in front of
    the NPC as BLOCKED whenever that target had a collider, and only appeared to
    work for collider-less targets, where the ray hit nothing at all.

    A test mock hid this for a long time -- it was shaped
    (True, Vec3, Vec3, None, ...), which is a flow-graph order the engine never
    produces, so the fixture agreed with the bug rather than with the contract.
    """
    scene = _scene(ctx)
    if scene is None:
        return False
    dx = bx - ax
    dy = by - ay
    dz = bz - az
    length = (dx * dx + dy * dy + dz * dz) ** 0.5
    if length <= 1e-6:
        return False
    try:
        hit = scene.Raycast(
            Wasteland.Vec3(ax, ay, az),
            Wasteland.Vec3(dx, dy, dz),
            length,
        )
    except Exception:
        return False
    if not hit or len(hit) < 6 or not hit[0]:
        return False
    hit_entity = hit[1]
    hittable = hit[5]
    if not hittable:
        # World geometry: no entity behind it. That blocks.
        return True
    if ignore is not None:
        # The endpoint entity itself is the target, not an obstacle. Only worth
        # comparing when the ray actually struck an entity.
        try:
            if hit_entity.GetUUID() == ignore.GetUUID():
                return False
        except Exception:
            pass
    return True


def _sense_nearest(agent, ctx, key, radius, half_angle_deg=None, require_los=False):
    """Write the nearest LIVE entity within radius into the blackboard key.

    Skips the agent itself -- an NPC that senses itself walks to where it
    already stands and never advances -- and skips anything without a
    HealthComponent or already dead.

    SUCCESS is returned even when nothing is found, because the key is
    CLEARED. Failing instead would leave a stale target and the next branch
    would attack it: an NPC shooting a corpse because the key was never
    invalidated.

    NB.7: the scan comes from `_perception_snapshot`, shared across every NPC
    and rebuilt on an interval. That changes the cost from O(R*N) per tick to
    O(R) per interval, which is the difference between fine at 12 entities and
    unusable at 200.

    `half_angle_deg` restricts sensing to a cone around the agent's facing
    (None = omnidirectional, the previous behaviour). `require_los` adds a real
    raycast so a target behind a wall is not acquired.

    Both are optional and both default to the old behaviour, so every existing
    authored tree keeps sensing exactly as it did. The cone needs a facing;
    without one it degrades to omnidirectional rather than going blind.
    """
    if agent is None:
        return FAILURE
    scene = _scene(ctx)
    if scene is None:
        # No world reachable: clear rather than raise. Perception with no
        # scene is a silent no-op by design, so a headless test or an early
        # startup tick does not take the tree down.
        # CLEAR, not set-to-None. Blackboard tracks validity separately from
        # value: set(key, None) marks the key WRITTEN, so a
        # BlackboardIsSet(key) gate reading true forever and the branch that
        # re-acquires a target could never run again. clear() drops it from
        # both dicts, which is what "no target" has to mean.
        ctx.bb.clear(key)
        return SUCCESS
    ax, ay, az = _position(agent)
    best = None
    best_d = radius * radius
    facing = None
    if half_angle_deg is not None:
        facing = _yaw_of(agent)

    # GetAllEntitiesWithTransform(), NOT GetEntities(): the latter returns an
    # empty list for runtime-created entities, so sensing through it finds
    # nothing and every NPC appears to be blind. Perception needs positions
    # anyway, so the transform-bearing set is both the populated one and the
    # right one.
    for ox, oy, oz, other in _perception_snapshot(ctx, scene):
        # Compare POSITION, not identity. Every call returns a fresh Python
        # wrapper, so `other is agent` and `other == agent` are both False for
        # the agent's own entity -- verified. Comparing identity therefore
        # never skips self, and the NPC senses itself as the nearest target
        # and walks to where it already stands.
        #
        # An exact position match is the right test anyway: it is the same
        # entity occupying the same point, and it cannot be fooled by wrapper
        # churn.
        if ox == ax and oy == ay and oz == az:
            continue
        dx = ox - ax
        dy = oy - ay
        dz = oz - az
        d2 = dx * dx + dy * dy + dz * dz
        if d2 > best_d:
            continue
        if facing is not None and d2 > 1e-9:
            # Cone test. Yaw 90 is +Z in this engine's convention, matching
            # FaceTarget, so the forward vector is (sin, cos) rather than the
            # (cos, sin) a Z-up engine would use.
            fx = math.sin(math.radians(facing))
            fz = math.cos(math.radians(facing))
            dist = d2 ** 0.5
            dot = (dx * fx + dz * fz) / dist
            if dot < math.cos(math.radians(half_angle_deg)):
                continue
        if require_los and _los_blocked(ctx, ax, ay, az, ox, oy, oz, other):
            continue
        best_d = d2
        best = other
    if best is None:
        # CLEAR, not set-to-None. The blackboard tracks validity SEPARATELY from
        # value: set(key, None) marks the key WRITTEN, so a BlackboardIsSet(key)
        # gate reads true forever and the branch that re-acquires a target can
        # never run again. The agent then holds a permanently "valid" empty
        # target and the tree stops acting on it without any error anywhere.
        #
        # This was set(key, best) with best == None, which is exactly that bug --
        # it survived because the earlier tests only checked the VALUE, which
        # reads None either way.
        ctx.bb.clear(key)
    else:
        ctx.bb.set(key, best)
    return SUCCESS



def _yaw_of(agent):
    """Current Y heading in degrees. None when it cannot be read."""
    try:
        return agent.GetTransform().Rotation.y
    except Exception:
        return None


def _set_yaw(agent, degrees_y):
    """Write only the Y component of the Euler rotation.

    Read-modify-write on all three axes so an existing pitch or roll survives,
    and scale is passed through untouched.
    """
    t = agent.GetTransform()
    r = t.Rotation
    agent.SetTransform(t.Translation, Wasteland.Vec3(r.x, degrees_y, r.z),
                       t.Scale)


def _face(agent, ctx, target, turn_rate):
    """Rotate toward the target at a capped rate, in DEGREES per second.

    SHORTEST ARC: the delta is wrapped into [-180, 180] before use, so a
    target just behind the NPC turns 2 degrees rather than 358. Without the
    wrap an NPC spins the long way round at spawn and looks broken.
    """
    if agent is None or target is None:
        return FAILURE
    ax, ay, az = _position(agent)
    bx, by, bz = _position(target)
    dx = bx - ax
    dz = bz - az
    if abs(dx) < 1e-6 and abs(dz) < 1e-6:
        return SUCCESS
    bearing = math.degrees(math.atan2(dx, dz))
    current = agent.GetTransform().Rotation.y
    delta = (bearing - current + 180.0) % 360.0 - 180.0
    step = turn_rate * ctx.seconds
    if abs(delta) <= step:
        _set_yaw(agent, bearing)
    else:
        _set_yaw(agent, current + (step if delta > 0 else -step))
    return SUCCESS


def _has_los(ctx, agent, target, max_distance):
    """Line of sight: in range AND not behind geometry.

    NB.7 replaced the previous version, which returned only
    `_dist(agent, target) <= max_distance` and so reported "visible" for a
    target on the far side of a wall. That was documented as a limitation rather
    than a claim, which was honest, but it meant every tree using
    `HasLineOfSight` was checking distance and calling it sight.

    Takes `ctx` now, so every call site must pass it -- that signature change is
    the whole point, since without the context there is no scene to trace
    against.

    When no trace can be made -- no scene, no physics world -- this falls back
    to the distance test, because the alternative is an NPC that can never see
    anything in a headless test, which is a worse failure than the documented
    limitation it replaces.
    """
    if agent is None or target is None:
        return False
    if _dist(agent, target) > max_distance:
        return False
    scene = _scene(ctx)
    if scene is None:
        return True
    ax, ay, az = _position(agent)
    bx, by, bz = _position(target)
    return not _los_blocked(ctx, ax, ay, az, bx, by, bz, target)



def _gate(ctx, node_id, every):
    """Deterministic modulo sampling, not random.

    Random sampling makes a tree unreproducible in a test, so a per-node
    frame counter is the whole mechanism. Returns True on one tick in
    `every`. The counter lives in node_states, so two Gate nodes in one tree
    do not share phase.
    """
    if every <= 0:
        return True
    # node_id is the NODE's authored id, emitted as a literal. Context
    # carries no node id of its own (__slots__ = bb, seconds, tree,
    # entity, now), and keying on the tree instead would make two Gate
    # nodes in one tree share a phase -- both would fire on the same tick.
    st = ctx.tree.node_states.setdefault("gate:" + node_id, {})
    n = st.get("n", 0) + 1
    st["n"] = n
    return n % every == 1


def _play_animation(agent, name, loop):
    """Drive animation by name.

    There is no AnimationComponent in this binding -- the Animation submodule
    is an ASSET registry, not a per-entity component -- so this reports
    FAILURE rather than referencing a name that is not bound.

    FAILURE, never a raise: the script host swallows exceptions and carries
    on, so a node that raised here would be silently inert on exactly the
    branch that needs it.
    """
    return FAILURE


def _call_python(agent, method, ctx):
    """Escape hatch: call a method on the agent's script component.

    Returns FAILURE, never raises, when there is no script or no such
    method -- an unbound name here would leave the node permanently inert
    while the tree walked past it cleanly every frame.
    """
    if agent is None or not agent.HasScriptComponent():
        return FAILURE
    script = agent.GetScriptComponent()
    fn = getattr(script, method, None)
    if fn is None or not callable(fn):
        return FAILURE
    fn()
    return SUCCESS
)WL_SHIM";

	// ------------------------------------------------------------- behaviour

	// Graph -> a Python behavior CLASS whose tree is DATA, not control flow.
	// Compiler.h states the reason: the runtime has to be able to suspend a
	// latent node, resume exactly it, and re-enter from the root so a
	// higher-priority branch can pre-empt a running lower one. Nested if/else
	// makes all three inexpressible.
	// ---
	// ---------------------------------------------------------------- behaviour
	//
	// The banner separates the three sections of this file: the runtime shim
	// above, the tree compiler here, and the flow compiler below.
	std::string Compiler::CompileBehavior(const Graph &graph, GraphError &error)
	// Returns the generated source, or an EMPTY string on failure with `error`
	// filled. Empty-string-as-failure rather than a bool, because the successful
	// return value is the payload.
	// ---
	// `graph` is const: compilation never mutates the authored graph.
	{
		// Validate even though callers are told to. The emitters interpolate
		// operator strings and literal values straight into generated Python, so
		// skipping this would turn a bad graph into an injection hole in a
		// generated file.
		// A local rather than an out-param, because nothing here needs the findings
		// -- only the first error, which becomes the caller's headline.
		ValidationResult validation;
		// Runs the tree validator. The comment above explains why this is not
		// redundant belt-and-braces: emitters interpolate operator strings and
		// literals straight into Python, so skipping validation would turn a bad
		// graph into an injection hole in a generated file.
		Validator::ValidateTree(graph, validation);
		// HasErrors() gates the scan below. Validator.h notes the caller must not
		// read this result as authoritative for flow graphs -- this is the tree
		// path only.
		if (validation.HasErrors())
		{
			// Iterates findings rather than stopping at the first, because IsError
			// filtering has to happen before the first error can be identified -- the
			// list is not pre-sorted by severity.
			for (const GraphError &finding : validation.Findings)
			{
				// Findings carry their own IsError flag, so warnings in the list do not
				// abort the compile.
				if (finding.IsError)
				{
					// The headline error is the FIRST error finding, not the first finding.
					error = finding;
					// Empty return = failure. Every early return in this function follows that
					// convention, which is why the assembly code below can simply check the
					// string at the end.
					return std::string();
				}
			}
		}

		// The root is the one node that is nobody's child.
		// childCount maps node id -> how many parents claim it. operator[]
		// default-constructs to 0, which is what makes the lookup on the next loop
		// work without a separate contains() check.
		// ---
		// DEFECT (documented, not fixed): operator[] INSERTS on lookup, so the map
		// is mutated while being read one loop later. Harmless here, but it means
		// the map grows to include every node that is claimed at least once.
		// ---
		// The root is found structurally: the one node no other node claims as a
		// child. There is no 'root' flag on GraphNode, so a graph with two
		// unclaimed nodes silently compiles from whichever is first in the vector.
		std::unordered_map<std::string, int> childCount;
		for (const GraphNode &node : graph.Nodes)
		{
			// A node may list the same child twice; the counter accumulates, so a
			// duplicate entry still counts as a claim and the child is not a root.
			for (const std::string &childId : node.ChildIds)
				// operator++ on an int mapping, with the implicit default-construct to 0.
				// A node id that appears as a child but is not in graph.Nodes still gets an
				// entry -- FindNode later returns null for it and the recursion below skips
				// it.
				childCount[childId]++;
		}
		// The root pointer, null until the next loop assigns it.
		const GraphNode *root = nullptr;
		for (const GraphNode &node : graph.Nodes)
		{
			// The root test. Note this breaks on the FIRST such node rather than
			// counting them, so the ambiguity is invisible.
			if (childCount[node.Id] == 0)
			// Takes the address of a node inside graph.Nodes. Safe for the rest of the
			// function because the graph outlives every use and the vector is not
			// modified -- unlike the childCount map, which is a local and does mutate.
			{
				// Closes the search loop.
				root = &node;
				// Break, not continue: one root only.
				break;
			}
		}
		// No node is nobody's child. An empty graph, or one where every node is
		// someone's child (a cycle), lands here.
		if (root == nullptr)
		{
			// The message string. Note it carries no node id and HasNode is left
			// alone, so the caller cannot highlight a location for this one -- there
			// is no location to highlight.
			error.Message = "no root node";
			return std::string();
		}

		// ---- build the topology table ----
		//
		// A flat dict keyed by node id, each entry carrying 'kids' as child IDS.
		// NOT nested if/else: the runtime walks this table so it can suspend a
		// latent node, resume exactly that node, and re-enter from the root so a
		// higher-priority branch can pre-empt a running lower one. Generated
		// control flow cannot express any of that.
		// The registry singleton, bound to a mutable reference. Read-only in
		// practice -- nothing here writes through it -- but a const& would be the
		// honest type.
		// ---
		// ---- build the topology table ----
		// The phase boundary: everything above discovers structure, everything from
		// here writes the topology DATA that the runtime walks.
		NodeRegistry &registry = NodeRegistry::Get();
		// The emitted table, streamed rather than accumulated in a vector of
		// strings. The indentation is literal tabs written by the stream operator on
		// the line that appends each entry, so the generated Python matches the
		// emitter's own convention.
		std::ostringstream table;
		// The already-emitted set. Two purposes at once: it prevents infinite
		// recursion on a cyclic graph and it stops a shared child being emitted
		// twice into the flat table.
		std::unordered_map<std::string, bool> emitted;

		// std::function rather than a plain lambda BECAUSE this lambda calls
		// itself, at the bottom of its own body. A plain lambda has no name to
		// capture; std::function gives it one.
		std::function<void(const GraphNode &)> emitNode =
			// The lambda's parameter. `node` is passed by const reference, matching the
			// GraphNode storage in the graph's vector.
			[&](const GraphNode &node)
		{
			// The cycle guard. A child that is already in the table returns without
			// re-emitting, so a cycle terminates and a diamond is emitted once.
			// ---
			// This is also why the emitted set must be captured by reference: it is the
			// recursion's memory.
			if (emitted[node.Id])
				// Returns void, so the early exits below are bare returns and the error is
				// reported out-of-band through the captured `error`.
				return;
			// Marked BEFORE emitting, so a node whose emitter somehow re-enters this
			// same node does not loop.
			emitted[node.Id] = true;

			// The def for this node's TYPE, looked up per node. A graph referencing an
			// unregistered type reaches the null check below.
			const NodeDef *def = registry.Find(node.TypeId);
			// DEFECT (documented, not fixed): a null def or a null Emit is SILENTLY
			// skipped. The node simply does not appear in the topology table, so the
			// runtime sees a child id with no entry and -- depending on the runtime --
			// either skips it or raises at tick time. Nothing here sets `error`, so a
			// graph naming an unregistered node type compiles to a file that misbehaves
			// only when run. The validator is supposed to catch it first.
			if (def == nullptr || def->Emit == nullptr)
				return;

			// Per-node emit context. Constructed fresh for each node, so no output from
			// one node can leak into the next.
			EmitContext ctx;
			// A back-pointer to the graph rather than a copy. The emitters need the
			// whole graph to resolve links, and copying it per node would be absurd.
			ctx.Graph = &graph;
			// The node being emitted, for the same reason.
			ctx.Node = &node;
			// Indent 3, matching the tabs the topology table is written with on the
			// append line far below. The two must agree or the generated dict literal
			// is malformed.
			// ---
			// Tree entries are one dict per node, so this is a CONSTANT for the tree
			// path -- unlike the flow path, which threads a real indent through
			ctx.Indent = 3;

			// A BT node's inputs are literals or blackboard key NAMES, never
			// data wires -- its structure comes from the child list. A linked
			// data pin here is meaningless and the validator rejects it.
			// Walks the DEF's pins rather than the node's links, so a pin with no
			// connection still gets its default -- that is how an unconnected input
			// arrives at the emitter as a value rather than being absent.
			for (const PinDef &pinDef : def->Pins)
			{
				// IN pins only, and Exec pins are skipped: an exec pin is a control edge
				// with no literal value, and in a tree it is represented by the child list
				// rather than by a resolved input.
				if (pinDef.Dir != PinDir::In || pinDef.Type == PinType::Exec)
					continue;
				// Out-param struct. IsSet distinguishes 'the author set this' from 'the
				// pin has a default', which matters because the next line chooses between
				// them.
				PinLiteral literal;
				bool isLinked = false;
				// linkNode / linkPin are populated by ResolveInput but UNUSED here -- in
				// the tree path a linked data pin is meaningless and is skipped two lines
				// down. They exist because ResolveInput has one signature for both paths.
				std::string linkNode;
				std::string linkPin;
				// The single graph query that answers 'is this pin wired, and if so to
				// what'. It writes five out-params at once; isLinked is the one that
				// decides the branch below.
				graph.ResolveInput(node.Id, pinDef.Id, literal, isLinked, linkNode, linkPin);
				// A LINKED data pin in a tree graph is rejected by the validator and
				// silently ignored here. The comment above explains the reasoning: a BT
				// node's inputs are literals or key names, never data wires.
				if (isLinked)
					continue;
				// Author-set value wins over the pin default. The ternary is the whole
				// precedence rule for literal inputs.
				std::string text = literal.IsSet ? literal.Value : pinDef.DefaultLiteral;
				// An empty literal with no default leaves the input ABSENT from ctx.Inputs,
				// rather than pushing an empty string. The emitter then sees a pin with no
				// entry and is expected to have its own fallback.
				// ---
				// DEFECT (documented, not fixed): 'empty' is used as the absent marker, so
				// a pin whose author deliberately typed an empty string is indistinguishable
				// from an unwired pin. For a String pin that means '' can never be passed.
				if (text.empty())
					continue;
				// `parsed` and `parseError` are the out-params of ParseLiteral.
				// ---
				// DEFECT (documented, not fixed): parseError is written but NEVER READ, at
				// all four ParseLiteral call sites in this file. A literal that fails to
				// parse -- a malformed float, a bad enum name -- is therefore dropped
				// silently and the node is emitted with that input missing, rather than
				// being reported to the user.
				std::string parsed;
				std::string parseError;
				// Only the boolean is consulted. See the note above on parseError.
				if (ParseLiteral(pinDef.Type, text, parsed, parseError))
					// The input reaches the emitter as a PAIR of pin id and already-parsed
					// text. Parsing here rather than in each emitter is what keeps twenty-odd
					// node types from each growing their own literal parser.
					ctx.Inputs.push_back({ pinDef.Id, parsed });
			}

			// Outputs is the emitter's return channel: it writes temporaries and, for
			// BT nodes, the dict entry that becomes the node itself.
			EmitOutputs outputs;
			// The node's emitter. This is the plugin point -- NodeRegistry holds a
			// table of these, which is why adding a node needs no compiler change.
			std::string nodeError = def->Emit(ctx, outputs);
			// A non-empty return means the emitter itself found a problem. Empty means
			// success, so the error channel is the same string that would otherwise
			// hold generated code.
			if (!nodeError.empty())
			{
				error = GraphError();
				// The message, from the emitter's return value.
				error.Message = nodeError;
				// NodeId is set alongside the message so the editor can highlight the
				// offending node rather than only printing text.
				error.NodeId = node.Id;
				error.HasNode = true;
				// Returns void, so this `return` only exits the LAMBDA. The remaining
				// recursion and the caller's error check both see the error through the
				// captured reference.
				return;
			}

			// The dict entry for this node, as generated Python source text. Defaults
			// to the Python None literal for a node that emitted nothing.
			// ---
			// DEFECT (documented, not fixed): if no output named "Value" exists, the
			// entry stays None and -- because the kids splice below is guarded on
			// entry.back() == '}' -- the node's CHILDREN are dropped without any error.
			// A parent can then reference a child id the runtime can never resolve,
			// reported as a runtime failure rather than as a compile error.
			std::string entry = "None";
			// Scans every output looking for the one named "Value".
			for (const auto &output : outputs.Outputs)
			{
				// "Value" is the conventional name BT emitters use for the entry that IS
				// the node. The comment at the flow emitter's output loop explains why the
				// same name means something different there -- carrying this convention
				// across the two emitters is the trap.
				if (output.first == "Value")
				{
					// First match wins. An emitter that emitted two outputs both named
					// "Value" would silently lose the second.
					entry = output.second;
					break;
				}
			}

			// Splice the child IDS in, in DECLARED order. Declaration order IS
			// priority for a tree, so it is never sorted: sorting would
			// silently change what the NPC does.
			// The `entry.back() == '}'` test is a TEXTUAL check that the emitter
			// produced a dict literal. It is the only thing distinguishing a BT node
			// entry from a bare expression, and it fails closed: a node whose entry is
			// an expression keeps its entry but silently loses its children.
			// ---
			// Child ids in DECLARED order, spliced into the dict as a 'kids' entry.
			// ---
			// Declaration order IS priority for a behaviour tree, which is exactly why
			// it is never sorted: sorting would silently change what the NPC does.
			if (!node.ChildIds.empty() && !entry.empty() && entry.back() == '}')
			{
				// Opening bracket of the kids list, built as text and spliced in as one
				// unit rather than streamed, because it has to be inserted before the
				// entry's closing brace.
				std::string kids = "[";
				// Index-based rather than range-free because the separator comma depends on
				// the index.
				for (size_t i = 0; i < node.ChildIds.size(); ++i)
				{
					// Comma only BETWEEN elements, so the generated list has no leading or
					// trailing comma and is valid Python.
					if (i > 0)
						// Comma between elements -- guarded on i > 0, so the list never leads with
						// one.
						kids += ", ";
					// Each child id goes through EscapePy, which is what keeps a quote or a
					// backslash in an authored id from breaking the generated literal.
					kids += "\"" + EscapePy(node.ChildIds[i]) + "\"";
				}
				// Closes the kids list.
				kids += "]";
				// Rewrites the entry's trailing brace into a comma plus the kids list plus
				// a new closing brace. Slicing off one character assumes the entry ends
				// exactly with that brace -- which the test above checked.
				// ---
				// A dict with 'kids' added AFTER other keys keeps those keys first, so the
				// entry's key order is emitter-determined and this one is appended.
				entry = entry.substr(0, entry.size() - 1) + ", 'kids': " + kids + "}";
			}

			// A leaf marked latent must EMIT latent. Checking the emitted text
			// rather than a list of known-latent node types is what makes this
			// self-maintaining: a new latent node is correct the moment its
			// emitter sets the flag, and a node that has the flag set but no
			// latent body is caught here.
			//
			// The failure this prevents is silent: a leaf that always decides
			// stops the tree on the first tick, so the trace looks clean and
			// the NPC never reaches a sibling that runs after it.
			// The latent-flag audit. A node declared latent must have emitted a
			// 'latent' key, or the runtime would treat a suspending node as finished
			// on the first tick.
			// ---
			// A TEXT search over generated source rather than a structural check. It is
			// self-maintaining (a new latent node is correct the moment its emitter
			// sets the flag) at the cost of being fooled by an emitter that merely
			// mentions the word in a comment or a string.
			if (node.Latent && entry.find("'latent'") == std::string::npos)
			{
				error = GraphError();
				// The message spans three source lines purely for width; the generated
				// text is assembled by the stream operator before the string literal ends.
				error.Message =
					"marked latent, but the emitted body carries no 'latent' "
					// Continuation of the message literal.
					"flag -- a leaf that always decides cannot suspend, and the "
					// Final continuation, ending the literal and the statement.
					"tree will treat it as finished on the first tick";
				error.NodeId = node.Id;
				error.HasNode = true;
				return;
			}

			// One table row: node id -> entry, followed by a comma. Every row carries
			// a trailing comma; the last one is removed further down so the generated
			// literal is not just valid but readable.
			// ---
			// Three tabs, matching ctx.Indent above and the _build() body's nesting.
			table << "\t\t\t\"" << EscapePy(node.Id) << "\": " << entry << ",\n";

			// Recurse into children AFTER writing this node's row. Order matters only
			// for readability here -- the table is data, so row order is not semantics.
			for (const std::string &childId : node.ChildIds)
			{
				// FindNode on a possibly-dangling child id. A null result is skipped below
				// rather than raising, so a graph naming a nonexistent child still compiles.
				const GraphNode *child = graph.FindNode(childId);
				// Null-guarded: a ChildIds entry naming a node that does not exist is
				// skipped, so the table simply omits that subtree.
				if (child != nullptr)
					// The self-call that forces the std::function indirection.
					emitNode(*child);
			}
		};

		// Kick off the walk at the root. Everything reachable from the root is
		// emitted; an unreachable node is never visited, which is another way a
		// graph can compile into a table with holes.
		emitNode(*root);

		// Drop only the trailing COMMA, never the newline. Removing ",\n"
		// pulls the closing brace of the dict up onto the last entry's line,
		// which is valid Python but makes the generated topology unreadable --
		// and the whole point of emitting it as data is that a human can read
		// it when a tree misbehaves.
		// Materialise the stream. Called once; str() on an ostringstream is not
		// destructive but is not cheap either, so it is not done inside the loop.
		// ---
		// Drop only the trailing COMMA, never the newline.
		// ---
		// Removing the comma AND the newline would pull the closing brace up onto
		// the last entry's line: still valid Python, but unreadable -- and the whole
		// point of emitting the topology as data is that a human can read it when a
		// tree misbehaves.
		std::string tableText = table.str();
		// Only when the table ends with a newline -- i.e. when at least one row was
		// written. An empty table leaves tableText empty and this whole block is
		// skipped, which is what lets an empty graph reach the assembly stage.
		if (!tableText.empty() && tableText.back() == '\n')
		{
			// Searches BACKWARDS from the second-to-last character, because the last
			// character is known to be the newline. find_last_not_of returns npos if
			// every character is a newline, which the next test catches.
			// ---
			// The second argument is a position, not a length -- reading it as a
			// length is the natural mistake and yields an off-by-one search.
			size_t last = tableText.find_last_not_of("\n", tableText.size() - 2);
			// The backward search for the last non-newline character, which is the
			// trailing comma if the emitters all appended one.
			if (last != std::string::npos && tableText[last] == ',')
			{
				// Erase exactly one character at `last`, not a range. The newline that
				// follows is deliberately left in place, per the comment above.
				tableText.erase(last, 1);
			}
		}

		// The deferred error check. Everything above can set `error` through the
		// lambda; this is where it is honoured.
		// ---
		// Placed after the table so the whole table is built even on failure. That
		// is wasted work on an error path, but it keeps the success path free of
		// error checks inside the recursion.
		if (!error.Message.empty())
			return std::string();

		// ---- assemble the file ----
		// ---- assemble the file ----
		// Header, imports, the shim, then the class. The generated file is a
		// complete, self-contained Python module.
		// ---
		// ---- assemble the file ----
		// Header, imports, the shim, then the class. The generated file is a
		// complete, self-contained Python module.
		std::string className = ClassNameFor(graph);
		// The output stream. Everything below is appended in the order it must
		// appear in the generated file; Python is order-sensitive here because
		// class bodies execute top to bottom.
		std::ostringstream out;

		// The banner comment. graph.Name is interpolated RAW here, unlike every id
		// below -- so a quote in a graph name lands in a generated comment line.
		// ---
		// DEFECT (minor, documented, not fixed): this is the one author-supplied
		// string in the file that does not go through EscapePy. It cannot break the
		// syntax (the text is inside a comment), but a newline in the name would
		// end the comment early and the remainder would execute as Python.
		out << "# Generated by Wasteland Visual Blocks from '" << graph.Name << "'.\n"
			<< "# Do not edit by hand -- regenerate from the source graph.\n"
			// The math import. Redundant with the shim's own import, and deliberate:
			// the shim imports it defensively because it may be exec'd anywhere, while
			// this is the generated module's own dependency.
			<< "import math\n"
			// The runtime module import.
			<< "import wl_behavior\n"
			// The three status constants as names, so generated code reads SUCCESS
			// rather than wl_behavior.SUCCESS.
			<< "from wl_behavior import SUCCESS, FAILURE, RUNNING\n"
			// The engine module import, needed by the shim's engine calls.
			<< "import Wasteland\n"
			<< kRuntimeShim << "\n\n";

		// The class statement. className comes from ClassNameFor, so it is
		// sanitised -- see SanitizeIdentifier's keyword defect for what that does
		// and does not cover.
		out << "class " << className << ":\n";
		// The class docstring, with the graph name interpolated the same unescaped
		// way as the banner above. A triple-quote in a graph name would terminate
		// the docstring early and the rest would execute as code.
		out << "\t\"\"\"Behavior compiled from the '" << graph.Name << "' tree.\"\"\"\n\n";

		// `scene` is OPTIONAL. Perception nodes need a world to search, and an
		// Entity carries no back-reference to its Scene -- so the caller that
		// owns both passes it in. Left None, perception degrades to a no-op
		// rather than raising.
		// `scene` defaults to None and is OPTIONAL. Perception nodes need a world
		// and an Entity carries no back-reference to its Scene, so the caller that
		// owns both injects it. Left None, the shim's _scene() returns None and
		// perception degrades to a no-op rather than raising.
		out << "\tdef __init__(self, entity, scene=None):\n";
		out << "\t\tself.entity = entity\n";
		// Stored on the behavior, not on the tree, because the tree has no scene
		// slot -- it is attached below so the runtime can stay generic.
		out << "\t\tself.scene = scene\n";

		// The blackboard is built from its DECLARED defaults, which is also how
		// a key becomes "declared". A declared default counts as WRITTEN, so a
		// tree declaring {"attackAt": 0.0} reads as set on tick one instead of
		// leaving its first branch to fire a frame late.
		// A declared default counts as WRITTEN, so a tree declaring
		// {"attackAt": 0.0} reads as set on tick one instead of leaving its
		// first branch to fire a frame late. Tree turns this dict into the
		// Blackboard, which is what makes the keys "declared" rather than
		// merely defaulted.
		// Opening of the dict. Every entry written below becomes a WRITTEN key in
		// Tree's Blackboard, which is the constraint that drives the omission rule
		// further down.
		// ---
		// The blackboard literal, built from the graph's DECLARED defaults.
		// ---
		// A declared default counts as WRITTEN, so a tree declaring an attackAt
		// key reads as set on tick one instead of leaving its first branch to fire
		// a frame late.
		// ---
		// DEFECT (documented, not fixed): the paragraph above is DUPLICATED. Lines
		// below restate the same 'declared default counts as WRITTEN' claim in
		// slightly different words, which is how a comment drifts out of sync with
		// the code it describes. Harmless at runtime, actively misleading to the
		// next reader.
		out << "\t\tself._blackboard = {\n";
		// One iteration per declared key, in declaration order -- the dict order in
		// the generated file mirrors the order the author listed them.
		for (const BlackboardKey &key : graph.Blackboard)
		{
			// The fallback for a key with no default. 'None' is a real Python value
			// here, and the rule below decides when it is allowed to appear.
			std::string defaultValue = "None";
			// Only keys WITH a default get parsed; an empty default skips straight to
			// the literal None.
			if (!key.DefaultLiteral.empty())
			{
				std::string parsed;
				std::string parseError;
				// ParseLiteral again, again dropping parseError on the floor. Same defect
				// as the pin path above: a malformed default is silently ignored and the
				// generated dict gets the None fallback.
				if (ParseLiteral(key.Type, key.DefaultLiteral, parsed, parseError))
					// On success `parsed` is the Python text; on failure the fallback above
					// stands. The two are indistinguishable afterwards, which is the defect.
					defaultValue = parsed;
			}
			// An ENTITY key with no default is deliberately OMITTED, not seeded
			// as None. Tree turns every entry of this dict into a WRITTEN
			// blackboard key, so {"target": None} makes BlackboardIsSet("target")
			// answer True on tick one -- with nothing in it. An NPC then reads
			// "I already have a target", never enters its acquire branch, and
			// idles forever, silently and perfectly.
			//
			// Omitting the key is the only honest encoding of "no target yet":
			// validity and presence become the same question, which is what an
			// entity slot actually means.
			// The omit predicate: Entity-typed AND no authored default. A String key
			// with no default IS emitted as None, because None is a legitimate string
			// value and 'not set yet' means something different for it.
			// ---
			// An ENTITY key with no default is deliberately OMITTED rather than
			// seeded as None.
			// ---
			// Tree turns every entry of this dict into a WRITTEN key, so seeding
			// {"target": None} makes BlackboardIsSet("target") answer True on tick
			// one, with nothing in it. An NPC then reads 'I already have a target',
			// never enters its acquire branch, and idles forever -- silently and
			// perfectly, which is the worst possible failure mode.
			const bool omit = key.Type == PinType::Entity && key.DefaultLiteral.empty();
			// The write side of the omit rule. Only a NON-omitted key reaches the
			// stream, so the generated dict's contents and the graph's declared keys
			// differ deliberately.
			if (!omit)
				// Key name through EscapePy, value from the parse above. One tab deeper
				// than the dict opener, so the generated literal is indented consistently.
				out << "\t\t\t\"" << EscapePy(key.Name) << "\": " << defaultValue << ",\n";
		}
		// Close of the blackboard dict.
		out << "\t\t}\n";
		// Declared types travel alongside so a trace or a future type check can
		// report a mismatched write instead of guessing at it.
		// Declared types travel alongside so a trace or a future type check can
		// report a mismatched write instead of guessing at it.
		// ---
		// This dict is separate from _blackboard and has no defaults, so it is
		// safe to populate for every key including the omitted ENTITY ones.
		// ---
		// Declared types travel alongside so a trace or a future type check can
		// report a mismatched write instead of guessing at it.
		// ---
		// This dict is separate from _blackboard and has no defaults, so it is
		// safe to populate for every key including the omitted ENTITY ones.
		out << "\t\tself._blackboard_types = {\n";
		for (const BlackboardKey &key : graph.Blackboard)
		{
			// The TYPE name as a string, via PinTypeName. Emitted for every key, so a
			// reader of the generated file can see the intended type without consulting
			// the graph file.
			out << "\t\t\t\"" << EscapePy(key.Name) << "\": \"" << PinTypeName(key.Type)
				// Continuation of the type-name emission, ending the statement.
				<< "\",\n";
		}
		out << "\t\t}\n";
		// Tree's signature is (entity, topology, blackboard=...,
		// blackboard_types=...). Passing the behavior class as the entity is
		// the bug this shape avoids: every generated leaf reads engine state
		// through the entity, and handing it `self` would send every call at
		// the behavior object instead.
		// Tree builds its OWN Blackboard from these dicts
		// (Tree(entity, topology, blackboard, blackboard_types)). Passing a
		// Blackboard instance instead makes Tree iterate it as a mapping, and
		// Blackboard exposes keys()/get() rather than items() -- so every
		// generated class raised AttributeError on construction.
		// The two dicts are passed as PLAIN DICTS, not as a Blackboard instance.
		// ---
		// Tree builds its own Blackboard from these. Passing a Blackboard instead
		// makes Tree iterate it as a mapping, and Blackboard exposes keys()/get()
		// rather than items() -- so every generated class raised AttributeError on
		// construction.
		// ---
		// The two dicts are passed as PLAIN DICTS, not as a Blackboard instance.
		// ---
		// Tree builds its own Blackboard from these. Passing a Blackboard instead
		// makes Tree iterate it as a mapping, and Blackboard exposes keys()/get()
		// rather than items() -- so every generated class raised AttributeError on
		// construction.
		out << "\t\tself.tree = wl_behavior.Tree(self.entity, self._build(),\n";
		// Continuation of the Tree construction call, wrapped so the generated
		// line stays readable.
		out << "\t\t\tself._blackboard, self._blackboard_types)\n";
		// Tree has no scene slot of its own; perception reads it through
		// getattr, so attaching it here keeps the runtime generic.
		// The scene is attached to the TREE, not passed to it: Tree has no scene
		// slot of its own and the shim reads it through getattr, which is what
		// keeps the runtime free of scene knowledge.
		out << "\t\tself.tree.scene = self.scene\n\n";

		// _build returns the topology dict. Building it in a method rather than
		// inline in __init__ keeps the constructor short and lets the table be
		// re-read without reconstructing the Tree.
		out << "\tdef _build(self):\n";
		// The generated comment inside _build, explaining why the topology is
		// data rather than control flow -- written into the OUTPUT so a reader of
		// the generated file sees the same note.
		out << "\t\t# Topology as DATA, so the runtime can suspend a latent node and\n";
		// The second line of that generated comment.
		out << "\t\t# resume exactly it. See the note at the top of wl_behavior.py.\n";
		// Opens the returned dict literal.
		out << "\t\treturn {\n";
		// The spliced table text, already comma-corrected above. Streamed in one
		// piece rather than entry by entry, so no escaping concern applies here.
		out << tableText;
		// Closes _build with two blank lines, separating the method from the next.
		out << "\t\t}\n\n";

		// The public entry point the script host calls. Delegates straight to the
		// runtime -- all the tree logic lives in wl_behavior, not in generated code.
		out << "\tdef tick(self, seconds=0.0):\n";
		// seconds defaults to 0.0, so a host that ticks with no argument still
		// produces a valid call rather than a TypeError.
		out << "\t\treturn self.tree.tick(seconds)\n";

		// Clears any error before the success return, so a caller that ignored a
		// previous failure does not see a stale message alongside valid source.
		error = GraphError();
		return out.str();
	}

	// ----------------------------------------------------------------- flow

	// Graph -> a Python COMPONENT class. The flow counterpart to CompileBehavior.
	// ---
	// Genuinely different output shape, not a variant of the tree path: this
	// emits nested control flow in the TARGET language, because a flow graph has
	// nothing to suspend and resume.
	std::string Compiler::CompileFlow(const Graph &graph, GraphError &error)
	// Same failure convention: empty string plus a filled `error`.
	{
		// A local, as in the tree path. The flow validator's findings are never
		// surfaced individually -- only the first error becomes the headline.
		ValidationResult validation;
		// ValidateFlow, not ValidateTree. Validator.h warns that the two validators
		// make DIFFERENT promises and that a caller using both must not read one as
		// authoritative over the other -- so this must not be 'simplified' to share
		// the tree's call.
		Validator::ValidateFlow(graph, validation);
		if (validation.HasErrors())
		{
			// Same first-error-wins scan as the tree path.
			for (const GraphError &finding : validation.Findings)
			{
				if (finding.IsError)
				{
					error = finding;
					return std::string();
				}
			}
		}

		// DEFECT (documented, not fixed): the flow class name is computed HERE,
		// by calling SanitizeIdentifier directly, with its own "GeneratedFlow"
		// fallback. Compiler.h exposes ClassNameFor as THE way a caller derives the
		// name the compiler will emit, and that helper is documented as covering
		// exactly this purpose -- but it hardcodes the tree's fallback string.
		// ---
		// Consequence: there is no public accessor for a flow graph's class name, so
		// a caller that must instantiate a generated flow component has to
		// re-derive it the way this line does. That is precisely the duplication
		// ClassNameFor exists to prevent, and it is already drifting.
		std::string className = SanitizeIdentifier(graph.Name, "GeneratedFlow");
		NodeRegistry &registry = NodeRegistry::Get();
		// The event node, which anchors the whole statement chain. Nullable: a flow
		// graph with no event produces a class whose method body is just `pass`.
		const GraphNode *event = graph.FindEvent();

		// Shorter preamble than the tree path -- no wl_behavior, no shim, no
		// blackboard. A flow graph is a plain component.
		std::ostringstream out;
		// The same unescaped graph.Name interpolation as the tree path, in both a
		// leading comment and a docstring. See the note there: a quote or a newline
		// in a graph name reaches the generated file unescaped.
		out << "# Generated by Wasteland Visual Blocks from '" << graph.Name << "'.\n"
			<< "# Do not edit by hand -- regenerate from the source graph.\n"
			// The import block. No wl_behavior and no shim -- a flow graph compiles to
			// a plain component, not to a tree.
			<< "import Wasteland\n\n";
		// The class statement, immediately followed by a blank line because the
		// next emission opens __init__.
		out << "class " << className << ":\n\n";

		// `scene` is OPTIONAL. Perception nodes need a world to search, and an
		// Entity carries no back-reference to its Scene -- so the caller that
		// owns both passes it in. Left None, perception degrades to a no-op
		// rather than raising.
		// Same optional scene parameter as the tree class, for the same reason:
		// perception needs a world and an Entity cannot supply one.
		out << "\tdef __init__(self, entity, scene=None):\n";
		out << "\t\tself.entity = entity\n";
		out << "\t\tself.scene = scene\n";
		// One attribute per declared GraphVariable, emitted in declaration order.
		// ---
		// This is the flow path's version of state: a flat set of instance
		// attributes, where the tree path instead uses a blackboard dict.
		for (const GraphVariable &variable : graph.Variables)
		{
			// Default None, as with blackboard keys.
			// ---
			// DEFECT (documented, not fixed): unlike the blackboard case just above,
			// there is NO omission rule here. A variable declared with no default is
			// emitted as `self.x = None`, so it is indistinguishable from one explicitly
			// set to None -- a flow node testing it for truthiness cannot tell 'unset'
			// from 'set to None'.
			std::string value = "None";
			// Non-empty default, so the parse below runs.
			if (!variable.DefaultLiteral.empty())
			{
				std::string parsed;
				std::string parseError;
				// ParseLiteral again, with its parseError discarded. Same silent-drop
				// defect documented on the tree path's pin parsing.
				if (ParseLiteral(variable.Type, variable.DefaultLiteral, parsed, parseError))
					// value keeps its None fallback when the parse fails.
					value = parsed;
			}
			// SanitizeIdentifier on an author-supplied variable name, so a name with
			// a space becomes a legal attribute.
			// ---
			// DEFECT (documented, not fixed): the character-set sanitiser cannot make a
			// Python KEYWORD legal. A variable named `class` emits `self.class = None`,
			// which is a SyntaxError -- and because `class` survives sanitisation
			// unchanged, it looks correct right up to the point the generated file is
			// parsed. CompileBehavior has the identical exposure via the class name.
			out << "\t\tself." << SanitizeIdentifier(variable.Name, "var") << " = " << value
				// Continuation of the attribute emission, ending the statement.
				<< "\n";
		}
		out << "\n";

		// The generated method's name comes from the EVENT's name, sanitised.
		// "OnUpdate" is the fallback, so a graph with no event still gets a
		// method the host can call -- which is why the null-event branch below
		// emits `pass` rather than omitting the def entirely.
		std::string methodName = "OnUpdate";
		// Both the event node AND its name must be present; an event node with a
		// blank EventName falls back too.
		if (event != nullptr && !event->EventName.empty())
			// Sanitised for the same reason as the class name -- the author's event
			// name becomes a Python def name.
			methodName = SanitizeIdentifier(event->EventName, "OnUpdate");

		// *args, **kwargs absorbs whatever the host passes, so a mismatch between
		// the event's real signature and the generated method is a silent no-op
		// rather than a TypeError. Deliberate: the host's calling convention is
		// not this file's to know.
		out << "\tdef " << methodName << "(self, *args, **kwargs):\n";

		// The no-event branch.
		if (event == nullptr)
		{
			// An event-less graph still gets a syntactically valid method. Python
			// requires the body to be non-empty.
			out << "\t\tpass\n\n";
		}
		else
		{
			// Follow exec links from the event, emitting each node's statement
			// in order. This is the FLOW shape and is a genuinely different
			// code path from the tree compiler: a flow graph has no
			// suspend/resume to express, so control flow maps onto the
			// target language's OWN constructs -- `if`/`else`, `while`,
			// `for` -- rather than onto an evaluator walking topology data.
			//
			// Declaration order of links is preserved: it is the only ordering
			// information a flow graph has, so it must not be reordered.
			// The emitted set, same role as in the tree path: it terminates cycles.
			// ---
			// DEFECT (documented, not fixed): in the FLOW path it also silently DROPS
			// a diamond. If two exec paths converge on one node, the first path emits
			// it and the second contributes nothing, because the guard below breaks on
			// an id already seen. The generated code is still valid; it is just missing
			// statements that the graph says should run twice. Nothing warns.
			std::unordered_map<std::string, bool> emitted;
			// DEFECT (documented, not fixed): statementCount is DEAD. It is declared
			// here and incremented at four sites below, and never read anywhere in the
			// file. The writer that would have used it -- emitting a comment or a
			// summary of what the graph compiled to -- was evidently dropped, leaving
			// the counter behind. It costs nothing at runtime and misleads every reader.
			size_t statementCount = 0;

			// Build the resolved DATA inputs for a node. A linked pin resolves
			// to the producer's temp; an unlinked one to its literal.
			// buildInputs: resolve a node's data inputs into the EmitContext.
			// Captured by reference so the exec walker and the control-flow path share
			// one implementation rather than each growing their own.
			// ---
			// This is the flow counterpart of the tree path's inline pin loop, and the
			// difference that matters is on the linked branch below.
			auto buildInputs = [&](const GraphNode &node, const NodeDef &def,
								   // Continuation of buildInputs' parameter list, wrapped for width.
								   EmitContext &ctx)
			{
				// Iterates the DEF's pins, not the node's links, so an unwired pin still
				// contributes its default.
				for (const PinDef &pinDef : def.Pins)
				{
					// Exec pins skipped: control edges, carried by the successor walk rather
					// than by a resolved input.
					if (pinDef.Dir != PinDir::In || pinDef.Type == PinType::Exec)
						continue;
					PinLiteral literal;
					bool isLinked = false;
					std::string linkNode;
					std::string linkPin;
					graph.ResolveInput(node.Id, pinDef.Id, literal, isLinked, linkNode,
									   linkPin);
					// THIS is where the two compilers differ most sharply. A LINKED input
					// resolves to the producer's temp attribute name, so the data flows
					// through generated variables.
					// ---
					// The tree path instead `continue`s on a linked pin, because a BT node's
					// inputs are literals or blackboard key names and a data wire there is
					// meaningless. Same registry, same pins, opposite contracts.
					if (isLinked)
					{
						// TempAttr for the producer's output. nodeId and pinId come straight from
						// the resolved link, so the read side of the wire and the write side (in
						// emitOwnStatement) must agree on the naming -- which is the whole reason
						// TempAttr is a single shared function.
						ctx.Inputs.push_back(
							// The producer's temp attribute name becomes this pin's input value.
							{ pinDef.Id, TempAttr(linkNode, linkPin) });
						continue;
					}
					// Unlinked pins take the author value, else the pin default -- the same
					// precedence rule as the tree path.
					std::string text = literal.IsSet ? literal.Value : pinDef.DefaultLiteral;
					if (text.empty())
						continue;
					// parseError discarded again; a literal that fails to parse is dropped
					// and the pin is simply absent from the emitter's context.
					std::string parsed;
					std::string parseError;
					if (ParseLiteral(pinDef.Type, text, parsed, parseError))
						// Pushed only on success, so a failed parse leaves the pin out entirely
						// rather than pushing an empty string.
						ctx.Inputs.push_back({ pinDef.Id, parsed });
				}
			};

			// The exec successors of `node`, in declaration order, reached
			// through `fromPin` (empty = every exec out pin). A control-flow
			// node names ONE pin per branch; a plain node chains off its
			// single "Out".
			// successorsOf: the nodes reachable from `node` over exec links.
			// fromPin empty means 'every exec out pin'; a control-flow node passes one
			// specific pin id to ask for just that branch.
			// ---
			// Declaration order of graph.Links is preserved throughout, because it is
			// the only ordering information a flow graph carries.
			auto successorsOf = [&](const GraphNode &node, const std::string &fromPin)
			{
				// Built by append in link order rather than sorted, for the reason above.
				std::vector<const GraphNode *> next;
				// The def is needed to inspect the FROM pin's type -- a link's own record
				// does not say whether the pin it starts at was an exec pin.
				const NodeDef *def = registry.Find(node.TypeId);
				if (def == nullptr)
					return next;
				// A full scan of graph.Links per node, so the walk is O(nodes x links).
				// Fine at authored-graph sizes; it would not be at generated-graph sizes.
				for (const GraphLink &link : graph.Links)
				{
					// Only links leaving this node.
					if (link.FromNode != node.Id)
						continue;
					// The fromPin filter. Empty means no filter, which is what a plain node's
					// 'follow my single Out' call passes.
					if (!fromPin.empty() && link.FromPin != fromPin)
						continue;
					// Rejects a link whose from pin is not Exec, so a data link cannot be
					// mistaken for a control edge.
					const PinDef *outPin = def->FindPin(link.FromPin);
					// Continuation of the out-pin type test.
					if (outPin == nullptr || outPin->Type != PinType::Exec)
						continue;
					// A link to a node that does not exist is skipped rather than raising, so
					// a graph with a dangling exec link still compiles -- to code that is
					// missing that step.
					const GraphNode *target = graph.FindNode(link.ToNode);
					// A link whose target node is missing is skipped, so the successor list
					// simply omits it.
					if (target != nullptr)
						// Appended in link order.
						next.push_back(target);
				}
				return next;
			};

			// Emit one node's own statement (no successors). Returns the
			// statement text, already indented to `indent` tabs.
			// emitOwnStatement: one node's own statement plus its output temps, with
			// NO successors. The chain walker calls this and decides separately what
			// comes next.
			auto emitOwnStatement = [&](const GraphNode &node, int indent) -> std::string
			{
				// An unregistered type or a null emitter yields an empty statement, which
				// the caller treats as 'this node contributed nothing' -- silently.
				const NodeDef *def = registry.Find(node.TypeId);
				if (def == nullptr || def->Emit == nullptr)
					return std::string();

				// A fresh EmitContext per call. The tree path hoists one per node; here it
				// must be per invocation, because emitDataNode re-enters this function
				// recursively for nested expressions.
				EmitContext ctx;
				ctx.Graph = &graph;
				ctx.Node = &node;
				// The REAL indent, threaded through as a parameter. Unlike the tree
				// path's constant 3, flow indentation varies with nesting depth, which is
				// why every emit site below takes it explicitly.
				ctx.Indent = indent;
				// Inputs resolved first, so the emitter sees them.
				// ---
				// This is a distinct call from the one in the control-flow branch below:
				// there the inputs are resolved into a ctx used for the shell, here into a
				// ctx used for the node's own text. Two separate EmitContexts, so a
				// control-flow node's temps and its shell's inputs never mix.
				buildInputs(node, *def, ctx);

				EmitOutputs outputs;
				// The emitter. Its return is STATEMENT text (possibly several lines); its
				// outputs are temps, handled just below.
				// ---
				// Note this call's return value is used, unlike the tree path where the
				// output struct carried everything.
				std::string statement = def->Emit(ctx, outputs);

				// An expression node's OUTPUTS become temps the producer's
				// successors read. This is what makes a math or variable node
				// usable as a wire source rather than a dead end.
				//
				// They go into this function's RETURNED block, not the
				// enclosing stream: a data node can sit inside a Branch body,
				// and writing its temp at method scope would both evaluate the
				// expression unconditionally -- wrong when the branch guard was
				// false -- and raise NameError if the branch never ran. The
				// temp is scoped exactly like the statement it feeds.
				// The temps go into a local stream rather than straight to the enclosing
				// block. That is what scopes a data node to the branch it sits inside:
				// writing at method scope would evaluate the expression even when the
				// branch guard was false, and would NameError if the branch never ran.
				std::ostringstream block;
				// Walks the emitter's outputs, skipping Exec below.
				for (const auto &pair : outputs.Outputs)
				{
				// "Exec" is control flow, not a value, so it gets no temp.
				//
				// "Value" is deliberately NOT skipped. In the tree emitter a
				// node's single dict entry is named "Value" and IS the node
				// rather than a wire, so it is skipped there. A flow graph's
				// expression nodes also name their result pin "Value", and
				// carrying that skip across made every math and comparison node
				// emit NO assignment -- so the consumer read a temp nobody ever
				// computed. Carrying a convention between two emitters with
				// opposite contracts is the trap, not the pin name.
					// "Exec" is control flow, not a value, so it gets no temp.
					// ---
					// "Value" is deliberately NOT skipped. In the TREE emitter a node's single
					// dict entry is named "Value" and IS the node rather than a wire, so it
					// is skipped there. A flow graph's expression nodes also name their result
					// pin "Value"; carrying that skip across made every math and comparison
					// node emit no assignment at all, and the consumer read a temp nobody ever
					// computed. Carrying a convention between two emitters with OPPOSITE
					// contracts is the trap, not the pin name.
					if (pair.first == "Exec")
						continue;
					// `indent` tabs of leading whitespace, matching the node's scope.
					// ---
					// The pattern (indent, name, '=', value) is duplicated verbatim three
					// times in this function's region. They must stay identical or a producer
					// writes a different name than a consumer reads.
					block << std::string((size_t)indent, '	')
						   // Continuation of the temp-assignment line.
						   << TempAttr(node.Id, pair.first) << " = "
						   // End of the assignment, including the newline that terminates it.
						   << pair.second << "\n";
				}

				// An expression-only node has an empty statement; the temps alone are
				// still a valid contribution, so the block is returned rather than dropped.
				if (statement.empty())
					return block.str();

				// The emitter's multi-line statement is re-parsed here and re-indented
				// line by line. The node's own text carries RELATIVE indentation (a
				// Branch body is indented by the node itself), so every line gets the same
				// base prefix and its own relative offset is preserved.
				std::istringstream lines(statement);
				std::string line;
				// The line loop over the statement.
				while (std::getline(lines, line))
				{
					// Blank lines dropped rather than emitted, so the generated file has no
					// gaps where a node chose to format its output across lines.
					if (line.empty())
						continue;
					// A node's own text carries RELATIVE indentation (a
					// multi-line if/else body is indented by the node), so
					// every line gets the same base prefix.
					// The single place the base indent is applied to a node's own text. The
					// control-flow path does the same thing for shell lines, further down --
					// which is why both must agree or one branch of an `if` lands at a different
					// column than the other.
					block << std::string((size_t)indent, '	') << line << "\n";
				}
				return block.str();
			};

			// DATA nodes -- a math or variable node has NO exec pins, so the exec
			// walk never reaches it, yet its output has to be assigned before
			// anything reads it. Without this pass the generated code contains
			// `self.dst = self._vb_m_Value` with no preceding line defining
			// `_vb_m_Value` -- an AttributeError at runtime.
			//
			// Emitted at the CONSUMER's scope, immediately before the consumer,
			// so a data node inside a Branch body is only evaluated when that
			// branch is taken. Hoisting every data node to method scope would
			// evaluate all of them every tick regardless of the guard:
			// correct-looking code doing the wrong work.
			// Data node id -> the indent it was emitted at.
			//
			// Keyed by indent, NOT a plain 'already done' flag: a temp assigned
			// at method scope holds the value from BEFORE a loop, so a loop
			// body that reads it would never see its own writes. Re-emitting at
			// the deeper indent puts the assignment inside the loop, where it
			// re-runs each iteration -- which is what makes `while i < 5` work
			// instead of testing once and then spinning.
			// data node id -> the indent it was last emitted at.
			// ---
			// Keyed BY INDENT rather than being a plain 'already done' flag, and that
			// distinction is load-bearing: a temp assigned at method scope holds the
			// value from BEFORE a loop, so a loop body reading it would never see its
			// own writes. Re-emitting at the deeper indent puts the assignment inside
			// the loop, where it re-runs each iteration -- which is what makes
			// `while i < 5` work instead of testing once and then spinning.
			std::unordered_map<std::string, int> dataEmitted;

			// A node is a pure DATA node when it has no exec pins at all --
			// nothing in the exec chain, so nothing else will emit it.
			// A node is a PURE DATA node when it declares no exec pin at all, so the
			// exec walk can never reach it.
			// ---
			// Structural, for the same reason HasBranchPins is: a flag would be a
			// second registry that has to be updated when a node is added.
			auto isPureDataNode = [&](const GraphNode &n) -> bool
			{
				// Closes the signature.
				const NodeDef *d = registry.Find(n.TypeId);
				// The def lookup. A null def means the type is unregistered, which the
				// caller treats as 'not a data node'.
				if (d == nullptr || d->Emit == nullptr)
					return false;
				// One exec pin of ANY direction is disqualifying -- the pin set is scanned
				// without filtering on direction, so an Exec IN pin also marks the node as
				// not-pure-data.
				for (const PinDef &p : d->Pins)
				{
					// The disqualifying test: any Exec pin at all, in or out.
					if (p.Type == PinType::Exec)
						return false;
				}
				// No Exec pins found: the node is pure data.
				return true;
			};

			// Emit the temps for every data node feeding `node`, in link
			// declaration order (the only ordering a flow graph has).
			// emitDataInputs: emit the temps for every data node feeding `node`, in
			// link declaration order.
			// ---
			// Called from three places -- the loop prologue, the branch guard and the
			// plain-node path -- with the CONSUMER's indent, which is what keeps a data
			// node inside the scope of whatever reads it.
			auto emitDataInputs = [&](const GraphNode &node, int indent) -> std::string
			{
				std::ostringstream block;
				const NodeDef *def = registry.Find(node.TypeId);
				if (def == nullptr)
					return block.str();

				// The consumer's pins, not the producer's.
				for (const PinDef &pinDef : def->Pins)
				{
					if (pinDef.Dir != PinDir::In || pinDef.Type == PinType::Exec)
						continue;
					PinLiteral literal;
					bool isLinked = false;
					std::string linkNode;
					std::string linkPin;
					graph.ResolveInput(node.Id, pinDef.Id, literal, isLinked, linkNode,
									   linkPin);
					// Only LINKED pins here. An unlinked pin is a literal the emitter already
					// has, so there is nothing to emit before it -- unlike buildInputs, which
					// resolves both cases.
					if (!isLinked)
						continue;

					// A link to a missing producer is skipped; the consumer then reads a temp
					// nobody assigned. Valid Python, wrong at runtime.
					const GraphNode *producer = graph.FindNode(linkNode);
					// Continuation of the producer lookup.
					if (producer == nullptr)
						continue;

					// RECURSIVE, because a data node's own inputs can be data
					// nodes: `add(get(i), 1)` feeding a comparison. Emitting only
					// one level deep emitted the outer expression while its own
					// inputs stayed undefined, so the generated line read a temp
					// that nothing had assigned. Depth is bounded by the
					// validator rejecting cycles, and by dataEmitted breaking a
					// diamond rather than looping on one.
					// `indent` (not a depth) -- it is the consumer's scope and every level of
					// the expression shares it, since the whole expression evaluates
					// in one place.
					// std::function again: emitDataNode calls itself for a producer's own data
					// inputs. Depth is bounded by the validator rejecting cycles and by
					// dataEmitted breaking a diamond rather than looping on one.
					std::function<std::string(const GraphNode &, int)> emitDataNode =
						// Continuation of the recursive lambda's signature.
						[&](const GraphNode &producer, int indent) -> std::string
					{
						// Already emitted at THIS indent: a diamond, not a cycle.
						// Already emitted at THIS indent -- a diamond, which terminates. At a
						// DIFFERENT indent it deliberately re-emits, per the note on dataEmitted.
						auto seen = dataEmitted.find(producer.Id);
						// Continuation of the already-emitted test.
						if (seen != dataEmitted.end() && seen->second == indent)
							return std::string();
						// Closes the diamond guard.
						if (!isPureDataNode(producer))
							return std::string();
						// Mark BEFORE recursing so a cycle terminates.
						// Marked BEFORE recursing, so a cyclic graph terminates here instead of
						// overflowing the stack. The validator is supposed to reject cycles first.
						// ---
						// DEFECT (minor, documented, not fixed): the marker is set before the
						// producer's own inputs are emitted, so on a cycle the INNER producer is
						// skipped and its temp is never assigned. Termination is correct; the
						// resulting code is not. Bounded by the validator, which is the only thing
						// preventing the case from arising.
						dataEmitted[producer.Id] = indent;
						// The inner stream: this producer's inputs, then its own statement.
						std::ostringstream inner;
						// The producer's def.
						const NodeDef *pd = registry.Find(producer.TypeId);
						// Continuation of the def guard.
						if (pd != nullptr)
						{
							// The producer's own INPUT pins -- the recursive case that makes
							// `add(get(i), 1)` work. Emitting only one level deep left the outer
							// expression referencing a temp nothing had assigned.
							for (const PinDef &pp : pd->Pins)
							{
								if (pp.Dir != PinDir::In || pp.Type == PinType::Exec)
									continue;
								PinLiteral pl;
								// Continuation of the three declarations.
								bool plLinked = false;
								// Continuation of the declaration list.
								std::string plNode;
								// Continuation of the ResolveInput call.
								std::string plPin;
								graph.ResolveInput(producer.Id, pp.Id, pl, plLinked,
														   // The upstream node, then the recursive emission on the following line.
														   plNode, plPin);
								if (!plLinked)
									continue;
								// An upstream link with a missing node terminates the recursion rather
								// than raising.
								const GraphNode *up = graph.FindNode(plNode);
								// A missing upstream node terminates the recursion rather than raising.
								if (up != nullptr)
									// Closes the pin loop.
									inner << emitDataNode(*up, indent);
							}
						}
						// The producer's own statement, emitted AFTER its inputs. That order is
						// what makes the generated assignments read top-to-bottom in evaluation
						// order.
						inner << emitOwnStatement(producer, indent);
						// Continuation of the return.
						return inner.str();
					};

					// Continuation of the block-append line.
					block << emitDataNode(*producer, indent);
				}
				return block.str();
			};

			// Emit the chain of statements starting at `node`, following exec
			// links until the graph runs out. A control-flow node (Branch,
			// While, ForEach) has its emitter RETURN a block containing
			// `pass` placeholders for its own branches, which are then spliced
			// in here -- so the node owns its body and this owns the walking.
			//
			// Splicing by placeholder rather than by letting the node emitter
			// call back into the compiler keeps node authors from needing a
			// compiler reference, which is the whole point of the registry
			// being a table of self-contained functions.
			// emitChain: walk exec links from `node` and emit each node's statement.
			// anyOut is an out-param reporting whether anything was emitted, so the
			// caller can decide whether the body needs a `pass`.
			// ---
			// Returns the block as a STRING. Both the method-level chain and each
			// branch body are produced this way and spliced as text, which is what
			// lets a node's shell be written before its bodies are known.
			std::function<std::string(const GraphNode *, int, bool &)> emitChain =
				// Continuation of emitChain's signature, including the anyOut out-param.
				[&](const GraphNode *node, int indent, bool &anyOut) -> std::string
			{
				// Per-call stream. The cursor advances along the chain; each iteration
				// emits one node.
				std::ostringstream block;
				// The chain cursor, null once the walk runs off the end.
				const GraphNode *cursor = node;
				// The loop terminates on a null cursor (no successors) or on the emitted
				// guard below (already seen / cycle).
				while (cursor != nullptr)
				{
					// The cycle AND diamond guard. Breaking rather than continuing is what
					// makes a cyclic graph terminate; see the note on the emitted set above for
					// what it costs on a diamond.
					if (emitted[cursor->Id])
						break;
					// Closes the guard.
					emitted[cursor->Id] = true;

					// An unregistered type ends the chain silently. The nodes after it are
					// never reached, and no error is reported.
					const NodeDef *def = registry.Find(cursor->TypeId);
					if (def == nullptr)
						break;

					// A control-flow node's own text carries branch markers;
					// resolve each marker to the chain hanging off that pin.
					// Tested BEFORE the plain path so the shell is emitted
					// exactly once -- emitting it here and again below would
					// duplicate the whole `if` in the generated source.
					// A control-flow node is one that is NOT a BT node, IS a statement node,
					// and owns branch pins. The BtRole check is what separates the two
					// compilers: a node with a BT role belongs to the tree path and never gets
					// marker resolution here.
					// ---
					// The HasBranchPins half is structural, so a newly registered
					// Branch-shaped node is handled correctly with no change to this file.
					const bool isControlFlow = def->BtRole.empty() &&
											  // Continuation of the isControlFlow expression.
											  def->IsStatement &&
											  // Closes the three-part condition.
											  HasBranchPins(*def);

					// The control-flow branch, entered only when the node owns branches.
					if (isControlFlow)
					{
					EmitContext ctx;
					ctx.Graph = &graph;
					// The node pointer.
					ctx.Node = cursor;
					ctx.Indent = indent;
					// The shell's data inputs, resolved into the context above.
					buildInputs(*cursor, *def, ctx);
					EmitOutputs outputs;
					// The SHELL: a head like `if <cond>:` with markers where the bodies
					// belong. Resolved on the next few lines, then written as-is.
					std::string shell = def->Emit(ctx, outputs);

					// Any data temps this node declares are emitted
					// before the shell, at this scope.
					// Temps this control-flow node declares are emitted BEFORE the shell, at
					// this scope, so the shell's condition can read them.
					// ---
					// The same Exec-skip as everywhere else; the block/indent/assign pattern
					// is the third copy of it in this function.
					for (const auto &pair : outputs.Outputs)
					{
						if (pair.first == "Exec")
							continue;
						// Continuation of the temp name.
						block << std::string((size_t)indent, '\t')
							   // Continuation of the assignment.
							   << TempAttr(cursor->Id, pair.first)
							   // End of the assignment, with its newline.
							   << " = " << pair.second << "\n";
					}

					// A LOOP's condition is re-evaluated every iteration, so
					// the data feeding it must be computed INSIDE the loop.
					// Emitted before the `while`, it is computed once and the
					// loop then spins on the value from before the first
					// iteration -- so `while i < 5` either never runs or never
					// terminates, while the graph looks correct in the
					// editor. Rewriting the test to `while True` and putting
					// `if not <test>: break` at the top of the body is the
					// standard Python shape, and it recomputes the inputs on
					// every pass.
					//
					// Detected from the SHELL TEXT, not from the node's name:
					// a node whose own emitter opens a `while` is a loop by
					// construction, so a newly registered loop-shaped node
					// needs no compiler change.
					// ---
					// A LOOP's condition is re-evaluated every iteration, so the data feeding
					// it must be computed INSIDE the loop.
					// ---
					// Emitted before the `while`, it is computed once and the loop then spins
					// on the value from before the first iteration -- so `while i < 5` either
					// never runs or never terminates, while the graph looks correct in the
					// editor. Rewriting the test to `while True` and putting `if not <test>:
					// break` at the top of the body is the standard Python shape, and it
					// recomputes the inputs on every pass.
					// ---
					// A BRANCH's condition, by contrast, is evaluated once -- see the separate
					// handling a few lines below.
					// The loop prologue: the temps plus the inverted test, spliced into the
					// loop body rather than emitted before it.
					// ---
					// Empty for a non-loop, and that emptiness is what the branch guard below
					// tests to tell a loop from a branch.
					std::string loopPrologue;
					{
						// find_first_not_of skips spaces and tabs but NOT a newline, so a shell
						// starting with a blank line would not be recognised as a loop. Emitters
						// evidently do not emit leading blank lines; nothing enforces it.
						const size_t head = shell.find_first_not_of(" \t");
						// Continuation of the while-detection test.
						if (head != std::string::npos &&
							// DETECTED FROM THE SHELL TEXT, not from a node-type list: a node whose own
							// emitter opens a `while` is a loop by construction, so a newly registered
							// loop-shaped node needs no compiler change. compare() with a length
							// beyond the string clamps, so a short shell safely fails the test.
							shell.compare(head, 6, "while ") == 0)
						{
							// DEFECT (documented, not fixed): rfind() takes the LAST colon in the
							// shell, which is only correct while the body is still an unsubstituted
							// marker -- markers contain no colon. It is also only correct if the TEST
							// contains no colon: a condition that is a slice (`a[1:2]`) or a dict
							// display would make rfind return that colon, truncating the extracted test
							// and emitting everything after it as the loop body. Inference: no current
							// node emitter produces such a condition, but nothing prevents one.
							const size_t colon = shell.rfind(':');
							// Continuation of the colon-position guard.
							if (colon != std::string::npos && colon > head + 6)
							{
								// The test text, sliced between the `while ` and the colon. It is later
								// emitted VERBATIM inside `if not (...)`, so a test with an unbalanced
								// bracket is caught by Python rather than by the compiler.
								const std::string test =
									// Continuation of the test slice.
									shell.substr(head + 6, colon - (head + 6));
								// The temps go in at indent+1 -- one level INSIDE the loop -- which is the
								// entire point: they must re-evaluate every iteration.
								loopPrologue = emitDataInputs(*cursor, indent + 1);
								// The inverted guard that replaces the while's own test.
								loopPrologue += std::string((size_t)indent + 1, '\t')
											   // Parenthesised so a compound test such as `a and b` stays a single
											   // operand of the negation.
											   + "if not (" + test + "): break\n";
								// Replace ONLY the `while <test>:` prefix and keep
								// the tail, which is where the body marker is.
								// Assigning a bare "while True:" dropped the
								// marker, left the loop with no body, and produced
								// "expected an indented block after 'while'".
								// Replace ONLY the `while <test>:` prefix and keep the tail, which is where
								// the body marker lives.
								// ---
								// Assigning a bare "while True:" dropped the marker, left the loop with no
								// body, and produced "expected an indented block after 'while'". Keeping
								// the tail is what makes the rewrite invisible to the marker resolver.
								shell = "while True:" + shell.substr(colon + 1);
							}
						}
					}

					// A BRANCH's condition is evaluated once, so its data
					// belongs immediately before the `if`, at the same indent.
					// A BRANCH, not a loop: its condition runs once, so its data temps go
					// immediately BEFORE the `if`, at the same indent.
					// ---
					// The loopPrologue emptiness test is the discriminator. A Branch emits its
					// guard here; a While carries its data inside the body instead.
					if (loopPrologue.empty())
					{
						// The branch's data temps, at THIS indent rather than inside a body.
						const std::string cfData = emitDataInputs(*cursor, indent);
						// Continuation of the emptiness test.
						if (!cfData.empty())
						{
							// Emitted into the block, ahead of the shell.
							// ---
							// anyOut and statementCount are both bumped here even though the temps are
							// not the branch's own text -- the count is meant to answer 'did this node
							// produce anything at all'.
							block << cfData;
							anyOut = true;
							statementCount++;
						}
					}

					// Marker resolution: each marker in the shell becomes the chain hanging
					// off that exec pin. This is the single point where a node's body is known.
					// ---
					// indent+1 because bodies sit one level in from the shell.
					shell = ResolveBranchMarkers(
						// Continuation of the ResolveBranchMarkers call.
						shell, cursor, indent + 1,
						// Continuation of the body lambda's signature.
						[&](const std::string &pin, int)
						{
							// Relative indentation: the shell writer below
							// adds the base indent to every line.
							// branchAny tracks whether THIS branch produced anything, separately from
							// the enclosing anyOut, so an empty branch can be given a `pass` without
							// masking an empty method.
							bool branchAny = false;
							// The per-branch stream, separate from the enclosing block.
							std::ostringstream body;
							// The loop prologue is injected only into the pin literally named "Body".
							// A While with a second exec out pin (a break target, say) gets no
							// prologue there.
							// ---
							// The pin name is a string comparison against the node's convention, so a
							// loop whose body pin is named differently would silently not receive its
							// guard -- the loop would then never re-evaluate its condition.
							if (!loopPrologue.empty() && pin == "Body")
							{
								// Strip one level: emitDataInputs emits
								// absolute indentation for the plain path,
								// but this body is spliced into a shell that
								// the writer below indents AGAIN. Leaving
								// the absolute tabs in produced a body at
								// 2*(indent+1) beside a sibling at indent+1,
								// which Python rejects as an unindent error.
								// Strip exactly one level of absolute indentation from each prologue line.
								// ---
								// emitDataInputs emits ABSOLUTE indentation for the plain path, but this
								// body is spliced into a shell that the writer below indents AGAIN.
								// Leaving the absolute tabs in produced a body at 2*(indent+1) beside a
								// sibling at indent+1, which Python rejects as an unindent error.
								const std::string strip = std::string((size_t)indent, '\t');
								// A local copy of one indent level's worth of tabs.
								std::string rel;
								// The prologue re-parsed for the strip.
								std::istringstream prologueLines(loopPrologue);
								// One prologue line at a time.
								std::string pline;
								while (std::getline(prologueLines, pline))
								{
									// compare() returns 0 only on an exact match, so a line shorter than the
									// prefix is left alone rather than sliced.
									if (pline.compare(0, strip.size(), strip) == 0)
										// The prefix test -- compare returns non-zero for a short line, leaving it
										// untouched rather than slicing it wrongly.
										pline = pline.substr(strip.size());
									// Continuation of the stripped reassignment.
									rel += pline + "\n";
								}
								// The stripped prologue written into the branch body.
								body << rel;
								branchAny = true;
							}
							// The successors of this specific exec pin -- the branch's contents.
							// ---
							// successorsOf is called with a NON-empty pin id here, unlike the plain
							// node path below which passes an empty string.
							std::vector<const GraphNode *> kids =
								// Continuation of the successorsOf call.
								successorsOf(*cursor, pin);
							// Index loop over the branch's successors.
							for (size_t i = 0; i < kids.size(); ++i)
							{
								// indent 1, RELATIVE. The writer below adds the base indent to every line
								// of the finished shell, so a body emitted at absolute indent would be
								// double-indented -- the same trap the prologue strip above avoids.
								body << emitChain(kids[i], 1, branchAny);
							}
							// An empty branch still needs a body in Python.
							if (!branchAny)
								// `pass` at ONE relative indent, so after the shell writer adds the base
								// it sits correctly inside the branch.
								body << "\tpass\n";
							// Closes the body lambda.
							return body.str();
						});

						// The resolved shell is written out line by line, exactly as the plain
						// path writes a node's own text. One writer for both shapes is what keeps
						// the two branches of an `if` at the same column.
						std::istringstream shellLines(shell);
						std::string line;
						// One shell line at a time.
						while (std::getline(shellLines, line))
						{
							// Blank lines dropped, matching the plain path -- otherwise a shell with an
							// internal blank line would produce a gap inside a Python block.
							if (line.empty())
								continue;
							block << std::string((size_t)indent, '	') << line << "\n";
						}
						// anyOut set only when the shell is non-empty, so a control-flow node that
						// emitted nothing does not suppress the method-level `pass`.
						if (!shell.empty())
						{
							anyOut = true;
							statementCount++;
						}
						// Control flow owns its successors; the chain does not
						// continue past it in the parent scope.
						// Control flow owns its successors: the chain does NOT continue past this
						// node in the parent scope. Its successors were emitted inside the branch
						// bodies above.
						// ---
						// This early return is what makes a Branch a statement rather than a
						// transparent link in the chain.
						return block.str();
					}

					// Plain node: emit its own statement/temps, then
					// continue down its exec out.
					// Its data inputs first, at this node's scope: a value read
					// by this statement must be computed before it runs, and
					// inside this branch if the node is inside one.
					// A PLAIN node's data inputs, at this node's own indent -- inside the
					// branch if the node is inside one, before the statement that reads them.
					std::string dataBlock = emitDataInputs(*cursor, indent);
					// Continuation of the dataBlock emptiness test.
					if (!dataBlock.empty())
					{
						// Non-empty rather than always emitted, so a node with no data inputs does
						// not bump the statement count.
						block << dataBlock;
						anyOut = true;
						statementCount++;
					}

					// The node's own statement and temps.
					std::string own = emitOwnStatement(*cursor, indent);
					// Continuation of the own-statement test.
					if (!own.empty())
					{
						block << own;
						anyOut = true;
						statementCount++;
					}

					// Follow the single exec out. successorsOf with an empty pin id returns
					// every exec successor, which for a plain node is exactly one.
					std::vector<const GraphNode *> kids = successorsOf(*cursor, std::string());
					// Ends the chain when there is no successor; otherwise continues at the
					// first one.
					// ---
					// DEFECT (documented, not fixed): `front()` silently DISCARDS any second
					// successor. A plain node with two exec outs is a fork with no ordering
					// semantics, and the validator is supposed to report it -- but if it slips
					// through, the second branch is dropped from the generated file with no
					// diagnostic here.
					cursor = kids.empty() ? nullptr : kids.front();
					// DEFECT (documented, not fixed): this entire block is DEAD CODE. The line
					// above already assigned cursor = kids.front() whenever kids is non-empty,
					// so this reassignment is always the same value. The comment explains the
					// intent (take the first of an invalid fork), but the code that implements
					// it is unreachable -- the real truncation happens silently on the
					// assignment above. Harmless, and actively misleading to the next reader.
					if (kids.size() > 1)
					{
						// Two successors off a PLAIN node is a fork with no
						// ordering semantics. The validator reports it; here
						// we take the first so the output stays well-formed.
						cursor = kids.front();
					}
				}
				return block.str();
			};

			// The method-level call. indent 2 -- a class body at 1, a method body at 2
			// -- matching the def emitted above.
			bool anyOut = false;
			// `event` may be null; emitChain's first statement dereferences it, so
			// this call is only reached inside the else branch where event is known
			// non-null.
			std::string chain = emitChain(event, 2, anyOut);
			// The chain appended to the output stream.
			out << chain;
			// An event that produced no statements still needs a valid body, or the
			// generated file will not parse.
			if (!anyOut)
				out << "		pass\n";
		}

		out << "\n";
		// A flow-generated class's tick is ALWAYS a no-op.
		// ---
		// DEFECT (documented, not fixed): unlike the tree class, whose tick
		// delegates to wl_behavior.Tree, this emits `pass`. A host that drives a
		// flow component through tick -- the entry point the tree path documents as
		// the public one -- gets silence, with no error to say the graph was
		// compiled into the wrong method. The real work is in the event-named
		// method above.
		out << "\tdef tick(self, seconds=0.0):\n";
		// The tick body: `pass`. See the defect note on this line's def.
		out << "\t\tpass\n";

		// Clears any error before the success return, as the tree path does.
		error = GraphError();
		return out.str();
	}

	// ----------------------------------------------------------------- file

	// Load, validate, compile -- the path Compiler.h says the editor actually
	// takes.
	bool Compiler::CompileBehaviorFile(const std::string &path, std::string &outPython,
									   // Continuation of the parameter list.
									   ValidationResult &outValidation,
									   // Continuation of the parameters.
									   GraphError &error)
	{
		// Loaded into a local rather than the caller's graph: this function owns
		// the whole pipeline and exposes only the emitted source.
		Graph graph;
		// A load failure short-circuits before validation, so a malformed file
		// reports a parse error rather than a cascade of validation findings.
		if (!GraphSerializer::Load(path, graph, error))
			return false;

		// The out-param is RESET before the tree validation writes into it, so a
		// caller reusing the struct does not see stale findings merged with new
		// ones.
		outValidation = ValidationResult();
		// ValidateTree -- and then CompileBehavior validates the SAME graph again
		// internally.
		// ---
		// DEFECT (minor, documented, not fixed): the graph is validated twice per
		// compile, walking every node and link both times. Harmless for authored
		// graph sizes; it is pure duplicated work on the editor's compile path.
		if (!Validator::ValidateTree(graph, outValidation))
		{
			// Report the FIRST error as the headline; the full list is in
			// outValidation for the caller to print.
			// First error becomes the headline; `break` rather than `return` because
			// the remaining findings stay in outValidation for the caller to print.
			for (const GraphError &finding : outValidation.Findings)
			{
				if (finding.IsError)
				{
					error = finding;
					break;
				}
			}
			return false;
		}

		// CompileBehavior re-validates, so this call is where the second
		// validation pass above happens. Its `error` out-param overwrites the one
		// set above -- harmless, because reaching here means validation passed.
		outPython = CompileBehavior(graph, error);
		// Success is 'the source is non-empty'. An empty return is the compiler's
		// single failure signal, so this one line covers compile errors and empty
		// graphs alike.
		return !outPython.empty();
	}

}
