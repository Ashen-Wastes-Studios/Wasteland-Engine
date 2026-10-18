#include "wlpch.h"
#include "LayerStack.h"

namespace Wasteland {

	// Constructs an empty stack. Nothing to do: both members are default
	// initialized in the header (empty vector, insert index 0), so there is no
	// member-init list here.
	LayerStack::LayerStack()
	{
	}

	// Destroys the stack, deleting every layer it owns.
	//
	// Note the asymmetry with the pop functions: this deletes each layer
	// WITHOUT calling its OnDetach first. A layer that acquires something in
	// OnAttach and releases it in OnDetach will therefore never run that
	// release if the stack is destroyed while it still holds the layer. In
	// practice the engine's own LayerStack outlives the layers it is given, so
	// this path is reached mainly in tests and in teardown-ordering bugs.
	//
	// Deleting rather than leaking is still the right default: the stack owns
	// these pointers, so it is the only thing that may free them.
	LayerStack::~LayerStack()
	{
		// Range-for over the raw pointers; nothing mutates the vector here,
		// so the iterators stay valid throughout.
		for (Layer* layer : m_Layers)
			// Each Layer has a virtual destructor, so this correctly runs the
			// most-derived destructor through the base pointer.
			delete layer;
	}

	// Inserts a layer at the moving insertion point and advances that point.
	//
	// Inserting at begin() + m_LayerInsertIndex rather than at the back is
	// what keeps regular layers ahead of overlays no matter what order the two
	// kinds were pushed in. Overlays occupy the tail of the vector, so this
	// index naturally lands just in front of the first overlay once one
	// exists.
	//
	// emplace (not push_back) to construct the pointer in place at the target
	// position, avoiding a temporary plus a move.
	void LayerStack::PushLayer(Layer* layer)
	{
		m_Layers.emplace(m_Layers.begin() + m_LayerInsertIndex, layer);
		m_LayerInsertIndex++;
	}

	// Appends an overlay at the very back of the vector.
	//
	// No index bookkeeping, which is correct precisely because overlays are
	// always added after every regular layer: m_LayerInsertIndex refers to a
	// position that this append cannot invalidate.
	void LayerStack::PushOverlay(Layer* overlay)
	{
		m_Layers.emplace_back(overlay);
	}

	// Detaches and unlinks a regular layer.
	//
	// The lookup is a linear scan by pointer; layer counts are small (single
	// digits in practice), so a map or index would cost more than it saves.
	//
	// OnDetach runs before the erase so the layer can still reach its own
	// state, but the layer is NOT deleted -- the caller retains that decision.
	// The m_LayerInsertIndex decrement keeps the insertion point aligned with
	// the layers that remain. It is applied whenever a match is found,
	// including when the match is an overlay, which is a latent
	// inconsistency: popping an overlay through this function steps the index
	// back as though a regular layer had been removed.
	void LayerStack::PopLayer(Layer* layer)
	{
		auto it = std::find(m_Layers.begin(), m_Layers.end(), layer);
		// No match means the layer was never pushed, or was already popped.
		// Silently ignoring that keeps callers from having to pre-check.
		if (it != m_Layers.end())
		{
			layer->OnDetach();
			m_Layers.erase(it);
			// Unsigned: if this is ever reached with the index already 0, the
			// decrement wraps rather than going negative, and the next
			// PushLayer would compute begin() + (huge value) -- undefined
			// behaviour. No guard exists today; popping something that was
			// never pushed as a regular layer is what would trigger it.
			m_LayerInsertIndex--;
		}
	}

	// Detaches and unlinks an overlay.
	//
	// Same linear-search-then-detach shape as PopLayer, minus the insertion
	// index bookkeeping -- correctly, since removing from the tail cannot move
	// the position that m_LayerInsertIndex refers to.
	void LayerStack::PopOverlay(Layer* overlay)
	{
		auto it = std::find(m_Layers.begin(), m_Layers.end(), overlay);
		// Note the missing braces: as written, only OnDetach is guarded by
		// the `if`. m_Layers.erase(it) sits outside it and therefore runs
		// unconditionally. When the pointer IS found that is harmless --
		// it is erasing the element it just found. When the pointer is NOT
		// found, it == end(), and erase(end()) is undefined behaviour. So a
		// caller that pops an overlay which was never pushed corrupts the
		// vector rather than getting the silent no-op that PopLayer gives.
		// Left exactly as written because this pass is comments-only; adding
		// braces would change behaviour and belongs in its own commit.
		if (it != m_Layers.end())
			overlay->OnDetach();
		m_Layers.erase(it);
	}

}