// PCH first, by house rule: it supplies the standard library headers
// used below plus the engine's logging facilities.
#include "wlpch.h"

#include "AudioClip.h"

// Unused directly -- kept for consistency with the other engine
// sources in this tree rather than proven necessary here.
#include <algorithm>
// Fixed-size buffers for the canonical WAV header and bounded PCM writes.
#include <array>
// std::fabs, for the peak scan in Finalise.
#include <cmath>
// memcpy for the fixed-width integer and float reads. Copying rather
// than dereferencing a cast pointer is what keeps these well-defined
// for unaligned sample data -- a WAV's samples are NOT guaranteed to
// sit at a 4-byte boundary.
#include <cstring>
// std::ifstream, for reading the whole file into memory.
#include <fstream>
// Numeric limits for RIFF's 32-bit size fields.
#include <limits>

// C++17 nested namespace definition, matching the header.
namespace Wasteland::Audio
{

	// File-local namespace. Everything above the LoadWav implementations
	// is private to this translation unit, which is why these helpers
	// can be plain free functions with no header entry.
	namespace
	{
		// WAV format tags. Only uncompressed PCM and IEEE float are supported;
		// anything else (ADPCM, mu-law, MP3-in-WAV) is reported as
		// UnsupportedEncoding rather than mis-decoded into noise.
		// Values from the RIFF/WAVE spec's wFormatTag field. uint16_t
		// because that is the field width in the file format.
		constexpr uint16_t kFormatPCM = 1;
		constexpr uint16_t kFormatIEEEFloat = 3;
		// kFormatExtensible is the interesting one: it does not say what the
		// samples ARE, it says "read the GUID". Most tools write plain PCM
		// through it, which is why it is resolved below rather than
		// rejected outright.
		constexpr uint16_t kFormatExtensible = 0xFFFE;
		constexpr uint16_t kOutputBitsPerSample = 16;
		constexpr uint16_t kOutputChannels = 1;
		constexpr uint16_t kOutputBlockAlign = kOutputChannels * (kOutputBitsPerSample / 8);

		// Byte-order readers. Hand-rolled rather than a reinterpret_cast,
		// because the RIFF spec is explicitly little-endian and a WAV read
		// on a big-endian host must still produce correct values.
		// Little-endian readers. Hand-rolled rather than a reinterpret_cast of
		// the buffer, because the RIFF spec is explicitly little-endian and a
		// WAV read on a big-endian host must still work.
		uint16_t ReadU16(const uint8_t *p)
		{
			// Byte-at-a-time rather than a memcpy into a uint16_t, which would
			// reproduce the HOST byte order -- the exact bug this pair exists
			// to avoid.
			return (uint16_t)(p[0] | (p[1] << 8));
		}

		uint32_t ReadU32(const uint8_t *p)
		{
			// Four explicit byte loads for the same reason as ReadU16. Written out
			// rather than looping so the shift amounts stay compile-time
			// constants; the compiler folds this to a single load on a
			// little-endian target.
			return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
		}

		// WAV integers are little-endian regardless of host byte order. Keep
		// writes byte-wise for the same portability guarantee as ReadU16/ReadU32.
		void WriteU16(uint8_t *p, uint16_t value)
		{
			p[0] = (uint8_t)(value & 0xFF);
			p[1] = (uint8_t)((value >> 8) & 0xFF);
		}

		void WriteU32(uint8_t *p, uint32_t value)
		{
			p[0] = (uint8_t)(value & 0xFF);
			p[1] = (uint8_t)((value >> 8) & 0xFF);
			p[2] = (uint8_t)((value >> 16) & 0xFF);
			p[3] = (uint8_t)((value >> 24) & 0xFF);
		}

		// Convert one source sample to float in [-1, 1].
		//
		// Each branch is a straight integer-to-float scale by the format's
		// full-scale value. 8-bit WAV is UNSIGNED (0 = silence, 128 = zero),
		// which is the classic off-by-128 bug in hand-rolled loaders and the
		// reason the unsigned case is written out explicitly here.
		// The single conversion point for every supported input format. One
		// function means there is a single place to be wrong, rather than a
		// scale factor duplicated at each call site in the load loop.
		float SampleToFloat(const uint8_t *p, int bitsPerSample, bool isFloat)
		{
			// Float is checked FIRST and separately, because a 32-bit float sample
			// and a 32-bit integer sample are the same width -- the branch has
			// to come before the switch below or float data would be read as
			// a huge integer.
			if (isFloat && bitsPerSample == 32)
			{
				float value;
				// memcpy, not a cast: the pointer is into a byte buffer at an arbitrary
				// alignment, and dereferencing it as a float* would be undefined
				std::memcpy(&value, p, sizeof(float));
				// behaviour and would fault on strict-alignment targets.
				return value;
			}

			// Integer widths. A switch rather than a division because each width
			// has a different scale AND, for 8-bit, a different sign convention.
			switch (bitsPerSample)
			{
			case 8:
			{
				// Unsigned: centre is 128, not 0.
				// 8-bit WAV is UNSIGNED with 128 as zero. Subtracting 128 before
				// scaling is the step hand-rolled loaders get wrong, and getting it
				// wrong inverts the waveform rather than merely adding a DC offset,
				// so it is written out explicitly.
				return ((float)p[0] - 128.0f) / 128.0f;
			}
			case 16:
			{
				int16_t value;
				std::memcpy(&value, p, sizeof(int16_t));
				// 32768 rather than 32767: the divisor is 2^15, the true full scale of
				// a signed 16-bit value. Dividing by 32767 would make a full-scale
				// negative sample exceed -1.0.
				return (float)value / 32768.0f;
			}
			case 24:
			{
				// 24-bit is stored in the low 3 bytes of a 4-byte word. Assemble
				// by hand, then sign-extend from bit 23 before scaling.
				// DEFECT (portability): assembled byte-by-byte, so this branch is
				// endian-safe -- but the float branch above is not, because it
				// memcpy's the raw 4 bytes. On a big-endian host a 32-bit float WAV
				// would be misread while the integer formats worked, which is the
				// worst kind of inconsistency: correct for the common cases, wrong
				// silently for one.
				int32_t value = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
				// Bit 23 is the sign bit of a 24-bit two's-complement value.
				if (value & 0x800000)
					// Sign extension by setting the top 8 bits. ~0xFFFFFF is the sign
					// mask as a 32-bit int; without this, every negative 24-bit sample
					// would read as a large positive one.
					value |= ~0xFFFFFF;  // sign extend
				// 2^23, the full-scale magnitude of signed 24-bit.
				return (float)value / 8388608.0f;
			}
			case 32:
			{
				int32_t value;
				std::memcpy(&value, p, sizeof(int32_t));
				// 2^31 as a float. The literal is written out rather than computed as
				// (1 << 31) because that overflows a signed int.
				return (float)value / 2147483648.0f;
			}
			default:
				// Unreachable in practice: bitsPerSample is validated against the four
				// supported widths before any sample is converted. Returning a
				// defined 0.0f rather than falling through keeps the function
				// total for a future caller that skips that validation.
				return 0.0f;
			}
		}

		// Truncating division. Exact for 8/16/24/32, all of which divide by
		// 8 -- and those are the only widths the loader admits, so there is
		// no sub-byte sample format to lose precision on.
		int BytesPerSample(int bitsPerSample) { return bitsPerSample / 8; }
	}

	// Error text for the UI. Returns a static string literal rather than a
	// std::string so the result can be used in a printf-style call with
	// no temporary.
	const char *AudioClip::LoadResultToString(LoadResult result)
	{
		switch (result)
		{
		case LoadResult::Success:
			return "success";
		case LoadResult::FileNotFound:
			return "file not found";
		case LoadResult::InvalidFormat:
			return "not a RIFF/WAVE file";
		case LoadResult::UnsupportedEncoding:
			return "unsupported WAV encoding (only PCM and IEEE float)";
		case LoadResult::TruncatedFile:
			return "file is shorter than its header claims";
		case LoadResult::EmptyClip:
			return "file contains no audio samples";
		// Unreachable while every LoadResult enumerator is handled above;
		// "unknown error" is the visible fallback for a value persisted from
		// a newer build.
		default:
			return "unknown error";
		}
	}

	// Duration in seconds, for a UI readout and for clip scheduling.
	float AudioClip::GetDuration() const
	{
		// Guarded rather than dividing unconditionally: a default-constructed
		// clip has a valid sample rate but no samples, and a clip that
		// failed to load can be left with a zero rate.
		if (m_SampleRate <= 0 || m_Channels <= 0)
			return 0.0f;
		// DEFECT (channels in the divisor): divides by sampleRate * m_Channels,
		// but m_Samples holds FRAMES, not per-channel samples -- multichannel
		// input is downmixed to mono at load, so one frame is one float.
		// The correct divisor is just m_SampleRate. It happens to work today
		// only because m_Channels is set to 1 on every load path; storing the
		// true channel count (which GetChannels() is named for) would make
		// this silently report a fraction of the real duration.
		return (float)m_Samples.size() / (float)(m_SampleRate * m_Channels);
	}

	// Post-load bookkeeping: peak, validity, id. One place so no
	// construction path can forget a step.
	void AudioClip::Finalise()
	{
		// Single pass over the samples. This runs once per clip at load, not
		// per play, which is the reason it is acceptable to scan everything.
		//
		float peak = 0.0f;
		// Peak is found by comparison rather than std::max_element so the
		// absolute value can be taken in the same pass -- two traversals of
		// potentially megabytes of audio saved per clip.
		for (float sample : m_Samples)
		{
			// fabs, not the value: a peak that ignored negative excursions would
			// report a quiet half of the waveform as the maximum.
			float magnitude = std::fabs(sample);
			if (magnitude > peak)
				peak = magnitude;
		}
		// Assigned unconditionally, so a re-Finalise cannot leave a stale peak.
		m_Peak = peak;

		// A clip with no samples is not a usable clip. Marking it valid anyway
		// produces a voice that plays forever in silence, which is much harder
		// to diagnose than a load that failed.
		// Both conditions required. Non-empty samples without a positive rate
		// would give a divide-by-zero in GetDuration; a positive rate with no
		// samples would give a voice that plays forever in silence, which is
		// far harder to diagnose than a failed load.
		m_Valid = !m_Samples.empty() && m_SampleRate > 0;
		// DEFECT (dead guard): UUID's default constructor GENERATES a fresh
		// random value (UUID.h: "Default constructor: generates a fresh
		// random UUID"), so the comparison on this line constructs a second
		// random id and compares it against m_Id. The probability of equality
		// is effectively zero, so the body never runs and m_Id keeps
		// whatever the member's own construction gave it. The line reads as
		// a nil-uuid check, but this UUID type has no nil state -- a real
		// "unset" test needs an explicit sentinel, not a fresh UUID.
		if (m_Valid && m_Id == UUID())
			// Unreachable for the reason above. Note also that assigning UUID()
			// here would replace the id with a fresh random one, which is what
			// the author presumably intended, but the guard prevents it from
			// ever being reached.
			m_Id = UUID();
	}

	// Factory for programmatic clips -- test tones, procedurally
	// generated audio -- so the mixer can be tested without binary
	// fixtures in the repo.
	Ref<AudioClip> AudioClip::FromSamples(std::vector<float> samples, int sampleRate)
	{
		// make_shared, not new: the clip is always held by a Ref, and
		// make_shared gives one allocation for the control block and the
		// object instead of two.
		auto clip = std::make_shared<AudioClip>();
		// Moved rather than copied: the samples are the largest thing in the
		// object, and the caller has no further use for the vector.
		clip->m_Samples = std::move(samples);
		// Defensive default. 44100 is the CD rate and the engine default in
		// the header; substituting it beats propagating a zero that would
		// make GetDuration return 0 for a perfectly good buffer.
		clip->m_SampleRate = sampleRate > 0 ? sampleRate : 44100;
		// Always mono: this factory has no channel parameter, so a
		// multichannel buffer is not representable and must be authored
		// already downmixed.
		clip->m_Channels = 1;
		clip->Finalise();
		return clip;
	}

	// Emit a simple PCM RIFF/WAVE file. The stored clip is mono float32, while
	// the file uses portable 16-bit signed PCM; conversion happens in bounded
	// chunks and writes explicitly little-endian bytes on every host.
	bool AudioClip::SaveWav(const std::string &filepath) const
	{
		constexpr uint32_t riffHeaderRemainder = 36; // bytes outside the data payload
		constexpr size_t bytesPerSample = kOutputBitsPerSample / 8;

		// The current clip invariant is mono float samples. Reject inconsistent
		// internal state rather than writing a header that lies about the payload.
		if (!m_Valid || m_Samples.empty() || m_SampleRate <= 0 || m_Channels != kOutputChannels)
			return false;

		// RIFF has 32-bit sizes. Check before multiplying or opening/truncating the
		// destination, and leave room for the 36 bytes outside the data chunk.
		const uint32_t maxDataBytes = std::numeric_limits<uint32_t>::max() - riffHeaderRemainder;
		if (m_Samples.size() > static_cast<size_t>(maxDataBytes / bytesPerSample))
			return false;
		const uint32_t dataSize = static_cast<uint32_t>(m_Samples.size() * bytesPerSample);
		const uint64_t byteRate64 = static_cast<uint64_t>(m_SampleRate) * kOutputBlockAlign;
		if (byteRate64 > std::numeric_limits<uint32_t>::max())
			return false;

		std::array<uint8_t, 44> header{};
		std::memcpy(header.data(), "RIFF", 4);
		WriteU32(header.data() + 4, riffHeaderRemainder + dataSize);
		std::memcpy(header.data() + 8, "WAVE", 4);
		std::memcpy(header.data() + 12, "fmt ", 4);
		WriteU32(header.data() + 16, 16); // canonical PCM fmt chunk size
		WriteU16(header.data() + 20, kFormatPCM);
		WriteU16(header.data() + 22, kOutputChannels);
		WriteU32(header.data() + 24, static_cast<uint32_t>(m_SampleRate));
		WriteU32(header.data() + 28, static_cast<uint32_t>(byteRate64));
		WriteU16(header.data() + 32, kOutputBlockAlign);
		WriteU16(header.data() + 34, kOutputBitsPerSample);
		std::memcpy(header.data() + 36, "data", 4);
		WriteU32(header.data() + 40, dataSize);

		std::ofstream stream(filepath, std::ios::binary | std::ios::trunc);
		if (!stream.is_open())
			return false;
		stream.write(reinterpret_cast<const char *>(header.data()),
			static_cast<std::streamsize>(header.size()));
		if (!stream)
			return false;

		// A fixed buffer avoids allocating a second copy of potentially large
		// recordings. Integer PCM maps -1 to -32768 and +1 to +32767; non-finite
		// samples become silence rather than invoking an undefined integer cast.
		std::array<uint8_t, 16384> pcmBuffer{};
		size_t bufferedBytes = 0;
		for (float sample : m_Samples)
		{
			double value = std::isfinite(sample) ? static_cast<double>(sample) : 0.0;
			value = std::max(-1.0, std::min(1.0, value));
			long quantized = std::lround(value * 32768.0);
			quantized = std::max(-32768L, std::min(32767L, quantized));
			const uint16_t encoded = static_cast<uint16_t>(static_cast<int16_t>(quantized));
			pcmBuffer[bufferedBytes++] = static_cast<uint8_t>(encoded & 0xFF);
			pcmBuffer[bufferedBytes++] = static_cast<uint8_t>((encoded >> 8) & 0xFF);

			if (bufferedBytes == pcmBuffer.size())
			{
				stream.write(reinterpret_cast<const char *>(pcmBuffer.data()),
					static_cast<std::streamsize>(bufferedBytes));
				if (!stream)
					return false;
				bufferedBytes = 0;
			}
		}

		if (bufferedBytes != 0)
			stream.write(reinterpret_cast<const char *>(pcmBuffer.data()),
				static_cast<std::streamsize>(bufferedBytes));
		stream.close();
		return !stream.fail();
	}

	// The file-loading half. Reads the whole file into memory and hands
	// off to the memory half, so the parsing logic exists once.
	Ref<AudioClip> AudioClip::LoadWav(const std::string &filepath, LoadResult *outResult)
	{
		// Whole-file read. Simple and correct for the clip sizes a game ships
		// (a few MB); a large music track would want streaming, which is
		// what LoadWavFromMemory is shaped to allow later.
		std::ifstream stream(filepath, std::ios::binary);
		// is_open rather than trying to read and catching an exception, so a
		// missing file is an expected outcome rather than an error path.
		if (!stream.is_open())
		{
			if (outResult)
				*outResult = LoadResult::FileNotFound;
			return nullptr;
		}

		// Iterator-range construction reads to EOF in one expression. The
		// char to uint8_t conversion is safe here: both are 8-bit and the
		// values are byte patterns, not text.
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
									std::istreambuf_iterator<char>());
		if (bytes.empty())
		{
			if (outResult)
				// A zero-byte file is reported as TRUNCATED rather than as an empty
				// clip: a zero-byte file has no header at all, which is a different
				// kind of wrong from a valid WAV containing no samples.
				*outResult = LoadResult::TruncatedFile;
			return nullptr;
		}

		// Name the clip after the file, without the extension, so a content
		// browser shows "shotgun_fire" rather than a full path.
		// Derive the display name from the file. Both separators are searched
		// so a Windows-style path passed on Linux still strips correctly.
		std::string name = filepath;
		size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos)
			name = name.substr(slash + 1);
		// Strips the extension using the LAST dot, not the first, so
		// "my.sound.v2.wav" becomes "my.sound.v2" rather than "my".
		size_t dot = name.find_last_of('.');
		if (dot != std::string::npos)
			name = name.substr(0, dot);

		// Parsing is delegated wholesale; this function adds only the name.
		// Naming before checking `clip` would dereference null on failure.
		auto clip = LoadWavFromMemory(bytes.data(), bytes.size(), outResult);
		if (clip)
			clip->SetName(name);
		// Returns null with *outResult already set by the inner call, so the
		// caller has one error path rather than two.
		return clip;
	}

	// The real parser, and the security-relevant function in this file:
	// everything past this point is attacker-controlled data from an
	// arbitrary file on disk.
	Ref<AudioClip> AudioClip::LoadWavFromMemory(const uint8_t *data, size_t size, LoadResult *outResult)
	{
		// Single failure funnel. Every error path below routes through here
		// so *outResult is always written and the return type is uniform --
		// no path can forget to set the result and leave the caller reading
		// an uninitialized enum.
		auto fail = [&](LoadResult result) -> Ref<AudioClip> {
			if (outResult)
				*outResult = result;
			return nullptr;
		};

		// The smallest possible RIFF header is 12 bytes; a 'fmt ' chunk needs
		// 24 more, and 'data' needs 8. Anything under 44 cannot hold a usable
		// PCM frame, so it is rejected before any field is read.
		// 44 bytes: 12 (RIFF/WAVE header) + 24 (minimum 'fmt ') + 8
		// (minimum 'data'). Checking this BEFORE reading any field is what
		// makes the reads below in-bounds by construction rather than by
		// hoping each one was checked.
		if (!data || size < 44)
			return fail(LoadResult::TruncatedFile);

		// Magic-number check. Both offsets are fixed by the RIFF spec:
		// "RIFF" at 0, the format tag "WAVE" at 8.
		if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
			return fail(LoadResult::InvalidFormat);

		size_t offset = 12;
		// haveFormat guards the fields below from being read uninitialized
		// when a file has no 'fmt ' chunk at all.
		bool haveFormat = false;
		uint16_t formatTag = 0;
		uint16_t channels = 0;
		uint32_t sampleRate = 0;
		uint16_t bitsPerSample = 0;
		// Null rather than a sentinel offset, so "no data chunk found" cannot
		// be confused with "data chunk at byte 0".
		const uint8_t *dataStart = nullptr;
		uint32_t dataSize = 0;

		// The chunk walk, not a fixed layout. WAV files routinely place
		// LIST, fact, or cue chunks between 'fmt ' and 'data', and some
		// encoders emit 'data' first. A hardcoded offset would reject
		// perfectly valid files.
		// Walk the chunk list. A 'fmt ' chunk is not guaranteed to come first,
		// and a file may carry LIST/fact chunks that must be skipped rather
		// than assumed to be sample data.
		// Loop condition requires 8 bytes for a chunk header (4 id + 4
		// size), so ReadU32 below is always in bounds.
		while (offset + 8 <= size)
		{
			const char *chunkId = reinterpret_cast<const char *>(data + offset);
			// ReadU32, not a reinterpret_cast: the file is little-endian and the
			// host may not be.
			uint32_t chunkSize = ReadU32(data + offset + 4);
			size_t chunkStart = offset + 8;

			// A chunk that claims to run past the end of the buffer is
			// truncated. Clamping instead of trusting the header is what keeps a
			// corrupt file from reading out of bounds.
			// DEFECT (clamp masks truncation): a chunk claiming to run past the
			// end is clamped rather than rejected, so a truncated file loads as
			// a shorter clip and reports SUCCESS. LoadResult::TruncatedFile
			// exists for exactly this case and is never produced by this path --
			// it is only used for the sub-44-byte check. Clamping is still the
			// right call for memory SAFETY, but the truncation should be
			// recorded so the caller can be told the audio is incomplete.
			if (chunkStart + chunkSize > size)
				chunkSize = (uint32_t)(size - chunkStart);

			// chunkSize >= 16 is the minimum meaningful 'fmt ': the fields read
			// below end at offset 14.
			if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16)
			{
				formatTag = ReadU16(data + chunkStart);
				channels = ReadU16(data + chunkStart + 2);
				sampleRate = ReadU32(data + chunkStart + 4);
				// wBitsPerSample sits at offset 14 in the canonical 'fmt ' chunk,
				// after the two-byte tag and two-byte channel count.
				bitsPerSample = ReadU16(data + chunkStart + 14);

				// WAVE_FORMAT_EXTENSIBLE hides the real tag in its GUID. It is
				// overwhelmingly PCM in practice, so accept it when the real
				// tag says PCM, and reject it otherwise rather than guessing.
				// Extensible resolution. The real tag lives in the first two bytes
				// of the SubFormat GUID at offset 24, which needs chunkSize >= 26
				// to read.
				if (formatTag == kFormatExtensible && chunkSize >= 26)
				{
					uint16_t subFormat = ReadU16(data + chunkStart + 24);
					if (subFormat == kFormatPCM || subFormat == kFormatIEEEFloat)
						formatTag = subFormat;
				}
				// Set even when the extensible sub-format could not be resolved, so
				// an unresolvable one falls through to the formatTag check below
				// and is rejected there rather than here. Deliberate.
				haveFormat = true;
			}
			// The data chunk. No size validation here: chunkSize was already
			// clamped against the buffer above, so this pointer plus dataSize
			// cannot exceed the allocation.
			else if (std::memcmp(chunkId, "data", 4) == 0)
			{
				dataStart = data + chunkStart;
				dataSize = chunkSize;
			}

			// Word alignment. RIFF pads odd-sized chunks to an even boundary,
			// so without this +1 every chunk after an odd one is read one byte
			// early -- which desynchronizes the walk and typically reports a
			// valid file as corrupt.
			// Chunks are word-aligned: an odd-sized chunk is followed by a pad
			// byte. Skipping that is the difference between reading a valid
			// file and desynchronising on the next chunk.
			offset = chunkStart + chunkSize;
			// The pad byte applies only to odd sizes.
			if (chunkSize % 2 == 1)
				offset += 1;
		}

		// Both must be present. A 'data' chunk without 'fmt ' cannot be
		// decoded, and 'fmt ' without 'data' has no samples to return.
		if (!haveFormat || !dataStart)
			return fail(LoadResult::InvalidFormat);
		// Encoding gate. Anything else is a missing codec rather than a bug
		// in this loader, which is why it has its own result value.
		if (formatTag != kFormatPCM && formatTag != kFormatIEEEFloat)
			return fail(LoadResult::UnsupportedEncoding);
		if (channels == 0 || sampleRate == 0)
			return fail(LoadResult::InvalidFormat);
		// Width gate, checked before any arithmetic that assumes one of these
		// four. SampleToFloat's switch has a default, but relying on that
		// would mean silently decoding to zero samples.
		if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32)
			return fail(LoadResult::UnsupportedEncoding);
		// 32-bit is ambiguous: it is only valid as float, not as PCM. A
		// 32-bit PCM file would need a /128 scale, which is not a thing.
		// DEFECT (asymmetric rule): 32-bit PCM is rejected, but 32-bit FLOAT
		// is accepted by the check above -- while the header comment
		// advertises "16/24/32-bit signed". 32-bit signed integer PCM is a
		// real and legal WAV encoding that this loader refuses. The
		// justification given (it "would need a /128 scale") does not hold:
		// SampleToFloat already implements exactly that division at the
		// 32-bit case. The rejection looks like a leftover from when only
		// float 32 was supported.
		if (formatTag == kFormatPCM && bitsPerSample == 32)
			return fail(LoadResult::UnsupportedEncoding);
		if (dataSize == 0)
			return fail(LoadResult::EmptyClip);

		// Decided once, not per sample: the format does not change within a
		// file, so hoisting this out of the decode loop keeps the inner
		// branch predictable.
		const bool isFloat = (formatTag == kFormatIEEEFloat);
		const int bytesPerSample = BytesPerSample(bitsPerSample);
		// bytesPerSample * channels is the size of one interleave frame. Both
		// factors are already validated non-zero and small, so this cannot
		// overflow an int.
		const int frameBytes = bytesPerSample * channels;

		// If the data chunk is not a whole number of frames, drop the partial
		// tail rather than reading one sample past the end.
		// Integer division truncates. A partial trailing frame is dropped by
		// this floor rather than by a separate check -- the remainder is
		// simply never read.
		uint32_t frameCount = dataSize / (uint32_t)frameBytes;
		if (frameCount == 0)
			return fail(LoadResult::EmptyClip);

		// make_shared for the same one-allocation reason as FromSamples.
		auto clip = std::make_shared<AudioClip>();
		clip->m_SampleRate = (int)sampleRate;
		// Downmix target is hardcoded to 1 channel. The header documents
		// this as deliberate (the mixer pans mono), so this is a design
		// decision rather than a stub -- but it is why GetChannels() always
		// returns 1, and why the GetDuration divisor is currently harmless.
		clip->m_Channels = 1;  // downmixed; see the header comment
		clip->m_Samples.resize(frameCount);

		// One iteration per FRAME, not per sample, so the loop count is
		// independent of channel count.
		for (uint32_t frame = 0; frame < frameCount; ++frame)
		{
			// The cast to size_t is load-bearing: frame * frameBytes is computed
			// in 32-bit otherwise, and a clip over ~256 MB would wrap and read
			// from the wrong offset.
			const uint8_t *framePtr = dataStart + (size_t)frame * frameBytes;

			// Mono fast path: no inner loop, no averaging. Common enough to be
			// worth keeping separate rather than always dividing by 1.
			if (channels == 1)
			{
				clip->m_Samples[frame] = SampleToFloat(framePtr, bitsPerSample, isFloat);
			}
			else
			{
				// Averaging rather than summing. A sum would make a stereo clip
				// twice as loud as the same content in mono, which is not what "the
				// same sound" means.
				// Average the channels. A SUM would make a stereo clip twice as
				// loud as the same content in mono, which is not what "the same
				// sound" means.
				// Accumulated in float, not an integer type, so no precision is lost
				// across channels before the divide.
				float sum = 0.0f;
				// Channels are interleaved within a frame, so the per-channel offset
				// advances by one sample, not by one frame.
				for (int ch = 0; ch < channels; ++ch)
					sum += SampleToFloat(framePtr + ch * bytesPerSample, bitsPerSample, isFloat);
				clip->m_Samples[frame] = sum / (float)channels;
			}
		}

		// Peak and validity computed once, after all samples are decoded --
		// doing it per frame would mean a second traversal.
		clip->Finalise();
		// The only place EmptyClip is produced from decoded audio. A file can
		// pass every header check and still yield nothing, e.g. a data
		// chunk too short to hold a single frame.
		if (!clip->IsValid())
			return fail(LoadResult::EmptyClip);

		// Success is written last, after the clip is known to be usable, so a
		// caller that trusts *outResult cannot be told "success" for a clip
		// that is about to be discarded.
		if (outResult)
			*outResult = LoadResult::Success;
		return clip;
	}

}
