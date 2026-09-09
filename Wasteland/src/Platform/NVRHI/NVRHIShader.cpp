#include "wlpch.h"
#include "NVRHIShader.h"
#include "NVRHIContext.h"
#include "NVRHIRendererAPI.h"
#include "Wasteland/Core/Log.h"
#include "Wasteland/Renderer/RenderCommand.h"

#include <fstream>
#include <sstream>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <d3d12shader.h>

#pragma comment(lib, "d3dcompiler.lib")

namespace Wasteland {

	// Helper to get the current NVRHI device
	static nvrhi::IDevice* GetDevice()
	{
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		return context ? context->GetDevice() : nullptr;
	}

	NVRHIShader::NVRHIShader(const std::string& filepath)
	{
		// Extract name from filepath
		auto lastSlash = filepath.find_last_of("/\\");
		lastSlash = lastSlash == std::string::npos ? 0 : lastSlash + 1;
		auto lastDot = filepath.rfind('.');
		auto count = lastDot == std::string::npos ? filepath.size() - lastSlash : lastDot - lastSlash;
		m_Name = filepath.substr(lastSlash, count);

		std::string path = filepath;
		auto* context = NVRHIRendererAPI::GetCurrentContext();
		if (context && context->GetAPI() != RendererAPI::API::NVRHI_Vulkan)
		{
			auto dot = path.rfind('.');
			if (dot != std::string::npos && path.substr(dot) == ".glsl")
			{
				std::string hlslPath = path.substr(0, dot) + ".hlsl";
				std::ifstream f(hlslPath);
				if (f.good())
				{
					path = hlslPath;
				}
			}
		}

		std::string source = ReadFile(path);
		auto shaderSources = PreProcess(source);

		if (shaderSources.find("vertex") != shaderSources.end() && 
		    shaderSources.find("pixel") != shaderSources.end())
		{
			Compile(shaderSources["vertex"], shaderSources["pixel"]);
		}
		else if (shaderSources.find("compute") != shaderSources.end())
		{
			CompileCompute(shaderSources["compute"]);
		}
		else
		{
			WL_CORE_ERROR("Shader file '{0}' does not contain valid shader types", filepath);
		}
	}

	NVRHIShader::NVRHIShader(const std::string& name, const std::string& vertexSrc, const std::string& fragmentSrc)
		: m_Name(name)
	{
		Compile(vertexSrc, fragmentSrc);
	}

	NVRHIShader::~NVRHIShader()
	{
		m_ComputeShader = nullptr;
		m_PixelShader = nullptr;
		m_VertexShader = nullptr;
	}

	std::string NVRHIShader::ReadFile(const std::string& filepath)
	{
		std::string result;
		std::ifstream in(filepath, std::ios::in | std::ios::binary);
		if (in)
		{
			in.seekg(0, std::ios::end);
			result.resize(in.tellg());
			in.seekg(0, std::ios::beg);
			in.read(&result[0], result.size());
			in.close();
		}
		else
		{
			WL_CORE_ERROR("Could not open file '{0}'", filepath);
		}
		return result;
	}

	std::unordered_map<std::string, std::string> NVRHIShader::PreProcess(const std::string& source)
	{
		std::unordered_map<std::string, std::string> shaderSources;

		const char* typeToken = "#type";
		size_t typeTokenLength = strlen(typeToken);
		size_t pos = source.find(typeToken, 0);
		while (pos != std::string::npos)
		{
			size_t eol = source.find_first_of("\r\n", pos);
			if (eol == std::string::npos)
			{
				WL_CORE_ERROR("Shader type directive has no body in {0}", m_Name);
				return {};
			}
			size_t begin = pos + typeTokenLength + 1;
			std::string type = source.substr(begin, eol - begin);

			size_t nextLinePos = source.find_first_not_of("\r\n", eol);
			if (nextLinePos == std::string::npos)
			{
				WL_CORE_ERROR("Empty shader stage in {0}", m_Name);
				return {};
			}
			pos = source.find(typeToken, nextLinePos);
			const size_t end = pos == std::string::npos ? source.size() : pos;
			shaderSources[type] = source.substr(nextLinePos, end - nextLinePos);
		}

		return shaderSources;
	}

	void NVRHIShader::Compile(const std::string& vertexSrc, const std::string& fragmentSrc)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIShader: No NVRHI device available");
			return;
		}

		// Compile vertex shader
		ID3DBlob* vertexBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		HRESULT hr = D3DCompile(
			vertexSrc.c_str(), vertexSrc.length(),
			m_Name.c_str(), nullptr, nullptr,
			"main", "vs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0,
			&vertexBlob, &errorBlob
		);

		if (FAILED(hr))
		{
			if (errorBlob)
			{
				WL_CORE_ERROR("Vertex shader compilation failed:\n{0}",
					(char*)errorBlob->GetBufferPointer());
				errorBlob->Release();
			}
			return;
		}

		if (errorBlob)
		{
			WL_CORE_WARN("Vertex shader compiler diagnostics for {0}:\n{1}", m_Name, (char*)errorBlob->GetBufferPointer());
			errorBlob->Release();
			errorBlob = nullptr;
		}

		nvrhi::ShaderDesc vsDesc;
		vsDesc.shaderType = nvrhi::ShaderType::Vertex;
		vsDesc.debugName = m_Name + "_VS";
		m_VertexShader = device->createShader(vsDesc, vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize());
		
		// Reflect vertex shader constants
		ReflectShaderConstants(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), true);

		// Compile pixel shader
		ID3DBlob* pixelBlob = nullptr;
		errorBlob = nullptr;
		hr = D3DCompile(
			fragmentSrc.c_str(), fragmentSrc.length(),
			m_Name.c_str(), nullptr, nullptr,
			"main", "ps_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0,
			&pixelBlob, &errorBlob
		);

		if (FAILED(hr))
		{
			if (errorBlob)
			{
				WL_CORE_ERROR("Pixel shader compilation failed:\n{0}",
					(char*)errorBlob->GetBufferPointer());
				errorBlob->Release();
			}
			vertexBlob->Release();
			return;
		}

		if (errorBlob)
		{
			WL_CORE_WARN("Pixel shader compiler diagnostics for {0}:\n{1}", m_Name, (char*)errorBlob->GetBufferPointer());
			errorBlob->Release();
			errorBlob = nullptr;
		}

		nvrhi::ShaderDesc psDesc;
		psDesc.shaderType = nvrhi::ShaderType::Pixel;
		psDesc.debugName = m_Name + "_PS";
		m_PixelShader = device->createShader(psDesc, pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize());
		
		// Reflect pixel shader constants
		ReflectShaderConstants(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), false);

		// Create constant buffers
		CreateConstantBuffers();

		vertexBlob->Release();
		pixelBlob->Release();
	}

	void NVRHIShader::CompileCompute(const std::string& computeSrc)
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
		{
			WL_CORE_ERROR("NVRHIShader: No NVRHI device available");
			return;
		}

		ID3DBlob* computeBlob = nullptr;
		ID3DBlob* errorBlob = nullptr;
		HRESULT hr = D3DCompile(
			computeSrc.c_str(), computeSrc.length(),
			m_Name.c_str(), nullptr, nullptr,
			"main", "cs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS, 0,
			&computeBlob, &errorBlob
		);

		if (FAILED(hr))
		{
			if (errorBlob)
			{
				WL_CORE_ERROR("Compute shader compilation failed:\n{0}", 
					(char*)errorBlob->GetBufferPointer());
				errorBlob->Release();
			}
			return;
		}

		if (errorBlob)
		{
			WL_CORE_WARN("Compute shader compiler diagnostics for {0}:\n{1}", m_Name, (char*)errorBlob->GetBufferPointer());
			errorBlob->Release();
			errorBlob = nullptr;
		}

		nvrhi::ShaderDesc csDesc;
		csDesc.shaderType = nvrhi::ShaderType::Compute;
		csDesc.debugName = m_Name + "_CS";
		m_ComputeShader = device->createShader(csDesc, computeBlob->GetBufferPointer(), computeBlob->GetBufferSize());
		computeBlob->Release();
	}

	void NVRHIShader::CreateConstantBuffers()
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
			return;

		// Create vertex shader constant buffer
		if (!m_VSConstantData.empty())
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = m_VSConstantData.size();
			desc.isConstantBuffer = true;
			desc.debugName = m_Name + "_VS_CB";
			m_VSConstantBuffer = device->createBuffer(desc);
			
			m_VSConstantsDirty = true;
		}

		// Create pixel shader constant buffer
		if (!m_PSConstantData.empty())
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = m_PSConstantData.size();
			desc.isConstantBuffer = true;
			desc.debugName = m_Name + "_PS_CB";
			m_PSConstantBuffer = device->createBuffer(desc);
			
			m_PSConstantsDirty = true;
		}

		// Create binding layout
		CreateBindingLayout();
	}

	void NVRHIShader::CreateBindingLayout()
	{
		nvrhi::IDevice* device = GetDevice();
		if (!device)
			return;

		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel);

		// Add constant buffer bindings
		if (m_VSConstantBuffer)
		{
			// VS constant buffer at slot 0
			layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
		}

		if (m_PSConstantBuffer)
		{
			// PS constant buffer at slot 1
			layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(1));
		}

		// Renderer2D and Renderer3D_Basic share a 32-entry texture array at t0
		// and one sampler at s0. Keeping this in the common NVRHI layout makes
		// both paths use the same binding contract.
		layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0).setSize(32));
		layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));

		// Create binding layout
		m_BindingLayout = device->createBindingLayout(layoutDesc);
	}

	void NVRHIShader::ReflectShaderConstants(const void* bytecode, size_t bytecodeSize, bool isVertexShader)
	{
		// D3D11 NVRHI retains bytecode only for VS input layouts, not PS.
		// Reflect the compiler blob before releasing it, for every stage.

		if (!bytecode || bytecodeSize == 0)
			return;

		// Create reflection interface
		ID3D11ShaderReflection* reflection = nullptr;
		HRESULT hr = D3DReflect(bytecode, bytecodeSize, IID_ID3D11ShaderReflection, (void**)&reflection);
		if (FAILED(hr) || !reflection)
		{
			WL_CORE_WARN("Failed to create shader reflection for {0}", m_Name);
			return;
		}

		// Get constant buffer description
		D3D11_SHADER_DESC shaderDesc;
		reflection->GetDesc(&shaderDesc);

		// Iterate through constant buffers
		for (UINT i = 0; i < shaderDesc.ConstantBuffers; i++)
		{
			ID3D11ShaderReflectionConstantBuffer* cb = reflection->GetConstantBufferByIndex(i);
			D3D11_SHADER_BUFFER_DESC cbDesc;
			cb->GetDesc(&cbDesc);

			// Determine which constant buffer this is (we assume one CB per shader stage)
			std::vector<uint8_t>& constantData = isVertexShader ? m_VSConstantData : m_PSConstantData;
			constantData.resize(cbDesc.Size);

			// Iterate through variables in the constant buffer
			for (UINT j = 0; j < cbDesc.Variables; j++)
			{
				ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(j);
				D3D11_SHADER_VARIABLE_DESC varDesc;
				var->GetDesc(&varDesc);

				// Store uniform location
				UniformInfo info;
				info.offset = varDesc.StartOffset;
				info.size = varDesc.Size;
				info.isVS = isVertexShader;
				m_UniformLocations[varDesc.Name] = info;
			}
		}

		reflection->Release();
	}

	void NVRHIShader::Bind() const
	{
		// Track this shader as the current shader for NVRHI pipeline creation
		RendererAPI* api = RenderCommand::GetRendererAPI();
		auto* nvrhiAPI = dynamic_cast<NVRHIRendererAPI*>(api);
		if (nvrhiAPI)
		{
			// Create a Ref from 'this' - safe because the shader is owned by the caller
			// and will remain valid during the draw call
			nvrhiAPI->SetCurrentShader(Ref<Shader>(const_cast<NVRHIShader*>(this), [](NVRHIShader*){}));
		}

	}

	void NVRHIShader::UploadConstants(nvrhi::ICommandList* cmd) const
	{
		// Setters normally run AFTER Bind; upload immediately before drawing.
		if (cmd)
		{
			
			if (m_VSConstantsDirty && m_VSConstantBuffer && !m_VSConstantData.empty())
			{
				cmd->writeBuffer(m_VSConstantBuffer, m_VSConstantData.data(), m_VSConstantData.size());
				m_VSConstantsDirty = false;
			}

			if (m_PSConstantsDirty && m_PSConstantBuffer && !m_PSConstantData.empty())
			{
				cmd->writeBuffer(m_PSConstantBuffer, m_PSConstantData.data(), m_PSConstantData.size());
				m_PSConstantsDirty = false;
			}
		}
	}

	void NVRHIShader::Unbind() const
	{
		// No-op in NVRHI
	}

	void NVRHIShader::SetUniformData(const std::string& name, const void* data, size_t size) const
	{
		auto it = m_UniformLocations.find(name);
		if (it == m_UniformLocations.end())
		{
			WL_CORE_WARN("Uniform '{0}' not found in shader '{1}'", name, m_Name);
			return;
		}

		const UniformInfo& info = it->second;
		std::vector<uint8_t>& constantData = info.isVS ? m_VSConstantData : m_PSConstantData;
		bool& dirtyFlag = info.isVS ? m_VSConstantsDirty : m_PSConstantsDirty;

		if (size > info.size || info.offset + size > constantData.size())
		{
			WL_CORE_ERROR("Uniform '{0}' data exceeds constant buffer size", name);
			return;
		}

		memcpy(constantData.data() + info.offset, data, size);
		dirtyFlag = true;
	}

	void NVRHIShader::SetInt(const std::string& name, int value)
	{
		SetUniformData(name, &value, sizeof(int));
	}

	void NVRHIShader::SetIntArray(const std::string& name, int* values, uint32_t count)
	{
		SetUniformData(name, values, count * sizeof(int));
	}

	void NVRHIShader::SetFloat(const std::string& name, float value)
	{
		SetUniformData(name, &value, sizeof(float));
	}

	void NVRHIShader::SetFloat2(const std::string& name, const glm::vec2& value)
	{
		SetUniformData(name, &value, sizeof(glm::vec2));
	}

	void NVRHIShader::SetFloat3(const std::string& name, const glm::vec3& value)
	{
		SetUniformData(name, &value, sizeof(glm::vec3));
	}

	void NVRHIShader::SetFloat4(const std::string& name, const glm::vec4& value)
	{
		SetUniformData(name, &value, sizeof(glm::vec4));
	}

	void NVRHIShader::SetMat4(const std::string& name, const glm::mat4& value)
	{
		SetUniformData(name, &value, sizeof(glm::mat4));
	}

	uint32_t NVRHIShader::GetRendererID() const
	{
		// Return hash of vertex shader pointer (or pixel if no vertex)
		void* ptr = m_VertexShader ? m_VertexShader.Get() : 
		            (m_PixelShader ? m_PixelShader.Get() : 
		            (m_ComputeShader ? m_ComputeShader.Get() : nullptr));
		return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
	}

}
