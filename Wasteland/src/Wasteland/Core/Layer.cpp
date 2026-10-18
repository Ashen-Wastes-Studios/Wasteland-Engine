#include "wlpch.h"
#include "Layer.h"

namespace Wasteland {

	// Constructs a layer, storing the supplied name for later retrieval
	// through GetName().
	//
	// The parameter is spelled `debugName` here but `name` in the Layer.h
	// declaration; the two name the same value and callers pass it
	// positionally, so the difference is inert and has been left alone.
	//
	// The initialiser list copies into m_DebugName directly, so m_DebugName
	// is fully constructed before the (empty) body runs. Nothing else is
	// initialised: a Layer holds no other state, and everything a subclass
	// adds is that subclass's responsibility.
	Layer::Layer(const std::string& debugName)
		: m_DebugName(debugName)
	{
		// Intentionally empty.
	}

	// Virtual destructor, defined out of line in this translation unit
	// rather than inline in the header.
	//
	// Empty by design: teardown belongs in OnDetach, which LayerStack calls
	// on an explicit pop. Note that ~LayerStack bypasses that and simply
	// deletes its layers, so a layer destroyed with the stack never gets
	// this chance to clean up.
	//
	// It must be virtual (see the header) so that deleting through a Layer*
	// -- which is how the LayerStack holds layers -- runs the most-derived
	// destructor. Without that, destroying a derived layer via a base
	// pointer would skip that layer's own destructor and leak whatever it
	// owned.
	Layer::~Layer()
	{
		// Intentionally empty.
	}

}
