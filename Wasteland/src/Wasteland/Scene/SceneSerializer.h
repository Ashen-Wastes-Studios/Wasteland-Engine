// SceneSerializer.h -- writes a Scene to disk and reads it back.
//
// Two format pairs exist. Deserialize is what the editor calls to OPEN a
// project; the *Runtime variants handle a leaner form used at game start, with
// no authoring metadata. They are deliberately separate methods rather than a
// flag, because the two paths read different fields and a flag would invite
// half-populated objects.

#pragma once

#include "Scene.h"

namespace Wasteland
{

	// Reads and writes scenes as YAML.
	//
	// An instance wraps one scene and serializes that scene -- it does not own
	// the scene, it shares it via Ref<Scene>. The point of the class rather
	// than free functions is simply to carry that reference, so a caller
	// holding a serializer has the scene it will write.
	class SceneSerializer
	{
	public:
		// Binds this serializer to a scene.
		//
		// Shares ownership with the scene, so the serializer cannot outlive the
		// scene's other holders.
		//
		// @param scene  The scene to read and write. May be null -- nothing
		//               validates that, and every method below would then
		//               dereference it.
		SceneSerializer(const Ref<Scene> &scene);

		// Writes the scene to a YAML file, in the full authoring format.
		//
		// Returns VOID, so a failed write is silent: an unwritable path, a full
		// disk, or a missing parent directory all look identical to success
		// from the caller's side. There is no error code and no exception.
		//
		// @param filepath  Where to write. A relative path resolves against
		//                  the process's current directory, not the project
		//                  root -- callers wanting project-relative paths
		//                  must build them against Project::GetProjectDirectory.
		void Serialize(const std::string &filepath);

		// Writes the scene in the runtime format: the data needed to play,
		// without the authoring-only fields.
		//
		// Void return, so equally silent about failure as Serialize.
		//
		// @param filepath  Where to write.
		void SerializeRuntime(const std::string &filepath);

		// Reads a scene from a YAML file, in the authoring format.
		//
		// The one method of the four that reports failure, which makes the
		// asymmetry with Serialize a genuine trap: a caller who checks the
		// bool here but has no way to check it there will assume a save
		// succeeded.
		//
		// @param filepath  The file to read.
		// @returns Whether the scene was loaded. False on a missing or
		//          unparseable file, and on a file whose contents are valid
		//          YAML but not a scene.
		bool Deserialize(const std::string &filepath);

		// Reads the runtime format written by SerializeRuntime.
		//
		// Same failure contract as Deserialize.
		//
		// @param filepath  The file to read.
		// @returns Whether the scene was loaded.
		bool DeserializeRuntime(const std::string &filepath);

		// Strips leading and trailing whitespace, tabs, carriage returns and
		// newlines.
		//
		// Public and static so the YAML tokenizer can share it -- and so
		// callers parsing other text can too, though it is not really a
		// general-purpose trim (it does not handle other whitespace, and does
		// not trim in place).
		//
		// @param str  The string to trim. Copied, not modified.
		// @returns The trimmed substring, or an EMPTY STRING if the input was
		//          entirely whitespace. Note that this is indistinguishable
		//          from an input that really was empty -- so a line of nothing
		//          but spaces and a line of zero length take the same path
		//          through the parser.
		static std::string Trim(const std::string &str)
		{
			// First character that is not whitespace. npos means the whole
			// string was whitespace, hence the empty return -- without this
			// check, last - first + 1 would underflow to a huge length.
			size_t first = str.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return "";

			// Last character that is not whitespace. Searching the whole
			// string rather than from `first` is safe and simpler, since
			// anything before first is whitespace by definition.
			size_t last = str.find_last_not_of(" \t\r\n");

			// Inclusive of both ends, hence the + 1. If first > last -- only
			// possible for an all-whitespace string, already handled above --
			// the unsigned subtraction would wrap, which the early return
			// prevents.
			return str.substr(first, (last - first + 1));
		}

	private:
		// The scene this serializer reads and writes.
		//
		// Shared, not unique: the serializer is a view onto a scene the rest
		// of the engine also holds, and a scene must outlive any serializer
		// pointing at it.
		Ref<Scene> m_Scene;
	};

}