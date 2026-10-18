#pragma once

// <string> for the names and the output path, <chrono> for the clock,
// <algorithm> for std::replace in the JSON escaper, <fstream> for the stream.
// std::thread is included for std::this_thread::get_id and std::hash below.
#include <string>
#include <chrono>
#include <algorithm>
#include <fstream>

#include <thread>

// A header-only pair of classes -- there is no Instrumentor.cpp in this
// directory. Everything is inline by virtue of being defined in-class, which is
// what lets WL_PROFILE_FUNCTION below work in any translation unit with no link
// dependency beyond this header.
namespace Wasteland {

	// One scoped timing sample, in the Chrome Trace Event format the output file
	// targets: `cat` is the category, `ph` "X" marks a complete event with a duration,
	// `pid` is a single process id and `tid` separates threads.
	//
	// Start/End are epoch counts in MILLISECONDS (see InstrumentationTimer::Stop),
	// whereas the Trace Event format specifies microseconds. DEFECT (unit mismatch):
	// durations are therefore 1000x too small when chrome://tracing or Perfetto
	// renders the file, so every span reads as zero-ish. Nothing here is broken for a
	// consumer that treats the numbers as opaque milliseconds. Not fixed -- comments
	// only.
	//
	// No default member initialisers: every field is filled by WriteProfile's braced
	// initialiser, so a default-constructed ProfileResult would hold indeterminate
	// values rather than zeros. Safe only because the single construction site is
	// the aggregate on the Stop() call.
	struct ProfileResult
	{
		std::string Name;
		long long Start, End;
		uint32_t ThreadID;
	};

	// A struct with one member, and no constructor: it exists so the session name
	// has somewhere to live. The profile counter below is on Instrumentor, not here,
	// so it counts entries across the whole session regardless of which session name
	// was in force.
	struct InstrumentationSession
	{
		std::string Name;
	};

	// The singleton profiler. Not thread-safe by design: WriteProfile mutates
	// m_ProfileCount and the ofstream with no lock, so two threads timing
	// overlapping scopes interleave their comma separators and can emit malformed
	// JSON. The "tid" field in ProfileResult makes it look as though concurrent
	// threads are supported; the writer is the part that is not.
	class Instrumentor
	{
	private:
		// Raw pointer plus `new` in BeginSession rather than a value member or
		// unique_ptr. DEFECT (redundant ownership): the session is created in
		// BeginSession and deleted in EndSession, and nothing reads it -- ProfileResult
		// carries its own name copy. The member could be removed entirely.
		//
		// It is also the leak in the pairing below: BeginSession called twice without an
		// intervening EndSession overwrites the pointer and orphans the first session.
		InstrumentationSession* m_CurrentSession;
		// Opened in text mode and flushed after every write. The per-entry flush is the
		// expensive part -- one fflush per profile sample -- and is what makes the file
		// survive a crash mid-session, which is presumably the point of a profiler.
		std::ofstream m_OutputStream;
		// The comma separator for JSON entries, decided by post-increment: the first
		// call sees 0 and writes nothing, every later call sees >= 1 and writes a comma.
		//
		// Coupling worth knowing: this counter also resets the JSON framing. EndSession
		// sets it back to 0, so a second BeginSession/EndSession pair in one process
		// writes correct output -- but any path that wrote entries without incrementing
		// this would break the framing.
		int m_ProfileCount;
	public:
		Instrumentor()
			: m_CurrentSession(nullptr), m_ProfileCount(0)
		{
		}

		// BeginSession opens with the default open() mode, so an existing results.json
		// is TRUNCATED rather than appended. A second run therefore discards the first
		// run's trace, and there is no "append" mode to switch to.
		//
		// The stream is not checked: if the path is unwritable the ofstream is in a failed
		// state, and every later << is a silent no-op -- the run continues, unprofiled,
		// with no message. Contrast ApiKeyStore::Save, which does check is_open().
		void BeginSession(const std::string& name, const std::string& filepath = "results.json")
		{
			// The header is written before the session exists, so the JSON is well-formed
			// from the first byte: an empty run still produces {"otherData":{},"traceEvents":[]}
			// rather than a truncated file.
			m_OutputStream.open(filepath);
			WriteHeader();
			// `new` rather than make_unique or a member. DEFECT: see the note on the member
			// above -- this allocation is never read, and a second BeginSession leaks it.
			// Brace initialisation of a one-member aggregate is the whole construction.
			m_CurrentSession = new InstrumentationSession{ name };
		}

		// EndSession is the only place the output is closed, so a session left open at
		// process exit leaves the file without its "]}" footer. The per-entry flush means
		// the entries survive; the JSON does not.
		void EndSession()
		{
			WriteFooter();
			m_OutputStream.close();
			// delete then null in that order, so a re-entrant call during destruction would
			// see the pointer rather than a dangling one. Matches BeginSession's raw-pointer
			// ownership -- the two are consistent, which is the important part.
			delete m_CurrentSession;
			m_CurrentSession = nullptr;
			m_ProfileCount = 0;
		}

		// The single writer. Everything about JSON correctness -- comma placement,
		// string escaping, field order -- lives here and nowhere else.
		void WriteProfile(const ProfileResult& result)
		{
			// DEFECT (escaping): only the double quote is replaced, with a single quote. A
			// name containing a BACKSLASH is passed through unchanged, and Chrome Trace JSON
			// treats backslash as an escape character -- so a function signature containing
			// one (WL_PROFILE_FUNCTION passes __FUNCSIG__, which is full of them) produces
			// invalid JSON. On MSVC __FUNCSIG__ renders as "void Class::Method(void)" with no
			// backslashes, so this does not fire today; it would the moment a name were
			// passed with a path or a template argument in it. Not fixed -- comments only.
			if (m_ProfileCount++ > 0)
				m_OutputStream << ",";

			std::string name = result.Name;
			std::replace(name.begin(), name.end(), '"', '\'');

			// "cat":"function" -- the category every entry is filed under. Not configurable,
			// which is why the file opens with an empty "otherData": there is nowhere else to
			// put metadata such as the session name.
			m_OutputStream << "{";
			m_OutputStream << "\"cat\":\"function\",";
			// dur = End - Start, i.e. the measured span in milliseconds. Both operands come
			// from the same clock in Stop(), so the subtraction cannot go negative except
			// across a clock adjustment -- which high_resolution_clock does not have, being
			// monotonic on every platform this engine targets.
			m_OutputStream << "\"dur\":" << (result.End - result.Start) << ",";
			m_OutputStream << "\"name\":\"" << name << "\",";
			m_OutputStream << "\"ph\":\"X\",";
			// pid 0 hardcoded: the format wants a process id and the file covers one process,
			// so any constant works. A merged trace from two runs would collide -- harmless,
			// because they are separate files.
			m_OutputStream << "\"pid\":0,";
			m_OutputStream << "\"tid\":" << result.ThreadID << ",";
			// ts is the absolute start time rather than a duration. With a monotonic epoch
			// this is an arbitrary but consistent origin, which is all a viewer needs to lay
			// spans out relative to each other.
			m_OutputStream << "\"ts\":" << result.Start;
			m_OutputStream << "}";

			m_OutputStream.flush();
		}

		// The Chrome Trace container. "otherData" is empty by design; a viewer reads
		// traceEvents and ignores the rest.
		void WriteHeader()
		{
			m_OutputStream << "{\"otherData\": {},\"traceEvents\":[";
			m_OutputStream.flush();
		}

		// The footer closes both the array and the object. Flushed like every other
		// write, for the same crash-survival reason.
		void WriteFooter()
		{
			m_OutputStream << "]}";
			m_OutputStream.flush();
		}

		// The Meyers singleton: thread-safe initialisation is guaranteed by the standard
		// since C++11, so the static is constructed exactly once even if two threads race
		// to call this. What is NOT thread-safe is everything the object then does.
		static Instrumentor& Get()
		{
			// Function-local static rather than a namespace-scope one: this delays
			// construction to first use, so an application that never profiles never opens a
			// file. Compare RenderCommand.cpp, which takes the eager global-initialiser
			// approach for its renderer.
			static Instrumentor instance;
			return instance;
		}
	};

	// A scope timer, not a manual start/stop pair: the measurement is tied to a C++
	// lifetime so it cannot be forgotten. Used by WL_PROFILE_SCOPE /
	// WL_PROFILE_FUNCTION at the end of this file.
	class InstrumentationTimer
	{
	public:
		// `name` is stored as a raw `const char*` and never copied. For
		// WL_PROFILE_FUNCTION that is __FUNCSIG__, a static string, so it is safe; for a
		// WL_PROFILE_SCOPE call with a computed name it would dangle. DEFECT (lifetime):
		// the member should be a std::string. Cheap to fix and not fixed here.
		InstrumentationTimer(const char* name)
			: m_Name(name), m_Stopped(false)
		{
			m_StartTimepoint = std::chrono::high_resolution_clock::now();
		}

		// The destructor, which is what actually records the sample. Stopping here
		// rather than in a caller means an early return or a thrown exception still
		// produces a measurement -- the timer cannot be leaked past.
		~InstrumentationTimer()
		{
			// Guarded so an explicit Stop() followed by scope exit does not record the same
			// span twice. Without it, `Stop()` followed by destruction would emit two entries
			// for one measurement and double-count the frame.
			if (!m_Stopped)
				Stop();
		}

		// Stop is public so a caller can end a measurement early -- to exclude teardown
		// from the span, for instance. Idempotent by virtue of the m_Stopped flag above.
		void Stop()
		{
			// Both endpoints are taken from high_resolution_clock, which is steady on every
			// platform this engine targets. A wall clock would allow a backward jump to
			// produce a negative duration.
			auto endTimepoint = std::chrono::high_resolution_clock::now();

			// DEFECT (precision loss): cast to MILLISECONDS before the difference, so a
			// sub-millisecond scope records Start == End and dur == 0. The underlying clock is
			// nanosecond-resolution; the truncation to milliseconds happens first and the two
			// casts never see finer detail. This is the same unit decision that makes the
			// trace 1000x slow for a viewer expecting microseconds.
			long long start = std::chrono::time_point_cast<std::chrono::milliseconds>(m_StartTimepoint).time_since_epoch().count();
			long long end = std::chrono::time_point_cast<std::chrono::milliseconds>(endTimepoint).time_since_epoch().count();

			// std::hash<thread::id> -- a hash of the thread id, not the id itself. The Trace
			// format wants an integer lane id, and any stable per-thread number serves; the
			// hash is stable for the lifetime of the thread, which is all that is required.
			uint32_t threadID = std::hash<std::thread::id>{}(std::this_thread::get_id());
			// The one call into the singleton. An aggregate initialiser, so ProfileResult is
			// filled without a constructor -- m_Stopped is set on the next line, not here.
			Instrumentor::Get().WriteProfile({ m_Name, start, end, threadID });

			m_Stopped = true;
		}
	private:
		// `const char*`, matching the lifetime caveat noted at the constructor. See the
		// DEFECT note there.
		const char* m_Name;
		// high_resolution_clock::time_point, default-constructed then assigned in the
		// constructor's body. Assigning rather than initialising in the member list is
		// harmless here -- it is the first statement either way.
		std::chrono::time_point<std::chrono::high_resolution_clock> m_StartTimepoint;
		// Not defaulted, and not `= false`: the constructor sets it, so there is no
		// window in which the destructor could observe an indeterminate flag if the
		// constructor threw between the two assignments.
		bool m_Stopped;
	};

}

// DEFECT (hardcoded on): the switch is written as a literal 1 rather than a
// build setting, so profiling is compiled into every build -- release included --
// and the WL_PROFILE==0 branch below is unreachable without editing this header.
// The cost is real: every WL_PROFILE_FUNCTION call site constructs a timer, and
// with an open session each one does a stream write plus an fflush.
// Not fixed here -- comments only.
#define WL_PROFILE 1
#if WL_PROFILE
// Fully qualified as ::Wasteland::..., because the macro is expanded at call
// sites that may be inside any namespace, including none at all.
#define WL_PROFILE_BEGIN_SESSION(name, filepath) ::Wasteland::Instrumentor::Get().BeginSession(name, filepath)
#define WL_PROFILE_END_SESSION() ::Wasteland::Instrumentor::Get().EndSession()
// timer##__LINE__ token-pastes the line number into the variable name, so two
// WL_PROFILE_FUNCTION calls on different lines of the same function do not
// collide. Two calls on the SAME line would not compile -- an acceptable
// limitation of a macro-based approach.
#define WL_PROFILE_SCOPE(name) ::Wasteland::InstrumentationTimer timer##__LINE__(name);
// __FUNCSIG__ rather than __func__: the MSVC-specific spelling gives the full
// signature including the class and return type, which is what makes a profile
// readable. It is also why the escaping note on WriteProfile matters -- this is a
// platform-specific macro in otherwise portable code.
#define WL_PROFILE_FUNCTION() WL_PROFILE_SCOPE(__FUNCSIG__)
#else
#define WL_PROFILE_BEGIN_SESSION(name, filepath)
#define WL_PROFILE_END_SESSION()
#define WL_PROFILE_SCOPE(name)
// Both branches expand to nothing, which is what keeps the call sites free of
// #ifdef. The asymmetry to watch: WL_PROFILE_FUNCTION() still leaves a trailing
// semicolon at every call site, so the disabled form must remain a valid
// statement -- which an empty macro does.
#define WL_PROFILE_FUNCTION()
#endif