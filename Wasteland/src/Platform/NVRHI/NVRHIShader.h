#pragma once

#include "Wasteland/Renderer/Shader.h"
#include <nvrhi/nvrhi.h>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace Wasteland {

	class NVRHIShader : public Shader
	{
	public:
		NVRHIShader(const std::string& filepath);
		NVRHIShader(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc);
		virtual ~NVRHIShader();

		virtual void Bind() const override;
		virtual void Unbind() const override;

		virtual void SetInt(const std::string& name, int value) override;
		virtual void SetIntArray(const std::string& name, int* values, uint32_t count) override;
		virtual void SetFloat(const std::string& name, float value) override;
		virtual void SetFloat2(const std::string& name, const glm::vec2& value) override;
		virtual void SetFloat3(const std::string& name, const glm::vec3& value) override;
		virtual void SetFloat4(const std::string& name, const glm::vec4& value) override;
		virtual void SetMat4(const std::string& name, const glm::mat4& value) override;

		virtual uint32_t GetRendererID() const override;
		virtual const std::string& GetName() const override { return m_Name; }

		// NVRHI-specific accessors
		nvrhi::ShaderHandle GetVertexShader() const { return m_VertexShader; }
		nvrhi::ShaderHandle GetPixelShader() const { return m_PixelShader; }
		nvrhi::ShaderHandle GetComputeShader() const { return m_ComputeShader; }
		
		// Constant buffer accessors
		nvrhi::BufferHandle GetVSConstantBuffer() const { return m_VSConstantBuffer; }
		nvrhi::BufferHandle GetPSConstantBuffer() const { return m_PSConstantBuffer; }
		
		// Binding layout accessors
		nvrhi::BindingLayoutHandle GetBindingLayout() const { return m_BindingLayout; }

	private:
		void Compile(const std::string& vertexSrc, const std::string& fragmentSrc);
		void CompileCompute(const std::string& computeSrc);
		void CreateConstantBuffers();
		void CreateBindingLayout();
		void ReflectShaderConstants(nvrhi::ShaderHandle shader, bool isVertexShader);
		void SetUniformData(const std::string& name, const void* data, size_t size) const;
		
		std::string ReadFile(const std::string& filepath);
		std::unordered_map<std::string, std::string> PreProcess(const std::string& source);

		std::string m_Name;
		nvrhi::ShaderHandle m_VertexShader;
		nvrhi::ShaderHandle m_PixelShader;
		nvrhi::ShaderHandle m_ComputeShader;

		// Constant buffers
		nvrhi::BufferHandle m_VSConstantBuffer;
		nvrhi::BufferHandle m_PSConstantBuffer;

		// Binding layout
		nvrhi::BindingLayoutHandle m_BindingLayout;

		// Uniform data storage (mutable for const methods)
		mutable std::vector<uint8_t> m_VSConstantData;
		mutable std::vector<uint8_t> m_PSConstantData;

		// Uniform name to offset mapping
		struct UniformInfo
		{
			uint32_t offset;
			uint32_t size;
			bool isVS; // true if in vertex shader, false if in pixel shader
		};
		std::unordered_map<std::string, UniformInfo> m_UniformLocations;

		mutable bool m_VSConstantsDirty = false;
		mutable bool m_PSConstantsDirty = false;
	};

}
