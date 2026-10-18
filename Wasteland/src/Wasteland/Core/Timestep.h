#pragma once

// Timestep.h -- a thin, type-safe wrapper around a frame delta in seconds.
//
// The point of this class is to stop a bare float from being passed around as
// a frame delta. A function taking `float dt` invites a caller to pass a
// distance, a speed, or an absolute timestamp; a function taking `Timestep`
// cannot, because Timestep yields a float only via an explicit unwrap or an
// implicit conversion in a context that genuinely wants a duration.
//
// It is deliberately tiny: a single float, no clamping, no accumulation, no
// smoothing. All of that belongs to the caller. See the note on OnUpdate in
// Layer.h -- Application::Run does NOT bound the value, so a Timestep can
// legitimately be very large.

namespace Wasteland {

	// A frame delta time, in seconds.
	//
	// Implicitly convertible to float, so existing arithmetic
	// (`position += velocity * ts`) keeps working without a call to
	// GetSeconds(). The conversion is implicit rather than explicit so that
	// Timestep stays a drop-in for the float it replaces.
	class Timestep
	{
	public:
		// Wraps a duration in seconds.
		//
		// The parameter is named `time` but holds a delta, not an absolute
		// time; the name is the original's and is left as-is. The default of
		// 0.0f means a default-constructed Timestep contributes no motion,
		// which is the safe default for a partially-initialized object.
		//
		// No clamping or validation is performed: a negative or enormous
		// value is stored verbatim.
		Timestep(float time = 0.0f)
			: m_Time(time)
		{
		}

		// Implicit conversion to float, so a Timestep can be used directly
		// anywhere a duration is expected -- multiplying a velocity, or
		// scaling a matrix -- without unwrapping it first.
		operator float() const { return m_Time; }

		// Returns the delta in seconds, exactly as supplied at construction.
		// This is the same value the float conversion yields; the named
		// accessor exists for call sites where that reads better than an
		// implicit conversion.
		float GetSeconds() const { return m_Time; }

		// Returns the delta in milliseconds, for UI that displays frame
		// timing and for profiling code that reports in ms.
		//
		// Computed on demand by scaling the stored seconds by 1000.0 rather
		// than storing a second representation, so the two units cannot
		// drift apart.
		float GetMilliseconds() const { return m_Time * 1000.0; }

	private:
		// The frame delta in seconds. Private with no setter: a Timestep is
		// an immutable value, so it is safe to pass by value or hold across
		// frames without worrying that something will mutate it midway.
		float m_Time;
	};

}