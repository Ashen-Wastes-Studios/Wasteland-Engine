#include <Wasteland.h>
#include "Wasteland/Core/EntryPoint.h"

#include "EditorLayer.h"

#include <cstdlib>

namespace Wasteland {

	// Test hook: WL_AUTO_SWITCH_TO=N flips the renderer to API N a few
	// frames after startup (2=DX11, 3=DX12, 4=Vulkan), WL_AUTO_SWITCH_QUIT
	// closes the app that many frames later. Hands-free reproduction of
	// Settings-combo backend switches.
	class AutoSwitchLayer : public Layer
	{
	public:
		AutoSwitchLayer(RendererAPI::API api, int quitFrames,
			RendererAPI::API api2 = RendererAPI::API::None, int api2Frame = 60)
			: m_API(api), m_QuitFrames(quitFrames), m_API2(api2), m_API2Frame(api2Frame) {}
		void OnUpdate(Timestep) override
		{
			m_Frames++;
			if (m_Frames == 5)
				Application::Get().GetWindow().SwitchRendererAPI(m_API);
			if (m_API2 != RendererAPI::API::None && m_Frames == m_API2Frame)
				Application::Get().GetWindow().SwitchRendererAPI(m_API2);
			if (m_QuitFrames > 0 && m_Frames >= m_QuitFrames)
				Application::Get().Close();
		}
	private:
		RendererAPI::API m_API;
		RendererAPI::API m_API2 = RendererAPI::API::None;
		int m_API2Frame = 60;
		int m_QuitFrames;
		int m_Frames = 0;
	};

	class DemonCoreEditor : public Application
	{
	public:
		DemonCoreEditor()
			: Application("DemonCore Editor")
		{
			PushLayer(new EditorLayer());
			if (const char *sw = std::getenv("WL_AUTO_SWITCH_TO"))
			{
				int quit = 0;
				if (const char *q = std::getenv("WL_AUTO_SWITCH_QUIT"))
					quit = std::atoi(q);
				RendererAPI::API api2 = RendererAPI::API::None;
				int api2Frame = 60;
				if (const char *sw2 = std::getenv("WL_AUTO_SWITCH_TO2"))
				{
					api2 = (RendererAPI::API)std::atoi(sw2);
					if (const char *f = std::getenv("WL_AUTO_SWITCH_TO2_FRAME"))
						api2Frame = std::atoi(f);
				}
				PushLayer(new AutoSwitchLayer((RendererAPI::API)std::atoi(sw), quit, api2, api2Frame));
			}
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
