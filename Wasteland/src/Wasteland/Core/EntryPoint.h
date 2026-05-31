#pragma once

#ifdef WL_PLATFORM_WINDOWS

extern Wasteland::Application *Wasteland::CreateApplication();

int main(int argc, char **argv)
{
	Wasteland::Log::Init();

	WL_PROFILE_BEGIN_SESSION("Startup", "WastelandProfile-Startup.json");
	auto app = Wasteland::CreateApplication();
	WL_PROFILE_END_SESSION();

	WL_PROFILE_BEGIN_SESSION("Runtime", "WastelandProfile-Runtime.json");
	app->Run();
	WL_PROFILE_END_SESSION();

	WL_PROFILE_BEGIN_SESSION("Shutdown", "WastelandProfile-Shutdown.json");
	delete app;
	WL_PROFILE_END_SESSION();
}

#endif