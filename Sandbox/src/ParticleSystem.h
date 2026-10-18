#pragma once

// The engine's umbrella header, so ParticleSystem can name Timestep,
// OrthographicCamera, Renderer2D, Shader and Ref without listing each.
#include <Wasteland.h>
// GLAD, for the GLuint VA member below. Unlike the engine library itself,
// this Sandbox code is GL-specific and unguarded -- it has no Vulkan or
// D3D path.
#include <glad/glad.h>

// The description handed to Emit() to spawn one particle: where it starts,
// how it moves, how it fades, how big it is, and how long it lives.
//
// Plain aggregate with no constructor, so it brace-initialises at the call
// site. It is the TEMPLATE for a particle, not a particle -- Emit
// randomises it, which is what the Variation fields are for.
struct ParticleProps
{
	// Spawn point in world space. The live Particle struct also holds a 2D
	// position: the system is 2D-only, with a fixed Z supplied at draw time.
	glm::vec2 Position;
	// Base velocity plus a per-axis random spread. Emit applies the variation as
	// VelocityVariation[i] * (rand() - 0.5f), i.e. a symmetric spread of half
	// the stated magnitude either side of Velocity -- so VelocityVariation is
	// a peak deviation, not a full range.
	glm::vec2 Velocity, VelocityVariation;
	// Colour at spawn and at death. OnRender lerps between them using the
	// remaining fraction of life, so the pair defines the fade curve.
	glm::vec4 ColorBegin, ColorEnd;
	// Start and end size, plus a random spread applied to SizeBegin only --
	// SizeEnd is used exactly as given, so all particles shrink toward the
	// same terminal size while starting at varied sizes.
	float SizeBegin, SizeEnd, SizeVariation;
	// Seconds the particle lives. Defaulted in the aggregate, so omitting it
	// gives a one-second particle.
	float LifeTime = 1.0f;
};

// A fixed-capacity, CPU-simulated 2D particle system.
//
// Every update and render walks the ENTIRE pool and skips inactive
// particles. At the default 100000 capacity that is 100k iterations per
// frame twice over, so cost scales with pool size and not with the number
// of live particles.
//
// The pool is also never compacted: a dead particle's slot is reused by the
// round-robin index rather than a free list, so emit needs no per-particle
// search.
class ParticleSystem
{
public:
	// Allocates the pool up front.
	//
	// DEFECT: the .cpp's member initialiser is m_PoolIndex(maxParticles - 1),
	// so a caller passing 0 underflows an unsigned to 0xFFFFFFFF and then
	// indexes m_ParticlePool with it -- an out-of-bounds access on the first
	// Emit. A zero guard belongs in this constructor.
	ParticleSystem(uint32_t maxParticles = 100000);

	// Advances every active particle by one timestep.
	//
	// @param ts  Frame delta, supplied by the caller so the simulation is
	//            frame-rate independent rather than assuming a fixed step.
	void OnUpdate(Wasteland::Timestep ts);
	// Draws every active particle.
	//
	// @param camera  Supplies the view-projection for Renderer2D::BeginScene.
	//
	// Despite this class owning GL and shader state, nothing in the current
	// .cpp touches GL directly -- the draws go through Renderer2D. That is why
	// the m_QuadVA and m_ParticleShader members below are never used.
	void OnRender(Wasteland::OrthographicCamera& camera);

	// Spawns one particle from the given template.
	//
	// Overwrites whatever occupied the slot without checking whether that
	// particle was still alive, so emitting faster than particles die silently
	// truncates lifetimes rather than growing the pool.
	void Emit(const ParticleProps& particleProps);
private:
	// The live per-particle state. Private, nested, and distinct from the
	// public ParticleProps: this one holds derived values (Rotation,
	// LifeRemaining) and a validity flag, whereas ParticleProps is what a
	// caller supplies.
	struct Particle
	{
		glm::vec2 Position;
		glm::vec2 Velocity;
		glm::vec4 ColorBegin, ColorEnd;
		// Spin in radians, randomised at spawn and advanced by a fixed 0.01 rad per
		// second in OnUpdate -- a constant angular velocity for every particle, not
		// a per-particle one, since ParticleProps has no rotation field.
		float Rotation = 0.0f;
		// The spawned sizes, after the SizeVariation spread has been applied to
		// SizeBegin. Particle carries no SizeVariation of its own -- that is
		// consumed at Emit time.
		float SizeBegin, SizeEnd;

		// The particle's full lifetime in seconds, copied from ParticleProps.
		float LifeTime = 1.0f;
		// Countdown timer, also copied from ParticleProps at spawn.
		//
		// DEFECT-adjacent: defaulting to 0.0f couples this to Active. OnUpdate
		// deactivates any particle whose LifeRemaining is <= 0 BEFORE subtracting,
		// so a value-initialised Particle is inactive on sight. That is the
		// intended effect for a freshly resized pool, but it does mean Emit has to
		// set Active and LifeRemaining together -- setting Active alone would kill
		// the particle on the next update.
		float LifeRemaining = 0.0f;

		// Liveness flag, and the only per-particle thing OnUpdate and OnRender
		// check. Defaulted false so a freshly resized pool is all inactive and
		// costs only the branch.
		bool Active = false;
	};
	// The fixed-capacity pool, resized in the constructor and never grown.
	std::vector<Particle> m_ParticlePool;
	// Round-robin cursor naming the slot Emit writes next. Starts at
	// maxParticles-1 and pre-decrements on each Emit, so emission walks
	// backwards through the pool.
	//
	// DEFECT: the .cpp's update is
	//   m_PoolIndex = --m_PoolIndex % m_ParticlePool.size();
	// The pre-decrement happens BEFORE the modulo, so the first emit from the
	// initial maxParticles-1 underflows to a huge value and the modulo then
	// reduces it -- landing in an arbitrary slot rather than the intended last
	// one. Worse, the reduced value feeds the next pre-decrement, so the
	// wrap-around is perturbed on every step and the sequence is not a true
	// round robin. The form that is actually correct adds the size before
	// subtracting one, so the modulo never sees an underflowed value.
	uint32_t m_PoolIndex;

	// Vertex array for the particle quad, zero-initialised.
	//
	// DEFECT: DEAD FIELD. Nothing in ParticleSystem.cpp reads or writes it --
	// the renderer path goes through Renderer2D::DrawRotatedQuad and never
	// binds a VA of its own. Left over from a version that drew particles with
	// its own shader.
	GLuint m_QuadVA = 0;
	// The particle shader.
	//
	// DEFECT: DEAD FIELD, same as m_QuadVA above -- never constructed and never
	// used by the .cpp, so it stays null for the object's whole life.
	//
	// unique_ptr is the right ownership shape for a shader, but with nothing
	// ever assigned to it, and with a Shader type that owns GL objects needing
	// destruction in a destructor this class does not have, putting it to use
	// would introduce a leak.
	std::unique_ptr<Wasteland::Shader> m_ParticleShader;
	// Cached uniform locations for that same unused shader.
	//
	// DEFECT: DEAD FIELDS, and worse than dead -- these are NOT initialised.
	// They are plain ints with no default, so a default-constructed
	// ParticleSystem holds indeterminate values in them. Harmless today only
	// because nothing reads them; restoring the old draw path would look up
	// garbage locations and set uniforms at nonsense indices. They need a -1
	// default at the very least, so a failed lookup is distinguishable from
	// location 0.
	GLint m_ParticleShaderViewProj, m_ParticleShaderTransform, m_ParticleShaderColor;
};