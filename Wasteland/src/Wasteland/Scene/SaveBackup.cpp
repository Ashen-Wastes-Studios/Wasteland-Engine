#include "wlpch.h"

#include "Wasteland/Scene/SaveBackup.h"

#include <system_error>

namespace Wasteland
{
	namespace
	{
		std::string Lowercase(const std::string &text)
		{
			std::string lowered = text;
			for (char &character : lowered)
			{
				if (character >= 'A' && character <= 'Z')
				{
					character = static_cast<char>(character - 'A' + 'a');
				}
			}
			return lowered;
		}
	}

	std::filesystem::path BackupPathFor(const SaveBackupRing &ring,
	                                    const std::filesystem::path &target,
	                                    int slot)
	{
		const std::filesystem::path directory =
		    ring.Directory.empty() ? target.parent_path() : ring.Directory;

		std::string stem = target.stem().string();
		std::string extension = target.extension().string();

		// The stem carries a content hash so two different files that happen to
		// share a name in different directories do not share a ring. The hash is of
		// the path, not the content: the ring is per FILE, and a content hash would
		// put two different versions of the same file in different rings.
		const std::size_t hash = std::hash<std::string>{}(Lowercase(target.string()));
		return directory / (stem + "." + std::to_string(hash) + "." +
		                    std::to_string(slot) + extension);
	}

	int SaveBackupRing::Count(const std::filesystem::path &target) const
	{
		if (KeepCount <= 0)
		{
			return 0;
		}

		int found = 0;
		for (int slot = 0; slot < KeepCount; ++slot)
		{
			std::error_code existsError;
			if (std::filesystem::exists(BackupPathFor(*this, target, slot), existsError))
			{
				++found;
			}
		}
		return found;
	}

	bool SaveBackupRing::Rotate(const std::filesystem::path &target,
	                           std::string *error)
	{
		const auto fail = [error](const std::string &message)
		{
			if (error)
			{
				*error = message;
			}
			return false;
		};

		if (KeepCount <= 0)
		{
			return true;
		}

		std::error_code targetError;
		if (!std::filesystem::exists(target, targetError) || targetError)
		{
			// Nothing to preserve. Not an error: the first save of a file has no
			// previous version, and a caller that rotates unconditionally must not be
			// forced to check first.
			return true;
		}

		const std::filesystem::path directory =
		    Directory.empty() ? target.parent_path() : Directory;

		std::error_code createError;
		std::filesystem::create_directories(directory, createError);
		if (createError)
		{
			return fail("could not create the backup directory " + directory.string() +
			            ": " + createError.message());
		}

		// Shift from the OLDEST end backwards so no live backup is overwritten
		// before it has been moved. Walking forwards would destroy slot 0 before
		// copying it to slot 1.
		std::error_code ignored;

		// Whatever is in the last slot falls off the ring.
		const std::filesystem::path oldest = BackupPathFor(*this, target, KeepCount - 1);
		std::filesystem::remove(oldest, ignored);

		for (int slot = KeepCount - 2; slot >= 0; --slot)
		{
			const std::filesystem::path from = BackupPathFor(*this, target, slot);
			std::error_code fromError;
			if (!std::filesystem::exists(from, fromError) || fromError)
			{
				continue;
			}

			const std::filesystem::path to = BackupPathFor(*this, target, slot + 1);
			std::filesystem::remove(to, ignored);
			std::error_code renameError;
			std::filesystem::rename(from, to, renameError);
			if (renameError)
			{
				return fail("could not shift backup " + from.string() + " to " +
				            to.string() + ": " + renameError.message());
			}
		}

		// Slot 0 is free now. Copy rather than rename: the target must survive this
		// call, because the caller publishes the new content by renaming a temporary
		// file over it. Renaming would leave the target missing and turn a crash
		// during the publish into a lost file.
		const std::filesystem::path newest = BackupPathFor(*this, target, 0);
		std::filesystem::copy_file(target, newest,
		                           std::filesystem::copy_options::overwrite_existing,
		                           ignored);
		if (ignored)
		{
			return fail("could not copy " + target.string() + " to " +
			            newest.string() + ": " + ignored.message());
		}

		return true;
	}
}
