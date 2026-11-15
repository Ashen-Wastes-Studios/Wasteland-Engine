// SaveBackup.h -- a rotation ring for saved files.
//
// WHY THIS EXISTS
// SceneSerializer::Save already publishes atomically: it writes a temporary file
// and renames it over the target, so a crash mid-write cannot leave a
// half-written scene behind. That protects against a CORRUPT file. It does not
// protect against the other failure: the editor crashes AFTER the rename, the
// user has already moved on, and the previous version is simply gone. There is
// no undo across a process death.
//
// The fix is a backup ring: before the new content replaces the target, the old
// target is moved into slot 0, slot 0 into slot 1, and so on, dropping whatever
// falls off the end. KeepCount is small (5 by default) because the ring exists to
// survive a crash, not to be a version history -- that is what source control is
// for, and the plan is explicit that a backup is the safety net for the gap
// between two commits.
//
// WHY THE DIRECTORY IS EXPLICIT
// A `backups/` folder next to the scenes would show up in the Content Browser as
// an asset, be drag-dropped into scenes and be serialized into .wastescene files.
// The same reasoning put the thumbnail cache outside the project tree. The caller
// therefore names the directory; the test names a temp directory. This type has
// no dependency on Project being open.
//
// WHY THE RING IS INDEXED RATHER THAN TIMESTAMPED
// A timestamp name can collide when two saves land in the same tick, and a
// collision would silently overwrite a backup. Slots cannot collide: slot i is
// always the i-th most recent version, and Count() can state that exactly.

#pragma once

#include <filesystem>
#include <string>

namespace Wasteland
{
	struct SaveBackupRing
	{
		// How many previous versions to keep. 0 disables the ring.
		int KeepCount = 5;

		// Where the backups live. Empty means "next to the target", which is only
		// appropriate for a scratch file -- the editor passes the user config
		// directory so the backups never enter the project tree.
		std::filesystem::path Directory;

		// How many backups currently exist for `target`, capped at KeepCount.
		int Count(const std::filesystem::path &target) const;

		// Moves the current `target` into slot 0, shifting the older copies up and
		// dropping the oldest beyond KeepCount.
		//
		// Call this BEFORE the new content replaces `target`. Rotating after the
		// publish would back up the NEW file, and the ring would hold nothing but
		// copies of the current version.
		//
		// A missing target is not an error: the first save of a file has no previous
		// version to preserve.
		//
		// Returns false with a message on a real failure. A backup failure must NOT
		// abort the save -- losing the backup is bad, losing the user's edit because
		// the backup failed is worse. The caller logs and continues.
		bool Rotate(const std::filesystem::path &target, std::string *error);
	};

	// The path a given slot would occupy for `target`. Slot 0 is the newest.
	// Exposed so a test can assert the naming without guessing it, and so a UI can
	// offer "restore previous version" without re-deriving it.
	std::filesystem::path BackupPathFor(const SaveBackupRing &ring,
	                                    const std::filesystem::path &target,
	                                    int slot);
}
