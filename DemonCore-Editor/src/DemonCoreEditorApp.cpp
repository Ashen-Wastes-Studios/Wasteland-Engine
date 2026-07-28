#include <Wasteland.h>
#include "Wasteland/Core/EntryPoint.h"

#include "EditorLayer.h"

namespace Wasteland {

	class DemonCoreEditor : public Application
	{
	public:
		DemonCoreEditor()
			: Application("DemonCore Editor")
		{
			PushLayer(new EditorLayer());
		}

		~DemonCoreEditor()
		{

		}

	};

	Application* CreateApplication()
	{
		return new DemonCoreEditor();
	}

}
