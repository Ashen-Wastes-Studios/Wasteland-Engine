#include "wlpch.h"
#include "ScriptInspectorPanel.h"

#include <imgui/imgui.h>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "Wasteland/Scripting/ScriptEngine.h"
#include "Wasteland/Scene/Components.h"

#include <glm/glm.hpp>

namespace py = pybind11;

namespace Wasteland
{

	static bool IsPublicAttribute(const std::string &name)
	{
		if (name.empty() || name[0] == '_')
			return false;
		if (name == "entity")
			return false;
		return true;
	}

	void ScriptInspectorPanel::OnImGuiRender()
	{
		ImGui::Begin("Script Inspector");

		if (!m_Entity.IsValid())
		{
			ImGui::TextDisabled("Select an entity with a script to inspect variables.");
			ImGui::End();
			return;
		}

		if (!m_Entity.HasComponent<ScriptComponent>())
		{
			ImGui::TextDisabled("Selected entity has no ScriptComponent.");
			ImGui::End();
			return;
		}

		auto &sc = m_Entity.GetComponent<ScriptComponent>();
		ImGui::Text("Script: %s (%s)", sc.ScriptName.c_str(), sc.ScriptPath.c_str());
		ImGui::Separator();

		auto id = m_Entity.GetComponent<IDComponent>().ID;
		py::object *instance = ScriptEngine::GetInstance(id);

		if (!instance)
		{
			ImGui::TextDisabled("Script instance not initialized yet. Run the scene first.");
			ImGui::End();
			return;
		}

		try
		{
			py::list attrs = instance->attr("__dict__").attr("keys")().cast<py::list>();

			for (auto attrObj : attrs)
			{
				std::string attrName = attrObj.cast<std::string>();

				if (!IsPublicAttribute(attrName))
					continue;

				py::object value;
				try
				{
					value = instance->attr(attrName.c_str());
				}
				catch (py::error_already_set &)
				{
					PyErr_Clear();
					continue;
				}

				if (py::isinstance<py::function>(value) || py::isinstance<py::module_>(value))
					continue;

				ImGui::PushID(attrName.c_str());

				if (py::isinstance<py::bool_>(value))
				{
					bool bval = value.cast<bool>();
					if (ImGui::Checkbox(attrName.c_str(), &bval))
					{
						try { instance->attr(attrName.c_str()) = bval; }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else if (py::isinstance<py::int_>(value) && !py::isinstance<py::bool_>(value))
				{
					int ival = value.cast<int>();
					if (ImGui::DragInt(attrName.c_str(), &ival))
					{
						try { instance->attr(attrName.c_str()) = ival; }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else if (py::isinstance<py::float_>(value))
				{
					float fval = value.cast<float>();
					if (ImGui::DragFloat(attrName.c_str(), &fval, 0.1f))
					{
						try { instance->attr(attrName.c_str()) = (double)fval; }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else if (py::isinstance<py::str>(value))
				{
					std::string sval = value.cast<std::string>();
					char buffer[512];
					memset(buffer, 0, sizeof(buffer));
					strncpy_s(buffer, sizeof(buffer), sval.c_str(), _TRUNCATE);
					if (ImGui::InputText(attrName.c_str(), buffer, sizeof(buffer)))
					{
						try { instance->attr(attrName.c_str()) = std::string(buffer); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else if (py::isinstance<glm::vec2>(value))
				{
					glm::vec2 v2 = value.cast<glm::vec2>();
					float vals[2] = {v2.x, v2.y};
					if (ImGui::DragFloat2(attrName.c_str(), vals, 0.1f))
					{
						try { instance->attr(attrName.c_str()) = glm::vec2(vals[0], vals[1]); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else if (py::isinstance<glm::vec3>(value))
				{
					glm::vec3 v3 = value.cast<glm::vec3>();
					float vals[3] = {v3.x, v3.y, v3.z};
					if (ImGui::DragFloat3(attrName.c_str(), vals, 0.1f))
					{
						try { instance->attr(attrName.c_str()) = glm::vec3(vals[0], vals[1], vals[2]); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else if (py::isinstance<glm::vec4>(value))
				{
					glm::vec4 v4 = value.cast<glm::vec4>();
					float vals[4] = {v4.x, v4.y, v4.z, v4.w};
					if (ImGui::DragFloat4(attrName.c_str(), vals, 0.1f))
					{
						try { instance->attr(attrName.c_str()) = glm::vec4(vals[0], vals[1], vals[2], vals[3]); }
						catch (py::error_already_set &) { PyErr_Clear(); }
					}
				}
				else
				{
					std::string repr;
					try
					{
						repr = py::str(value).cast<std::string>();
						if (repr.size() > 80)
							repr = repr.substr(0, 80) + "...";
					}
					catch (py::error_already_set &)
					{
						PyErr_Clear();
						repr = "<error reading value>";
					}
					ImGui::Text("%s:", attrName.c_str());
					ImGui::SameLine();
					ImGui::TextDisabled("%s", repr.c_str());
				}

				ImGui::PopID();
			}
		}
		catch (py::error_already_set &e)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Python error: %s", e.what());
			PyErr_Clear();
		}

		ImGui::End();
	}

}
