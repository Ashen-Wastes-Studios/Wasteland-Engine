// Include guard for the VertexArray interface.
#pragma once

// <memory> alone does NOT provide Ref -- that alias is declared in
// Wasteland/Core/Core.h, which this header does not include.

// DEFECT (self-containment): both Ref<VertexBuffer> and Ref<IndexBuffer> are
// used below, and neither name is declared by this file's own includes.
// Buffer.h (included on the next line) does not include Core.h either, so
// this header compiles only because every consumer reaches it after wlpch.h
// or Core.h -- e.g. Wasteland.h includes Core.h first, VertexArray.h last. A
// translation unit including VertexArray.h on its own fails to compile.
#include <memory>
// Supplies VertexBuffer and IndexBuffer, the two things a vertex array binds
// together, plus BufferLayout.

// Including <memory> directly is also redundant: Buffer.h does not include
// Core.h either, so <memory> is not what makes Ref available.
#include "Wasteland/Renderer/Buffer.h"

namespace Wasteland {

	// A vertex array object: the GL/NVRHI structure that remembers which vertex
	// buffers are enabled, how their attributes are interleaved, and which index
	// buffer draws use.

	// This is the unit a draw call binds. It exists because vertex attribute
	// state is global in OpenGL -- set it once here at construction rather than
	// per draw.
	class VertexArray
	{
	public:
		// Inline, empty, and written out rather than `= default`: equivalent for a
		// class with no members needing destruction, since Ref members in derived
		// classes destruct themselves.

		// `virtual` is redundant here -- a base-class destructor is already virtual
		// -- but harmless.
		virtual ~VertexArray() {}

		// Makes this array current. Const because it changes GL state, not the object.

		// Pure virtual: this class is an interface with no implementation. The factory
		// at the bottom of the class is the only way to obtain one.
		virtual void Bind() const = 0;
		virtual void Unbind() const = 0;

		// Registers a vertex buffer and applies its BufferLayout as attribute state.

		// Takes a Ref, so the array keeps the buffer alive; the caller may drop its
		// own reference immediately.

		// "Add" rather than "Set": a vertex array can hold several vertex buffers
		// (typically one per attribute stream), each at a different attribute offset.
		virtual void AddVertexBuffer(const Ref<VertexBuffer>& vertexBuffer) = 0;
		// Attaches the index buffer. Singular -- one per array -- hence "Set" not
		// "Add": calling it again replaces, which matters because OpenGL captures
		// the element-array binding per vertex array object.
		virtual void SetIndexBuffer(const Ref<IndexBuffer>& indexBuffer) = 0;

		// Read-only view of the bound vertex buffers.

		// Returns a const reference to the internal vector, so the caller pays no
		// copy and cannot mutate it. The reference is only valid while the array
		// lives and until the next AddVertexBuffer call, which may reallocate.

		// std::vector is used without <vector> being included here -- it arrives via
		// Buffer.h.
		virtual const std::vector<Ref<VertexBuffer>>& GetVertexBuffers() const = 0;
		// The attached index buffer, or a null Ref if none was ever set. Const
		// reference, same lifetime caveat as above.
		virtual const Ref<IndexBuffer>& GetIndexBuffer() const = 0;

		// The only way to obtain one: dispatches on Renderer::GetAPI() to construct
		// OpenGLVertexArray or the NVRHI equivalent.

		// No spec arguments, unlike Shader::Create or Framebuffer::Create -- a vertex
		// array needs no configuration, so the factory is parameterless.
		static Ref<VertexArray> Create();
	};

}