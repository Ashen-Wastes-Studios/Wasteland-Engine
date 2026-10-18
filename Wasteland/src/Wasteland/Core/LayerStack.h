#pragma once

// LayerStack.h -- owns a set of Layers and defines the order they run in.
//
// The stack keeps two kinds of layer in one vector: regular layers, inserted
// at a moving index near the front, and overlays, appended at the back. Both
// kinds are updated and drawn in the same forward pass; the distinction is
// only about insertion position, which is what lets a UI overlay end up on
// top of the game layers no matter when it was pushed.
//
// Ownership: the stack takes ownership of every pointer pushed into it and
// deletes them in its destructor. See the destructor note in LayerStack.cpp --
// it does not call OnDetach, so prefer popping over letting the stack die.

#include "Wasteland/Core/Core.h"
#include "Layer.h"

#include <vector>

namespace Wasteland {

	// Ordered container of Layers, owning the layers it holds.
	//
	// Thread-confined: like Layer, a LayerStack is driven from the main
	// thread only and performs no locking.
	class WL_API LayerStack
	{
	public:
		// Constructs an empty stack. m_LayerInsertIndex starts at 0, so the
		// first pushed layer lands at the front.
		LayerStack();

		// Deletes every layer the stack owns. See the destructor comment in
		// LayerStack.cpp: this does NOT call OnDetach on them first.
		~LayerStack();

		// Inserts a regular layer at the current insertion point, which sits
		// after every previously pushed regular layer but before all
		// overlays, then advances that point.
		//
		// Does not call OnAttach -- Application::PushLayer does that, so
		// pushing straight onto the stack leaves the attach hook unrun.
		// Takes ownership; the caller must not delete the layer.
		//
		// @param layer  Non-null pointer to the layer to take over.
		void PushLayer(Layer* layer);

		// Appends an overlay after every existing layer, so it updates and
		// draws last and therefore appears on top.
		//
		// Does not call OnAttach -- see PushLayer. Takes ownership.
		//
		// @param overlay  Non-null pointer to the overlay to take over.
		void PushOverlay(Layer* overlay);

		// Removes a regular layer: finds it by pointer, calls OnDetach on it,
		// erases it, and steps the insertion point back.
		//
		// Does NOT delete the layer -- OnDetach runs, but destruction is left
		// to the caller (or to the stack's destructor, if the pointer is never
		// removed from the stack in time). Silently does nothing if the
		// pointer is not in the stack.
		//
		// Note this decrements the insertion index even when the pointer
		// found is an overlay, which can pull the insertion point in front of
		// where it should be. Prefer PopOverlay for overlays.
		//
		// @param layer  Pointer to the layer to detach and unlink.
		void PopLayer(Layer* layer);

		// Removes an overlay: finds it by pointer, calls OnDetach on it, and
		// erases it.
		//
		// Same ownership behaviour as PopLayer -- OnDetach runs, deletion does
		// not. Silently does nothing if the pointer is not in the stack.
		//
		// @param overlay  Pointer to the overlay to detach and unlink.
		void PopOverlay(Layer* overlay);

		// Forward iterator to the first layer, for range-for iteration.
		// Non-const: callers use this to drive the per-frame update and draw
		// passes, which need Layer*.
		std::vector<Layer*>::iterator begin() { return m_Layers.begin(); }

		// Forward iterator one past the last layer, completing the iterator
		// pair with begin().
		std::vector<Layer*>::iterator end() { return m_Layers.end(); }

	private:
		// All layers and overlays, in update order: regular layers first in
		// push order, then overlays in push order. Held as raw pointers
		// because the stack owns them and, on destruction, deletes them.
		std::vector<Layer*> m_Layers;

		// Index in m_Layers where the next PushLayer will insert.
		//
		// Advances on every push and retreats on every pop, so regular layers
		// always accumulate in front of the overlays regardless of the
		// interleaving of push and push-overlay calls. Unsigned, which means a
		// PopLayer that drives it below zero wraps to a huge value and turns
		// the next PushLayer's begin() + index into undefined behaviour;
		// nothing currently guards against that.
		unsigned int m_LayerInsertIndex = 0;
	};

}