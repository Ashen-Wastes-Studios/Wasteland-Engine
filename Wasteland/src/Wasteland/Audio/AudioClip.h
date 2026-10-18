// A decoded audio buffer and the loaders that produce it. AudioClip.cpp is the
// implementation; this header is the contract the mixer and any importer
// program against.
#pragma once

// For uint8_t in the LoadWavFromMemory signature and the RIFF parser's byte
// arithmetic.
//
// A fixed-width type rather than `unsigned char` so the memory-loader's pointer
// type is unambiguous about being raw bytes.
#include <cstdint>
// For the file path and m_Name.
#include <string>
// For the sample storage. The class holds it by value, so a clip is
// self-contained once loaded.
#include <vector>

// For the m_Id member and GetId.
//
// Every clip carries a UUID from construction rather than being identified by its
// path, so a clip can be renamed on disk and keep its identity -- and so two
// clips from the same path are distinguishable. The engine's convention is
// UUID-keyed handles rather than raw pointers or names.
#include "Wasteland/Core/UUID.h"

// A nested namespace definition (C++17 syntax), rather than the two-level brace
// form used in headers like Skeleton.h.
//
// Both spellings mean the same thing; only one can be used in a single file, and
// the engine does not standardise on either.
namespace Wasteland::Audio
{

	// The class's core invariant, and the pre-existing comment states it exactly:
	// samples are ALWAYS interleaved float32 in [-1, 1] regardless of source format.
	// Decoded, device-independent PCM audio.
	//
	// Samples are ALWAYS stored as interleaved float32 in [-1, 1] regardless of
	// the source format. Every consumer (the mixer, the device) then has one
	// format to handle, and format conversion happens exactly once, at load
	// time. The alternative -- passing 16-bit ints and converting per-voice per
	// mix -- is both slower and a source of clipping bugs.
	//
	// Deliberately no device handle here. A clip must be loadable, inspectable
	// and testable with no audio hardware present, which is what makes the
	// mixer headlessly verifiable.
	// A value type with static factory functions and NO public constructor for
	// loaded audio -- every instance comes from LoadWav, LoadWavFromMemory or
	// FromSamples, all of which return a Ref<AudioClip>.
	//
	// So a clip cannot exist in a half-initialised state from the outside: m_Valid
	// is set by Finalise(), which only those three paths call. The default constructor
	// below is still public, which is the one hole -- see that line.
	class AudioClip
	{
	public:
		// Where a load failed and why. Distinguishing "file missing" from
		// "corrupt" from "unsupported format" matters: the first is a content
		// bug, the second a pipeline bug, the third usually a missing codec.
		// The failure taxonomy, and the pre-existing comment gives the reason it is this
		// fine-grained: "file missing", "corrupt" and "unsupported format" are three
		// DIFFERENT bugs -- content, pipeline, and missing codec respectively.
		//
		// Which means a caller can branch on the result rather than collapsing everything
		// into false and guessing. Worth its own enum for a loader that cannot be
		enum class LoadResult
		{
			Success,
			FileNotFound,
			InvalidFormat,  // not RIFF/WAVE at all
			UnsupportedEncoding,
			TruncatedFile,  // header says N bytes, file has fewer
			EmptyClip
		};

		// A PUBLIC default constructor, which is the one way to get an AudioClip that was
		// never loaded.
		//
		// DEFECT (inconsistent with the factory design): the three loaders are static
		// factories returning Ref<AudioClip>, which implies construction is meant to go
		// through them -- but this public default constructor lets a caller create a clip
		// with m_Valid false, no samples and a default 44100 rate, and only discover it at
		// use time via IsValid(). Making it private or protected would match the factory
		// pattern; the accessors below all assume a valid clip.
		AudioClip() = default;

		// Load a RIFF/WAVE file containing uncompressed PCM or IEEE float.
		//
		// Handles 8-bit unsigned, 16/24/32-bit signed, and 32-bit float, mono
		// or multichannel, by downmixing multichannel to mono at load time.
		//
		// Downmixing is the right default for a game that positions sounds in
		// 3D: the mixer pans mono samples. Keeping N channels would mean every
		// voice fights over the same position, which is not a useful model.
		// True multichannel (5.1 music) is a later problem and needs a
		// different path, not a flag on this one.
		// The primary loader, and the second parameter is the pattern worth noting: an
		// OPTIONAL out-pointer for the failure reason.
		//
		// `LoadResult *outResult = nullptr` rather than returning LoadResult directly, so
		// the success case needs no unwrapping at the common call site. A null return
		// means failure and the caller can then read the specific reason.
		//
		// DEFECT (inconsistent signature style): all three loaders use the optional
		// out-pointer, but ComputeXXX-style engine APIs more often return the enum
		static Ref<AudioClip> LoadWav(const std::string &filepath,
									  // The default makes the common case a single-token call:
									  LoadResult *outResult = nullptr);

		// Same, from memory. Used by tests and by any future streaming path
		// that has already fetched the bytes.
		// The in-memory loader, and the pre-existing comment names its two uses: tests,
		// and any future streaming path that has already fetched the bytes.
		//
		// DEFECT (no lifetime documentation): nothing here states that `data` must remain
		// valid for the duration of the call but is NOT retained afterwards. That is
		// almost certainly the intent -- a loader that kept the pointer would be a
		// dangling-pointer trap -- and the class stores a vector, which supports it. But
		// it is an unstated contract on a raw pointer, and a streaming caller would have
		// to know it.
		static Ref<AudioClip> LoadWavFromMemory(const uint8_t *data,
												// The byte length, and it is REQUIRED rather than defaulted -- a caller cannot ask
												// for "load until the end", because there is no end without a length.
												//
												// size_t so a multi-gigabyte buffer is representable and the size is compared
												size_t size,
												LoadResult *outResult = nullptr);

		// Build a clip from raw float samples. The main use is generating test
		// tones, which keeps the mixer tests free of binary fixtures.
		// The raw-samples factory, and the pre-existing comment gives the reason it
		// exists: generating test tones, which keeps mixer tests free of binary
		// fixtures.
		//
		// `samples` is taken BY VALUE and by rvalue-reference-like intent -- a caller with
		// a temporary moves it in, and a caller with an lvalue pays a copy. The engine
		static Ref<AudioClip> FromSamples(std::vector<float> samples, int sampleRate);

		// The validity query, and the only accessor that distinguishes a loaded clip from
		// a default-constructed one.
		//
		// NOTE: none of the other accessors check it. GetSampleRate() on an invalid clip
		// returns the 44100 default and GetSamples() returns an empty vector, both
		// without an error. So every caller has to remember to check IsValid() first, and
		// there is nothing to stop them forgetting.
		bool IsValid() const { return m_Valid; }
		// A const REFERENCE to the UUID, not a copy.
		//
		// Correct: the member outlives the call, and UUID is presumably a 16-byte value
		// whose copy is not free. The const reference also prevents a caller from
		const UUID &GetId() const { return m_Id; }
		const std::string &GetName() const { return m_Name; }
		// The only mutator in the public surface, and it mutates DISPLAY DATA ONLY -- not
		// samples, not m_Valid, not m_Peak.
		//
		// So renaming a clip is free and always safe, which is what a content browser
		// needs. Taken by const reference and assigned, so one copy at most.
		void SetName(const std::string &name) { m_Name = name; }

		// The rate in Hz, defaulting to 44100.
		//
		// DEFECT (default is a lie for an unloaded clip): 44100 is a plausible rate, so
		// reading it from a clip that was never loaded gives a plausible wrong answer
		// rather than an obviously invalid one. An uninitialised or zero default would
		// be fail-loud; this one is fail-quiet.
		int GetSampleRate() const { return m_SampleRate; }
		int GetChannels() const { return m_Channels; }

		// Mono, because multichannel is downmixed at load. Kept as a separate
		// accessor so the mixer never has to branch.
		// FRAMES, not samples -- and the pre-existing comment says why it is Mono:
		// multichannel is downmixed to mono at load, so frames == samples and the two are
		// the same number.
		//
		// Keeping the accessor separate is deliberate so the mixer never branches on
		// channel count. That is a real simplification, and it is also a hard commitment:
		// the moment true multichannel is needed, every caller has to be revisited rather
		size_t GetFrameCount() const { return m_Samples.size(); }
		// Out of line rather than inline, because it divides by the sample rate -- and an
		// unloaded clip has the 44100 default, so this returns 0 for empty samples
		// rather than dividing by zero. Convenient, and quietly the wrong answer for a
		// clip that failed to load.
		float GetDuration() const;
		// Direct read access to the raw sample buffer, and const so the mixer cannot
		// mutate the clip's audio through it.
		//
		// That is the right encapsulation boundary for a decode-once, read-many buffer: a
		// copy would be wasteful per voice, and a pointer-to-const would be
		// indistinguishable from the reference at the call site.
		const std::vector<float> &GetSamples() const { return m_Samples; }

		// Peak absolute amplitude, 0..1. A clip peaking above 1.0 will clip
		// when summed with others, so the mixer scales against this when asked
		// to normalise. Precomputed rather than scanned per play.
		// The peak amplitude, and the pre-existing comment explains both why it exists
		// (a clip peaking above 1.0 clips when summed with others, so the mixer scales
		// against this) and why it is cached rather than scanned ("precomputed rather
		// than scanned per play" -- a scan would be O(samples) per voice start).
		float GetPeak() const { return m_Peak; }

		// Human-readable form of a load result, for error messages a player or
		// developer can act on.
		// The enum-to-text helper, returning const char* rather than std::string.
		//
		// Correct for a table of string literals: no allocation, and the pointer is to
		// static storage that outlives any caller. Compare LoadResultToString's sibling
		// BandName in the Age module, which returns const std::string& because its names
		static const char *LoadResultToString(LoadResult result);

	private:
		// The post-load finaliser, private and called by all three factories.
		//
		// Centralising it is what makes the class's invariant hold: whatever the source
		// format, the result is float32 mono in [-1,1] with m_Peak computed and m_Valid
		// decided. A fourth construction path added later that skipped Finalise would
		// produce a clip that looks loaded and is not.
		//
		// The trailing comment names the three jobs. DEFECT (no return value): void, so
		// a validation failure inside it can only be expressed by leaving m_Valid false --
		// which the caller must then check via IsValid(), and which loses the SPECIFIC
		void Finalise();  // computes peak, validates, sets m_Valid

		// The identity, generated per clip and used as the engine's handle key.
		//
		// Present from construction, so it exists even on an unloaded clip -- which is
		// fine, and means two failed loads still have distinct ids.
		UUID m_Id;
		// The display name, EMPTY for a loaded clip unless something calls SetName.
		//
		// DEFECT (asymmetric with the path): LoadWav knows the filepath but the class
		// does not store or derive a name from it, so a freshly loaded clip has an empty
		// name while GetPath-like information is simply unavailable. Storing the stem of
		// the filename as the default name would be the obvious behaviour, and its
		std::string m_Name;
		// The decoded buffer: interleaved float32, mono, [-1, 1].
		//
		// Publicly accessible only through GetSamples()'s const reference, so the
		std::vector<float> m_Samples;
		// 44100 is a member initialiser rather than a load-time assignment, which is why
		// an unloaded clip reports it. See GetSampleRate's note.
		int m_SampleRate = 44100;
		// Always 1 after a successful load, because multichannel is downmixed at load
		// time. Kept as a member so a caller can confirm that, and so a future
		// multichannel path has somewhere to record the real count.
		int m_Channels = 1;
		// The cached peak, 0.0 for an empty clip -- which is correct (an empty buffer has
		// no amplitude), unlike the sample rate above.
		float m_Peak = 0.0f;
		// The single validity flag, and it is set ONLY by Finalise.
		//
		// Defaulting false is the fail-closed choice and is what makes the public default
		// constructor safe-ish: an unloaded clip announces itself as unloaded through
		// every accessor except the ones that quietly return plausible defaults.
		bool m_Valid = false;
	};

}
