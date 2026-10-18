#include "wlpch.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "Wasteland/Terrain/Terrain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Wasteland::Terrain
{
namespace
{
constexpr std::size_t kMaximumSplatRules = 64;
constexpr std::uint32_t kMaximumNoiseOctaves = 12;
constexpr double kMaximumSplatWeight = 1.0e6;

void SetError(std::string* error, const std::string& message)
{
	if (error)
		*error = message;
}

bool IsStableId(const std::string& value)
{
	if (value.empty() || value.size() > kMaximumTerrainStableIdBytes)
		return false;
	for (unsigned char c : value)
	{
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' ||
			c == '/' || c == ':' ))
			return false;
	}
	return true;
}

std::string EncodeStableIdForPath(std::string_view value)
{
	constexpr char kHexDigits[] = "0123456789abcdef";
	std::string encoded;
	encoded.reserve(value.size() * 2);
	for (unsigned char byte : value)
	{
		encoded.push_back(kHexDigits[byte >> 4]);
		encoded.push_back(kHexDigits[byte & 0x0f]);
	}
	return encoded;
}

std::string EncodeChunkCoordinateForPath(std::int64_t coordinate)
{
	constexpr char kHexDigits[] = "0123456789abcdef";
	std::uint64_t bits = static_cast<std::uint64_t>(coordinate);
	std::string encoded(16, '0');
	for (std::size_t index = encoded.size(); index > 0; --index)
	{
		encoded[index - 1] = kHexDigits[bits & 0x0f];
		bits >>= 4;
	}
	return encoded;
}

bool ParseChunkCoordinateHex(std::string_view encoded, std::int64_t& outCoordinate)
{
	if (encoded.size() != 16)
		return false;
	std::uint64_t bits = 0;
	for (unsigned char digit : encoded)
	{
		std::uint64_t value = 0;
		if (digit >= '0' && digit <= '9')
			value = digit - '0';
		else if (digit >= 'a' && digit <= 'f')
			value = digit - 'a' + 10;
		else if (digit >= 'A' && digit <= 'F')
			value = digit - 'A' + 10;
		else
			return false;
		bits = (bits << 4) | value;
	}

	const std::uint64_t maximumSigned = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
	if (bits <= maximumSigned)
		outCoordinate = static_cast<std::int64_t>(bits);
	else
		outCoordinate = -1 - static_cast<std::int64_t>(~bits);
	return true;
}

bool ParseCanonicalChunkFilename(const std::string& filename,
	std::int64_t& outChunkX, std::int64_t& outChunkZ)
{
	constexpr std::string_view kPrefix = "chunk-";
	constexpr std::string_view kExtension = ".wlterrainstate";
	constexpr std::size_t kCoordinateDigits = 16;
	constexpr std::size_t kExpectedLength = kPrefix.size() + kCoordinateDigits + 1 +
		kCoordinateDigits + kExtension.size();
	if (filename.size() != kExpectedLength || filename.compare(0, kPrefix.size(), kPrefix) != 0 ||
		filename[kPrefix.size() + kCoordinateDigits] != '-' ||
		filename.compare(filename.size() - kExtension.size(), kExtension.size(), kExtension) != 0)
		return false;

	const std::size_t xStart = kPrefix.size();
	const std::size_t zStart = xStart + kCoordinateDigits + 1;
	return ParseChunkCoordinateHex(std::string_view(filename).substr(xStart, kCoordinateDigits), outChunkX) &&
		ParseChunkCoordinateHex(std::string_view(filename).substr(zStart, kCoordinateDigits), outChunkZ);
}

bool HasCanonicalTerrainStateExtension(const std::string& filename)
{
	constexpr std::string_view kExtension = ".wlterrainstate";
	return filename.size() >= kExtension.size() &&
		filename.compare(filename.size() - kExtension.size(), kExtension.size(), kExtension) == 0;
}

#if defined(_WIN32)
bool PrepareWindowsFileIoPath(const std::filesystem::path& path,
	std::filesystem::path& outPath, std::error_code& error)
{
	error.clear();
	outPath = path;
	if (path.empty())
		return true;
	const std::wstring original = path.native();
	if (original.rfind(L"\\\\?\\", 0) == 0 || original.rfind(L"\\\\.\\", 0) == 0 ||
		original.size() < MAX_PATH)
		return true;

	std::filesystem::path absolutePath = path.lexically_normal();
	absolutePath.make_preferred();
	if (!absolutePath.is_absolute())
	{
		if (!absolutePath.has_root_name() && !absolutePath.has_root_directory())
		{
			const std::filesystem::path currentDirectory = std::filesystem::current_path(error);
			if (error)
				return false;
			absolutePath = (currentDirectory / absolutePath).lexically_normal();
		}
		else
		{
			absolutePath = std::filesystem::absolute(absolutePath, error);
			if (error)
				return false;
			absolutePath.make_preferred();
		}
	}

	std::wstring native = absolutePath.native();
	if (native.rfind(L"\\\\?\\", 0) == 0)
	{
		outPath = std::move(absolutePath);
		return true;
	}
	if (native.size() < MAX_PATH)
	{
		outPath = std::move(absolutePath);
		return true;
	}

	if (native.rfind(L"\\\\", 0) == 0)
		native = L"\\\\?\\UNC\\" + native.substr(2);
	else if (native.size() >= 3 && native[1] == L':' && native[2] == L'\\')
		native = L"\\\\?\\" + native;
	else
	{
		error = std::make_error_code(std::errc::invalid_argument);
		return false;
	}

	outPath = std::filesystem::path(std::move(native));
	return true;
}
#endif

double CanonicalZero(double value) noexcept
{
	return value == 0.0 ? 0.0 : value;
}

bool ReadBoundedTextFile(const std::filesystem::path& path, std::size_t maximumBytes,
	std::string_view label, std::string& outText, std::string* error)
{
	std::filesystem::path ioPath = path;
#if defined(_WIN32)
	std::error_code pathError;
	if (!PrepareWindowsFileIoPath(path, ioPath, pathError))
	{
		SetError(error, "could not prepare terrain file path: " + pathError.message());
		return false;
	}
#endif
	std::ifstream input(ioPath, std::ios::binary);
	if (!input)
	{
		SetError(error, "could not open " + std::string(label) + ": " + path.string());
		return false;
	}

	std::string contents(maximumBytes + 1, '\0');
	input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
	const std::streamsize bytesRead = input.gcount();
	if (input.bad())
	{
		SetError(error, "could not read " + std::string(label) + ": " + path.string());
		return false;
	}
	if (static_cast<std::size_t>(bytesRead) > maximumBytes)
	{
		SetError(error, std::string(label) + " exceeds the " + std::to_string(maximumBytes) + "-byte limit");
		return false;
	}
	contents.resize(static_cast<std::size_t>(bytesRead));
	outText = std::move(contents);
	return true;
}

std::uint64_t HashBytes(std::string_view text)
{
	std::uint64_t hash = 14695981039346656037ULL;
	for (unsigned char byte : text)
	{
		hash ^= byte;
		hash *= 1099511628211ULL;
	}
	return hash;
}

std::string HashString(std::string_view text)
{
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << std::hex << std::setfill('0') << std::setw(16) << HashBytes(text);
	return output.str();
}

std::uint64_t Mix64(std::uint64_t value)
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

std::uint64_t NoiseHash(const TerrainAsset& asset, std::uint32_t octave,
	std::int64_t latticeX, std::int64_t latticeZ)
{
	const std::uint64_t generatorHash = HashBytes(asset.GeneratorId);
	std::uint64_t key = Mix64(asset.Seed ^ generatorHash);
	key = Mix64(key ^ static_cast<std::uint64_t>(asset.GeneratorVersion));
	key = Mix64(key ^ static_cast<std::uint64_t>(octave));
	key = Mix64(key ^ static_cast<std::uint64_t>(latticeX));
	return Mix64(key ^ (static_cast<std::uint64_t>(latticeZ) + 0x517cc1b727220a95ULL));
}

double HashValue(const TerrainAsset& asset, std::uint32_t octave,
	std::int64_t latticeX, std::int64_t latticeZ)
{
	const std::uint64_t bits = NoiseHash(asset, octave, latticeX, latticeZ) >> 11;
	const double unit = static_cast<double>(bits) * (1.0 / 9007199254740992.0);
	return unit * 2.0 - 1.0;
}

std::int64_t FloorDivide(std::int64_t value, std::int64_t divisor)
{
	std::int64_t quotient = value / divisor;
	const std::int64_t remainder = value % divisor;
	if (remainder < 0)
		--quotient;
	return quotient;
}

std::int64_t PositiveRemainder(std::int64_t value, std::int64_t divisor)
{
	std::int64_t remainder = value % divisor;
	if (remainder < 0)
		remainder += divisor;
	return remainder;
}

bool ValueNoise(const TerrainAsset& asset, std::uint32_t octave, std::int64_t x, std::int64_t z,
	std::int64_t cellSize, double& result)
{
	const std::int64_t cellX = FloorDivide(x, cellSize);
	const std::int64_t cellZ = FloorDivide(z, cellSize);
	const std::int64_t remainderX = PositiveRemainder(x, cellSize);
	const std::int64_t remainderZ = PositiveRemainder(z, cellSize);
	const double fractionX = static_cast<double>(remainderX) / static_cast<double>(cellSize);
	const double fractionZ = static_cast<double>(remainderZ) / static_cast<double>(cellSize);
	const double smoothX = fractionX * fractionX * (3.0 - 2.0 * fractionX);
	const double smoothZ = fractionZ * fractionZ * (3.0 - 2.0 * fractionZ);

	if ((cellX == (std::numeric_limits<std::int64_t>::max)() && fractionX != 0.0) ||
		(cellZ == (std::numeric_limits<std::int64_t>::max)() && fractionZ != 0.0))
		return false;
	const std::int64_t nextX = fractionX == 0.0 ? cellX : cellX + 1;
	const std::int64_t nextZ = fractionZ == 0.0 ? cellZ : cellZ + 1;
	const double a = HashValue(asset, octave, cellX, cellZ);
	const double b = HashValue(asset, octave, nextX, cellZ);
	const double c = HashValue(asset, octave, cellX, nextZ);
	const double d = HashValue(asset, octave, nextX, nextZ);
	const double top = a + (b - a) * smoothX;
	const double bottom = c + (d - c) * smoothX;
	result = top + (bottom - top) * smoothZ;
	return std::isfinite(result);
}

bool ParseLine(const std::string& line, const std::string& expectedKey, std::istringstream& fields)
{
	fields.clear();
	fields.str(line);
	std::string key;
	return static_cast<bool>(fields >> key) && key == expectedKey;
}

bool HasOnlyWhitespace(std::istringstream& fields)
{
	fields >> std::ws;
	return fields.eof();
}

bool ReadLine(std::istringstream& input, std::string& line)
{
	if (!std::getline(input, line))
		return false;
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	return true;
}

bool HasValidHeightSamplingParameters(const TerrainAsset& asset)
{
	return asset.SchemaVersion == kTerrainSchemaVersion && IsStableId(asset.GeneratorId) &&
		asset.GeneratorVersion != 0 && asset.ChunkResolution == kTerrainChunkCells &&
		std::isfinite(asset.OriginY) && std::isfinite(asset.BaseHeight) &&
		std::isfinite(asset.HeightAmplitude) && asset.HeightAmplitude >= 0.0 &&
		std::isfinite(asset.NoiseFrequency) && asset.NoiseFrequency >= 1.0e-9 && asset.NoiseFrequency <= 1.0 &&
		asset.NoiseOctaves > 0 && asset.NoiseOctaves <= kMaximumNoiseOctaves &&
		std::isfinite(asset.Lacunarity) && asset.Lacunarity >= 1.0 && asset.Lacunarity <= 4.0 &&
		std::isfinite(asset.Persistence) && asset.Persistence >= 0.0 && asset.Persistence <= 1.0;
}

bool HasValidSplatRules(const TerrainAsset& asset)
{
	if (asset.SplatRules.empty() || asset.SplatRules.size() > kMaximumSplatRules)
		return false;
	for (const SplatRule& rule : asset.SplatRules)
	{
		const double heightSpan = rule.MaximumHeight - rule.MinimumHeight;
		if (!IsStableId(rule.MaterialAssetId) || !std::isfinite(rule.MinimumHeight) ||
			!std::isfinite(rule.MaximumHeight) || rule.MinimumHeight > rule.MaximumHeight ||
			!std::isfinite(heightSpan) || !std::isfinite(rule.MinimumSlopeDegrees) ||
			!std::isfinite(rule.MaximumSlopeDegrees) || rule.MinimumSlopeDegrees < 0.0 ||
			rule.MaximumSlopeDegrees > 90.0 || rule.MinimumSlopeDegrees > rule.MaximumSlopeDegrees ||
			!std::isfinite(rule.Weight) || rule.Weight <= 0.0 || rule.Weight > kMaximumSplatWeight)
			return false;
	}
	return true;
}

bool IsValidEditSet(const TerrainAsset& asset, const TerrainChunkEdits& edits, std::string* error)
{
	if (edits.TerrainSchemaVersion != kTerrainSchemaVersion)
	{
		SetError(error, "terrain edit schema does not match the supported schema");
		return false;
	}
	if (edits.WorldId != asset.WorldId || edits.BaseFingerprint != TerrainAssetFingerprint(asset))
	{
		SetError(error, "terrain edit world or base fingerprint does not match the asset");
		return false;
	}
	for (const auto& delta : edits.HeightDeltas)
	{
		if (delta.first.first < 0 || delta.first.first > kTerrainChunkCells ||
			delta.first.second < 0 || delta.first.second > kTerrainChunkCells || !std::isfinite(delta.second))
		{
			SetError(error, "height delta has an invalid base-lattice coordinate or value");
			return false;
		}
	}
	for (const auto& overrideValue : edits.MaterialOverrides)
	{
		const auto x = overrideValue.first.first;
		const auto z = overrideValue.first.second;
		if (x < 0 || x >= kTerrainChunkCells || z < 0 || z >= kTerrainChunkCells)
		{
			SetError(error, "material override is outside the base-cell extent");
			return false;
		}
		const auto found = std::find_if(asset.SplatRules.begin(), asset.SplatRules.end(),
			[&overrideValue](const SplatRule& rule) { return rule.MaterialAssetId == overrideValue.second; });
		if (found == asset.SplatRules.end())
		{
			SetError(error, "material override references an asset not declared by the terrain source");
			return false;
		}
	}
	return true;
}

std::string SerializeEditsPrefix(const TerrainChunkEdits& edits)
{
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << std::setprecision(std::numeric_limits<double>::max_digits10);
	output << "WL_TERRAIN_STATE " << kTerrainStateSchemaVersion << '\n';
	output << "terrain_schema " << edits.TerrainSchemaVersion << '\n';
	output << "world_id " << std::quoted(edits.WorldId) << '\n';
	output << "base_fingerprint " << std::quoted(edits.BaseFingerprint) << '\n';
	output << "chunk " << edits.ChunkX << ' ' << edits.ChunkZ << '\n';
	output << "delta_count " << edits.HeightDeltas.size() << '\n';
	for (const auto& delta : edits.HeightDeltas)
		output << "delta " << delta.first.first << ' ' << delta.first.second << ' ' << delta.second << '\n';
	output << "override_count " << edits.MaterialOverrides.size() << '\n';
	for (const auto& overrideValue : edits.MaterialOverrides)
		output << "override " << overrideValue.first.first << ' ' << overrideValue.first.second << ' '
			<< std::quoted(overrideValue.second) << '\n';
	return output.str();
}

std::string SerializeEdits(const TerrainChunkEdits& edits)
{
	const std::string prefix = SerializeEditsPrefix(edits);
	return prefix + "checksum " + HashString(prefix) + '\n';
}

bool ParseEdits(const std::string& text, const TerrainAsset& asset,
	std::int64_t expectedChunkX, std::int64_t expectedChunkZ, TerrainChunkEdits& outEdits,
	std::string* error)
{
	if (text.size() > kMaximumTerrainStateBytes)
	{
		SetError(error, "terrain sidecar exceeds the configured byte limit");
		return false;
	}
	const std::size_t checksumMarker = text.rfind("checksum ");
	if (checksumMarker == std::string::npos || (checksumMarker != 0 && text[checksumMarker - 1] != '\n'))
	{
		SetError(error, "terrain sidecar has no checksum record");
		return false;
	}
	const std::string prefix = text.substr(0, checksumMarker);
	const std::size_t checksumEnd = text.find('\n', checksumMarker);
	const std::string checksumLine = text.substr(checksumMarker,
		checksumEnd == std::string::npos ? std::string::npos : checksumEnd - checksumMarker);
	std::istringstream checksumFields(checksumLine);
	std::string checksumKey;
	std::string declaredChecksum;
	if (!(checksumFields >> checksumKey >> declaredChecksum) || checksumKey != "checksum" ||
		!HasOnlyWhitespace(checksumFields) || declaredChecksum != HashString(prefix) ||
		(checksumEnd != std::string::npos && !std::all_of(text.begin() + static_cast<std::ptrdiff_t>(checksumEnd + 1), text.end(),
			[](unsigned char c) { return std::isspace(c) != 0; })))
	{
		SetError(error, "terrain sidecar checksum is invalid");
		return false;
	}

	std::istringstream input(prefix);
	input.imbue(std::locale::classic());
	std::string line;
	std::istringstream fields;
	std::uint32_t sidecarVersion = 0;
	if (!ReadLine(input, line) || !ParseLine(line, "WL_TERRAIN_STATE", fields) || !(fields >> sidecarVersion) ||
		!HasOnlyWhitespace(fields) || sidecarVersion != kTerrainStateSchemaVersion)
	{
		SetError(error, "terrain sidecar schema is malformed or unsupported");
		return false;
	}

	TerrainChunkEdits parsed;
	std::uint32_t terrainSchema = 0;
	if (!ReadLine(input, line) || !ParseLine(line, "terrain_schema", fields) || !(fields >> terrainSchema) || !HasOnlyWhitespace(fields))
	{
		SetError(error, "terrain sidecar terrain schema record is malformed");
		return false;
	}
	parsed.TerrainSchemaVersion = terrainSchema;
	if (!ReadLine(input, line) || !ParseLine(line, "world_id", fields) || !(fields >> std::quoted(parsed.WorldId)) || !HasOnlyWhitespace(fields))
	{
		SetError(error, "terrain sidecar world id record is malformed");
		return false;
	}
	if (!ReadLine(input, line) || !ParseLine(line, "base_fingerprint", fields) || !(fields >> std::quoted(parsed.BaseFingerprint)) || !HasOnlyWhitespace(fields))
	{
		SetError(error, "terrain sidecar fingerprint record is malformed");
		return false;
	}
	if (!ReadLine(input, line) || !ParseLine(line, "chunk", fields) || !(fields >> parsed.ChunkX >> parsed.ChunkZ) || !HasOnlyWhitespace(fields))
	{
		SetError(error, "terrain sidecar chunk key record is malformed");
		return false;
	}

	std::size_t deltaCount = 0;
	if (!ReadLine(input, line) || !ParseLine(line, "delta_count", fields) || !(fields >> deltaCount) ||
		!HasOnlyWhitespace(fields) || deltaCount > static_cast<std::size_t>(kTerrainChunkSamples) * kTerrainChunkSamples)
	{
		SetError(error, "terrain sidecar delta count is malformed or out of range");
		return false;
	}
	for (std::size_t index = 0; index < deltaCount; ++index)
	{
		std::int32_t x = 0;
		std::int32_t z = 0;
		double delta = 0.0;
		if (!ReadLine(input, line) || !ParseLine(line, "delta", fields) || !(fields >> x >> z >> delta) || !HasOnlyWhitespace(fields) ||
			!parsed.HeightDeltas.emplace(std::make_pair(x, z), delta).second)
		{
			SetError(error, "terrain sidecar contains a malformed or duplicate height delta");
			return false;
		}
	}

	std::size_t overrideCount = 0;
	if (!ReadLine(input, line) || !ParseLine(line, "override_count", fields) || !(fields >> overrideCount) ||
		!HasOnlyWhitespace(fields) || overrideCount > static_cast<std::size_t>(kTerrainChunkCells) * kTerrainChunkCells)
	{
		SetError(error, "terrain sidecar material override count is malformed or out of range");
		return false;
	}
	for (std::size_t index = 0; index < overrideCount; ++index)
	{
		std::int32_t x = 0;
		std::int32_t z = 0;
		std::string material;
		if (!ReadLine(input, line) || !ParseLine(line, "override", fields) ||
			!(fields >> x >> z >> std::quoted(material)) || !HasOnlyWhitespace(fields) ||
			!parsed.MaterialOverrides.emplace(std::make_pair(x, z), std::move(material)).second)
		{
			SetError(error, "terrain sidecar contains a malformed or duplicate material override");
			return false;
		}
	}
	if (ReadLine(input, line))
	{
		SetError(error, "terrain sidecar has unexpected records before its checksum");
		return false;
	}
	if (parsed.ChunkX != expectedChunkX || parsed.ChunkZ != expectedChunkZ)
	{
		SetError(error, "terrain sidecar chunk key does not match the requested chunk");
		return false;
	}
	if (!IsValidEditSet(asset, parsed, error))
		return false;
	parsed.Dirty = false;
	outEdits = std::move(parsed);
	return true;
}

bool AtomicReplace(const std::filesystem::path& path, const std::string& bytes, std::string* error)
{
	if (path.empty())
	{
		SetError(error, "terrain file path is empty");
		return false;
	}
	std::filesystem::path ioPath = path;
#if defined(_WIN32)
	std::error_code pathError;
	if (!PrepareWindowsFileIoPath(path, ioPath, pathError))
	{
		SetError(error, "could not prepare terrain file path: " + pathError.message());
		return false;
	}
#endif
	std::error_code filesystemError;
	const auto parent = ioPath.parent_path();
	if (!parent.empty())
	{
		std::filesystem::create_directories(parent, filesystemError);
		if (filesystemError)
		{
			SetError(error, "could not create terrain file directory: " + filesystemError.message());
			return false;
		}
	}

	static std::atomic<std::uint64_t> temporaryCounter{ 0 };
	const auto tick = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const auto suffix = std::to_string(tick) + "." + std::to_string(temporaryCounter.fetch_add(1));
	std::filesystem::path temporary = ioPath;
	temporary += ".tmp." + suffix;
	// Keep the temp as a sibling before preparation; it may need an extended prefix while the target stays unprefixed.
	std::filesystem::path temporaryIoPath = temporary;
#if defined(_WIN32)
	std::error_code temporaryPathError;
	if (!PrepareWindowsFileIoPath(temporary, temporaryIoPath, temporaryPathError))
	{
		SetError(error, "could not prepare temporary terrain file path: " + temporaryPathError.message());
		return false;
	}
#endif
	{
		std::ofstream output(temporaryIoPath, std::ios::binary | std::ios::trunc);
		if (!output)
		{
			SetError(error, "could not open temporary terrain file");
			return false;
		}
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if (!output)
		{
			output.close();
			std::filesystem::remove(temporaryIoPath, filesystemError);
			SetError(error, "could not flush temporary terrain file");
			return false;
		}
		output.close();
		if (!output)
		{
			std::filesystem::remove(temporaryIoPath, filesystemError);
			SetError(error, "could not close temporary terrain file cleanly");
			return false;
		}
	}

#if defined(_WIN32)
	if (!::MoveFileExW(temporaryIoPath.c_str(), ioPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
	{
		const DWORD code = ::GetLastError();
		std::filesystem::remove(temporaryIoPath, filesystemError);
		SetError(error, "atomic terrain file replace failed with Windows error " + std::to_string(code));
		return false;
	}
#else
	std::filesystem::rename(temporaryIoPath, ioPath, filesystemError);
	if (filesystemError)
	{
		const std::string replaceError = filesystemError.message();
		std::filesystem::remove(temporaryIoPath, filesystemError);
		SetError(error, "atomic terrain file replace failed: " + replaceError);
		return false;
	}
#endif
	return true;
}
}

bool CreateDefaultTerrainAsset(const std::string& worldId, TerrainAsset& outAsset,
	std::string* error)
{
	if (error)
		error->clear();
	TerrainAsset candidate;
	candidate.WorldId = worldId;
	candidate.GeneratorId = "value-noise";
	candidate.SplatRules = {
		{ "materials/default", -1000000.0, 1000000.0, 0.0, 90.0, 1.0 },
	};
	if (!ValidateTerrainAsset(candidate, error))
		return false;
	outAsset = std::move(candidate);
	return true;
}

bool ValidateTerrainAsset(const TerrainAsset& asset, std::string* error)
{
	if (asset.SchemaVersion != kTerrainSchemaVersion)
	{
		SetError(error, "unsupported terrain asset schema");
		return false;
	}
	if (!IsStableId(asset.WorldId) || !IsStableId(asset.GeneratorId))
	{
		SetError(error, "world and generator ids must be non-empty stable identifiers");
		return false;
	}
	if (asset.GeneratorVersion == 0 || asset.ChunkResolution != kTerrainChunkCells)
	{
		SetError(error, "generator version must be nonzero and initial chunk resolution must be 64 cells");
		return false;
	}
	if (!std::isfinite(asset.OriginX) || !std::isfinite(asset.OriginZ) ||
		!std::isfinite(asset.OriginY) || !std::isfinite(asset.SampleSpacing) || asset.SampleSpacing <= 0.0 ||
		!std::isfinite(asset.BaseHeight) || !std::isfinite(asset.HeightAmplitude) ||
		asset.HeightAmplitude < 0.0 || !std::isfinite(asset.NoiseFrequency) ||
		asset.NoiseFrequency < 1.0e-9 || asset.NoiseFrequency > 1.0 ||
		asset.NoiseOctaves == 0 || asset.NoiseOctaves > kMaximumNoiseOctaves ||
		!std::isfinite(asset.Lacunarity) || asset.Lacunarity < 1.0 || asset.Lacunarity > 4.0 ||
		!std::isfinite(asset.Persistence) || asset.Persistence < 0.0 || asset.Persistence > 1.0)
	{
		SetError(error, "terrain origin, spacing or height/noise parameters are invalid");
		return false;
	}
	if (!HasValidSplatRules(asset))
	{
		SetError(error, "terrain asset must declare one to 64 splat rules with valid materials, ranges and weights");
		return false;
	}
	std::unordered_set<std::string> materialIds;
	materialIds.reserve(asset.SplatRules.size());
	for (const SplatRule& rule : asset.SplatRules)
		if (!materialIds.insert(rule.MaterialAssetId).second)
		{
			SetError(error, "terrain splat material asset ids must be unique");
			return false;
		}
	return true;
}

std::string SerializeTerrainAsset(const TerrainAsset& asset)
{
	std::ostringstream output;
	output.imbue(std::locale::classic());
	output << std::setprecision(std::numeric_limits<double>::max_digits10);
	output << "WL_TERRAIN " << asset.SchemaVersion << '\n';
	output << "world_id " << std::quoted(asset.WorldId) << '\n';
	output << "generator_id " << std::quoted(asset.GeneratorId) << '\n';
	output << "generator_version " << asset.GeneratorVersion << '\n';
	output << "seed " << asset.Seed << '\n';
	output << "origin " << CanonicalZero(asset.OriginX) << ' ' << CanonicalZero(asset.OriginY) << ' '
		<< CanonicalZero(asset.OriginZ) << '\n';
	output << "sample_spacing " << CanonicalZero(asset.SampleSpacing) << '\n';
	output << "chunk_resolution " << asset.ChunkResolution << '\n';
	output << "height " << CanonicalZero(asset.BaseHeight) << ' ' << CanonicalZero(asset.HeightAmplitude) << '\n';
	output << "noise " << CanonicalZero(asset.NoiseFrequency) << ' ' << asset.NoiseOctaves << ' '
		<< CanonicalZero(asset.Lacunarity) << ' ' << CanonicalZero(asset.Persistence) << '\n';
	output << "splat_count " << asset.SplatRules.size() << '\n';
	for (const SplatRule& rule : asset.SplatRules)
		output << "splat " << std::quoted(rule.MaterialAssetId) << ' ' << CanonicalZero(rule.MinimumHeight) << ' '
			<< CanonicalZero(rule.MaximumHeight) << ' ' << CanonicalZero(rule.MinimumSlopeDegrees) << ' '
			<< CanonicalZero(rule.MaximumSlopeDegrees) << ' ' << CanonicalZero(rule.Weight) << '\n';
	output << "end\n";
	return output.str();
}

bool ParseTerrainAsset(const std::string& text, TerrainAsset& outAsset, std::string* error)
{
	if (text.size() > kMaximumTerrainAssetBytes)
	{
		SetError(error, "terrain asset exceeds the configured byte limit");
		return false;
	}
	std::istringstream input(text);
	input.imbue(std::locale::classic());
	std::string line;
	std::istringstream fields;
	TerrainAsset parsed;
	if (!ReadLine(input, line) || !ParseLine(line, "WL_TERRAIN", fields) || !(fields >> parsed.SchemaVersion) || !HasOnlyWhitespace(fields))
	{
		SetError(error, "terrain asset header is malformed");
		return false;
	}
	if (parsed.SchemaVersion != kTerrainSchemaVersion)
	{
		SetError(error, "unsupported terrain asset schema");
		return false;
	}
	if (!ReadLine(input, line) || !ParseLine(line, "world_id", fields) || !(fields >> std::quoted(parsed.WorldId)) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "generator_id", fields) || !(fields >> std::quoted(parsed.GeneratorId)) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "generator_version", fields) || !(fields >> parsed.GeneratorVersion) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "seed", fields) || !(fields >> parsed.Seed) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "origin", fields) || !(fields >> parsed.OriginX >> parsed.OriginY >> parsed.OriginZ) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "sample_spacing", fields) || !(fields >> parsed.SampleSpacing) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "chunk_resolution", fields) || !(fields >> parsed.ChunkResolution) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "height", fields) || !(fields >> parsed.BaseHeight >> parsed.HeightAmplitude) || !HasOnlyWhitespace(fields) ||
		!ReadLine(input, line) || !ParseLine(line, "noise", fields) || !(fields >> parsed.NoiseFrequency >> parsed.NoiseOctaves >> parsed.Lacunarity >> parsed.Persistence) || !HasOnlyWhitespace(fields))
	{
		SetError(error, "terrain asset is missing or has a malformed required field");
		return false;
	}
	std::size_t splatCount = 0;
	if (!ReadLine(input, line) || !ParseLine(line, "splat_count", fields) || !(fields >> splatCount) ||
		!HasOnlyWhitespace(fields) || splatCount > kMaximumSplatRules)
	{
		SetError(error, "terrain asset splat count is malformed or out of range");
		return false;
	}
	parsed.SplatRules.reserve(splatCount);
	for (std::size_t index = 0; index < splatCount; ++index)
	{
		SplatRule rule;
		if (!ReadLine(input, line) || !ParseLine(line, "splat", fields) ||
			!(fields >> std::quoted(rule.MaterialAssetId) >> rule.MinimumHeight >> rule.MaximumHeight >>
				rule.MinimumSlopeDegrees >> rule.MaximumSlopeDegrees >> rule.Weight) || !HasOnlyWhitespace(fields))
		{
			SetError(error, "terrain asset contains a malformed splat rule");
			return false;
		}
		parsed.SplatRules.push_back(std::move(rule));
	}
	if (!ReadLine(input, line) || line != "end" || ReadLine(input, line))
	{
		SetError(error, "terrain asset has unexpected or trailing records");
		return false;
	}
	if (!ValidateTerrainAsset(parsed, error))
		return false;
	outAsset = std::move(parsed);
	return true;
}

bool LoadTerrainAsset(const std::filesystem::path& path, TerrainAsset& outAsset, std::string* error)
{
	std::string contents;
	if (!ReadBoundedTextFile(path, kMaximumTerrainAssetBytes, "terrain asset", contents, error))
		return false;
	return ParseTerrainAsset(contents, outAsset, error);
}

bool SaveTerrainAsset(const std::filesystem::path& path, const TerrainAsset& asset, std::string* error)
{
	if (!ValidateTerrainAsset(asset, error))
		return false;
	return AtomicReplace(path, SerializeTerrainAsset(asset), error);
}

std::string TerrainAssetFingerprint(const TerrainAsset& asset)
{
	return HashString(SerializeTerrainAsset(asset));
}

std::optional<std::int64_t> WorldToSampleLattice(double worldCoordinate, double origin, double spacing) noexcept
{
	if (!std::isfinite(worldCoordinate) || !std::isfinite(origin) || !std::isfinite(spacing) || spacing <= 0.0)
		return std::nullopt;
	const double sample = std::floor((worldCoordinate - origin) / spacing);
	const double minimum = -9223372036854775808.0;
	const double maximumExclusive = 9223372036854775808.0;
	if (!std::isfinite(sample) || sample < minimum || sample >= maximumExclusive)
		return std::nullopt;
	return static_cast<std::int64_t>(sample);
}

ChunkCoordinate ChunkCoordinateForLattice(std::int64_t latticeCoordinate) noexcept
{
	const std::int64_t chunk = FloorDivide(latticeCoordinate, kTerrainChunkCells);
	const std::int64_t local = PositiveRemainder(latticeCoordinate, kTerrainChunkCells);
	return { chunk, static_cast<std::int32_t>(local) };
}

std::optional<double> SampleBaseHeight(const TerrainAsset& asset, std::int64_t globalLatticeX, std::int64_t globalLatticeZ) noexcept
{
	if (!HasValidHeightSamplingParameters(asset))
		return std::nullopt;
	double sum = 0.0;
	double amplitude = 1.0;
	double amplitudeSum = 0.0;
	double frequency = asset.NoiseFrequency;
	for (std::uint32_t octave = 0; octave < asset.NoiseOctaves; ++octave)
	{
		const double cells = std::floor(1.0 / frequency);
		const std::int64_t cellSize = cells < 1.0 ? 1 : static_cast<std::int64_t>(cells);
		double noise = 0.0;
		if (!ValueNoise(asset, octave, globalLatticeX, globalLatticeZ, cellSize, noise))
			return std::nullopt;
		sum += noise * amplitude;
		amplitudeSum += amplitude;
		amplitude *= asset.Persistence;
		frequency *= asset.Lacunarity;
	}
	if (amplitudeSum <= 0.0 || !std::isfinite(sum))
		return std::nullopt;
	const double height = asset.OriginY + asset.BaseHeight + asset.HeightAmplitude * (sum / amplitudeSum);
	if (!std::isfinite(height))
		return std::nullopt;
	return height;
}

std::optional<double> SampleChunkHeight(const TerrainAsset& asset, std::int64_t chunkX, std::int64_t chunkZ,
	std::int32_t localSampleX, std::int32_t localSampleZ) noexcept
{
	if (localSampleX < 0 || localSampleX > kTerrainChunkCells ||
		localSampleZ < 0 || localSampleZ > kTerrainChunkCells)
		return std::nullopt;
	auto toGlobal = [](std::int64_t chunk, std::int32_t local, std::int64_t& global)
	{
		const std::int64_t minimumChunk = (std::numeric_limits<std::int64_t>::min)() / kTerrainChunkCells;
		const std::int64_t maximumChunk = (std::numeric_limits<std::int64_t>::max)() / kTerrainChunkCells;
		if (chunk < minimumChunk || chunk > maximumChunk)
			return false;
		const std::int64_t base = chunk * kTerrainChunkCells;
		if (base > (std::numeric_limits<std::int64_t>::max)() - local)
			return false;
		global = base + local;
		return true;
	};
	std::int64_t globalX = 0;
	std::int64_t globalZ = 0;
	if (!toGlobal(chunkX, localSampleX, globalX) || !toGlobal(chunkZ, localSampleZ, globalZ))
		return std::nullopt;
	return SampleBaseHeight(asset, globalX, globalZ);
}

bool BuildTerrainChunkMesh(const TerrainAsset& asset, std::int64_t chunkX, std::int64_t chunkZ,
	std::uint32_t lodLevel, TerrainChunkMesh& outMesh,
	const TerrainChunkEdits* edits, std::string* error)
{
	if (error)
		error->clear();
	if (!ValidateTerrainAsset(asset, error))
		return false;
	if (lodLevel > 6)
	{
		SetError(error, "terrain chunk LOD must be between 0 and 6");
		return false;
	}
	if (edits)
	{
		if (edits->ChunkX != chunkX || edits->ChunkZ != chunkZ)
		{
			SetError(error, "terrain edit chunk identity does not match the mesh request");
			return false;
		}
		if (!IsValidEditSet(asset, *edits, error))
			return false;
	}

	const std::int32_t sampleStep = static_cast<std::int32_t>(1u << lodLevel);
	const std::uint32_t cellsPerSide = static_cast<std::uint32_t>(kTerrainChunkCells / sampleStep);
	const std::uint32_t verticesPerSide = cellsPerSide + 1;
	auto chunkLocalToGlobal = [](std::int64_t chunk, std::int32_t local, std::int64_t& global)
	{
		constexpr std::int64_t minimum = (std::numeric_limits<std::int64_t>::min)();
		constexpr std::int64_t maximum = (std::numeric_limits<std::int64_t>::max)();
		if (chunk < minimum / kTerrainChunkCells || chunk > maximum / kTerrainChunkCells)
			return false;
		const std::int64_t base = chunk * kTerrainChunkCells;
		if ((local > 0 && base > maximum - local) || (local < 0 && base < minimum - local))
			return false;
		global = base + local;
		return true;
	};
	auto sampleHeight = [&](std::int32_t localX, std::int32_t localZ) -> std::optional<double>
	{
		std::int64_t globalX = 0;
		std::int64_t globalZ = 0;
		if (!chunkLocalToGlobal(chunkX, localX, globalX) ||
			!chunkLocalToGlobal(chunkZ, localZ, globalZ))
			return std::nullopt;
		auto height = SampleBaseHeight(asset, globalX, globalZ);
		if (!height)
			return std::nullopt;
		if (edits && localX >= 0 && localX <= kTerrainChunkCells &&
			localZ >= 0 && localZ <= kTerrainChunkCells)
		{
			const auto delta = edits->HeightDeltas.find({ localX, localZ });
			if (delta != edits->HeightDeltas.end())
				*height += delta->second;
		}
		if (!std::isfinite(*height))
			return std::nullopt;
		return height;
	};

	TerrainChunkMesh generated;
	generated.LodLevel = lodLevel;
	generated.CellsPerSide = cellsPerSide;
	generated.Vertices.reserve(static_cast<std::size_t>(verticesPerSide) * verticesPerSide);
	generated.Indices.reserve(static_cast<std::size_t>(cellsPerSide) * cellsPerSide * 6);
	for (std::uint32_t z = 0; z < verticesPerSide; ++z)
	{
		const std::int32_t localZ = static_cast<std::int32_t>(z) * sampleStep;
		for (std::uint32_t x = 0; x < verticesPerSide; ++x)
		{
			const std::int32_t localX = static_cast<std::int32_t>(x) * sampleStep;
			std::int64_t globalX = 0;
			std::int64_t globalZ = 0;
			if (!chunkLocalToGlobal(chunkX, localX, globalX) ||
				!chunkLocalToGlobal(chunkZ, localZ, globalZ))
			{
				SetError(error, "terrain chunk lattice coordinate overflows the signed sample range");
				return false;
			}
			const auto height = sampleHeight(localX, localZ);
			const auto leftHeight = sampleHeight(localX - sampleStep, localZ);
			const auto rightHeight = sampleHeight(localX + sampleStep, localZ);
			const auto backHeight = sampleHeight(localX, localZ - sampleStep);
			const auto frontHeight = sampleHeight(localX, localZ + sampleStep);
			if (!height || (!leftHeight && !rightHeight) || (!backHeight && !frontHeight))
			{
				SetError(error, "terrain mesh height or normal sample is outside the valid lattice range");
				return false;
			}
			const double xDistance = static_cast<double>(sampleStep) * asset.SampleSpacing;
			const double zDistance = static_cast<double>(sampleStep) * asset.SampleSpacing;
			double slopeX = 0.0;
			double slopeZ = 0.0;
			if (leftHeight && rightHeight)
				slopeX = (*rightHeight - *leftHeight) / (2.0 * xDistance);
			else if (rightHeight)
				slopeX = (*rightHeight - *height) / xDistance;
			else
				slopeX = (*height - *leftHeight) / xDistance;
			if (backHeight && frontHeight)
				slopeZ = (*frontHeight - *backHeight) / (2.0 * zDistance);
			else if (frontHeight)
				slopeZ = (*frontHeight - *height) / zDistance;
			else
				slopeZ = (*height - *backHeight) / zDistance;
			const double normalLength = std::hypot(slopeX, 1.0, slopeZ);
			const double worldX = asset.OriginX + static_cast<double>(globalX) * asset.SampleSpacing;
			const double worldZ = asset.OriginZ + static_cast<double>(globalZ) * asset.SampleSpacing;
			bool invalidWorldSpacing = false;
			if (x < cellsPerSide)
			{
				std::int64_t nextGlobalX = 0;
				if (!chunkLocalToGlobal(chunkX, localX + sampleStep, nextGlobalX))
				{
					SetError(error, "terrain chunk lattice coordinate overflows the signed sample range");
					return false;
				}
				const double nextWorldX = asset.OriginX + static_cast<double>(nextGlobalX) * asset.SampleSpacing;
				invalidWorldSpacing = !std::isfinite(nextWorldX) || !(nextWorldX > worldX);
			}
			if (z < cellsPerSide)
			{
				std::int64_t nextGlobalZ = 0;
				if (!chunkLocalToGlobal(chunkZ, localZ + sampleStep, nextGlobalZ))
				{
					SetError(error, "terrain chunk lattice coordinate overflows the signed sample range");
					return false;
				}
				const double nextWorldZ = asset.OriginZ + static_cast<double>(nextGlobalZ) * asset.SampleSpacing;
				invalidWorldSpacing = invalidWorldSpacing ||
					!std::isfinite(nextWorldZ) || !(nextWorldZ > worldZ);
			}
			if (!std::isfinite(xDistance) || xDistance <= 0.0 ||
				!std::isfinite(zDistance) || zDistance <= 0.0 ||
				!std::isfinite(slopeX) || !std::isfinite(slopeZ) ||
				!std::isfinite(normalLength) || normalLength <= 0.0 ||
				!std::isfinite(worldX) || !std::isfinite(worldZ) || invalidWorldSpacing)
			{
				SetError(error, invalidWorldSpacing ?
					"adjacent terrain lattice vertices are not finite and distinct in world coordinates" :
					"terrain mesh position or normal is not finite");
				return false;
			}
			TerrainMeshVertex vertex;
			vertex.Position = { worldX, *height, worldZ };
			vertex.Normal = {
				static_cast<float>(-slopeX / normalLength),
				static_cast<float>(1.0 / normalLength),
				static_cast<float>(-slopeZ / normalLength)
			};
			generated.Vertices.push_back(vertex);
		}
	}

	for (std::uint32_t z = 0; z < cellsPerSide; ++z)
	{
		for (std::uint32_t x = 0; x < cellsPerSide; ++x)
		{
			const std::uint32_t a = z * verticesPerSide + x;
			const std::uint32_t b = a + 1;
			const std::uint32_t c = a + verticesPerSide;
			const std::uint32_t d = c + 1;
			generated.Indices.insert(generated.Indices.end(), { a, c, b, b, c, d });
		}
	}
	outMesh = std::move(generated);
	return true;
}

bool ValidateTerrainStreamingConfig(const TerrainStreamingConfig& config, std::string* error)
{
	if (error)
		error->clear();
	if (config.LoadRadiusChunks > kTerrainMaximumStreamingRadiusChunks)
	{
		SetError(error, "terrain streaming radius exceeds the bounded CPU cache limit");
		return false;
	}
	if (config.MaxChunkBuildsPerUpdate == 0)
	{
		SetError(error, "terrain streaming build budget must be nonzero");
		return false;
	}
	for (std::size_t index = 1; index < config.LodDistanceThresholds.size(); ++index)
	{
		if (config.LodDistanceThresholds[index] <= config.LodDistanceThresholds[index - 1])
		{
			SetError(error, "terrain LOD distance thresholds must be strictly increasing");
			return false;
		}
	}
	return true;
}

namespace
{
bool SameTerrainEdits(const TerrainChunkEdits& left, const TerrainChunkEdits& right)
{
	return left.WorldId == right.WorldId && left.ChunkX == right.ChunkX && left.ChunkZ == right.ChunkZ &&
		left.BaseFingerprint == right.BaseFingerprint && left.TerrainSchemaVersion == right.TerrainSchemaVersion &&
		left.HeightDeltas == right.HeightDeltas && left.MaterialOverrides == right.MaterialOverrides;
}

bool FilterTerrainWorldState(const TerrainAsset& asset, const TerrainWorldState& source,
	TerrainWorldState& filtered, std::string* error)
{
	TerrainWorldState generated;
	for (const auto& entry : source.Chunks)
	{
		const TerrainChunkKey& key = entry.first;
		const TerrainChunkEdits& edits = entry.second;
		if (std::get<0>(key) != asset.WorldId)
			continue;
		if (edits.WorldId != std::get<0>(key) || edits.ChunkX != std::get<1>(key) ||
			edits.ChunkZ != std::get<2>(key) || !IsValidEditSet(asset, edits, error))
		{
			if (error && error->empty())
				SetError(error, "terrain world-state key does not match its chunk edit identity");
			return false;
		}
		generated.Chunks.emplace(key, edits);
	}
	filtered = std::move(generated);
	return true;
}
}

bool TerrainChunkStreamer::Configure(const TerrainAsset& asset, const TerrainWorldState& worldState,
	const TerrainStreamingConfig& config, std::string* error)
{
	if (error)
		error->clear();
	if (!ValidateTerrainAsset(asset, error) || !ValidateTerrainStreamingConfig(config, error))
		return false;
	TerrainWorldState filteredState;
	if (!FilterTerrainWorldState(asset, worldState, filteredState, error))
		return false;

	m_Asset = asset;
	m_WorldState = std::move(filteredState);
	m_Config = config;
	m_Configured = true;
	m_HasView = false;
	m_CenterChunkX = 0;
	m_CenterChunkZ = 0;
	m_DesiredLods.clear();
	m_LoadedChunks.clear();
	m_AppliedEdits.clear();
	m_InvalidatedChunks.clear();
	return true;
}

bool TerrainChunkStreamer::SetWorldState(const TerrainWorldState& worldState, std::string* error)
{
	if (error)
		error->clear();
	if (!m_Configured)
	{
		SetError(error, "terrain chunk streamer is not configured");
		return false;
	}
	TerrainWorldState filteredState;
	if (!FilterTerrainWorldState(m_Asset, worldState, filteredState, error))
		return false;

	for (const auto& loaded : m_LoadedChunks)
	{
		const ChunkKey& chunk = loaded.first;
		const auto applied = m_AppliedEdits.find(chunk);
		const auto replacement = filteredState.Chunks.find({ m_Asset.WorldId, chunk.first, chunk.second });
		const bool unchanged = applied == m_AppliedEdits.end()
			? replacement == filteredState.Chunks.end()
			: replacement != filteredState.Chunks.end() && SameTerrainEdits(applied->second, replacement->second);
		if (unchanged)
			m_InvalidatedChunks.erase(chunk);
		else
			m_InvalidatedChunks.insert(chunk);
	}
	m_WorldState = std::move(filteredState);
	return true;
}

bool TerrainChunkStreamer::UpdateView(double worldX, double worldZ, std::string* error)
{
	if (error)
		error->clear();
	if (!m_Configured)
	{
		SetError(error, "terrain chunk streamer is not configured");
		return false;
	}
	const auto sampleX = WorldToSampleLattice(worldX, m_Asset.OriginX, m_Asset.SampleSpacing);
	const auto sampleZ = WorldToSampleLattice(worldZ, m_Asset.OriginZ, m_Asset.SampleSpacing);
	if (!sampleX || !sampleZ)
	{
		SetError(error, "terrain streaming view center is not a finite representable sample coordinate");
		return false;
	}
	const std::int64_t centerX = ChunkCoordinateForLattice(*sampleX).Chunk;
	const std::int64_t centerZ = ChunkCoordinateForLattice(*sampleZ).Chunk;
	const std::int64_t radius = static_cast<std::int64_t>(m_Config.LoadRadiusChunks);
	const std::int64_t minimum = (std::numeric_limits<std::int64_t>::min)();
	const std::int64_t maximum = (std::numeric_limits<std::int64_t>::max)();
	if (centerX < minimum + radius || centerX > maximum - radius ||
		centerZ < minimum + radius || centerZ > maximum - radius)
	{
		SetError(error, "terrain streaming window would overflow signed chunk coordinates");
		return false;
	}

	std::map<ChunkKey, std::uint32_t> desired;
	for (std::int64_t offsetX = -radius; offsetX <= radius; ++offsetX)
	{
		for (std::int64_t offsetZ = -radius; offsetZ <= radius; ++offsetZ)
		{
			const std::uint32_t distance = static_cast<std::uint32_t>(
				(std::max)(offsetX < 0 ? -offsetX : offsetX, offsetZ < 0 ? -offsetZ : offsetZ));
			std::uint32_t lod = 0;
			while (lod < m_Config.LodDistanceThresholds.size() &&
				distance >= m_Config.LodDistanceThresholds[lod])
				++lod;
			desired.emplace(ChunkKey{ centerX + offsetX, centerZ + offsetZ }, lod);
		}
	}

	m_CenterChunkX = centerX;
	m_CenterChunkZ = centerZ;
	m_DesiredLods = std::move(desired);
	m_HasView = true;
	for (auto loaded = m_LoadedChunks.begin(); loaded != m_LoadedChunks.end();)
	{
		if (m_DesiredLods.find(loaded->first) == m_DesiredLods.end())
		{
			m_AppliedEdits.erase(loaded->first);
			m_InvalidatedChunks.erase(loaded->first);
			loaded = m_LoadedChunks.erase(loaded);
		}
		else
			++loaded;
	}
	return true;
}

bool TerrainChunkStreamer::ProcessPending(std::uint32_t* outBuilt, std::string* error)
{
	return ProcessPending(m_Config.MaxChunkBuildsPerUpdate, outBuilt, error);
}

bool TerrainChunkStreamer::ProcessPending(std::uint32_t maxBuildsPerCall,
	std::uint32_t* outBuilt, std::string* error)
{
	if (outBuilt)
		*outBuilt = 0;
	if (error)
		error->clear();
	if (!m_Configured || !m_HasView)
	{
		SetError(error, "terrain chunk streamer needs a configured asset and view center");
		return false;
	}
	struct Candidate
	{
		ChunkKey Key;
		std::uint32_t Lod = 0;
		std::uint32_t Distance = 0;
	};
	std::vector<Candidate> candidates;
	for (const auto& desired : m_DesiredLods)
	{
		const auto loaded = m_LoadedChunks.find(desired.first);
		if (loaded != m_LoadedChunks.end() && loaded->second.LodLevel == desired.second &&
			m_InvalidatedChunks.find(desired.first) == m_InvalidatedChunks.end())
			continue;
		const std::int64_t offsetX = desired.first.first - m_CenterChunkX;
		const std::int64_t offsetZ = desired.first.second - m_CenterChunkZ;
		const std::uint32_t distance = static_cast<std::uint32_t>(
			(std::max)(offsetX < 0 ? -offsetX : offsetX, offsetZ < 0 ? -offsetZ : offsetZ));
		candidates.push_back({ desired.first, desired.second, distance });
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right)
	{
		if (left.Distance != right.Distance)
			return left.Distance < right.Distance;
		return left.Key < right.Key;
	});

	const std::size_t buildBudget = (std::min)(static_cast<std::size_t>(m_Config.MaxChunkBuildsPerUpdate),
		static_cast<std::size_t>(maxBuildsPerCall));
	const std::size_t buildCount = (std::min)(buildBudget, candidates.size());
	std::uint32_t built = 0;
	for (std::size_t index = 0; index < buildCount; ++index)
	{
		const Candidate& candidate = candidates[index];
		const auto edits = m_WorldState.Chunks.find({ m_Asset.WorldId, candidate.Key.first, candidate.Key.second });
		const TerrainChunkEdits* editData = edits == m_WorldState.Chunks.end() ? nullptr : &edits->second;
		TerrainChunkMesh generated;
		std::string buildError;
		if (!BuildTerrainChunkMesh(m_Asset, candidate.Key.first, candidate.Key.second,
			candidate.Lod, generated, editData, &buildError))
		{
			SetError(error, buildError.empty() ? "terrain chunk mesh generation failed" : buildError);
			if (outBuilt)
				*outBuilt = built;
			return false;
		}
		m_LoadedChunks.insert_or_assign(candidate.Key, std::move(generated));
		if (editData)
			m_AppliedEdits.insert_or_assign(candidate.Key, *editData);
		else
			m_AppliedEdits.erase(candidate.Key);
		m_InvalidatedChunks.erase(candidate.Key);
		++built;
	}
	if (outBuilt)
		*outBuilt = built;
	return true;
}

std::size_t TerrainChunkStreamer::GetPendingChunkCount() const noexcept
{
	std::size_t pending = 0;
	for (const auto& desired : m_DesiredLods)
	{
		const auto loaded = m_LoadedChunks.find(desired.first);
		if (loaded == m_LoadedChunks.end() || loaded->second.LodLevel != desired.second ||
			m_InvalidatedChunks.find(desired.first) != m_InvalidatedChunks.end())
			++pending;
	}
	return pending;
}

void TerrainChunkStreamer::Reset() noexcept
{
	m_Configured = false;
	m_HasView = false;
	m_Asset = TerrainAsset{};
	m_WorldState.Chunks.clear();
	m_Config = TerrainStreamingConfig{};
	m_CenterChunkX = 0;
	m_CenterChunkZ = 0;
	m_DesiredLods.clear();
	m_LoadedChunks.clear();
	m_AppliedEdits.clear();
	m_InvalidatedChunks.clear();
}

std::vector<double> EvaluateSplatWeights(const TerrainAsset& asset, double height, double slopeDegrees)
{
	if (!HasValidSplatRules(asset))
		return {};
	std::vector<double> weights(asset.SplatRules.size(), 0.0);
	if (!std::isfinite(height) || !std::isfinite(slopeDegrees))
	{
		weights.front() = 1.0;
		return weights;
	}
	auto influence = [](double value, double minimum, double maximum)
	{
		if (value >= minimum && value <= maximum)
			return 1.0;
		const double span = maximum - minimum;
		const double scale = (std::max)(span, 1.0);
		const double distance = value < minimum ? minimum - value : value - maximum;
		if (!std::isfinite(distance))
			return 0.0;
		const double ratio = distance / scale;
		if (!std::isfinite(ratio))
			return 0.0;
		return (std::max)(0.0, 1.0 - ratio);
	};
	double total = 0.0;
	double bestDistance = std::numeric_limits<double>::infinity();
	std::size_t nearestRule = 0;
	for (std::size_t index = 0; index < asset.SplatRules.size(); ++index)
	{
		const SplatRule& rule = asset.SplatRules[index];
		const double heightInfluence = influence(height, rule.MinimumHeight, rule.MaximumHeight);
		const double slopeInfluence = influence(slopeDegrees, rule.MinimumSlopeDegrees, rule.MaximumSlopeDegrees);
		weights[index] = rule.Weight * heightInfluence * slopeInfluence;
		total += weights[index];
		const double heightDistance = height < rule.MinimumHeight ? rule.MinimumHeight - height :
			height > rule.MaximumHeight ? height - rule.MaximumHeight : 0.0;
		const double slopeDistance = slopeDegrees < rule.MinimumSlopeDegrees ? rule.MinimumSlopeDegrees - slopeDegrees :
			slopeDegrees > rule.MaximumSlopeDegrees ? slopeDegrees - rule.MaximumSlopeDegrees : 0.0;
		const double distance = heightDistance / (std::max)(rule.MaximumHeight - rule.MinimumHeight, 1.0) +
			slopeDistance / (std::max)(rule.MaximumSlopeDegrees - rule.MinimumSlopeDegrees, 1.0);
		if (distance < bestDistance)
		{
			bestDistance = distance;
			nearestRule = index;
		}
	}
	if (!std::isfinite(total) || total <= 0.0)
	{
		std::fill(weights.begin(), weights.end(), 0.0);
		weights[nearestRule] = 1.0;
		return weights;
	}
	for (double& weight : weights)
		weight /= total;
	return weights;
}

bool ResolveTerrainChunkEditsPath(const std::filesystem::path& root, const TerrainAsset& asset,
	std::int64_t chunkX, std::int64_t chunkZ, std::filesystem::path& outPath,
	std::string* error)
{
	if (root.empty())
	{
		SetError(error, "terrain sidecar root path must not be empty");
		return false;
	}
	if (!ValidateTerrainAsset(asset, error))
		return false;

	constexpr std::size_t kWorldIdHexCharactersPerPathComponent = 64;
	const std::string encodedWorldId = EncodeStableIdForPath(asset.WorldId);
	const std::filesystem::path normalizedRoot = root.lexically_normal();
	std::filesystem::path resolved = normalizedRoot / ".terrain-chunk-edits" / "v1" / "world";
	for (std::size_t offset = 0; offset < encodedWorldId.size();
		offset += kWorldIdHexCharactersPerPathComponent)
	{
		resolved /= encodedWorldId.substr(offset, kWorldIdHexCharactersPerPathComponent);
	}
	resolved /= "chunk-" + EncodeChunkCoordinateForPath(chunkX) + "-" +
		EncodeChunkCoordinateForPath(chunkZ) + ".wlterrainstate";

	const std::filesystem::path relative = resolved.lexically_relative(normalizedRoot);
	if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
	{
		SetError(error, "resolved terrain sidecar path is not lexically beneath its root");
		return false;
	}
	outPath = std::move(resolved);
	return true;
}

bool SaveTerrainChunkEdits(const std::filesystem::path& path, const TerrainAsset& asset,
	TerrainChunkEdits& edits, std::string* error)
{
	if (!ValidateTerrainAsset(asset, error) || !IsValidEditSet(asset, edits, error))
		return false;
	const std::string bytes = SerializeEdits(edits);
	if (!AtomicReplace(path, bytes, error))
		return false;
	edits.Dirty = false;
	return true;
}

bool LoadTerrainChunkEdits(const std::filesystem::path& path, const TerrainAsset& asset,
	std::int64_t expectedChunkX, std::int64_t expectedChunkZ, TerrainWorldState& activeState,
	std::string* error)
{
	if (!ValidateTerrainAsset(asset, error))
		return false;
	std::string contents;
	if (!ReadBoundedTextFile(path, kMaximumTerrainStateBytes, "terrain sidecar", contents, error))
		return false;
	TerrainChunkEdits parsed;
	if (!ParseEdits(contents, asset, expectedChunkX, expectedChunkZ, parsed, error))
		return false;
	const TerrainChunkKey key{ asset.WorldId, expectedChunkX, expectedChunkZ };
	const auto active = activeState.Chunks.find(key);
	if (active != activeState.Chunks.end() && active->second.Dirty)
	{
		SetError(error, "refusing to replace dirty active terrain edits from a sidecar load");
		return false;
	}
	activeState.Chunks.insert_or_assign(key, std::move(parsed));
	return true;
}

bool SaveTerrainChunkEditsForWorld(const std::filesystem::path& root, const TerrainAsset& asset,
	TerrainWorldState& worldState, std::string* error)
{
	if (error)
		error->clear();
	if (root.empty())
	{
		SetError(error, "terrain sidecar root path must not be empty");
		return false;
	}
	if (!ValidateTerrainAsset(asset, error))
		return false;

	struct PendingWrite
	{
		TerrainChunkEdits* Edits = nullptr;
		std::filesystem::path Path;
		std::string Bytes;
	};
	std::vector<PendingWrite> pending;
	for (auto& entry : worldState.Chunks)
	{
		const TerrainChunkKey& key = entry.first;
		TerrainChunkEdits& edits = entry.second;
		if (edits.WorldId != std::get<0>(key) || edits.ChunkX != std::get<1>(key) ||
			edits.ChunkZ != std::get<2>(key))
		{
			SetError(error, "terrain world-state key does not match its chunk edit identity");
			return false;
		}
		if (std::get<0>(key) != asset.WorldId)
			continue;
		if (!IsValidEditSet(asset, edits, error))
			return false;
		if (!edits.Dirty)
			continue;

		std::filesystem::path path;
		if (!ResolveTerrainChunkEditsPath(root, asset, edits.ChunkX, edits.ChunkZ, path, error))
			return false;
		pending.push_back({ &edits, std::move(path), SerializeEdits(edits) });
	}

	for (PendingWrite& write : pending)
	{
		if (!AtomicReplace(write.Path, write.Bytes, error))
			return false;
		write.Edits->Dirty = false;
	}
	return true;
}

bool LoadTerrainChunkEditsForWorld(const std::filesystem::path& root, const TerrainAsset& asset,
	TerrainWorldState& activeState, std::string* error)
{
	if (error)
		error->clear();
	std::filesystem::path sampleSidecarPath;
	if (!ResolveTerrainChunkEditsPath(root, asset, 0, 0, sampleSidecarPath, error))
		return false;
	const std::filesystem::path worldDirectory = sampleSidecarPath.parent_path();
	std::filesystem::path ioDirectory = worldDirectory;
#if defined(_WIN32)
	std::error_code pathError;
	std::filesystem::path enumerationPattern;
	if (!PrepareWindowsFileIoPath(worldDirectory / L"*", enumerationPattern, pathError))
	{
		SetError(error, "could not prepare terrain sidecar directory path: " + pathError.message());
		return false;
	}
	ioDirectory = enumerationPattern.parent_path();
#endif

	std::error_code filesystemError;
	const std::filesystem::file_status directoryStatus =
		std::filesystem::status(ioDirectory, filesystemError);
	if (filesystemError)
	{
		if (filesystemError == std::errc::no_such_file_or_directory)
			return true;
		SetError(error, "could not inspect terrain sidecar directory: " + filesystemError.message());
		return false;
	}
	if (directoryStatus.type() == std::filesystem::file_type::not_found)
		return true;
	if (!std::filesystem::is_directory(directoryStatus))
	{
		SetError(error, "terrain sidecar world path is not a directory");
		return false;
	}

	std::filesystem::directory_iterator iterator(ioDirectory, filesystemError);
	if (filesystemError)
	{
		SetError(error, "could not enumerate terrain sidecar directory: " + filesystemError.message());
		return false;
	}
	const std::filesystem::directory_iterator end;
	TerrainWorldState candidate = activeState;
	std::set<std::pair<std::int64_t, std::int64_t>> decodedCoordinates;
	while (iterator != end)
	{
		const std::filesystem::directory_entry entry = *iterator;
		const std::string filename = entry.path().filename().string();
		if (HasCanonicalTerrainStateExtension(filename))
		{
			std::int64_t chunkX = 0;
			std::int64_t chunkZ = 0;
			if (!ParseCanonicalChunkFilename(filename, chunkX, chunkZ))
			{
				SetError(error, "malformed terrain sidecar filename: " + filename);
				return false;
			}
			if (!decodedCoordinates.emplace(chunkX, chunkZ).second)
			{
				SetError(error, "duplicate terrain sidecar filenames decode to the same chunk coordinates");
				return false;
			}

			std::error_code entryError;
			if (!entry.is_regular_file(entryError) || entryError)
			{
				SetError(error, "terrain sidecar entry is not a readable regular file: " +
					(entryError ? entryError.message() : filename));
				return false;
			}
			if (!LoadTerrainChunkEdits(entry.path(), asset, chunkX, chunkZ, candidate, error))
				return false;
		}

		iterator.increment(filesystemError);
		if (filesystemError)
		{
			SetError(error, "could not enumerate terrain sidecar directory: " + filesystemError.message());
			return false;
		}
	}
	activeState = std::move(candidate);
	return true;
}
}
