#pragma once

#include <string>
#include <unordered_map>

#include "NodeCanvas.h"
#include "Wasteland/Avb/Graph/Compiler.h"
#include "Wasteland/Avb/Graph/Graph.h"
#include "Wasteland/Avb/Graph/NodeRegistry.h"
#include "Wasteland/Avb/Graph/Validator.h"

namespace Wasteland
{
	// Authoring surface for .wlavb audio flow graphs. NodeCanvas is only the
	// presentation/interaction layer; AVB keeps its own graph model and schema.
	class AvbGraphPanel
	{
	public:
		void OnImGuiRender();
		bool OpenGraph(const std::string& path);
		bool SaveGraph();
		bool ValidateGraph();

		bool IsVisible() const { return m_Visible; }
		void SetVisible(bool visible) { m_Visible = visible; }
		const std::string& GetPath() const { return m_Path; }

	private:
		bool LoadGraphFile(const std::string& path);
		void DrawToolbar();
		void DrawOpenConfirmation();
		void DrawCanvas();
		void DrawGraphInspector();
		void DrawNodeInspector();
		void DrawLinks();
		void DrawFindings();
		void RefreshValidation();
		void OnGraphEdited();
		void AddNode(const std::string& typeId);
		void Connect(const std::string& fromNode, const std::string& fromPin,
		             const std::string& toNode, const std::string& toPin);
		void DeleteNode(const std::string& nodeId);
		std::string& EditBuffer(const std::string& key, const std::string& seed);
		bool m_Visible = false;
		bool m_HasGraph = false;
		bool m_Dirty = false;
		bool m_FramedOnce = false;
		bool m_SizedOnce = false;
		bool m_ConfirmOpen = false;
		std::string m_Path;
		std::string m_PendingOpenPath;
		std::string m_Status;
		std::string m_CompiledOutput;
		Avb::AvbGraph m_Graph;
		Avb::AvbValidationResult m_Validation;
		std::unordered_map<std::string, std::string> m_EditBuffers;
		NodeCanvas m_Canvas;
	};
}
