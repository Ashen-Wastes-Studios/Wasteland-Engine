#include "wlpch.h"

#include "ProjectRuntime.h"

#include "Compiler.h"
#include "GraphSerializer.h"
#include "Wasteland/Audio/AudioClip.h"
#include "Wasteland/Project/Project.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace Wasteland::Avb
{
	struct AvbProjectRuntime::Impl
	{
		// Runtime contains references into Compiled and Graph. Keep the owners
		// before it so destruction runs Runtime -> Compiled -> Graph.
		AvbGraph Graph;
		AvbCompiledGraph Compiled;
		AvbRuntime Runtime;
	};

	namespace
	{
		bool Fail(std::string* error, std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
		}

		bool ResolveProjectAssetPath(const Wasteland::Project& project,
		                            const std::string& assetRef,
		                            std::filesystem::path& resolvedPath,
		                            std::string& error)
		{
			const std::filesystem::path relativePath(assetRef);
			if (relativePath.empty() || relativePath.has_root_name() || relativePath.has_root_directory())
			{
				error = "AVB asset reference must be project-relative: '" + assetRef + "'";
				return false;
			}
			for (const std::filesystem::path& component : relativePath)
			{
				if (component == "..")
				{
					error = "AVB asset reference cannot escape the project assets directory: '" + assetRef + "'";
					return false;
				}
			}

			// Canonical paths catch static symlink escapes. They do not make this
			// path-based lookup race-free against concurrent filesystem mutation.
			std::error_code ec;
			const std::filesystem::path canonicalRoot = std::filesystem::canonical(project.GetAssetRoot(), ec);
			if (ec)
			{
				error = "could not resolve project asset root: " + ec.message();
				return false;
			}
			const std::filesystem::path canonicalAsset =
				std::filesystem::canonical(project.GetAssetPath(assetRef), ec);
			if (ec)
			{
				error = "could not resolve AVB asset reference '" + assetRef + "': " + ec.message();
				return false;
			}

			const std::filesystem::path relativeToAssets = canonicalAsset.lexically_relative(canonicalRoot);
			if (relativeToAssets.empty() || relativeToAssets.is_absolute() ||
				*relativeToAssets.begin() == "..")
			{
				error = "AVB asset reference resolves outside the project assets directory: '" + assetRef + "'";
				return false;
			}

			resolvedPath = canonicalAsset;
			return true;
		}
	}

	AvbProjectRuntime::AvbProjectRuntime() = default;
	AvbProjectRuntime::~AvbProjectRuntime() = default;

	bool AvbProjectRuntime::Load(const std::string& graphAssetRef,
	                             std::uint32_t sampleRate,
	                             std::size_t framesPerBlock,
	                             std::size_t maxTriggersPerBlock,
	                             std::string* error)
	{
		if (error)
			error->clear();

		Wasteland::Project& project = Wasteland::Project::Get();
		if (!project.IsOpen())
			return Fail(error, "AVB project runtime requires an open Wasteland Project");
		if (graphAssetRef.empty())
			return Fail(error, "AVB graph asset reference is empty");

		const std::filesystem::path relativePath(graphAssetRef);
		if (relativePath.extension() != ".wlavb")
			return Fail(error, "AVB graph asset must use the .wlavb extension");

		try
		{
			auto candidate = std::make_unique<Impl>();
			std::filesystem::path graphPath;
			std::string pathError;
			if (!ResolveProjectAssetPath(project, graphAssetRef, graphPath, pathError))
				return Fail(error, std::move(pathError));
			AvbGraphError graphError;
			if (!AvbGraphSerializer::Load(graphPath.string(), candidate->Graph, graphError))
				return Fail(error, graphError.Format(graphPath.string()));

			AvbValidationResult validation;
			if (!AvbCompiler::Compile(candidate->Graph, candidate->Compiled, validation))
				return Fail(error, validation.Report());

			std::string assetLoadError;
			const AvbAssetResolver resolver = [&project, &assetLoadError](const std::string& assetRef)
				-> std::shared_ptr<Wasteland::Audio::AudioClip>
			{
				std::filesystem::path wavPath;
				std::string pathError;
				if (!ResolveProjectAssetPath(project, assetRef, wavPath, pathError))
				{
					assetLoadError = std::move(pathError);
					return {};
				}
				Wasteland::Audio::AudioClip::LoadResult loadResult =
					Wasteland::Audio::AudioClip::LoadResult::Success;
				auto clip = Wasteland::Audio::AudioClip::LoadWav(wavPath.string(), &loadResult);
				if (!clip || !clip->IsValid())
				{
					assetLoadError = "could not load WavePlayer AssetRef '" + assetRef + "' at '" +
						wavPath.string() + "' (" + Wasteland::Audio::AudioClip::LoadResultToString(loadResult) + ")";
					return {};
				}
				return clip;
			};

			std::string prepareError;
			if (!candidate->Runtime.Prepare(candidate->Graph, candidate->Compiled,
			                                sampleRate, framesPerBlock, maxTriggersPerBlock,
			                                &prepareError, resolver))
			{
				if (!assetLoadError.empty())
					prepareError += "; " + assetLoadError;
				return Fail(error, std::move(prepareError));
			}

			m_Impl.swap(candidate);
			return true;
		}
		catch (const std::exception& ex)
		{
			return Fail(error, std::string("could not load AVB project graph: ") + ex.what());
		}
	}

	void AvbProjectRuntime::Unload() noexcept
	{
		m_Impl.reset();
	}

	void AvbProjectRuntime::Reset() noexcept
	{
		if (m_Impl)
			m_Impl->Runtime.Reset();
	}

	bool AvbProjectRuntime::QueueTrigger(const std::string& nodeId,
	                                     const std::string& inputPin,
	                                     int frame) noexcept
	{
		return m_Impl && m_Impl->Runtime.QueueTrigger(nodeId, inputPin, frame);
	}

	bool AvbProjectRuntime::RenderBlock(float* monoOutput, std::size_t frames,
	                                    AudioBusGraph* buses,
	                                    AvbBusBlockMode busMode) noexcept
	{
		if (!m_Impl)
		{
			if (monoOutput)
				std::fill_n(monoOutput, frames, 0.0f);
			return false;
		}
		return m_Impl->Runtime.RenderBlock(monoOutput, frames, buses, busMode);
	}

	bool AvbProjectRuntime::RenderBlockStereo(float* interleavedStereoOutput,
	                                          std::size_t frames,
	                                          AudioBusGraph* buses,
	                                          AvbBusBlockMode busMode) noexcept
	{
		if (!m_Impl)
		{
			if (interleavedStereoOutput)
				std::fill_n(interleavedStereoOutput, frames * 2, 0.0f);
			return false;
		}
		return m_Impl->Runtime.RenderBlockStereo(
			interleavedStereoOutput, frames, buses, busMode);
	}

	const AvbTrigger* AvbProjectRuntime::FindTriggerOutput(const std::string& nodeId,
	                                                       const std::string& outputPin) const noexcept
	{
		return m_Impl ? m_Impl->Runtime.FindTriggerOutput(nodeId, outputPin) : nullptr;
	}

	bool AvbProjectRuntime::IsPrepared() const noexcept
	{
		return m_Impl && m_Impl->Runtime.IsPrepared();
	}

	std::uint32_t AvbProjectRuntime::SampleRate() const noexcept
	{
		return IsPrepared() ? m_Impl->Runtime.SampleRate() : 0;
	}

	std::size_t AvbProjectRuntime::FramesPerBlock() const noexcept
	{
		return IsPrepared() ? m_Impl->Runtime.FramesPerBlock() : 0;
	}
}
