#pragma once

#include "Scene.h"

namespace Wasteland
{

	class SceneSerializer
	{
	public:
		SceneSerializer(const Ref<Scene> &scene);

		void Serialize(const std::string &filepath);
		void SerializeRuntime(const std::string &filepath);

		bool Deserialize(const std::string &filepath);
		bool DeserializeRuntime(const std::string &filepath);

		static std::string Trim(const std::string &str)
		{
			size_t first = str.find_first_not_of(" \t\r\n");
			if (first == std::string::npos)
				return "";
			size_t last = str.find_last_not_of(" \t\r\n");
			return str.substr(first, (last - first + 1));
		}

	private:
		Ref<Scene> m_Scene;
	};

}