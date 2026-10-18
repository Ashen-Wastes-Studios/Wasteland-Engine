// First by convention: the engine-wide prefix every translation unit opens with.
#include "wlpch.h"

// Graph.h FIRST. GraphSerializer.h only forward-declares `class Graph;`, and a
// member function taking `const Graph&` mangles differently against an
// INCOMPLETE type than a complete one. Including the forward-declaring header
// first produces AEBU here and AEBV in every translation unit that includes
// Graph.h first -- so the definition and its callers disagree and the symbol
// never links. The order of these two includes is load-bearing.
// Deliberately before GraphSerializer.h, and the reason is in the comment above
// this line: a member function taking a const Graph& mangles differently against
// an incomplete type than against a complete one, so the order of these two
// includes is load-bearing rather than cosmetic.
#include "Graph.h"
// The declaration of the two functions defined below. Included second for the
// reason just described.
#include "GraphSerializer.h"

// The parser and emitter. Nothing else in the engine uses YAML directly; this
// format is confined to visual blocks.
#include <yaml-cpp/yaml.h>

// Both file entry points stream bytes rather than text, so the file is read and
// written exactly as it lies on disk with no newline translation.
#include <fstream>
// The rdbuf slurp in Load.
#include <sstream>

// Nested-namespace form, so the file opens without a separate namespace
// statement. Everything below, including the anonymous-namespace helpers, is
// scoped to visual blocks.
namespace Wasteland::VisualBlocks
{

	// Renders a load failure as one human-readable line: file, then the most
	// specific location the error carries, then the message. Every producer in Parse
	// below fills in whichever of HasNode / NodeId / PinId it can, so the depth of
	// this line is entirely determined by how much the failing check knew.
	std::string GraphError::Format(const std::string &fileName) const
	{
		// The file name is a parameter, not a member, because one GraphError can be
		// formatted for more than one file by a caller that batches loads.
		std::string out = fileName;
		// HasNode is a separate flag from NodeId being non-empty. That redundancy is
		// deliberate: a variable or blackboard key sets HasNode with a key name in
		// NodeId, so the flag is what distinguishes 'this error is located' from 'this
		// error has no location yet'.
		if (HasNode && !NodeId.empty())
		{
			// Always says 'node', even when NodeId holds a VARIABLE or BLACKBOARD KEY name.
			/// The label is therefore wrong for those two error classes, which is a small
			// wording defect the producers below cannot fix without a wider format.
			out += ": node '" + NodeId + "'";
			// Optional: only the link cross-reference checks have a pin to name.
			if (!PinId.empty())
				// Appended before the colon so the shape is file: node N pin P: message.
				out += " pin '" + PinId + "'";
			// Only reached when HasNode was set, so an unlocated error reads file: message
			// with no doubled separator.
			out += ": ";
		}
		// The message goes last and unquoted, because every producer already embeds its
		// own quoting.
		out += Message;
		// No trailing newline: the caller decides how the line is displayed.
		return out;
	}

	namespace
	{
		// The top-level keys we recognise. Anything else is an error rather
		// than a drop, so a typo in a hand-authored or agent-authored file is
		// reported instead of silently deleting the section it names.
		// The allowlist for the document root. Rejecting unknown keys rather than
		// ignoring them is a deliberate choice: a typo in a hand-authored or
		// agent-authored file is reported instead of silently deleting the section it
		// named.
		bool IsKnownTopLevelKey(const std::string &key)
		{
			// Seven keys. Note the asymmetry this creates with the section parsers further
			// down: the ROOT is allowlisted, but nothing inside variables, blackboard or a
			// node's children list is, so a mistyped key there is ignored silently.
			return key == "kind" || key == "schema" || key == "name" ||
				   key == "variables" || key == "blackboard" ||
				   key == "nodes" || key == "links";
		}

		// The allowlist for one node mapping. The comment below records the bug this
		// function was written to fix, which is worth keeping because it explains why
		// 'title' appears in an otherwise unremarkable list of four scalars.
		bool IsKnownNodeKey(const std::string &key)
		{
			// 'title' belongs here. It was written and read correctly on both
			// sides, but was missing from this allowlist -- so every graph
			// carrying a node title was REJECTED on load with "unknown node
			// key 'title'". Since the writer emits `title` whenever Title is
			// non-empty, that meant a graph saved by the editor could not be
			// read back by the editor once a node was renamed.
			// The id/type pair is mandatory, the rest are optional. Ordering here matches
			// neither the writer nor the reader; it is a lookup list, not a schema.
			return key == "id" || key == "type" || key == "x" || key == "y" ||
				   key == "title" || key == "event" || key == "latent" ||
				   key == "abort" || key == "children" || key == "pins";
		}

		// Pin literals are the smallest mappings in the format: an id and a value.
		bool IsKnownPinKey(const std::string &key)
		{
			// A literal with only a pin id is legal on load (see the pin loop below), so
			// 'value' is optional in this allowlist.
			return key == "pin" || key == "value";
		}

		// A link has exactly two keys, from and to. Anything else is a typo.
		bool IsKnownLinkKey(const std::string &key)
		{
			// Both required in practice, but enforced by the presence checks further down
			// rather than here.
			return key == "from" || key == "to";
		}

		// Inside a from/to ref the keys are a node id and a pin id. Checked separately
		// from IsKnownLinkKey because this is one level deeper.
		bool IsKnownRefKey(const std::string &key)
		{
			// 'node' is mandatory and 'pin' is not; see the asymmetry noted at the bottom
			// of ReadRef.
			return key == "node" || key == "pin";
		}

		// `from: { node: n1, pin: Out }` -- a nested mapping of stable ids.
		// Parses one { node: ..., pin: ... } mapping. Written to two out-parameters
		// rather than returning a struct, because the caller has a GraphLink half-built
		// and would only copy the fields across.
		bool ReadRef(const YAML::Node &node, std::string &outNode, std::string &outPin,
					 std::string &error)
		{
			// The only shape check in this file. Compare the section parsers below, which
			// perform no IsMap or IsSequence check on the containers they iterate.
			if (!node.IsMap())
			{
				// Says what was expected rather than what was found; the caller cannot include
				// a YAML dump of the offending node here without pulling in more machinery.
				error = "expected a { node: ..., pin: ... } mapping";
				return false;
			}
			// Key order in the file is irrelevant, so this iterates whatever order YAML
			// preserved. That is what lets 'pin' be written before 'node'.
			for (const auto &entry : node)
			{
				// DEFECT (class-wide): as<T>() throws on a conversion failure, and the only
				// try/catch in this file wraps YAML::Load alone. Every such conversion below --
				// here and in Parse -- is therefore an unguarded throw rather than a
				// GraphError.
				std::string key = entry.first.as<std::string>();
				// Same allowlist policy as the other levels: report the typo rather than drop
				// the key and let the ref resolve to something unintended.
				if (!IsKnownRefKey(key))
				{
					// Names the offending key and states the two legal ones, so the author does not
					// have to consult the format documentation to fix it.
					error = "unknown key '" + key + "' in a node/pin reference "
							"(expected 'node' or 'pin')";
					// Bails out of the whole ref, not just this key, so a partially-filled ref is
					// never handed back as if it were complete.
					return false;
				}
				// Order-independent: whichever key appears first is stored, and the required
				// check at the bottom of this function is what enforces presence.
				if (key == "node")
					// Overwrites unconditionally, so a duplicated 'node' key takes the LAST value
					// rather than being reported. The allowlist does not catch repeats.
					outNode = entry.second.as<std::string>();
				else
					// A pin may legitimately be absent, which is how an exec link is written --
					// see how DrawWires in the editor canvas treats a pin-less link.
					outPin = entry.second.as<std::string>();
			}
			// The presence test is emptiness, not key presence, so an explicit
			// 'node:' with no value is caught too. Both produce the same message.
			if (outNode.empty())
			{
				// DEFECT (asymmetry): there is no matching check for 'pin', so a ref carrying
				// only a node id loads successfully and produces a link with an empty pin id.
				// Nothing downstream in this file notices; the consumer has to cope with an
				// unaddressed link.
				error = "reference is missing its 'node'";
				return false;
			}
			// A ref with a node and no pin is still considered well-formed, as above.
			return true;
		}
	}

	// ------------------------------------------------------------------ load

	// The file entry point. Parse below holds the real logic; this exists to turn a
	// path into bytes and to put the file name into the error.
	bool GraphSerializer::Load(const std::string &path, Graph &outGraph,
							   GraphError &error)
	{
		// Binary, so a graph containing a Windows newline keeps it and a hand-authored
		// file is not silently rewritten on load.
		std::ifstream file(path, std::ios::binary);
		// The only failure reported without a message body: a missing file has no
		// content to describe.
		if (!file.is_open())
		{
			// DEFECT (information loss): unlike every other message in this file, this one
			// does not include the path, and this is the one failure where the path is the
			// only thing the author needs.
			error.Message = "could not open file";
			return false;
		}

		// The slurp buffer. No size limit; a graph file is expected to be hand-editable
		// text, so this is not considered a risk.
		std::stringstream buffer;
		// Inserting the whole buffer in one operation. A read error partway through is
		// not detected: the loop ends and whatever was read is parsed as if complete.
		buffer << file.rdbuf();

		// On failure `outGraph` is untouched, because Parse builds into a local and
		// only assigns on success.
		if (!Parse(buffer.str(), outGraph, error))
		{
			// The path is pushed into Message here, while GraphError::Format takes a file
			// name as an argument and prepends it too. A caller that formats this error with
			// the same path gets it twice: 'x.wlblocks: x.wlblocks: ...'. Only one of the two
			// should be doing this.
			error.Message = path + ": " + error.Message;
			return false;
		}
		// `error` is not cleared here; Parse already reset it on the success path.
		return true;
	}

	// The whole reader. Every failure returns false with `error` filled in, and the
	// caller's graph is left exactly as it was.
	bool GraphSerializer::Parse(const std::string &text, Graph &outGraph,
								GraphError &error)
	{
		// Built into a LOCAL and only swapped in at the end. A partial load is
		// the failure mode the plan calls out: the caller gets half a graph,
		// compiles it, and ships a tree that quietly lost a task.
		// Built into a local and swapped in at the end. The comment above explains why:
		// a partial load is the failure mode that ships a tree which quietly lost a
		// task.
		Graph scratch;

		// A node handle rather than a parsed document type: yaml-cpp's model is
		// reference-based and every lookup below re-wraps a handle.
		YAML::Node root;
		// The ONLY try block in this file, and it covers YAML::Load alone. Every as<T>()
		// conversion below, and every indexing of a node of the wrong shape, is outside
		// it.
		try
		{
			// The library's own scanner throws YAML::ParserException on malformed input,
			// which is caught immediately below.
			root = YAML::Load(text);
		}
		// Caught by base-class reference because yaml-cpp's exception hierarchy derives
		// from std::runtime_error.
		catch (const std::exception &e)
		{
			// e.what() carries the parser's line and column, which is the most useful part
			// of the message and the reason the catch exists at all.
			error.Message = std::string("malformed YAML: ") + e.what();
			return false;
		}

		// The one structural check at the document level. An empty file parses to a
		// null node, which is not a map, so an empty file is reported rather than
		// accepted as an empty graph.
		if (!root.IsMap())
		{
			// Says what was required, not what was found.
			error.Message = "top level must be a mapping";
			return false;
		}

		// --- top level ---
		// A full validation pass BEFORE any field is read. That ordering is what lets a
		// typo be reported as an unknown key instead of being silently ignored by the
		// per-field readers below.
		for (const auto &entry : root)
		{
			// Same unguarded conversion as in ReadRef; a non-string key at the root throws
			// out of Parse.
			std::string key = entry.first.as<std::string>();
			// Rejects rather than skips, so a misspelled section name cannot delete a
			// section.
			if (!IsKnownTopLevelKey(key))
			{
				// Carries no location fields, so Format will render this as file: message with
				// nothing to narrow it down.
				error.Message = "unknown top-level key '" + key + "'";
				return false;
			}
		}

		// Every optional field below is guarded by an explicit test on the YAML node.
		if (root["kind"])
		{
			// Both spellings of the same key are indexed; the second lookup is a second
			// map walk, which is free at this size.
			scratch.Kind = root["kind"].as<std::string>();
			// The only closed vocabulary in the format, and the reason a graph authored by
			// a newer build with a third kind is refused rather than half-read.
			if (scratch.Kind != "flow" && scratch.Kind != "tree")
			{
				// The value is echoed so the author can see what was rejected.
				error.Message = "unknown kind '" + scratch.Kind +
								"' (expected 'flow' or 'tree')";
				return false;
			}
		}
		// The schema version, read but never acted on: there is no migration branch
		// here, so a file declaring a newer schema is loaded exactly like an older one
		// and may fail later in the compiler rather than here.
		if (root["schema"])
			// DEFECT: as<int>() accepts only an unquoted integer token. A hand-written
			// 'schema: "1"' raises TypedBadConversion, which is outside the one try block
			// and therefore escapes Parse entirely.
			scratch.Schema = root["schema"].as<int>();
		// The graph's display name. Optional, so a file without one loads with an empty
		// name rather than failing.
		if (root["name"])
			// An empty name is indistinguishable from an absent key, which matters only if
			// something downstream treats the name as an identifier.
			scratch.Name = root["name"].as<std::string>();

		// --- variables ---
		// Variables are the flow-graph form of named storage. The blackboard below is
		// the behaviour-tree equivalent, and the two are parsed by near-identical code.
		if (root["variables"])
		{
			// DEFECT: no IsSequence() check on root["variables"]. yaml-cpp's node_data::begin
			// returns an empty range for a scalar (vendor/yaml-cpp/src/node_data.cpp), so
			// 'variables: oops' iterates zero times and loads as a graph with no variables,
			// with no diagnostic. A mapping here would instead throw on the item indexing
			// below, outside the single try block.
			for (const YAML::Node &item : root["variables"])
			{
				// Constructed per item and defaulted, so an absent optional field keeps the
				// struct's default rather than an uninitialised value.
				GraphVariable variable;
				// Read with a default rather than guarded by an if, which is the same effect in
				// a third of the characters. Note there is no allowlist check for the other keys
				// of a variable, so a misspelled 'type' is silently ignored here.
				variable.Name = item["name"] ? item["name"].as<std::string>() : "";
				// A name is mandatory: it is the key every other part of the format refers to.
				if (variable.Name.empty())
				{
					// DEFECT (locatability): unlike the type error below, this one sets no NodeId
					// and no HasNode, so Format cannot say WHICH variable is unnamed. With a dozen
					// variables in a file this is an unhelpful diagnostic.
					error.Message = "a variable has no 'name'";
					return false;
				}
				// Optional: an absent type leaves the struct's default rather than failing.
				if (item["type"])
				{
					// Read as text and translated below, rather than cast straight to the enum, so
					// an unknown name can produce a diagnostic instead of an exception.
					std::string typeName = item["type"].as<std::string>();
					// A name-to-enum lookup with an out-parameter, so an unknown name is a reportable
					// error rather than a default value.
					if (!PinTypeFromName(typeName, variable.Type))
					{
						// Names the offending variable, which is the whole point: a type typo in a
						// thirty-variable file has to be findable.
						error.Message = "variable '" + variable.Name +
										"' has unknown type '" + typeName + "'";
						// The variable's name is put in the field named NodeId, because GraphError has
						// no separate variable field. Format therefore prints this error as
						// "node 'x'" when it is a variable.
						error.NodeId = variable.Name;
						// Marks the error as located, which is what makes Format emit the node/pin
						// prefix at all.
						error.HasNode = true;
						return false;
					}
				}
				// The initialiser. DEFECT (asymmetry): the default is stored here WITHOUT being
				// parsed, while a blackboard key's default IS parsed and validated a few lines
				// below. So a variable whose default is malformed for its declared type loads
				// cleanly and fails later, at an unrelated place.
				if (item["default"])
					// Kept as raw authored text, so the same string can be re-emitted byte for byte
					// on save. That is the reason it is not parsed at all -- and also why the
					// validation asymmetry above exists.
					variable.DefaultLiteral = item["default"].as<std::string>();
				// DEFECT: no duplicate-name check, unlike the node loop below which rejects a
				// repeated id. Two variables sharing a name load successfully, and which one a
				// read resolves to becomes a matter of list order.
				scratch.Variables.push_back(variable);
			}
		}

		// --- blackboard (behaviour trees) ---
		// The behaviour-tree form of named storage. Same shape as variables, separate
		// key, separate error wording.
		if (root["blackboard"])
		{
			// The same missing IsSequence check as the variables loop above, with the same
			// consequence: a scalar section loads as empty.
			for (const YAML::Node &item : root["blackboard"])
			{
				// Defaults to PinType::Float, so an undeclared blackboard key is a float rather
				// than an error.
				BlackboardKey key;
				// Same optional-read idiom as the variable name above.
				key.Name = item["name"] ? item["name"].as<std::string>() : "";
				// Mandatory, as for variables.
				if (key.Name.empty())
				{
					// DEFECT (locatability): as with variables, no location is attached, so the
					// error cannot say which key is unnamed.
					error.Message = "a blackboard key has no 'name'";
					return false;
				}
				// Optional; the struct default applies when absent.
				if (item["type"])
				{
					std::string typeName = item["type"].as<std::string>();
					// The same name-to-enum translation as variables, so both sections reject the
					// same spellings.
					if (!PinTypeFromName(typeName, key.Type))
					{
						// Names the key, as the variable branch does.
						error.Message = "blackboard key '" + key.Name +
										"' has unknown type '" + typeName + "'";
						// The key name in the node field again, so Format says "node" for a blackboard
						// key.
						error.NodeId = key.Name;
						error.HasNode = true;
						return false;
					}
				}
				// Raw text, as for variables.
				if (item["default"])
					// Stored unparsed. Unlike a variable's default, the very next block validates
					// it -- the only default in the format that is checked at load time.
					key.DefaultLiteral = item["default"].as<std::string>();

				// A malformed default is a load error naming the key, not a
				// silent zero -- a blackboard key defaulting to 0 because its
				// default was malformed is nearly impossible to diagnose later.
				// Two guards: an empty default is legal (the key simply starts at its type's
				// zero), and an Exec-typed key has no literal to validate at all.
				if (!key.DefaultLiteral.empty() && key.Type != PinType::Exec)
				{
					// The parsed value is discarded -- this is a validation pass, not a conversion.
					std::string parsed;
					// Collected so the diagnostic can name the type-specific reason rather than
					// just saying the default is bad.
					std::string parseError;
					// Type-directed parsing, so 'speed: fast' is rejected for a Float key here
					// rather than producing a silently wrong zero later.
					if (!ParseLiteral(key.Type, key.DefaultLiteral, parsed, parseError))
					{
						// Names the key again, and includes the parse error verbatim.
						error.Message = "blackboard key '" + key.Name +
										"' has a malformed default: " + parseError;
						error.NodeId = key.Name;
						error.HasNode = true;
						return false;
					}
				}
				// DEFECT: as with variables, no duplicate-name check, so two blackboard keys can
				// share a name and the first match wins for every read.
				scratch.Blackboard.push_back(key);
			}
		}

		// --- nodes ---
		// The node list. Note this key is not required: a file with no nodes key loads as
		// a graph with none, which is how an empty graph is expressed.
		if (root["nodes"])
		{
			// Again no IsSequence check, with the same silent-empty consequence.
			for (const YAML::Node &item : root["nodes"])
			{
				// Default-constructed, so X and Y are 0 and every string is empty unless the
				// file says otherwise.
				GraphNode node;
				// Node-key validation, done before ANY field is read so a typo is reported as a
				// typo. The equivalent loops for links and pins below do the same.
				for (const auto &field : item)
				{
					std::string key = field.first.as<std::string>();
					// The allowlist that caught 'title' before it was added; see its own comment.
					if (!IsKnownNodeKey(key))
					{
						// DEFECT (the bug this file is partly a history of): the message cannot say
						// WHICH node. The next line tries to attach the node id, but at this point
						// node.Id is still the default-constructed empty string, because the id is not
						// read until below. So error.NodeId is always empty here and error.HasNode is
						// always false, and every 'unknown node key' error reaches the user as a bare
						// message with no location. Reading the id before the validation loop would fix
						// it.
						error.Message = "unknown node key '" + key + "'";
						// Always empty at this point in the function. See the note above.
						error.NodeId = node.Id;
						// Always false for the same reason, so the HasNode flag carries no information
						// in this branch.
						error.HasNode = !node.Id.empty();
						return false;
					}
				}

				// The FIRST field actually read, and therefore after the validation loop above.
				node.Id = item["id"] ? item["id"].as<std::string>() : "";
				// An id is mandatory: links and children refer to nodes by this string.
				if (node.Id.empty())
				{
					// No location fields, and none could be set: without an id there is nothing to
					// locate the node by.
					error.Message = "a node has no 'id'";
					return false;
				}
				// The registry key. NOT validated against the registry here: this file has no
				// access to it, so an unknown type is a load success and a compile-time
				// diagnostic instead.
				node.TypeId = item["type"] ? item["type"].as<std::string>() : "";
				// A node with no type cannot be compiled, so it is refused at load.
				if (node.TypeId.empty())
				{
					error.Message = "node has no 'type'";
					// From here on the id IS known, so the location fields do their job -- unlike
					// the unknown-key case above, which is the one place they silently do not.
					error.NodeId = node.Id;
					error.HasNode = true;
					return false;
				}
				// Position is optional and defaults to 0,0, which is what makes a
				// hand-authored graph with no coordinates still load.
				if (item["x"])
					// DEFECT: as<float>() on an integer token is a yaml-cpp conversion failure for
					// types where the scalar was decoded as an integer, and this is outside the one
					// try block. Depending on yaml-cpp's convert specialisation this either works or
					// throws; either way it is unguarded.
					node.X = item["x"].as<float>();
				// Same optionality as x.
				if (item["y"])
					node.Y = item["y"].as<float>();
				// The user-supplied display name. Present in this allowlist thanks to the fix
				// recorded in IsKnownNodeKey; before that, any graph with a renamed node was
				// rejected on load.
				if (item["title"])
					node.Title = item["title"].as<std::string>();
				// The event a flow-graph entry node listens for. Optional, and only meaningful
				/// on the one event node; nothing here enforces that.
				if (item["event"])
					node.EventName = item["event"].as<std::string>();
				// A flag meaning the node's task yields across ticks. Emitted only when true, so
				// absence means false on the round trip.
				if (item["latent"])
					node.Latent = item["latent"].as<bool>();
				// A second closed vocabulary, validated immediately because an unknown mode
				// would otherwise reach the tree compiler as a silent default.
				if (item["abort"])
				{
					node.Abort = item["abort"].as<std::string>();
					// Three legal values. 'none' is also what Emit omits, so the round trip is
					// stable: an explicit none and an absent key both load as none.
					if (node.Abort != "none" && node.Abort != "self" &&
						node.Abort != "lower")
					{
						// Echoes the bad value and lists the legal ones.
						error.Message = "unknown abort mode '" + node.Abort +
										"' (expected 'none', 'self' or 'lower')";
						error.NodeId = node.Id;
						error.HasNode = true;
						return false;
					}
				}
				// Ordered child ids for a behaviour-tree node. Declaration order is priority,
				// which is why Emit never sorts this list.
				if (item["children"])
				{
					// Bare strings, not mappings. A child written as a mapping yields a node whose
					// as<std::string>() conversion throws, outside the try block.
					for (const YAML::Node &child : item["children"])
						// Existence is NOT checked here; the cross-reference pass at the bottom does
						// that, once every node is known. Deferring it is what makes forward references
						// legal.
						node.ChildIds.push_back(child.as<std::string>());
				}
				// Literal values for this node's input pins.
				if (item["pins"])
				{
					// Again no IsSequence check on the container.
					for (const YAML::Node &pinItem : item["pins"])
					{
						// Per-literal key validation, matching the node-level policy of rejecting
						// rather than ignoring.
						for (const auto &pinField : pinItem)
						{
							// Another unguarded as<std::string>, as at the document root and in ReadRef.
							std::string pinKey = pinField.first.as<std::string>();
							// Two legal keys, so this is the strictest allowlist in the format.
							if (!IsKnownPinKey(pinKey))
							{
								// Correctly located, because the node id HAS been read by this point in the
								// function -- which is exactly what the node-key check above could not do.
								error.Message = "unknown pin key '" + pinKey + "'";
								error.NodeId = node.Id;
								error.HasNode = true;
								return false;
							}
						}
						// IsSet stays false until a value is seen, which is how 'authored as 0' is kept
						// distinct from 'not authored'.
						PinLiteral literal;
						// The pin id this literal belongs to. The value itself is NOT checked against
						// the type's pin list here, because this file has no registry access.
						literal.Key = pinItem["pin"] ? pinItem["pin"].as<std::string>() : "";
						// Mandatory: a literal with no pin id has nothing to attach to.
						if (literal.Key.empty())
						{
							error.Message = "a pin literal has no 'pin' id";
							error.NodeId = node.Id;
							error.HasNode = true;
							return false;
						}
						// DEFECT: the presence of a 'value' key is what sets IsSet, so a literal
						// written as 'value:' with nothing after it is treated as authored-and-empty,
						// while an absent value key leaves IsSet false and the literal is dropped on
						// save. The two are not distinguishable after a round trip.
						if (pinItem["value"])
						{
							literal.Value = pinItem["value"].as<std::string>();
							// Set here rather than in the constructor, so the distinction the header
							// describes is preserved on disk.
							literal.IsSet = true;
						}
						// Duplicates are not rejected: two literals for the same pin both load, and
						// which one a read picks depends on list order.
						node.Literals.push_back(literal);
					}
				}

				// Duplicate ids make every link ambiguous, and the symptom is a
				// link silently binding to the wrong node.
				// The duplicate-id check. It runs against everything pushed so far, so it
				// catches a repeat anywhere in the file, not just an adjacent one.
				if (scratch.FindNode(node.Id) != nullptr)
				{
					// The most consequential validation in the file, and the comment above it
					// explains why: duplicate ids make every link ambiguous and the symptom is a
					// link binding to the wrong node.
					error.Message = "duplicate node id '" + node.Id + "'";
					error.NodeId = node.Id;
					error.HasNode = true;
					return false;
				}
				// Appended only after every check passes, which is what keeps the duplicate scan
				// above honest.
				scratch.Nodes.push_back(node);
			}
		}

		// --- links ---
		// Wires between pins, addressed by node id and pin id.
		if (root["links"])
		{
			// No IsSequence check, as everywhere else in this file.
			for (const YAML::Node &item : root["links"])
			{
				// Key validation before the link is read, mirroring the node loop.
				for (const auto &field : item)
				{
					std::string key = field.first.as<std::string>();
					// Two legal keys, so anything else is a typo.
					if (!IsKnownLinkKey(key))
					{
						// DEFECT (locatability): no location fields are set here at all -- not even the
						// HasNode flag the node-key check above attempts. The link has not been parsed
						// yet, so unlike that case there is nothing available, and the message reaches
						// the user as file: message with no indication of which link is wrong.
						error.Message = "unknown link key '" + key + "'";
						return false;
					}
				}

				// Built after validation, so the two refs below can be read into it directly.
				GraphLink link;
				// Shared by both ref reads. Reused without clearing between them, which is safe
				// only because ReadRef either fills it on failure or leaves it alone on success.
				std::string linkError;
				// Short-circuit: an absent 'from' never reaches ReadRef. DEFECT: the || also
				// means a present-but-malformed from and an absent from produce different
				// messages, but only because of the ternary below -- and the ternary is keyed on
				// linkError being empty, not on which branch failed.
				if (!item["from"] || !ReadRef(item["from"], link.FromNode, link.FromPin,
											linkError))
				{
					// Prefixed with which end is at fault, which is the useful half of this
					// diagnostic.
					error.Message = "link 'from' is malformed: " +
									// Distinguishes the absent key from a malformed mapping. It works because
									// ReadRef leaves linkError untouched on the paths that do not fail.
									(linkError.empty() ? std::string("missing") : linkError);
					return false;
				}
				// The second ref, into the same reused error string. Note that a successful
				// 'from' read does not clear linkError, so it must already have been empty.
				if (!item["to"] || !ReadRef(item["to"], link.ToNode, link.ToPin, linkError))
				{
					// The same shape for the target end.
					error.Message = "link 'to' is malformed: " +
									(linkError.empty() ? std::string("missing") : linkError);
					return false;
				}
				// DEFECT: no duplicate-link check, so the same from/to pair can appear twice and
				// both survive to the compiler, which then sees two wires where the author
				// intended one.
				scratch.Links.push_back(link);
			}
		}

		// --- cross-reference checks ---
		// A child or link naming a node that does not exist is caught here, at
		// load, rather than becoming an empty branch that runs silently.
		// The deferred validation pass. It can only run now because every node id is
		// known, which is what lets a file reference a node declared later or earlier
		// without complaint.
		for (const GraphNode &node : scratch.Nodes)
		{
			// Children are checked for existence only. A cycle among children is NOT
			// detected here and would have to be caught by the compiler.
			for (const std::string &childId : node.ChildIds)
			{
				// FindNode is a linear scan and this runs per child per node, so the whole pass
				// is quadratic in the number of nodes. Fine at hand-authored graph sizes.
				if (scratch.FindNode(childId) == nullptr)
				{
					// Locates the error on the PARENT node, not on the missing child, which is the
					// only handle available.
					error.Message = "child '" + childId + "' does not exist";
					error.NodeId = node.Id;
					error.HasNode = true;
					return false;
				}
			}
		}
		// The same idea for wires: a link naming a deleted node is caught at load
		// rather than becoming an empty branch that runs silently.
		for (const GraphLink &link : scratch.Links)
		{
			// Source end checked first, so a link with two missing ends reports the source.
			if (scratch.FindNode(link.FromNode) == nullptr)
			{
				// The parenthetical names the real-world cause, because in practice this fires
				// after an edit removed a node that a wire still pointed at.
				error.Message = "link source node '" + link.FromNode +
								"' does not exist (a dangling link to a deleted node)";
				// DEFECT (confusing): the location points at the link's TARGET node, not at
				// either node named in the message. The author is told the source is missing and
				// then sent to the other end of the wire to look for it.
				error.NodeId = link.ToNode;
				error.HasNode = true;
				// Likewise the target's pin, continuing the same misdirection.
				error.PinId = link.ToPin;
				return false;
			}
			// Target end, for the mirror-image case.
			if (scratch.FindNode(link.ToNode) == nullptr)
			{
				error.Message = "link target node '" + link.ToNode + "' does not exist";
				// The mirror of the line above, and the mirror of its defect: the location is
				// the other end again.
				error.NodeId = link.FromNode;
				error.HasNode = true;
				error.PinId = link.FromPin;
				return false;
			}
		}

		// The single commit point. Nothing above this line has touched outGraph, which
		// is the whole reason the scratch local exists.
		outGraph = scratch;
		// The error is reset on success, so a caller that reuses one GraphError across
		// loads cannot read a stale message.
		error = GraphError();
		// Success. Note Parse does not clear `error` on the early-return failure paths
		// beyond the ones above, so a caller reusing the object must not inspect it after
		// a failure.
		return true;
	}

	// ------------------------------------------------------------------ emit

	// The writer. Everything is emitted through a YAML::Emitter rather than string
	// concatenation, so quoting and indentation are the library's problem.
	std::string GraphSerializer::Emit(const Graph &graph)
	{
		// The emitter is separate from `text` below: c_str() is only valid once the
		// emitter has been finished.
		YAML::Emitter out;

		// The document is one mapping, matching the IsMap check in Parse.
		out << YAML::BeginMap;
		// DEFECT: emitted unconditionally. Emit writes whatever Kind holds, while Parse
		// accepts only 'flow' or 'tree', so a Graph whose Kind was left empty or set to
		// anything else is written out as a file this same reader will reject on the next
		// load. The round trip is only safe because Graph::Kind defaults to "flow".
		out << YAML::Key << "kind" << YAML::Value << graph.Kind;
		// Also unconditional. Parse reads it into scratch.Schema and then does nothing
		// with it, so the field is written purely for another tool's benefit.
		out << YAML::Key << "schema" << YAML::Value << graph.Schema;
		// Also unconditional, so an unnamed graph round-trips as an explicit empty name
		// rather than an absent key.
		out << YAML::Key << "name" << YAML::Value << graph.Name;

		// Optional sections are omitted when empty, unlike the three scalars above. The
		// file is therefore not symmetric: it always says what the graph is and only
		// says what it has.
		if (!graph.Variables.empty())
		{
			out << YAML::Key << "variables" << YAML::Value << YAML::BeginSeq;
			// Declaration order, so variable order in the file is order in memory.
			for (const GraphVariable &variable : graph.Variables)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value << variable.Name;
				out << YAML::Key << "type" << YAML::Value << PinTypeName(variable.Type);
				// DEFECT (round-trip asymmetry): 'default' is emitted even when the literal is
				// empty, and the key order here is name, type, default. Parse's IsKnownNodeKey
				// does not apply to variables, so nothing rejects it -- but a variable written
				// with an empty default comes back with DefaultLiteral set to an empty string
				// rather than absent, which a reader cannot tell from 'no default key'.
				out << YAML::Key << "default" << YAML::Value << variable.DefaultLiteral;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}

		// The blackboard section, emitted only when non-empty and in the same key order
		// as variables.
		if (!graph.Blackboard.empty())
		{
			out << YAML::Key << "blackboard" << YAML::Value << YAML::BeginSeq;
			for (const BlackboardKey &key : graph.Blackboard)
			{
				out << YAML::BeginMap;
				out << YAML::Key << "name" << YAML::Value << key.Name;
				out << YAML::Key << "type" << YAML::Value << PinTypeName(key.Type);
				out << YAML::Key << "default" << YAML::Value << key.DefaultLiteral;
				out << YAML::EndMap;
			}
			out << YAML::EndSeq;
		}

		// Unlike the optional sections, nodes is emitted even when the list is empty, so
		// the file always has a nodes: key.
		out << YAML::Key << "nodes" << YAML::Value << YAML::BeginSeq;
		// List order, which is also the order the editor's hit test draws in.
		for (const GraphNode &node : graph.Nodes)
		{
			out << YAML::BeginMap;
			out << YAML::Key << "id" << YAML::Value << node.Id;
			out << YAML::Key << "type" << YAML::Value << node.TypeId;
			// DEFECT (round-trip asymmetry, opposite of the scalars above): coordinates
			// are emitted unconditionally, including when they are both zero. Parse accepts
			// an absent x, so both forms load, but the saved file of an untouched graph is
			// full of x: 0 and y: 0 lines.
			out << YAML::Key << "x" << YAML::Value << node.X;
			// Same, for the vertical coordinate.
			out << YAML::Key << "y" << YAML::Value << node.Y;
			// From here on node fields are omitted when empty or false, which is the
			// convention the rest of the writer follows.
			if (!node.Title.empty())
				out << YAML::Key << "title" << YAML::Value << node.Title;
			// Only the event node normally has one, but nothing here enforces that.
			if (!node.EventName.empty())
				out << YAML::Key << "event" << YAML::Value << node.EventName;
			// A flag, so emitted only when true; absent means false on the way back in.
			if (node.Latent)
				out << YAML::Key << "latent" << YAML::Value << node.Latent;
			// DEFECT (latent): an Abort value that is neither empty nor 'none' -- including
			// any value this reader would reject as unknown -- is written out verbatim. Emit
			// performs none of the vocabulary validation Parse does, so a graph built in
			// memory can be saved into a file its own reader will reject.
			if (!node.Abort.empty() && node.Abort != "none")
				out << YAML::Key << "abort" << YAML::Value << node.Abort;
			// Children are omitted entirely for a leaf, which is the common case.
			if (!node.ChildIds.empty())
			{
				out << YAML::Key << "children" << YAML::Value << YAML::BeginSeq;
				// The reason there is no sort anywhere in this function: reordering children
				// would change the behaviour of the compiled tree.
				// Declaration order IS priority for a tree, so it is written in
				// order and never sorted.
				for (const std::string &childId : node.ChildIds)
					// Bare scalars in the sequence, matching the reader.
					out << childId;
				out << YAML::EndSeq;
			}
			// DEFECT (round trip): the sequence is opened and closed on the strength of the
			// vector being non-empty, but the loop below skips unset literals. A node whose
			// literals are all unset therefore emits an empty 'pins: []', which Parse reads
			// back as a node with no literals -- so the file grows a key that carries no
			// information.
			if (!node.Literals.empty())
			{
				out << YAML::Key << "pins" << YAML::Value << YAML::BeginSeq;
				// Unset literals are dropped individually rather than the whole section being
				// omitted, which is what produces the empty sequence noted above.
				for (const PinLiteral &literal : node.Literals)
				{
					// The distinction that lets a pin authored as 0 survive a save; an unset
					// literal is not written at all.
					if (!literal.IsSet)
						continue;
					out << YAML::BeginMap;
					out << YAML::Key << "pin" << YAML::Value << literal.Key;
					// The raw authored text, never re-parsed or normalised, so an edit that only
					// moves a node cannot rewrite its literals.
					out << YAML::Key << "value" << YAML::Value << literal.Value;
					out << YAML::EndMap;
				}
				out << YAML::EndSeq;
			}
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;

		// Links are emitted even when empty, matching nodes.
		out << YAML::Key << "links" << YAML::Value << YAML::BeginSeq;
		// List order, which for a flow graph is the order the editor drew the wires in.
		for (const GraphLink &link : graph.Links)
		{
			out << YAML::BeginMap;
			// Nested two deep, which is what ReadRef's IsMap check requires.
			out << YAML::Key << "from" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "node" << YAML::Value << link.FromNode;
			// DEFECT (round trip): emitted unconditionally, so a link with an empty pin id
			// -- which Parse explicitly allows, see ReadRef -- round-trips to the same empty
			// pin id. That is stable, but it means the format can express a wire to a pin
			// that does not exist, and the reader accepts it.
			out << YAML::Key << "pin" << YAML::Value << link.FromPin;
			out << YAML::EndMap;
			// The mirror end, in the same key order as from so a hand-edited diff of a link
			// is one line rather than a reshuffle.
			out << YAML::Key << "to" << YAML::Value << YAML::BeginMap;
			out << YAML::Key << "node" << YAML::Value << link.ToNode;
			// Same unconditional emission, same caveat as the source pin.
			out << YAML::Key << "pin" << YAML::Value << link.ToPin;
			// Closes the document. Everything above must be balanced; the emitter will
			// complain through c_str() being empty if it is not.
			out << YAML::EndMap;
			out << YAML::EndMap;
		}
		out << YAML::EndSeq;

		out << YAML::EndMap;

		// yaml-cpp leaves a trailing newline off; add it so the file ends with
		// one like every other text file in the tree, and so a byte-comparison
		// test is not comparing against a missing final newline.
		// Materialised once. The emitter's c_str() is only valid until the next write,
		// so this copy is what makes the string safe to return.
		std::string text = out.c_str();
		// yaml-cpp leaves the trailing newline off. Adding it keeps the file consistent
		// with every other text file in the tree, so a byte-comparison test is not
		// comparing against a missing final newline.
		if (text.empty() || text.back() != '\n')
			text += '\n';
		// Always ends with a newline, so Save writes a well-formed text file.
		return text;
	}

	// Write the whole graph. There is no partial save and no in-place update: the
	// file is truncated and rewritten, so a failure mid-write loses the old
	// contents.
	bool GraphSerializer::Save(const std::string &path, const Graph &graph,
							   GraphError &error)
	{
		// Binary and truncating. Binary keeps the LF line endings Emit produces from
		// being rewritten to CRLF on Windows, which is what makes the saved file
		// byte-identical across platforms.
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		// The one failure detected before anything is written.
		if (!file.is_open())
		{
			// Unlike the matching message in Load, this one includes the path.
			error.Message = "could not open '" + path + "' for writing";
			return false;
		}
		// Serialised before the write so an emitter failure cannot leave a truncated
		// file behind. Note that Emit performs no validation, so this cannot fail for any
		// reason the reader would later object to.
		std::string text = Emit(graph);
		// A single write of the whole buffer. The cast to streamsize is required because
		// size() is size_t and the parameter is signed; a graph larger than 2^63 bytes
		// is not a real concern.
		file.write(text.data(), (std::streamsize)text.size());
		// DEFECT (incomplete check): good() is evaluated before the stream is flushed,
		// and the flush happens when `file` is destroyed at return. A write that fails at
		// flush time -- a full disk being the usual case -- is therefore reported as
		// success. Checking after an explicit close, or flushing here, would catch it.
		if (!file.good())
		{
			// The message a caller would show for a disk-full condition it will not
			// actually be told about, per the note above.
			error.Message = "failed while writing '" + path + "'";
			return false;
		}
		return true;
	}

}
