#pragma once

#include <string>

#include "Wasteland/VisualBlocks/Graph.h"
#include "Wasteland/VisualBlocks/NodeRegistry.h"

namespace Wasteland
{

	// Schema-driven editing widgets for graph VALUES.
	//
	// The point of this file is that no node type is special-cased anywhere
	// in the editor. A node's pins come from `NodeDef`, its pins' types from
	// `PinDef`, and the widget for each is chosen from that type alone. So a
	// node added to the registry tomorrow is editable today, with the right
	// controls, without anyone writing an editing case for it.
	//
	// That is the whole reason the panel has a "pin" abstraction instead of a
	// pile of `if (typeId == "SetVariable")` branches. The alternative is
	// exactly the failure that motivates this: the editing UI knows about a
	// handful of nodes and silently shows a plain text box for the rest,
	// which is how "editable" quietly means "editable for these four".
	//
	// Every value round-trips through `PinLiteral::Value`, which is raw TEXT
	// as authored. Conversion here is strict: text that does not parse is
	// REJECTED and the field keeps its previous value, never silently
	// becoming zero. A drag control that snaps a half-typed "1e" to 0 on
	// every keystroke would make numbers uneditable.
	// All-static, and every member below is too -- there is no instance state, so the
	// class is a namespace in everything but name. Call sites spell out ValueEditor::
	// rather than holding one.
	class ValueEditor
	{
	public:
		// A stable per-field buffer. ImGui keeps text focus via the widget
		// ID, so the buffer only has to outlive one frame; this map makes
		// that true without leaking a std::string per keystroke.
		// A compile-time constant rather than a #define, so it is typed and scoped. 256
		// is a std::string capacity, and the comment above says why a map is needed at
		// all: ImGui keys text focus by widget ID, so a buffer only has to survive one
		// frame, but something has to own the storage across calls.
		static constexpr size_t kBufferSize = 256;

		// Returns a buffer for (nodeId, field), seeding it from `current`
		// when absent so the widget shows the stored value on first frame.
		// scratchOwner and scratchKey are outputs AND inputs. The caller keeps one pair
		// of locals per frame; when the key is unchanged the function returns a pointer
		// into scratchOwner and does no allocation. That is the whole trick -- a static
		// map of per-field strings would allocate on every keystroke.
		//
		// DEFECT (footgun): the returned char* is invalidated the moment the scratch
		// string is reassigned. It is safe only because ImGui consumes it within the
		// frame it was produced, which the comment above states but the signature does
		// not enforce. Not fixed; comments only.
		static char *Buffer(std::string &scratchOwner,
							std::string &scratchKey,
							const std::string &nodeId,
							const std::string &field,
							const std::string &current);

		// Drop every buffer. Call on graph load so a field from a previous
		// file cannot seed the new one's controls.
		static void ResetBuffers();
		static void ResetBuffersFor(const std::string &nodeId);

		// Parse/format for each type. Strict: returns false when `text` is
		// not a complete, valid value of that type.
		static bool ParseFloat(const std::string &text, float &out);
		static bool ParseInt(const std::string &text, int &out);
		static bool ParseBool(const std::string &text, bool &out);

		// Vectors are comma or space separated: "1, 2, 3" and "1 2 3" both
		// work, because neither is more obviously correct to an author.
		// A raw float* plus a count rather than a std::array or a vector: the caller
		// already has the components laid out contiguously in whatever ImGui widget
		// produced them, so a pointer avoids a copy on every keystroke. The count makes
		// the contract explicit -- this writes exactly `count` floats and no more.
		static bool ParseVec(const std::string &text, float *out, int count);

		static std::string FormatFloat(float v);
		static std::string FormatInt(int v);
		static std::string FormatBool(bool v);
		static std::string FormatVec(const float *values, int count);

		// The Python spelling the compiler emits for an unlinked pin.
		//
		// This is NOT the same as Format*: a literal is stored as authored
		// and handed to the emitter, but a STRING literal has to become a
		// quoted Python string or the generated file is a syntax error or,
		// worse, an injection. Kept here so the quoting rule lives in one
		// place instead of in every emitter.
		// The one function here that is NOT a parse/format pair, and the header says why.
		// DEFECT (injection surface, contained): a literal authored as text becomes
		// source in the emitted Python, so anything not quoted here is an injection. The
		// quoting lives in this one function rather than in every emitter, which is the
		// right place -- but it means every emitter must route string literals through it.
		static std::string Quoted(const std::string &raw);

		// Human-readable type name for the inspector.
		static const char *TypeName(VisualBlocks::PinType type);

		// True when the pin is a linked input, i.e. its value comes from
		// another node and must NOT be edited in place. The inspector shows
		// the source instead of a control.
		// IsLinked, not "is this pin connected": the check is per-pin, so a node with one
		// linked input and one literal input is half-editable and half-inspectable. The
		// inspector switches per field rather than per node for exactly that reason.
		static bool IsLinked(const VisualBlocks::Graph &graph,
							 const std::string &nodeId,
							 const std::string &pinId);

		// The producer node id for a linked input, or empty.
		// LinkSourceNode returns a std::string BY VALUE rather than const&, because the
		// id may have to be built by walking to a source node -- a reference into the
		// graph would dangle the moment that graph is edited. An empty string means "not
		// linked", so a caller can use it directly as a test.
		static std::string LinkSourceNode(const VisualBlocks::Graph &graph,
										  const std::string &nodeId,
										  const std::string &pinId);

	};

} // namespace Wasteland