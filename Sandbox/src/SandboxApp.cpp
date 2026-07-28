#include <Wasteland.h>
#include "Wasteland/Core/EntryPoint.h"

#include "Sandbox2D.h"

class Sandbox : public Wasteland::Application
{
public:
	Sandbox()
	{
		PushLayer(new Sandbox2D());
	}

	~Sandbox()
	{

	}

};

Wasteland::Application* Wasteland::CreateApplication()
{
	return new Sandbox();
}
