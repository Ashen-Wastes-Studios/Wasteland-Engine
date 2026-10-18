// Angle-bracket form of the engine's own umbrella header, which pulls in
// Application, Layer and the Ref<> smart pointer. Layer is what Sandbox2D
// derives from below, and Application is the base of the Sandbox class.
#include <Wasteland.h>
// This is the header that DEFINES main(). Including it here is what makes this
// file a client executable rather than another library: EntryPoint.h's main()
// logs off, bootstraps the project, calls CreateApplication(), runs it, and
// deletes it.
//
// EntryPoint.h is Windows-only (WL_PLATFORM_WINDOWS), and it is compiled into
// exactly one translation unit per client -- this one for Sandbox.
//
// It also declares `extern Wasteland::Application* Wasteland::CreateApplication`,
// which is the contract implemented at the bottom of this file.
#include "Wasteland/Core/EntryPoint.h"

// The one layer this client has. Declared here rather than forward-declared
// because the constructor below news it up, which needs the complete type.
#include "Sandbox2D.h"

// The client's Application subclass -- the only thing the engine knows about
// this executable. It holds no state of its own: everything is in the single
// Sandbox2D layer it pushes.
//
// Sandbox is at global scope, not inside namespace Wasteland, because
// CreateApplication() below is defined with the qualified return type
// `Wasteland::Application*`. Either placement compiles; this one keeps the
// qualification on the definition consistent with the declaration in
// EntryPoint.h.
class Sandbox : public Wasteland::Application
{
public:
	// The layer is pushed from the CONSTRUCTOR, before main() has called Run().
	// That matters because Application::PushLayer -- not LayerStack::PushLayer --
	// is what fires Sandbox2D::OnAttach(), so attaching this early gives the layer
	// its textures and shader before the first frame.
	//
	// PushLayer takes a raw Layer* and takes ownership: LayerStack deletes it on
	// teardown, which is why there is no matching delete in the destructor below.
	Sandbox()
	{
		// raw new, deliberately leaked into the layer stack's ownership. Passing a
		// smart pointer would not compile against PushLayer's Layer* parameter.
		//
		// Pushed as a regular layer, not an overlay, so it sits at the bottom of the
		// stack and draws before anything else -- here, before ImGui, which
		// Application installs separately.
		//
		// No null check on the result: a bad_alloc here propagates out of the
		// Application constructor and out of CreateApplication, and main() has no
		// handler for it.
		PushLayer(new Sandbox2D());
	}

	// Empty, and the body is blank rather than `= default`: the layer is NOT
	// deleted here. The base Application destructor tears down the layer stack,
	// which owns and deletes Sandbox2D.
	//
	// Writing a manual delete of the layer here would be a double free, since
	// Application's own teardown would delete the same pointer again.
	//
	// Why a user-provided destructor at all, then? It is redundant -- `= default`
	// would behave identically. Appears to be a leftover template stub.
	~Sandbox()
	{

	}

};

// The engine-to-client factory, declared extern in EntryPoint.h and defined
// here. The return type is fully qualified even though no namespace is opened
// -- legal, and it is what makes this definition match the declaration.
//
// main() treats the result as non-null (it calls app->Run() directly), so a
// client that returned nullptr here would crash on the first statement after
// the project bootstrap.
Wasteland::Application* Wasteland::CreateApplication()
{
	// Ownership passes to main(), which deletes it after Run() returns (and after
	// wrapping that delete in a shutdown profiling scope). The Application keeps no
	// static owner of it beyond s_Instance during its lifetime.
	//
	// Constructed rather than merely allocated: the Sandbox2D layer is pushed, and
	// OnAttach fires, as a side effect of this `new`.
	return new Sandbox();
}
