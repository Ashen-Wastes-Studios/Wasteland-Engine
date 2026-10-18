// Precompiled header first, by engine convention.
#include "wlpch.h"
// This panel class's own declaration.
#include "ScriptInspectorPanel.h"

// ImGui. Included by project path rather than as a bare <imgui.h>, matching
// how ImGuiLayer.cpp refers to it.
#include <imgui/imgui.h>

// The C++/Python bridge. This is the file that reaches into a live Python
// object and reflects over its attributes.
#include <pybind11/pybind11.h>
// STL casters. This is what makes value.cast<py::list>() and
// value.cast<std::string>() work: without these, pybind11 cannot convert
// between Python lists/strings and their C++ counterparts automatically.
#include <pybind11/stl.h>

// ScriptEngine::GetInstance -- how a scene entity's ID maps to its live
// py::object. The only engine dependency beyond the scene types.
#include "Wasteland/Scripting/ScriptEngine.h"
// ScriptComponent (the script attached to the entity) and IDComponent (the
// UUID used to look the instance up).
#include "Wasteland/Scene/Components.h"

// glm, because the type switch below handles glm::vec2/3/4 specially and
// pybind11 must already know those types for isinstance<glm::vec2> to work.
#include <glm/glm.hpp>

// The conventional pybind11 namespace alias, shortening every use below.
namespace py = pybind11;

namespace Wasteland
{

	// Decides whether a Python instance attribute is worth showing.

	// Static and file-local rather than a member: it holds no state.
	static bool IsPublicAttribute(const std::string &name)
	{
		// Leading underscore means private by Python convention, so internals stay
		// hidden.

		// Note this is a CONVENTION check, not an access check -- a name starting
		// with an underscore is still readable from C++, and one without an
		// underscore is not automatically public. It is the right heuristic for an
		// inspector, though.

		// name[0] is safe here only because the empty case short-circuits on the
		// left of the ||.
		if (name.empty() || name[0] == '_')
			return false;
		// The engine's injected back-reference to the owning entity.

		// Hidden because it is an object, not a value: the loop below can only
		// render it through the read-only repr fallback, which would show a useless
		// handle dump.
		if (name == "entity")
			return false;
		return true;
	}

	// The Layer hook: draws the panel every frame while it is visible.

	// The whole body is one long reflection loop wrapped in a try/catch, because
	// any pybind11 call can throw and a panel that propagates a C++ exception
	// out of a render callback would take down the frame.
	void ScriptInspectorPanel::OnImGuiRender()
	{
		// Hidden panels do no work at all -- not even ImGui::Begin/End -- so a
		// closed inspector costs one branch per frame.

		// The flag is passed by address to ImGui::Begin below, so ImGui itself can
		// clear it when the user clicks the panel's close box.
		if (!m_Visible)
			return;

		// Opens the window. Returns bool indicating whether its contents are
		// visible (collapsed, clipped, or hidden), and that value is discarded here.

		// Not discarding it means the panel still builds its widget list while
		// scrolled out of view -- a real cost with this many per-attribute ImGui
		// calls, and the reason the usual Begin/End-skip pattern exists.
		ImGui::Begin("Script Inspector", &m_Visible);

		// No entity selected. Entity::IsValid() checks the handle is not entt::null
		// AND that the owning scene is non-null AND that the handle is still live in
		// the registry -- so this also covers a destroyed entity, not just an
		// unselected one.
		if (!m_Entity.IsValid())
		{
			// Greyed-out placeholder text. TextDisabled rather than Text so it reads as
			// a hint rather than content.
			ImGui::TextDisabled("Select an entity with a script to inspect variables.");
			ImGui::End();
			return;
		}

		// A valid entity that has no script. Distinct message from the case above
		// because the user DID select something -- this says the selection is wrong,
		// not that they have not made one.
		if (!m_Entity.HasComponent<ScriptComponent>())
		{
			ImGui::TextDisabled("Selected entity has no ScriptComponent.");
			ImGui::End();
			return;
		}

		// Now safe: the HasComponent check above guarantees this succeeds.

		// Note Entity::GetComponent asserts internally, so this line is only correct
		// because of the guard.
		auto &sc = m_Entity.GetComponent<ScriptComponent>();
		// Shows both the display name and the file path, since a script's identity
		// in the editor is usually its path.
		ImGui::Text("Script: %s (%s)", sc.ScriptName.c_str(), sc.ScriptPath.c_str());
		// Visual break between the header and the variable list.
		ImGui::Separator();

		// The entity's UUID, used to find the Python instance.

		// DEFECT: this calls GetComponent<IDComponent>() with no HasComponent guard.
		// The check above only proves a ScriptComponent exists, not an IDComponent.
		// GetComponent asserts in debug and, in release, calls entt's get<T> on a
		// missing component -- undefined behaviour. Every entity created through the
		// engine's own APIs has one, so this does not fire in practice, but an
		// entity assembled by hand (or a ScriptComponent added to an entity created
		// without an ID) would fault here rather than show an error.
		auto id = m_Entity.GetComponent<IDComponent>().ID;
		// The live Python object for this entity, or null if the script has not
		// been instantiated.

		// Returns a RAW POINTER into the script engine's storage, with no ownership:
		// the panel must not delete it, and nothing here keeps the ScriptEngine
		// alive. Borrowed for the duration of the frame only, which is why the null
		// check below matters -- an entity can be deselected while the script engine
		// tears its instances down.
		py::object *instance = ScriptEngine::GetInstance(id);

		// The script exists as a component but has no live Python object -- the
		// scene has not been run yet, or the script failed to initialise.
		if (!instance)
		{
			ImGui::TextDisabled("Script instance not initialized yet. Run the scene first.");
			ImGui::End();
			return;
		}

		// The outer guard for the whole reflection loop.

		// Necessary because attribute enumeration, __dict__ access and repr() can all
		// raise, and a Python exception escaping into the render loop would unwind
		// through ImGui with its internal state unbalanced -- leaving the window and
		// ID stack in a state the next frame cannot recover from.
		try
		{
			// Enumerates the instance dictionary's keys.

			// __dict__ rather than dir() or inspect: it lists what is actually SET on the
			// instance, which for an inspector is the right question -- class-level
			// defaults and methods are handled separately below (methods by the
			// isinstance<py::function> skip).

			// A three-step call: fetch the dict, fetch its keys method, call it. Each
			// step can raise, which is why this sits inside the try.

			// .cast<py::list>() requires pybind11/stl.h, included above. Without it this
			// would not compile.
			py::list attrs = instance->attr("__dict__").attr("keys")().cast<py::list>();

			// Iterates attribute names as Python objects, cast to std::string below.

			// A snapshot list, not a live view -- so mutating the instance inside the
			// loop cannot invalidate the iterator.
			for (auto attrObj : attrs)
			{
				// Key objects from a dict are always strings, so this cast cannot fail in
				// practice.
				std::string attrName = attrObj.cast<std::string>();

				// Skips underscore-prefixed names and the injected "entity" reference.

				// `continue` rather than wrapping the body in an else -- the flat style is
				// what keeps this long type dispatch below flat enough to read.
				if (!IsPublicAttribute(attrName))
					continue;

				// Default-constructed, i.e. a null py::object, so the catch below can leave
				// it null and the skip applies.
				py::object value;
				try
				{
					// Fetches the attribute's value. A property with a getter can raise here
					// even though the name came from __dict__ -- for instance a descriptor
					// installed on the class.
					value = instance->attr(attrName.c_str());
				}
				// A property getter that threw.

				// Caught BY REFERENCE, which matters: pybind11's error_already_set owns the
				// fetched Python error indicator, and leaving it un-restored would leave the
				// error state set for unrelated later code.
				catch (py::error_already_set &)
				{
					// Clears the Python error indicator.

					// DEFECT: PyErr_Clear() clears the GLOBAL interpreter error state, but
					// pybind11's error_already_set holds its own copy and restores the original
					// error state in its destructor. The idiomatic pybind11 call here is
					// e.discard_as_unraisable() (or restoring and re-raising), not PyErr_Clear.
					// As written the catch swallows a genuine Python exception with no record
					// anywhere -- not even in the panel -- so a failing property getter is
					// invisible. The same PyErr_Clear pattern repeats nine more times below.
					PyErr_Clear();
					continue;
				}

				// Skips methods and imported modules.

				// Only these two callable-ish types are filtered. Anything else non-numeric
				// falls through to the repr fallback at the bottom, so an instance attribute
				// holding a class, a generator or a bound method-wrapper shows as its repr
				// rather than being hidden.
				if (py::isinstance<py::function>(value) || py::isinstance<py::module_>(value))
					continue;

				// Scopes the widget ID to this attribute name.

				// Essential, not decorative: without it, two attributes with the same label -- or an ImGui::Checkbox whose label collides with another widget --
				// would share an ID and make one of them non-interactive. Every PushID here
				// is matched by the PopID at the bottom of the loop.
				ImGui::PushID(attrName.c_str());

				// Type dispatch begins. Order is significant: bool_ is tested FIRST and the
				// int case below explicitly excludes bool_, because in Python bool is a
				// subclass of int -- without that exclusion a boolean would render as a
				// number spinner.

				// Consequence worth knowing: this is an if/else-if chain, so a Python type
				// with several type registrations (an enum, a subclass) matches only its
				// FIRST registered type. An IntEnum would fall to the int branch and lose the
				// named-value choices.
				if (py::isinstance<py::bool_>(value))
				{
					// Copies out to a local, because ImGui needs an address to write through.

					// A copy-out/copy-back pattern: every branch reads the Python value into a
					// C++ local, lets the user edit it, and writes it back only if it changed.
					bool bval = value.cast<bool>();
					// The bool return is ImGui's "changed" flag -- true only on the frame the
					// user clicked, not every frame. That is what makes the write-back
					// conditional rather than a per-frame Python assignment.
					if (ImGui::Checkbox(attrName.c_str(), &bval))
					{
						// Writes the edited value back to the instance.

						// Setting an attribute on a Python object can raise -- a read-only property
						// or a __slots__ class will -- hence the local try/catch, repeated here and
						// in every other write branch.
						try { instance->attr(attrName.c_str()) = bval; }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				// Integers, excluding bools as noted above.

				// DragInt returns a signed 32-bit value while Python ints are arbitrary
				// precision, so a value beyond INT32_MAX loses its high bits on the way in.
				else if (py::isinstance<py::int_>(value) && !py::isinstance<py::bool_>(value))
				{
					int ival = value.cast<int>();
					if (ImGui::DragInt(attrName.c_str(), &ival))
					{
						try { instance->attr(attrName.c_str()) = ival; }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				// Floats, edited as a drag control with a 0.1 step.

				// The step is a fixed 0.1 per frame, so it is frame-rate dependent and there
				// is no way to type an exact value. Fine for tuning sliders, wrong for any
				// value that must be entered precisely.
				else if (py::isinstance<py::float_>(value))
				{
					float fval = value.cast<float>();
					if (ImGui::DragFloat(attrName.c_str(), &fval, 0.1f))
					{
						// The explicit double cast is deliberate: the value goes BACK as a Python
						// float, not as the C++ float. Without it a float literal would widen to
						// whatever the overload picked, risking the attribute changing type.
						try { instance->attr(attrName.c_str()) = (double)fval; }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				// Strings.
				else if (py::isinstance<py::str>(value))
				{
					// Copy into managed memory, because ImGui needs a stable writable buffer.

					// The panel is EDIT-IN-PLACE rather than add/remove, so a user cannot create
					// a new attribute from the inspector -- only change existing ones.
					std::string sval = value.cast<std::string>();
					// Fixed 512-byte stack buffer handed to ImGui as the edit target.

					// DEFECT: a silent truncation ceiling. A Python string attribute longer than
					// 511 characters is cut off by the copy below, and -- because ImGui treats the
					// buffer as authoritative and only fires its changed callback on user edit --
					// the truncated text is written back if the user touches the field at all.
					// There is no indication in the UI that the value was clipped. A std::string
					// built to the ImGui value would avoid both the limit and the copy.
					char buffer[512];
					// Zero-fills first, so the buffer is NUL-terminated even before the copy.
					memset(buffer, 0, sizeof(buffer));
					// Copies with truncation instead of overflowing.

					// Two portability notes: strncpy_s and _TRUNCATE are Microsoft-specific, so
					// this file is Windows/MSVC-only as written -- it will not compile with GCC
					// or Clang, where the portable spelling is strncpy plus an explicit
					// termination. Given the editor already builds for Windows this is a latent
					// rather than active constraint.

					// Combined with the 512-byte ceiling above, anything longer is discarded
					// here without a word to the user.
					strncpy_s(buffer, sizeof(buffer), sval.c_str(), _TRUNCATE);
					if (ImGui::InputText(attrName.c_str(), buffer, sizeof(buffer)))
					{
						try { instance->attr(attrName.c_str()) = std::string(buffer); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				// glm vector types, given their own widgets rather than falling to repr.

				// These work only because glm's pybind11 type caster is registered somewhere
				// in the scripting bindings; if it is not, isinstance returns false and the
				// attribute silently degrades to the read-only repr branch.
				else if (py::isinstance<glm::vec2>(value))
				{
					glm::vec2 v2 = value.cast<glm::vec2>();
					// Copies components into a flat array, which is the shape ImGui's
					// DragFloatN widgets expect. The same array serves as the write-back buffer.
					float vals[2] = {v2.x, v2.y};
					// 0.1f is the per-drag step, matching the scalar float branch above.
					if (ImGui::DragFloat2(attrName.c_str(), vals, 0.1f))
					{
						// Writes back as a reconstructed glm::vec2 rather than the array, so the
						// Python-visible type is preserved.
						try { instance->attr(attrName.c_str()) = glm::vec2(vals[0], vals[1]); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				// vec3, same pattern as vec2.
				else if (py::isinstance<glm::vec3>(value))
				{
					glm::vec3 v3 = value.cast<glm::vec3>();
					float vals[3] = {v3.x, v3.y, v3.z};
					if (ImGui::DragFloat3(attrName.c_str(), vals, 0.1f))
					{
						try { instance->attr(attrName.c_str()) = glm::vec3(vals[0], vals[1], vals[2]); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else if (py::isinstance<glm::vec4>(value))
				{
					glm::vec4 v4 = value.cast<glm::vec4>();
					float vals[4] = {v4.x, v4.y, v4.z, v4.w};
					if (ImGui::DragFloat4(attrName.c_str(), vals, 0.1f))
					{
						try { instance->attr(attrName.c_str()) = glm::vec4(vals[0], vals[1], vals[2], vals[3]); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				// The fallback: anything not matched above is displayed read-only as its
				// repr().

				// This is what keeps the panel total -- an exotic attribute type shows
				// something rather than crashing or vanishing.
				else
				{
					std::string repr;
					try
					{
						// str() rather than repr(): for a string-valued attribute this would show the
						// quotes, though strings are handled by an earlier branch so it cannot arise
						// here.
						repr = py::str(value).cast<std::string>();
						// Truncates long reprs so one enormous object cannot stretch the panel.
						if (repr.size() > 80)
							// The truncation marker, appended unconditionally rather than trimming to
							// 78 characters to leave room -- so the total is 83, not 80.
							repr = repr.substr(0, 80) + "...";
					}
					// __str__ can raise, and often does for objects with a broken __repr__.
					catch (py::error_already_set &)
					{
						PyErr_Clear();
						// The graceful-degradation answer.

						// Better than the PyErr_Clear swallow above, because at least the user sees
						// that something went wrong -- though the error text itself is discarded.
						repr = "<error reading value>";
					}
					// Name and value on one line: the label first, then SameLine below.
					ImGui::Text("%s:", attrName.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("%s", repr.c_str());
				}

				// Balances the PushID at the top of the loop.

				// Safe on every path because the loop body uses `continue` for skips rather
				// than an early return; the only `continue`s (the IsPublicAttribute and
				//  attribute-read failures) both occur BEFORE the PushID.
				ImGui::PopID();
			}
		}
		// The outer catch for the whole loop.

		// Reports rather than swallows: unlike the inner handlers this one shows
		// the message in the panel, which is the right call for an error that means
		// the entire attribute list could not be built.
		catch (py::error_already_set &e)
		{
			// Red, so it is visibly an error rather than content.

			// e.what() is the formatted Python traceback, so the user gets the actual
			// cause -- which is exactly what the inner PyErr_Clear handlers throw away.

			// DEFECT (minor, format string): "%s" is passed to a variadic printf-style
			// formatter along with an ImVec4. If the formatting backend is the default
			// ImGui one this happens to work by accident, but a float argument consumed by
			// a %s conversion is undefined behaviour in general, and this would be a
			// hard fault on a stricter or 64-bit-checking formatter.
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Python error: %s", e.what());
			// Same global-clear issue as the inner handlers; see the note there. Here it
			// is at least paired with a visible message.
			PyErr_Clear();
		}

		// Closes the window opened at line 33.

		// Every early return above pairs its own End() before returning, so the
		// Begin/End nesting stays balanced on all paths -- the invariant ImGui asserts
		// on in debug builds.
		ImGui::End();
	}

}
