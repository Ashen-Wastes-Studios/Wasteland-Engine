// Own header first, per include-what-you-use convention. It in turn
// pulls the engine umbrella and GLAD (see its line 5-9).
#include "ParticleSystem.h"

// glm::pi<float>() used at line 83 for the random starting rotation.
#include <glm/gtc/constants.hpp>
// Must be defined BEFORE any glm/gtx header is seen.
// DEFECT: it is defined here, after <glm/gtc/constants.hpp> on line 3
// has already been included. If constants.hpp (or anything else that
// pulled in a glm header) transitively defines GLM_EXPERIMENTAL_AREAS
// or friends, this define arrives too late to enable the compatibility
// helpers on line 5. The include on line 3 should come after.
#define GLM_ENABLE_EXPERIMENTAL
// Supplies glm::lerp. That helper lives in an EXTENSIONS header, which
// is why line 4 defines GLM_ENABLE_EXPERIMENTAL at all.
#include <glm/gtx/compatibility.hpp>
// DEFECT: nothing in this file calls glm::value_ptr / make_vec3 and
// friends. ParticleSystem.h already includes <glm/glm.hpp> via
// <Wasteland.h>, so this include is unused.
#include <glm/gtc/type_ptr.hpp>

// DEFECT: a #pragma once in the middle of a .cpp file. It does nothing
// (the guard is only meaningful at the top of a header) and it is
// almost certainly a leftover from a header that was split here. It
// is harmless to the compiler but misleading to a reader.
#pragma once

// Needed for the Random helper defined inline below. ParticleSystem.h
// deliberately does not include this, so the engine headers stay free
// of the random machinery.
#include <random>

// Sandbox-local helper: a seeded mt19937 behind two static functions.
// All members are static, so there is no instance and no thread
// affinity -- but that also means it is NOT thread safe. Two threads
// calling Random::Float() concurrently would race on the shared engine
// and the shared distribution.
class Random
{
public:
	// Seeds from std::random_device, i.e. from the OS entropy source.
	// DEFECT: nothing in this translation unit ever calls Init(), so the
	// engine is left default-constructed -- mt19937 with its fixed default
	// seed of 5489. Every run therefore produces the IDENTICAL particle
	// sequence. This is a visible bug: the particle effect looks the same
	// on every launch.
	static void Init()
	{
		// std::random_device is constructed then immediately invoked, and the
		// resulting value is narrowed to the engine's seed_type. Constructing
		// the device per call is also the slow way to do this, though for a
		// once-per-launch seed it does not matter.
		s_RandomEngine.seed(std::random_device()());
	}

	// Returns a float in [0, 1] by scaling a 32-bit integer sample.
	static float Float()
	{
		// s_Distribution is default-constructed (line 31), which for an
		// integral uniform_int_distribution gives the full range of the
		// result type. mt19937::result_type is uint_fast32_t, so the sample
		// range matches the uint32_t divisor here and the result is [0, 1].
		// Caveat: the full maximum is inclusive, so this can return exactly
		// 1.0f. Every call site below uses (Float() - 0.5f) for spread and
		// multiplies by 2*pi for rotation, so an exact 1.0 is harmless.
		// A std::uniform_real_distribution would be clearer and would avoid
		// the float rounding entirely.
		return (float)s_Distribution(s_RandomEngine) / (float)std::numeric_limits<uint32_t>::max();
	}

private:
	// The Mersenne Twister itself. 624 words of state, so this is a
	// process-wide object rather than anything cheap to create.
	static std::mt19937 s_RandomEngine;
	// DEFECT-adjacent: declared as a static but never given a range. The
	// default-constructed distribution happens to span the full type
	// range, which is what makes line 22 correct, but that is implicit and
	// fragile -- an explicit distribution(0, max) would state the intent.
	static std::uniform_int_distribution<std::mt19937::result_type> s_Distribution;
};

// Out-of-class definitions of the statics declared above, which are
// private so they can only be named from here.
std::mt19937 Random::s_RandomEngine;
// Default-constructed at namespace scope, i.e. before main() runs.
std::uniform_int_distribution<std::mt19937::result_type> Random::s_Distribution;

// Out-of-line definition; the default argument (100000) is declared in
// ParticleSystem.h line 58 and must not be repeated here.
ParticleSystem::ParticleSystem(uint32_t maxParticles)
	// Initialises the round-robin cursor to the LAST slot, so the first
	// Emit -- which pre-decrements (line 99) -- writes slot maxParticles-2.
	// DEFECT: with maxParticles == 0 the unsigned subtraction underflows to
	// 0xFFFFFFFF while the pool is resized to zero elements below, so the
	// first Emit indexes an empty vector: a genuine out-of-bounds access.
	// A guard rejecting 0 belongs here.
	// Note the pre-decrement itself is NOT the overflow source for the
	// normal case -- 99999 - 1 stays well within range, and line 99's
	// modulo reduces anything the decrement could have wrapped.
	: m_PoolIndex(maxParticles - 1)
{
	// Fixed capacity, allocated once. Particle has in-class initialisers
	// for Rotation/LifeRemaining/Active, so every element comes out
	// inactive with LifeRemaining 0 -- which is exactly what makes the
	// OnUpdate early-exit on line 46 correct for a fresh pool.
	// DEFECT: this is a 100000-element resize on the default path, done
	// with no reserve and no reserve-friendly sizing, so it can realloc
	// and zero-fill several times.
	m_ParticlePool.resize(maxParticles);
}

// Advances simulation. Takes the timestep by value; Timestep has an
// implicit float conversion (Timestep.h line 44), which is what makes
// the arithmetic below compile without an explicit unwrap.
void ParticleSystem::OnUpdate(Wasteland::Timestep ts)
{
	// Non-const reference because every active particle is mutated in
	// place. DEFECT: this walks all 100000 slots every frame regardless
	// of how many are alive, so cost scales with pool capacity and not
	// with live particle count. OnRender (line 61) walks them a second
	// time, doubling it.
	for (auto& particle : m_ParticlePool)
	{
		// Cheap liveness gate. `continue` rather than a nested if, keeping
		// the body of the loop at one indent level.
		if (!particle.Active)
			continue;

		// Checks BEFORE subtracting. A freshly spawned particle has
		// LifeRemaining set to LifeTime by Emit, so it survives its first
		// update; a value-initialised one has 0 and dies here.
		if (particle.LifeRemaining <= 0.0f)
		{
			// Clears the flag but leaves Position/Velocity/Colour holding the last
			// values. That is fine because both loops gate on Active, and Emit
			// overwrites every field it renders.
			// Note Active is not reset to false anywhere else, so this is the
			// only death path.
			particle.Active = false;
			continue;
		}

		// Counts down in seconds. Because the <= 0 test ran first, a particle
		// can overshoot slightly negative before being deactivated on the
		// NEXT frame -- so it lives one frame longer than LifeTime.
		particle.LifeRemaining -= ts;
		// Explicit (float) cast is redundant given Timestep's implicit
		// conversion operator, but harmless and it documents the unit.
		// Sem Euler integration: no acceleration term is applied, so
		// gravity is not supported at all despite being the usual reason to
		// want particles.
		particle.Position += particle.Velocity * (float)ts;
		// 0.01 radians per SECOND is an extremely slow constant spin (about
		// one full turn every 10 minutes). DEFECT: it is hardcoded rather
		// than taken from ParticleProps, so every particle shares one angular
		// velocity and there is no way to configure or disable it.
		particle.Rotation += 0.01f * ts;
	}
}

// camera is non-const but is only forwarded to Renderer2D::BeginScene,
// which has this same signature -- see Renderer.cpp for the same
// pattern flagged on Renderer::BeginScene.
void ParticleSystem::OnRender(Wasteland::OrthographicCamera& camera)
{
	// Opens a 2D scene. Everything drawn below runs against this camera,
	// which is why each particle re-does the view-projection math rather
	// than batching with an instanced shader.
	Wasteland::Renderer2D::BeginScene(camera);
	for (auto& particle : m_ParticlePool)
	{
		// Same liveness gate as OnUpdate; dead particles are not drawn.
		if (!particle.Active)
			continue;

		// Existing comment: 1.0 for a freshly spawned particle, approaching
		// 0.0 as it dies. Drives both the colour and size lerps below.
		// Fade away particles
		// Normalised remaining life. Division, not multiplication by a
		// precomputed rate, so this divides by zero if LifeTime is ever 0 --
		// ParticleProps defaults LifeTime to 1.0f but does not prevent a
		// caller from passing 0.
		float life = particle.LifeRemaining / particle.LifeTime;
		// Arguments are End-first, Begin-second with life as the blend factor:
		// that ordering is what makes life=1 select ColorBegin. Reversing the
		// two vectors would make particles fade in instead of out.
		glm::vec4 color = glm::lerp(particle.ColorEnd, particle.ColorBegin, life);
		// DEFECT (dead code): an alpha fade that was deliberately disabled by
		// commenting it out rather than deleting it. As written the colour
		// alpha is constant for a particle's whole life, so particles pop
		// out at full opacity instead of dissolving.
		//color.a = color.a * life;

		// Same End-to-Begin ordering as the colour: starts at SizeBegin (which
		// Emit already randomised by SizeVariation) and shrinks to SizeEnd.
		float size = glm::lerp(particle.SizeEnd, particle.SizeBegin, life);
		// Particle position is 2D (see ParticleSystem.h line 87), so Z is
		// hardcoded here to 0.2f. That constant is what puts particles in
		// front of anything drawn at Z=0 -- there is no depth sorting.
		// It is also the only place a particle is given a Z, so per-particle
		// layering is not expressible.
		glm::vec3 position = { particle.Position.x, particle.Position.y, 0.2f };
		// One draw call PER PARTICLE. DEFECT: with no instancing or batching,
		// a thousand live particles means a thousand draw calls and a
		// thousand uniform uploads. The unused m_QuadVA and m_ParticleShader
		// members in the header suggest a batched path was intended.
		Wasteland::Renderer2D::DrawRotatedQuad(position, { size, size }, particle.Rotation, color);
	}
	// Closes the scene opened on line 60. Unlike Renderer::EndScene, which
	// is empty, this one presumably tears down 2D pipeline state.
	Wasteland::Renderer2D::EndScene();
}

// Spawns one particle. Takes the template by const reference so a
// caller can reuse one ParticleProps for many emits without copying.
void ParticleSystem::Emit(const ParticleProps& particleProps)
{
	// Reference into the pool, so every write below lands in the slot.
	// DEFECT: no bounds check. Combined with the maxParticles==0 underflow
	// noted at line 34, this is the read/write that goes out of range.
	// Also no check whether the slot held a live particle -- emitting
	// faster than particles die silently truncates their lifetimes
	// rather than growing the pool.
	Particle& particle = m_ParticlePool[m_PoolIndex];
	particle.Active = true;
	particle.Position = particleProps.Position;
	// Uniform random start angle over a full turn. glm::pi<float>() from
	// the constants header added on line 3.
	particle.Rotation = Random::Float() * 2.0f * glm::pi<float>();

	// Velocity
	// Starts from the base velocity; the two lines below add symmetric
	// per-axis spread on top.
	particle.Velocity = particleProps.Velocity;
	// Random - 0.5 maps [0,1] to [-0.5, 0.5], so VelocityVariation is a
	// PEAK deviation, not a full range. The header comment on
	// ParticleProps line 22-25 documents that contract.
	particle.Velocity.x += particleProps.VelocityVariation.x * (Random::Float() - 0.5f);
	// Same spread on Y. Two independent samples, so the result fills the
	// full square rather than a line.
	particle.Velocity.y += particleProps.VelocityVariation.y * (Random::Float() - 0.5f);

	// Color
	// Colour pair copied verbatim -- no per-particle colour variation
	// exists in ParticleProps, so every particle in an emit shares them.
	particle.ColorBegin = particleProps.ColorBegin;
	particle.ColorEnd = particleProps.ColorEnd;

	// Stored as the denominator for the lerps in OnRender.
	particle.LifeTime = particleProps.LifeTime;
	// Start the countdown at full lifetime. Must be set together with
	// Active on line 81 -- if only Active were set, OnUpdate line 46 would
	// kill the particle on its very next tick because LifeRemaining
	// defaults to 0.0f.
	particle.LifeRemaining = particleProps.LifeTime;
	// Same symmetric spread as the velocity. DEFECT: this line is much
	// longer than its neighbours and is the one line in the file that was
	// presumably wrapped at some point; it reads as a wall of text.
	particle.SizeBegin = particleProps.SizeBegin + particleProps.SizeVariation * (Random::Float() - 0.5f);
	// No variation applied to the end size, so every particle converges
	// on the same terminal size -- only the start sizes differ.
	particle.SizeEnd = particleProps.SizeEnd;

	// Advance the round-robin cursor. The pre-decrement binds FIRST, so
	// the modulo operates on the already-decremented value.
	// Because the types differ, m_PoolIndex (uint32_t) is promoted to the
	// size_t returned by size(), so the modulo is well-defined even when
	// the pre-decrement wraps -- which is why this line does not itself
	// fault. Emission therefore walks backwards through the pool and
	// overwrites the oldest slot first, bounding memory at the cost of
	// truncating lifetimes.
	// Style: no spaces around the % operator, unlike every other operator
	// in this file.
	m_PoolIndex = --m_PoolIndex % m_ParticlePool.size();
}