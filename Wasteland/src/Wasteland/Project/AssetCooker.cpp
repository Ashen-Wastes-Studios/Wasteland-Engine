#include "wlpch.h"

#include "Wasteland/Project/AssetCooker.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace Wasteland
{
	uint64_t Fnv1a64(const uint8_t *data, std::size_t size)
	{
		uint64_t hash = 14695981039346656037ull;
		for (std::size_t i = 0; i < size; ++i)
		{
			hash ^= static_cast<uint64_t>(data[i]);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	uint64_t HashBytesOfFile(const std::filesystem::path &path, std::string *read_error)
	{
		// The whole file into memory. A cooked source is an asset, not a giant
		// capture, so this is fine; hashing in chunks would be a constant-factor
		// win that is not worth the code until a project ships gigabytes of
		// individual assets.
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			if (read_error)
				*read_error = "could not open " + path.string();
			return 0;
		}
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		const std::string bytes = buffer.str();
		return Fnv1a64(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
	}

	std::filesystem::path AssetCooker::StampPath(const std::filesystem::path &output)
	{
		return output.string() + ".cook";
	}

	CookResult AssetCooker::CookIfStale(
	    const std::filesystem::path &source,
	    const std::filesystem::path &output,
	    const std::function<bool(const std::filesystem::path &output)> &op)
	{
		CookResult result;

		std::string readError;
		const uint64_t hash = HashBytesOfFile(source, &readError);
		if (!hash)
		{
			result.Ok = false;
			result.Error = "could not read source for cooking: " + readError;
			return result;
		}

		// A stamp that matches is the ONLY thing that skips the cook. Nothing else
		// -- not the output's presence, not its mtime -- is evidence the output is
		// current, because those say nothing about what the source looked like when
		// it was made.
		const std::filesystem::path stampPath = StampPath(output);
		{
			std::ifstream stampIn(stampPath, std::ios::binary);
			if (stampIn)
			{
				std::ostringstream stamped;
				stamped << stampIn.rdbuf();
				uint64_t previous = 0;
				if (!stamped.str().empty())
					previous = static_cast<uint64_t>(
					    std::strtoull(stamped.str().c_str(), nullptr, 16));
				if (previous == hash)
				{
					// Current. The operation must not run -- this is the entire
					// point of the cook.
					result.Cooked = false;
					return result;
				}
			}
		}

		// The cook operation writes the output. On failure the old artifact and
		// stamp are left alone, so a failed cook cannot put the tree into a state
		// where the next run believes the output is fresh.
		if (!op(output))
		{
			result.Ok = false;
			result.Error = "the cook operation failed for " + source.string();
			return result;
		}

		// Write the stamp only AFTER the output exists, so a stamp without an output
		// (the failure mode that would make the next run skip a missing artifact)
		// cannot occur.
		//
		// The write length is the snprintf return (16 hex digits), NOT the size of
		// the stack buffer: the buffer is NUL-terminated, so writing its full size
		// would append the terminator and stack garbage to the hash.
		char hashText[32];
		const int hashDigits = std::snprintf(hashText, sizeof(hashText), "%016llx",
		                                     static_cast<unsigned long long>(hash));
		{
			std::ofstream stampOut(stampPath, std::ios::binary | std::ios::trunc);
			if (!stampOut)
			{
				result.Ok = false;
				result.Error = "could not write the cook stamp for " +
				               output.string();
				return result;
			}
			if (hashDigits > 0)
				stampOut.write(hashText, hashDigits);
			if (!stampOut.good())
			{
				result.Ok = false;
				result.Error = "could not flush the cook stamp for " +
				               output.string();
				return result;
			}
		}

		result.Ok = true;
		result.Cooked = true;
		return result;
	}
}
