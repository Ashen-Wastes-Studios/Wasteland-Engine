#include "wlpch.h"

#include "NodeRegistry.h"
#include "Validator.h"   // IsAllowedOperator

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Wasteland::VisualBlocks
{

	// ================================================================
	// NB.4 -- the v1 behaviour taxonomy.
	//
	// One registry entry per node, named as the plan's taxonomy names them
	// (IsDead, IsLowHealth, SenseNearest, ...) rather than the earlier
	// Condition*/Attack prefix. Those older ids stay registered: a saved tree
	// references them by string, and renaming a node type breaks every graph
	// already on disk.
	//
	// Every body here reaches engine state through `ctx.entity` -- the AGENT,
	// not the behavior class. See the shim note in Compiler.cpp: inside a
	// `_build()` lambda, `self` is the class, so a `self.health()` silently
	// raises AttributeError on the first tick.
	//
	// Latent is a property of the DEF below, never inferred from what a body
	// happens to return. An accidental latent task stops forever; a task that
	// cannot return RUNNING cannot express "walk there over several ticks".
	// ================================================================

	namespace
	{
		std::string StrPinV1(const EmitContext &ctx, const char *pin,
							 const char *fallback = "")
		{
			std::string value = ctx.In(pin);
			if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
				value = value.substr(1, value.size() - 2);
			return value.empty() ? std::string(fallback) : value;
		}

		// Emit a generated-leaf entry: {'t': 'Leaf', 'leaf': <expr>}.
		//
		// 'latent': True is included when the NODE is marked latent, and the
		// Compiler rejects a latent node whose emitted body lacks that flag --
		// so a leaf cannot claim to suspend while emitting a body that decides
		// immediately.
		void EmitLeaf(EmitOutputs &out, const EmitContext &ctx, const std::string &body)
		{
			std::string entry = "{'t': 'Leaf', 'leaf': " + body;
			if (ctx.Node->Latent)
				entry += ", 'latent': True";
			entry += "}";
			out.Outputs.push_back({ "Value", entry });
		}
	}

	// Register the v1 taxonomy. Called from RegisterAllNodes().
	void RegisterBehaviourV1Nodes()
	{
		// Mutable(), not Get(): Get() lazily calls RegisterAllNodes() while
		// the table is still empty, and RegisterAllNodes() calls this
		// function. Asking Get() here re-entered that lazy-init every time
		// through, so importing Wasteland blew the stack during module init
		// -- a silent exit-127 crash with no traceback. NodeLibrary.cpp made
		// the same fix; this call site was missed.
		NodeRegistry &registry = NodeRegistry::Mutable();

		// ---------------------------------------------------------------
		// CONDITIONS -- leaves, no side effects, decide in one tick.
		// ---------------------------------------------------------------

		{
			// IsDead -- reads HealthComponent.IsDead on the AGENT.
			// Missing component means "not dead": an entity without health is
			// scenery, and treating it as dead would make a tree succeed on
			// the first branch for a target that simply has no health.
			NodeDef def;
			def.TypeId = "IsDead";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = { MakeOut("Value", "Tree", PinType::Bool) };
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				EmitLeaf(out, ctx,
						 "lambda ctx: SUCCESS if _is_dead(ctx.entity) else FAILURE");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// IsLowHealth -- fraction below threshold, as a fraction not a raw
			// number, so one threshold reads correctly at any max health.
			NodeDef def;
			def.TypeId = "IsLowHealth";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = {
				MakeIn("Fraction", "Fraction", PinType::Float, true, "0.25"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				EmitLeaf(out, ctx,
						 "lambda ctx: SUCCESS if _health_fraction(ctx.entity) <= " +
							 ctx.InFloat("Fraction") + " else FAILURE");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// HasLineOfSight -- Scene.Raycast from the agent's eye toward the
			// target, honouring RaycastIgnoreComponent so friendly bodies do
			// not block every sighting.
			NodeDef def;
			def.TypeId = "HasLineOfSight";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = {
				MakeIn("Target", "Target", PinType::String, true, "target"),
				MakeIn("MaxDistance", "MaxDist", PinType::Float, true, "30.0"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string target = StrPinV1(ctx, "Target", "target");
				EmitLeaf(out, ctx,
						 // The first argument is the runtime Context, not the entity.
						 // _has_los grew that parameter when NB.7 replaced its distance
						 // heuristic with a real Scene.Raycast trace, and it needs ctx
						 // to reach the scene. Passing ctx.entity here type-checks
						 // fine and fails at the first tick with "_has_los called
						 // with 3 args (takes 4)".
							"lambda ctx: SUCCESS if _has_los(ctx, ctx.entity, ctx.bb.get(\"" +
								target + "\"), " + ctx.InFloat("MaxDistance") +
							") else FAILURE");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// BlackboardIsSet -- reads VALIDITY, not truthiness.
			//
			// This distinction is the whole reason the runtime tracks validity
			// separately: `attackAt = 0.0` is a written value, and testing it
			// as a bool says "unset" when it means "due now".
			NodeDef def;
			def.TypeId = "BlackboardIsSet";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = {
				MakeIn("Key", "Key", PinType::String, true, "target"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string key = StrPinV1(ctx, "Key", "target");
				EmitLeaf(out, ctx,
						 "lambda ctx: SUCCESS if ctx.bb.is_set(\"" + key +
							 "\") else FAILURE");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// Compare -- two float pins and an operator.
			//
			// The operator is checked against the allowlist before it is
			// interpolated: this text lands verbatim in generated Python, so
			// an unvalidated pin is a code-injection vector, not a typo.
			NodeDef def;
			def.TypeId = "Compare";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = {
				MakeIn("A", "A", PinType::Float, true, "0.0"),
				MakeIn("B", "B", PinType::Float, true, "0.0"),
				MakeIn("Op", "Op", PinType::String, true, "<="),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string op = StrPinV1(ctx, "Op", "<=");
				if (!IsAllowedOperator(op))
					op = "<=";
				EmitLeaf(out, ctx,
						 "lambda ctx: SUCCESS if (" + ctx.InFloat("A") + " " + op +
							 " " + ctx.InFloat("B") + ") else FAILURE");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// Gate -- deterministic modulo sampling.
			//
			// Useful for spreading per-frame work (a guard re-checking its
			// surroundings) across frames WITHOUT a TickEvery decorator and
			// a counter. Deterministic on purpose: a random gate makes a tree
			// impossible to reproduce in a test.
			NodeDef def;
			def.TypeId = "Gate";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.Pins = {
				MakeIn("Every", "Every", PinType::Int, true, "4"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				EmitLeaf(out, ctx,
						 "lambda ctx: SUCCESS if _gate(ctx, \"" +
						 EscapePy(ctx.Node->Id) + "\", " + ctx.InInt("Every") +
							") else FAILURE");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// DistanceTo -- the taxonomy spelling. ConditionDistanceTo was
			// registered first and stays: saved trees reference nodes by
			// string, so renaming breaks every graph already on disk. An alias
			// costs one entry and keeps both spellings loadable.
			NodeDef def;
			def.TypeId = "DistanceTo";
			def.Category = "Behaviour/Condition";
			def.BtRole = "condition";
			def.AliasOf = "ConditionDistanceTo";
			registry.Register(def);
		}


		// ---------------------------------------------------------------
		// TASKS -- leaves that do things.
		// ---------------------------------------------------------------

		{
			NodeDef def;
			def.TypeId = "FaceTarget";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				MakeIn("Target", "Target", PinType::String, true, "target"),
				MakeIn("TurnRate", "Turn", PinType::Float, true, "540.0"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string target = StrPinV1(ctx, "Target", "target");
				// Not latent: facing resolves in one tick by rotating toward
				// the bearing at a capped rate. Declaring it latent would let
				// a tree sit on this node forever while it turns.
				EmitLeaf(out, ctx,
						 "lambda ctx: _face(ctx.entity, ctx, ctx.bb.get(\"" + target +
							 "\"), " + ctx.InFloat("TurnRate") + ")");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// ApplyDamage -- damage the target held in the key.
			//
			// NOT latent and NOT internally rate-limited: the tree's
			// Cooldown + SetTimer pair is what limits the rate. Hiding a
			// cooldown inside the node would give two different answers for
			// "how often does this hit" and make the tree's own declarative
			// timing a lie.
			NodeDef def;
			def.TypeId = "ApplyDamage";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				MakeIn("Target", "Target", PinType::String, true, "target"),
				MakeIn("Damage", "Damage", PinType::Float, true, "12.0"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string target = StrPinV1(ctx, "Target", "target");
				EmitLeaf(out, ctx,
						 "lambda ctx: _damage(ctx.entity, ctx.bb.get(\"" + target +
							 "\"), " + ctx.InFloat("Damage") + ")");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// SenseNearest -- nearest LIVE entity within radius, into a key.
			//
			// Skips the agent itself (otherwise the NPC always senses itself
			// and walks to where it already stands) and skips anything without
			// a HealthComponent or already dead.
			//
			// Writes SUCCESS even when it finds nothing, having CLEARED the
			// key. Failing instead would leave a stale target in the key, and
			// the next branch would happily attack it -- an NPC shooting a
			// corpse because the key was never invalidated.
			NodeDef def;
			def.TypeId = "SenseNearest";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				MakeIn("Key", "Key", PinType::String, true, "target"),
				MakeIn("Radius", "Radius", PinType::Float, true, "20.0"),
				// NB.7: sight cone and occlusion, both OFF by default.
				//
				// HalfAngle 0 means "no cone" rather than "a zero-width cone".
				// 0 is the value an author leaves on a pin they do not care
				// about, and it must not silently make an NPC blind. A
				// negative angle also means "any", so -1 cannot be an
				// accidental "see nothing".
				MakeIn("HalfAngle", "Half Angle (deg, 0 = all)", PinType::Float, false, "0"),
				// LOS off by default: it is a BEHAVIOUR CHANGE, not an
				// optimisation. Every authored tree keeps acquiring targets
				// through walls exactly as before, and visibility becomes a
				// per-node decision rather than changing under every NPC at
				// once.
				MakeIn("RequireLOS", "Require Line Of Sight", PinType::Bool, false, "False"),
				MakeOut("Found", "Found", PinType::Entity),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string key = StrPinV1(ctx, "Key", "target");
				// HalfAngle <= 0 -> None, i.e. omnidirectional, which is the
				// pre-NB.7 behaviour. Compared as a string first and parsed
				// with strtod, never std::stod: an unparseable pin literal
				// makes stod throw out of a graph compile, and a graph compile
				// is the wrong place to throw.
				std::string half = ctx.InFloat("HalfAngle");
				std::string cone = "None";
				if (!half.empty())
				{
					const char *begin = half.c_str();
					char *end = nullptr;
					double degrees = std::strtod(begin, &end);
					// Only accept a clean parse of the whole literal.
					if (end != begin && *end == '\0' && degrees > 0.0)
						cone = half;
				}
				EmitLeaf(out, ctx,
						 "lambda ctx: _sense_nearest(ctx.entity, ctx, \"" + key +
						 "\", " + ctx.InFloat("Radius") + ", " + cone + ", " +
						 (ctx.InBool("RequireLOS") == "True" ? "True" : "False") + ")");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// MoveToTransform -- walk to an explicit POSITION, not an entity.
			//
			// Flee and return-to-home need a goal with no target attached; the
			// plan's second tree (the patrolling guard) is the reason this
			// exists rather than overloading MoveTo.
			NodeDef def;
			def.TypeId = "MoveToTransform";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				MakeIn("Position", "Position", PinType::Vec3, true, "0,0,0"),
				MakeIn("Speed", "Speed", PinType::Float, true, "3.0"),
				MakeIn("Arrival", "Arrive", PinType::Float, true, "0.5"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				EmitLeaf(out, ctx,
						 "lambda ctx: _move_to_point(ctx.entity, ctx, " +
							 ctx.InVec3("Position") + ", " + ctx.InFloat("Speed") +
							 ", " + ctx.InFloat("Arrival") + ")");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// PlayAnimation -- drive the animation component by name.
			//
			// Uses SetAnimation (not an Animator handle) so the node does not
			// require an animation instance to have been authored first: an
			// NPC with no animator should log and continue, not crash the
			// tick.
			NodeDef def;
			def.TypeId = "PlayAnimation";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				MakeIn("Animation", "Animation", PinType::String, true, "idle"),
				MakeIn("Loop", "Loop", PinType::Bool, true, "true"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string anim = StrPinV1(ctx, "Animation", "idle");
				EmitLeaf(out, ctx,
						 "lambda ctx: _play_animation(ctx.entity, \"" + anim +
							 "\", " + ctx.InBool("Loop") + ")");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// CallPython -- the hot-loop escape hatch.
			//
			// Calls a method on the agent's ScriptComponent. If there is no
			// script, or no such method, it FAILS rather than raising: the
			// script host swallows errors and continues, so an unbound name
			// here would leave the node silently inert.
			NodeDef def;
			def.TypeId = "CallPython";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				MakeIn("Method", "Method", PinType::String, true, "OnBehaviorStep"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				std::string method = StrPinV1(ctx, "Method", "OnBehaviorStep");
				EmitLeaf(out, ctx,
						 "lambda ctx: _call_python(ctx.entity, \"" + method +
							 "\", ctx)");
				return std::string();
			};
			registry.Register(def);
		}

		{
			// SetTimer -- write `now + Seconds` into a named blackboard key.
			//
			// NB.9. This node was in BehaviorTree::RuntimeTypes() from the start
			// and wl_behavior.py has had a handler for it (`_eval_set_timer`,
			// reached from the `elif ntype == "SetTimer"` arm), but NO NodeDef
			// ever registered it. The failure was the worst kind for a designer:
			// the compiler accepted the graph, the runtime had the branch, and
			// LOADING died with "unknown node type 'SetTimer'". A node added to
			// one of the two tables and not the other is invisible until a tree
			// names it -- which is why tests/headless/test_runtime_types.py now
			// cross-checks RuntimeTypes() against the registry in BOTH
			// directions. Registering the node is half the fix; the cross-check
			// is what stops the next one.
			//
			// A TASK, not a decorator, despite wl_behavior.py listing SetTimer
			// among its DECORATORS. Those two lists answer different questions:
			// DECORATORS there is "types exempt from the leaf-must-carry-a-body
			// check", and SetTimer is exempt precisely because it carries NO
			// `leaf` key -- the runtime implements it directly. BtRole is what
			// the VALIDATOR reads, and it demands exactly one child of a
			// decorator (Validator.cpp:628). SetTimer wraps nothing, so BtRole
			// "decorator" would make every legal use of this node a validation
			// error. Wait -- the other runtime leaf, and the same shape -- is
			// filed BtRole "task" for the same reason.
			NodeDef def;
			def.TypeId = "SetTimer";
			def.Category = "Behaviour/Task";
			def.BtRole = "task";
			def.Pins = {
				// The blackboard key to write. Defaults to "attackAt", the name
				// the Cooldown comment above and wl_behavior.py's own header
				// both use for a rate-limit deadline.
				MakeIn("Key", "Key", PinType::String, true, "attackAt"),
				// The same Seconds pin as Cooldown, Timeout and Wait, reused
				// verbatim -- a fourth time, which is the point: one spelling
				// for "how long", so a designer reading four time nodes is not
				// reading four different units.
				MakeIn("Seconds", "Seconds", PinType::Float, true, "1.0"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				// StrPinV1 strips the quotes ParseString puts on a String
				// literal; EscapePy puts them back AND escapes the contents,
				// because EmitTreeNode's `extra` is spliced into the generated
				// Python VERBATIM (see its note in NodeRegistry.h). An authored
				// key containing a quote would otherwise terminate the literal
				// and inject code into the generated file.
				std::string key = StrPinV1(ctx, "Key", "attackAt");
				// InFloat rather than In: it supplies the "0.0" fallback, so an
				// unset pin emits valid Python instead of nothing at all.
				return EmitTreeNode(ctx, out, "SetTimer",
						"'pin': \"" + EscapePy(key) + "\", 'seconds': " +
							ctx.InFloat("Seconds"));
			};
			registry.Register(def);
		}

		// ---------------------------------------------------------------
		// DECORATORS the taxonomy adds beyond the v1 set already present.
		// ---------------------------------------------------------------

		{
			// TickEvery -- the LOD node. Skips the subtree unless enough
			// seconds have passed.
			NodeDef def;
			def.TypeId = "TickEvery";
			def.Category = "Behaviour/Decorator";
			def.BtRole = "decorator";
			def.Pins = {
				MakeIn("Seconds", "Seconds", PinType::Float, true, "0.25"),
				MakeOut("Value", "Tree", PinType::Bool),
			};
			def.Emit = [](const EmitContext &ctx, EmitOutputs &out) {
				return EmitTreeNode(ctx, out, "TickEvery",
									"'seconds': " + ctx.InFloat("Seconds"));
			};
			registry.Register(def);
		}
	}

}
