// YamlGlm.h -- glm vector overloads for a yaml-cpp Emitter.
//
// WHY THIS IS A HEADER AND NOT A FILE-LOCAL HELPER
// -----------------------------------------------
// These overloads were originally defined in SceneSerializer.cpp's anonymous
// namespace. That meant MaterialSerializer.cpp -- which emits the SAME vec4
// fields (`Albedo`, `EmissionColor`) for .wlmaterial -- could not see them, and
// a file that compiled the vec4 line as a float would have been a silent
// divergence rather than a compile error.
//
// The alternative, duplicating the overloads in each file, is worse: two
// copies of the on-disk spelling of a vec4 would then be able to drift apart
// with nothing to catch it. This header makes the spelling single-sourced.
//
// NOTE ON THE STICKY FLOW STYLE
// ----------------------------
// YAML::Flow is sticky on an Emitter and nothing resets it -- every overload
// here leaves the emitter in flow style for whatever is emitted next. The
// serialize path does not appear to hit a problem because every sequence is
// explicitly bracketed with BeginSeq/EndSeq, but a caller emitting a
// block-style construct after a vector on the same emitter inherits flow
// style. That is a yaml-cpp limitation, not something these overloads can
// fix; do not "correct" it by dropping YAML::Flow, which would change every
// scene file on disk.
#pragma once

#include <yaml-cpp/yaml.h>

#include <glm/glm.hpp>

// yaml-cpp convert<T> specializations for glm vectors.
//
// These used to live at the top of SceneSerializer.cpp, which meant ONLY
// that translation unit could see them -- MaterialSerializer.cpp, which
// reads the same vec4 fields for .wlmaterial, could not compile
// `node["Albedo"].as<glm::vec4>()` at all. They must be visible in every
// TU that reads or writes a glm vector, hence the header.
//
// yaml-cpp customization namespace.
//
// Specializing convert<T> here is how a third-party library is taught to
// handle a type it has never heard of: node.as<glm::vec3>() on load and
// the convert path on save then both work without any call-site changes.
// The alternative -- converting by hand at every component -- is what
// layer 2 below is the residue of.
namespace YAML
{

	template <>
	// Teaches yaml-cpp to treat a glm::vec2 as a 2-element flow sequence,
	// e.g. `[1, 2]`.
	//
	// A vector is a SEQUENCE, not a mapping, so it round-trips as `[x, y]`
	// rather than as named keys. That is what makes these converters
	// non-obvious on read: position carries the meaning, and no key says
	// which component is which.
	//
	// Consequently reordering a vector's components on disk silently
	// reinterprets the value rather than failing -- a vec3 with its y and z
	// swapped loads as valid data that is wrong.
	struct convert<glm::vec2>
	{
		// Builds the Node for a vec2.
		//
		// Uses push_back to build a sequence of the components in order, then
		// sets Flow style so it EMITS as `[x, y]` rather than as a block sequence
		// with dashes. Without the style call a vec3 would be written as three
		// `- value` lines, which is valid YAML but triples the file size for
		// every transform in the scene and makes it unreadable.
		static Node encode(const glm::vec2 &rhs)
		{
			// push_back on a default-constructed Node yields a SEQUENCE, one element
			// per call, in call order. There is no size argument, so the arity is
			// whatever the number of push_backs -- which is why encode below and the
			// size check in decode have to be kept in step by hand.
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			// Flow style makes the emitter write `[x, y]` on one line. Without it the
			// same sequence is written as block style with a dash per element, which
			// is valid YAML but three lines per vector -- a readability and file-size
			// cost multiplied by every transform in the scene.
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		// Reads a Node into a vec2, reporting failure by returning false.
		//
		// Returning bool is yaml-cpp's failure channel for convert -- the caller
		// decides what a bad node means.
		//
		// HAZARD: the false return covers a wrong SHAPE only (not a sequence, or
		// not exactly 2 elements). A correctly-shaped sequence containing a
		// non-float -- `[1, "red"]` -- reaches node[1].as<float>() below, which
		// THROWS YAML::TypedBadConversion. decode does not catch it, so it
		// propagates out of node.as<glm::vec2>() and out of Deserialize. Editing
		// a scene file by hand and mistyping one number therefore throws out of
		// the load path rather than returning false.
		static bool decode(const Node &node, glm::vec2 &rhs)
		{
			// The only guard: wrong KIND, or wrong COUNT. Both return false, which is
			// yaml-cpp's failure channel for convert -- there is no exception and no
			// log here.
			// Note node.size() is compared against an exact 2, so a 3-element sequence
			// fails rather than being truncated. Safer than the alternative.
			if (!node.IsSequence() || node.size() != 2)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			return true;
		}
	};

	template <>
	// glm::vec3 as a 3-element flow sequence, `[x, y, z]`.
	//
	// This is the workhorse: TransformComponent's Translation, Rotation and
	// Scale are all vec3, so almost every entity in a scene round-trips
	// through this converter.
	//
	// NOTE the arity check is strict -- exactly 3 elements, not 'at least
	// 3'. A hand-edited file with an extra element fails rather than
	// ignoring it, which is the safer of the two behaviours.
	struct convert<glm::vec3>
	{
		// Three push_backs for three components, matching the size() != 3 check in
		// decode below. Same manual correspondence as the vec2 converter.
		static Node encode(const glm::vec3 &rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		static bool decode(const Node &node, glm::vec3 &rhs)
		{
			if (!node.IsSequence() || node.size() != 3)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			return true;
		}
	};

	template <>
	// glm::vec4 as a 4-element flow sequence, `[r, g, b, a]`.
	//
	// Used by every Color field in the primitive renderer components and by
	// MaterialComponent's Albedo and EmissionColor.
	//
	// For a COLOR this positional format is a real hazard: `[1, 1, 1, 1]`
	// carries no indication of which element is alpha, so a file edited by
	// hand can shift the channels and produce a wrong colour rather than a
	// load error.
	struct convert<glm::vec4>
	{
		// Four components. The [r, g, b, a] ordering is positional and therefore
		// invisible in the file: nothing on disk says which element is alpha.
		static Node encode(const glm::vec4 &rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.push_back(rhs.w);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		static bool decode(const Node &node, glm::vec4 &rhs)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;

			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			rhs.w = node[3].as<float>();
			return true;
		}
	};

}

namespace Wasteland
{

	// Emitted as a FLOW sequence -- `[x, y]`, not a block list. This is what
	// SceneSerializer writes today, and the on-disk format is not up for
	// renegotiation.
	//
	// The components are emitted as separate `<<` operations rather than in a
	// loop because there is no reflection to iterate a glm vector generically;
	// each overload is written out at its own arity.
	inline YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec2 &v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << YAML::EndSeq;
		return out;
	}

	inline YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec3 &v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << YAML::EndSeq;
		return out;
	}

	inline YAML::Emitter &operator<<(YAML::Emitter &out, const glm::vec4 &v)
	{
		out << YAML::Flow;
		out << YAML::BeginSeq << v.x << v.y << v.z << v.w << YAML::EndSeq;
		return out;
	}

} // namespace Wasteland
