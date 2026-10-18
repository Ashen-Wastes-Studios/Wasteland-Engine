#include "wlpch.h"

#include "Runtime.h"

#include "Wasteland/Audio/AudioClip.h"
#include "Wasteland/Avb/AudioBus.h"
#include "Wasteland/Avb/AvbDsp.h"
#include "Wasteland/Avb/AvbVoices.h"
#include "Compiler.h"
#include "NodeRegistry.h"
#include "Validator.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Wasteland::Avb
{
	void AvbTrigger::Prepare(std::size_t maxFrames, std::size_t maxTriggersPerBlock)
	{
		m_Frames.reset(maxTriggersPerBlock ? new int[maxTriggersPerBlock] : nullptr);
		m_Capacity = maxTriggersPerBlock;
		m_MaxFrames = maxFrames;
		m_BlockFrames = maxFrames;
		m_Count = 0;
	}

	void AvbTrigger::AdvanceBlock(std::size_t frames) noexcept
	{
		m_Count = 0;
		m_BlockFrames = frames <= m_MaxFrames ? frames : 0;
	}

	bool AvbTrigger::TriggerFrame(int frame) noexcept
	{
		if (frame < 0 || static_cast<std::size_t>(frame) >= m_BlockFrames || m_Count >= m_Capacity)
			return false;
		std::size_t insertAt = m_Count;
		while (insertAt > 0 && m_Frames[insertAt - 1] > frame)
		{
			m_Frames[insertAt] = m_Frames[insertAt - 1];
			--insertAt;
		}
		m_Frames[insertAt] = frame;
		++m_Count;
		return true;
	}

	int AvbTrigger::FrameAt(std::size_t index) const noexcept
	{
		return index < m_Count ? m_Frames[index] : -1;
	}

	std::size_t AvbFramesPerBlock(std::uint32_t sampleRate, double targetBlockRate,
	                              std::size_t simdLanes) noexcept
	{
		if (sampleRate == 0 || !std::isfinite(targetBlockRate) || targetBlockRate <= 0.0 || simdLanes == 0)
			return 0;
		const double requested = static_cast<double>(sampleRate) / targetBlockRate;
		if (!std::isfinite(requested) || requested <= 0.0 ||
		    requested > static_cast<double>((std::numeric_limits<std::size_t>::max)() - simdLanes))
			return 0;
		const std::size_t wholeFrames = static_cast<std::size_t>(std::ceil(requested));
		const std::size_t remainder = wholeFrames % simdLanes;
		if (remainder == 0)
			return wholeFrames;
		const std::size_t adjustment = simdLanes - remainder;
		if (wholeFrames > (std::numeric_limits<std::size_t>::max)() - adjustment)
			return 0;
		return wholeFrames + adjustment;
	}

	namespace
	{
		constexpr std::size_t kNoIndex = (std::numeric_limits<std::size_t>::max)();

		enum class RuntimeOperation : std::uint8_t
		{
			Sine,
			Noise,
			WavePlayer,
			Gain,
			WaveShaper,
			StereoMixer,
			AudioBusReader,
			AudioBusWriter,
			ADEnvelope,
			TriggerDelay,
			TriggerOnce,
			Output,
			Unsupported
		};

		RuntimeOperation OperationFor(const std::string& typeId) noexcept
		{
			if (typeId == "Sine") return RuntimeOperation::Sine;
			if (typeId == "Noise") return RuntimeOperation::Noise;
			if (typeId == "WavePlayer") return RuntimeOperation::WavePlayer;
			if (typeId == "Gain") return RuntimeOperation::Gain;
			if (typeId == "WaveShaper" || typeId == "Waveshaper") return RuntimeOperation::WaveShaper;
			if (typeId == "StereoMixer") return RuntimeOperation::StereoMixer;
			if (typeId == "AudioBusReader") return RuntimeOperation::AudioBusReader;
			if (typeId == "AudioBusWriter") return RuntimeOperation::AudioBusWriter;
			if (typeId == "ADEnvelope") return RuntimeOperation::ADEnvelope;
			if (typeId == "TriggerDelay") return RuntimeOperation::TriggerDelay;
			if (typeId == "TriggerOnce") return RuntimeOperation::TriggerOnce;
			if (typeId == "Output") return RuntimeOperation::Output;
			return RuntimeOperation::Unsupported;
		}

		struct RuntimePin
		{
			const AvbPinDef* Definition = nullptr;
			std::vector<float> Audio;
			AvbTrigger Triggers;
			std::vector<std::size_t> Links;
		};

		struct RuntimeNode
		{
			const AvbNode* Source = nullptr;
			const AvbNodeDef* Definition = nullptr;
			RuntimeOperation Operation = RuntimeOperation::Unsupported;
			std::vector<RuntimePin> Inputs;
			std::vector<RuntimePin> Outputs;
			std::unique_ptr<Dsp::Sine> Sine;
			std::unique_ptr<Dsp::Noise> Noise;
			std::unique_ptr<Dsp::WavePlayer> WavePlayer;
			std::shared_ptr<Wasteland::Audio::AudioClip> Clip;
			std::unique_ptr<Dsp::AdEnvelope> Envelope;
			Dsp::WaveShaper WaveShaper;
			std::vector<float> StereoScratch;
			std::vector<std::uint64_t> PendingTriggerFrames;
			std::size_t PendingTriggerCount = 0;
			std::uint32_t NoiseSeed = 0;
			AudioBusId Bus = kInvalidAudioBusId;
			AudioBusTap BusTap = AudioBusTap::PostFader;
			float FrequencyHz = 440.0f;
			float Amplitude = 1.0f;
			float Gain = 1.0f;
			float GainA = 1.0f;
			float GainB = 1.0f;
			float Amount = 1.0f;
			float Bias = 0.0f;
			float OutputGain = 1.0f;
			float AttackSeconds = 0.01f;
			float DecaySeconds = 0.1f;
			float StartTimeSeconds = 0.0f;
			float PitchSemitones = 0.0f;
			float NearlyFinishedLeadSeconds = 0.02f;
			double DelaySeconds = 0.0;
			bool Loop = false;
			bool HasFired = false;
		};

		struct RuntimeLink
		{
			std::size_t FromNode = 0;
			std::size_t FromPin = 0;
			std::size_t ToNode = 0;
			std::size_t ToPin = 0;
			bool Feedback = false;
			std::size_t FeedbackIndex = kNoIndex;
		};

		struct FeedbackState
		{
			std::vector<float> PreviousAudio;
			std::vector<float> NextAudio;
			AvbTrigger PreviousTriggers;
			AvbTrigger NextTriggers;
		};

		const AvbLiteral* EffectiveLiteral(const AvbNode& node, const AvbPinDef& pin) noexcept
		{
			if (const AvbLiteral* value = node.FindLiteral(pin.Id))
				return value;
			return pin.DefaultValue ? &*pin.DefaultValue : nullptr;
		}

		double NumericValue(const AvbNode& node, const AvbNodeDef& definition,
		                    const char* pinId, double fallback)
		{
			const AvbPinDef* pin = definition.FindPin(pinId);
			if (!pin)
				return fallback;
			const AvbLiteral* value = EffectiveLiteral(node, *pin);
			if (!value)
				return fallback;
			if (const double* number = std::get_if<double>(&value->Value))
				return *number;
			if (const std::int64_t* integer = std::get_if<std::int64_t>(&value->Value))
				return static_cast<double>(*integer);
			return fallback;
		}

		bool FloatValue(const AvbNode& node, const AvbNodeDef& definition,
		                const char* pinId, float fallback, float& result)
		{
			const double value = NumericValue(node, definition, pinId, fallback);
			const double maxFloat = static_cast<double>((std::numeric_limits<float>::max)());
			if (!std::isfinite(value) || value < -maxFloat || value > maxFloat)
				return false;
			result = static_cast<float>(value);
			return std::isfinite(result);
		}

		std::int64_t IntegerValue(const AvbNode& node, const AvbNodeDef& definition,
		                         const char* pinId, std::int64_t fallback)
		{
			const AvbPinDef* pin = definition.FindPin(pinId);
			if (!pin)
				return fallback;
			const AvbLiteral* value = EffectiveLiteral(node, *pin);
			if (!value)
				return fallback;
			if (const std::int64_t* integer = std::get_if<std::int64_t>(&value->Value))
				return *integer;
			return fallback;
		}

		bool BooleanValue(const AvbNode& node, const AvbNodeDef& definition,
		                  const char* pinId, bool fallback)
		{
			const AvbPinDef* pin = definition.FindPin(pinId);
			if (!pin)
				return fallback;
			const AvbLiteral* value = EffectiveLiteral(node, *pin);
			if (!value)
				return fallback;
			if (const bool* enabled = std::get_if<bool>(&value->Value))
				return *enabled;
			return fallback;
		}


		float SanitizeAudioSample(float sample) noexcept
		{
			return std::isfinite(sample) ? sample : 0.0f;
		}

		std::string_view StringValue(const AvbNode& node, const AvbNodeDef& definition,
		                             const char* pinId)
		{
			const AvbPinDef* pin = definition.FindPin(pinId);
			if (!pin)
				return {};
			const AvbLiteral* value = EffectiveLiteral(node, *pin);
			if (!value)
				return {};
			if (const std::string* text = std::get_if<std::string>(&value->Value))
				return *text;
			return {};
		}

		bool ParseBusId(std::string_view text, AudioBusId& out) noexcept
		{
			if (text.empty())
				return false;
			std::uint32_t value = 0;
			const char* first = text.data();
			const char* last = first + text.size();
			const auto parsed = std::from_chars(first, last, value, 10);
			if (parsed.ec != std::errc{} || parsed.ptr != last || value == kInvalidAudioBusId)
				return false;
			out = value;
			return true;
		}

		bool ParseBusTap(std::string_view text, AudioBusTap& out) noexcept
		{
			if (text == "PreFx" || text == "PreFX") { out = AudioBusTap::PreFx; return true; }
			if (text == "PostFx" || text == "PostFX") { out = AudioBusTap::PostFx; return true; }
			if (text == "PostFader" || text == "PostFade") { out = AudioBusTap::PostFader; return true; }
			return false;
		}

		std::size_t FindPinIndex(const std::vector<RuntimePin>& pins, const std::string& pinId) noexcept
		{
			for (std::size_t i = 0; i < pins.size(); ++i)
				if (pins[i].Definition && pins[i].Definition->Id == pinId)
					return i;
			return kNoIndex;
		}

		std::size_t FindPinIndex(const std::vector<RuntimePin>& pins, const char* pinId) noexcept
		{
			for (std::size_t i = 0; i < pins.size(); ++i)
				if (pins[i].Definition && pins[i].Definition->Id == pinId)
					return i;
			return kNoIndex;
		}
	}

	struct AvbRuntime::Impl
	{
		std::uint32_t SampleRate = 0;
		std::size_t Frames = 0;
		AvbChannelFormat OutputFormat = AvbChannelFormat::Mono;
		std::uint64_t SampleClock = 0;
		bool UsesBuses = false;
		bool HasBusWriters = false;
		std::vector<RuntimeNode> Nodes;
		std::vector<RuntimeLink> Links;
		std::vector<FeedbackState> Feedback;
		std::vector<std::size_t> Order;
		std::vector<std::size_t> BusWriterNodes;
		std::vector<AudioBusInput> BusInputs;
		std::vector<float> BusOutput;

		std::size_t FindNode(const std::string& id) const noexcept
		{
			for (std::size_t i = 0; i < Nodes.size(); ++i)
				if (Nodes[i].Source && Nodes[i].Source->Id == id)
					return i;
			return kNoIndex;
		}

		float* InputAudio(std::size_t nodeIndex, const char* pinId) noexcept
		{
			RuntimeNode& node = Nodes[nodeIndex];
			const std::size_t pinIndex = FindPinIndex(node.Inputs, pinId);
			if (pinIndex == kNoIndex || node.Inputs[pinIndex].Links.empty())
				return nullptr;
			const RuntimeLink& link = Links[node.Inputs[pinIndex].Links.front()];
			if (link.Feedback)
				return Feedback[link.FeedbackIndex].PreviousAudio.data();
			return Nodes[link.FromNode].Outputs[link.FromPin].Audio.data();
		}

		bool MergeTriggerInputs(std::size_t nodeIndex) noexcept
		{
			RuntimeNode& node = Nodes[nodeIndex];
			for (RuntimePin& input : node.Inputs)
			{
				if (!input.Definition || input.Definition->Type != AvbPinType::Trigger)
					continue;
				for (std::size_t linkIndex : input.Links)
				{
					const RuntimeLink& link = Links[linkIndex];
					const AvbTrigger& source = link.Feedback
						? Feedback[link.FeedbackIndex].PreviousTriggers
						: Nodes[link.FromNode].Outputs[link.FromPin].Triggers;
					for (std::size_t event = 0; event < source.Count(); ++event)
						if (!input.Triggers.TriggerFrame(source.FrameAt(event)))
							return false;
				}
			}
			return true;
		}

		bool ProcessNode(std::size_t nodeIndex, AudioBusGraph* buses) noexcept
		{
			RuntimeNode& node = Nodes[nodeIndex];
			if (!MergeTriggerInputs(nodeIndex))
				return false;
			const std::size_t frames = Frames;
			const std::size_t outAudio = FindPinIndex(node.Outputs, "Out");
			const char* eventPin = node.Operation == RuntimeOperation::Sine ? "Sync" :
				(node.Operation == RuntimeOperation::TriggerDelay || node.Operation == RuntimeOperation::TriggerOnce ? "In" : "Trigger");
			const std::size_t inputEvents = FindPinIndex(node.Inputs, eventPin);
			const AvbTrigger* events = inputEvents == kNoIndex ? nullptr : &node.Inputs[inputEvents].Triggers;

			switch (node.Operation)
			{
			case RuntimeOperation::Sine:
			{
				node.Sine->SetFrequency(node.FrequencyHz);
				node.Sine->SetAmplitude(node.Amplitude);
				RuntimePin& output = node.Outputs[outAudio];
				std::size_t event = 0;
				for (std::size_t frame = 0; frame < frames; ++frame)
				{
					bool sync = false;
					while (events && event < events->Count() &&
					       static_cast<std::size_t>(events->FrameAt(event)) == frame)
					{
						sync = true;
						++event;
					}
					output.Audio[frame] = SanitizeAudioSample(node.Sine->ProcessSample(sync));
				}
				return true;
			}
			case RuntimeOperation::Noise:
			{
				RuntimePin& output = node.Outputs[outAudio];
				for (std::size_t frame = 0; frame < frames; ++frame)
					output.Audio[frame] = SanitizeAudioSample(node.Noise->ProcessSample());
				return true;
			}
			case RuntimeOperation::WavePlayer:
			{
				const std::size_t playIndex = FindPinIndex(node.Inputs, "Play");
				const std::size_t stopIndex = FindPinIndex(node.Inputs, "Stop");
				const std::size_t finishedIndex = FindPinIndex(node.Outputs, "OnFinished");
				const std::size_t nearlyIndex = FindPinIndex(node.Outputs, "OnNearlyFinished");
				const std::size_t loopedIndex = FindPinIndex(node.Outputs, "OnLooped");
				if (!node.WavePlayer || outAudio == kNoIndex || playIndex == kNoIndex ||
				    stopIndex == kNoIndex || finishedIndex == kNoIndex ||
				    nearlyIndex == kNoIndex || loopedIndex == kNoIndex)
					return false;
				const AvbTrigger& playEvents = node.Inputs[playIndex].Triggers;
				const AvbTrigger& stopEvents = node.Inputs[stopIndex].Triggers;
				AvbTrigger& finished = node.Outputs[finishedIndex].Triggers;
				AvbTrigger& nearly = node.Outputs[nearlyIndex].Triggers;
				AvbTrigger& looped = node.Outputs[loopedIndex].Triggers;
				std::size_t playEvent = 0;
				std::size_t stopEvent = 0;
				RuntimePin& output = node.Outputs[outAudio];
				for (std::size_t frame = 0; frame < frames; ++frame)
				{
					bool play = false;
					while (playEvent < playEvents.Count() &&
					       static_cast<std::size_t>(playEvents.FrameAt(playEvent)) == frame)
					{
						play = true;
						++playEvent;
					}
					bool stop = false;
					while (stopEvent < stopEvents.Count() &&
					       static_cast<std::size_t>(stopEvents.FrameAt(stopEvent)) == frame)
					{
						stop = true;
						++stopEvent;
					}
					const Dsp::WavePlayerFrame voice =
						node.WavePlayer->ProcessSample(play, stop, node.StartTimeSeconds);
					output.Audio[frame] = SanitizeAudioSample(voice.Sample);
					if (voice.OnFinished && !finished.TriggerFrame(static_cast<int>(frame))) return false;
					if (voice.OnNearlyFinished && !nearly.TriggerFrame(static_cast<int>(frame))) return false;
					if (voice.OnLooped && !looped.TriggerFrame(static_cast<int>(frame))) return false;
				}
				return true;
			}
			case RuntimeOperation::Gain:
			{
				const float* input = InputAudio(nodeIndex, "In");
				RuntimePin& output = node.Outputs[outAudio];
				for (std::size_t frame = 0; frame < frames; ++frame)
					output.Audio[frame] = SanitizeAudioSample((input ? input[frame] : 0.0f) * node.Gain);
				return true;
			}
			case RuntimeOperation::WaveShaper:
			{
				const float* input = InputAudio(nodeIndex, "In");
				node.WaveShaper.SetParameters(Dsp::WaveShape::HardClip, node.Amount, node.Bias, node.OutputGain);
				RuntimePin& output = node.Outputs[outAudio];
				for (std::size_t frame = 0; frame < frames; ++frame)
					output.Audio[frame] = SanitizeAudioSample(
					node.WaveShaper.ProcessSample(input ? input[frame] : 0.0f));
				return true;
			}
			case RuntimeOperation::StereoMixer:
			{
				const float* a = InputAudio(nodeIndex, "A");
				const float* b = InputAudio(nodeIndex, "B");
				RuntimePin& output = node.Outputs[outAudio];
				for (std::size_t frame = 0; frame < frames; ++frame)
					output.Audio[frame] = SanitizeAudioSample(
					(a ? a[frame] : 0.0f) * node.GainA + (b ? b[frame] : 0.0f) * node.GainB);
				return true;
			}
			case RuntimeOperation::AudioBusReader:
			{
				if (!buses || !buses->GetBus(node.Bus))
					return false;
				const float* stereo = buses->GetTapBuffer(node.Bus, node.BusTap);
				if (!stereo)
					return false;
				const std::size_t available = (std::min)(frames, buses->LastRenderedFrames());
				RuntimePin& output = node.Outputs[outAudio];
				for (std::size_t frame = 0; frame < available; ++frame)
					output.Audio[frame] = SanitizeAudioSample(
						0.5f * (stereo[frame * 2] + stereo[frame * 2 + 1]) * node.Gain);
				std::fill(output.Audio.begin() + available, output.Audio.begin() + frames, 0.0f);
				return true;
			}
			case RuntimeOperation::AudioBusWriter:
			{
				const float* input = InputAudio(nodeIndex, "In");
				for (std::size_t frame = 0; frame < frames; ++frame)
				{
					float sample = input ? input[frame] * node.Gain : 0.0f;
					if (!std::isfinite(sample)) sample = 0.0f;
					node.StereoScratch[frame * 2] = sample;
					node.StereoScratch[frame * 2 + 1] = sample;
				}
				return true;
			}
			case RuntimeOperation::ADEnvelope:
			{
				node.Envelope->SetParameters(node.AttackSeconds, node.DecaySeconds);
				RuntimePin& output = node.Outputs[outAudio];
				std::size_t event = 0;
				for (std::size_t frame = 0; frame < frames; ++frame)
				{
					while (events && event < events->Count() &&
					       static_cast<std::size_t>(events->FrameAt(event)) == frame)
					{
						node.Envelope->Trigger();
						++event;
					}
					output.Audio[frame] = SanitizeAudioSample(node.Envelope->ProcessSample());
				}
				return true;
			}
			case RuntimeOperation::TriggerOnce:
			{
				const std::size_t outputIndex = FindPinIndex(node.Outputs, "Out");
				if (!events || outputIndex == kNoIndex)
					return true;
				AvbTrigger& output = node.Outputs[outputIndex].Triggers;
				for (std::size_t event = 0; event < events->Count(); ++event)
				{
					if (node.HasFired)
						break;
					if (!output.TriggerFrame(events->FrameAt(event)))
						return false;
					node.HasFired = true;
				}
				return true;
			}
			case RuntimeOperation::TriggerDelay:
			{
				const std::size_t outputIndex = FindPinIndex(node.Outputs, "Out");
				if (outputIndex == kNoIndex)
					return false;
				AvbTrigger& output = node.Outputs[outputIndex].Triggers;
				const std::uint64_t blockEnd = SampleClock + frames;
				std::size_t keep = 0;
				for (std::size_t i = 0; i < node.PendingTriggerCount; ++i)
				{
					const std::uint64_t due = node.PendingTriggerFrames[i];
					if (due < blockEnd)
					{
						const std::uint64_t relative = due > SampleClock ? due - SampleClock : 0;
						if (!output.TriggerFrame(static_cast<int>(relative)))
							return false;
					}
					else
						node.PendingTriggerFrames[keep++] = due;
				}
				node.PendingTriggerCount = keep;
				const auto delayFrames = static_cast<std::uint64_t>(std::llround(node.DelaySeconds * SampleRate));
				if (events)
				{
					for (std::size_t event = 0; event < events->Count(); ++event)
					{
						const std::uint64_t due = SampleClock + static_cast<std::uint64_t>(events->FrameAt(event)) + delayFrames;
						if (due < blockEnd)
					{
						if (!output.TriggerFrame(static_cast<int>(due - SampleClock)))
							return false;
						continue;
					}
					if (node.PendingTriggerCount >= node.PendingTriggerFrames.size())
						return false;
					std::size_t insertAt = node.PendingTriggerCount++;
					while (insertAt > 0 && node.PendingTriggerFrames[insertAt - 1] > due)
					{
						node.PendingTriggerFrames[insertAt] = node.PendingTriggerFrames[insertAt - 1];
						--insertAt;
					}
					node.PendingTriggerFrames[insertAt] = due;
					}
				}
				return true;
			}
			case RuntimeOperation::Output:
			{
				const float* input = InputAudio(nodeIndex, "In");
				if (input)
					for (std::size_t frame = 0; frame < frames; ++frame)
						m_Output[frame] = SanitizeAudioSample(m_Output[frame] + input[frame]);
				return true;
			}
			case RuntimeOperation::Unsupported:
				return false;
			}
			return false;
		}

		std::vector<float> m_Output;

		bool Render(float* output, std::size_t frames, AudioBusGraph* buses,
		            AvbBusBlockMode busMode, bool stereo) noexcept
		{
			const std::size_t channels = stereo ? 2 : 1;
			if (!output || frames > (std::numeric_limits<std::size_t>::max)() / channels)
				return false;
			const std::size_t outputSamples = frames * channels;
			const AvbChannelFormat expectedFormat = stereo ? AvbChannelFormat::Stereo : AvbChannelFormat::Mono;
			if (OutputFormat != expectedFormat || frames != Frames ||
			    SampleClock > (std::numeric_limits<std::uint64_t>::max)() - frames)
			{
				std::fill_n(output, outputSamples, 0.0f);
				return false;
			}
			std::fill_n(output, outputSamples, 0.0f);
			if (UsesBuses && !buses)
				return false;

			auto clearInputEvents = [&]() noexcept
			{
				for (RuntimeNode& node : Nodes)
					for (RuntimePin& pin : node.Inputs)
						if (pin.Definition && pin.Definition->Type == AvbPinType::Trigger)
							pin.Triggers.AdvanceBlock(Frames);
			};
			auto failClosed = [&]() noexcept
			{
				std::fill_n(output, outputSamples, 0.0f);
				clearInputEvents();
				return false;
			};

			std::fill(m_Output.begin(), m_Output.end(), 0.0f);
			for (RuntimeNode& node : Nodes)
				for (RuntimePin& pin : node.Outputs)
					if (pin.Definition && pin.Definition->Type == AvbPinType::Trigger)
						pin.Triggers.AdvanceBlock(frames);

			for (std::size_t nodeIndex : Order)
				if (!ProcessNode(nodeIndex, buses))
					return failClosed();

			if (stereo)
			{
				for (std::size_t frame = 0; frame < frames; ++frame)
				{
					const float sample = SanitizeAudioSample(m_Output[frame]);
					output[frame * 2] = sample;
					output[frame * 2 + 1] = sample;
				}
			}
			else
				std::copy_n(m_Output.data(), frames, output);
			if (HasBusWriters)
			{
				for (std::size_t i = 0; i < BusWriterNodes.size(); ++i)
				{
					const RuntimeNode& writer = Nodes[BusWriterNodes[i]];
					if (!buses->GetBus(writer.Bus))
						return failClosed();
					BusInputs[i] = { writer.Bus, writer.StereoScratch.data(), 1.0f };
				}
				const bool busSuccess = busMode == AvbBusBlockMode::Defer
					? buses->AccumulateInputs(BusInputs.data(), BusInputs.size(), frames)
					: buses->Process(BusInputs.data(), BusInputs.size(), BusOutput.data(), frames);
				if (!busSuccess)
					return failClosed();
			}

			for (RuntimeLink& link : Links)
			{
				if (!link.Feedback)
					continue;
				FeedbackState& state = Feedback[link.FeedbackIndex];
				const RuntimePin& source = Nodes[link.FromNode].Outputs[link.FromPin];
				if (source.Definition->Type == AvbPinType::Audio)
				{
					std::copy_n(source.Audio.data(), frames, state.NextAudio.data());
					state.PreviousAudio.swap(state.NextAudio);
				}
				else
				{
					state.NextTriggers.AdvanceBlock(frames);
					for (std::size_t event = 0; event < source.Triggers.Count(); ++event)
						if (!state.NextTriggers.TriggerFrame(source.Triggers.FrameAt(event)))
							return failClosed();
					std::swap(state.PreviousTriggers, state.NextTriggers);
				}
			}

			clearInputEvents();
			SampleClock += frames;
			return true;
		}
	};

	AvbRuntime::AvbRuntime() = default;
	AvbRuntime::~AvbRuntime() = default;
	AvbRuntime::AvbRuntime(AvbRuntime&&) noexcept = default;
	AvbRuntime& AvbRuntime::operator=(AvbRuntime&&) noexcept = default;

	bool AvbRuntime::Prepare(const AvbGraph& graph, const AvbCompiledGraph& compiled,
	                         std::uint32_t sampleRate, std::size_t framesPerBlock,
	                         std::size_t maxTriggersPerBlock, std::string* error,
	                         AvbAssetResolver assetResolver)
	{
		auto reject = [error](const std::string& message)
		{
			if (error) *error = message;
			return false;
		};
		if (sampleRate != kAvbAudioSampleRate)
			return reject("AVB runtime sample rate must match the fixed 48000 Hz engine mix rate");
		if (framesPerBlock == 0 || framesPerBlock > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
			return reject("AVB runtime block size must fit a positive trigger frame index");
		if (maxTriggersPerBlock == 0)
			return reject("AVB runtime trigger capacity must be positive");
		if (compiled.Nodes.size() != graph.Nodes.size())
			return reject("compiled graph node count does not match the source graph");

		AvbValidationResult validation;
		if (!AvbValidator::Validate(graph, validation))
			return reject(validation.Report());

		try
		{
			auto state = std::make_unique<Impl>();
			state->SampleRate = sampleRate;
			state->Frames = framesPerBlock;
			state->OutputFormat = graph.Output;
			if (framesPerBlock > (std::numeric_limits<std::size_t>::max)() / 2)
				return reject("AVB runtime block buffer size overflows");
			state->m_Output.assign(framesPerBlock, 0.0f);
			state->BusOutput.assign(framesPerBlock * 2, 0.0f);
			state->Nodes.reserve(compiled.Nodes.size());

			std::map<std::string, std::size_t> nodeIndices;
			for (const AvbNode* source : compiled.Nodes)
			{
				if (!source || graph.FindNode(source->Id) != source)
					return reject("compiled graph contains a node outside its source graph");
				const AvbNodeDef* definition = AvbNodeRegistry::Get().Find(source->TypeId);
				if (!definition)
					return reject("runtime encountered unregistered node type '" + source->TypeId + "'");
				RuntimeNode node;
				node.Source = source;
				node.Definition = definition;
				node.Operation = OperationFor(source->TypeId);
				auto assignFloat = [&](const char* pinId, float fallback, float& value)
				{
					return FloatValue(*source, *definition, pinId, fallback, value);
				};
				auto rejectFloatPin = [&](const char* pinId)
				{
					return reject("node '" + source->Id +
					              "' has a non-finite or out-of-range numeric literal on pin '" + pinId + "'");
				};
				if (!assignFloat("Frequency", 440.0f, node.FrequencyHz)) return rejectFloatPin("Frequency");
				if (!assignFloat("Amplitude", 1.0f, node.Amplitude)) return rejectFloatPin("Amplitude");
				if (!assignFloat("Gain", 1.0f, node.Gain)) return rejectFloatPin("Gain");
				if (!assignFloat("GainA", 1.0f, node.GainA)) return rejectFloatPin("GainA");
				if (!assignFloat("GainB", 1.0f, node.GainB)) return rejectFloatPin("GainB");
				if (!assignFloat("Amount", 1.0f, node.Amount)) return rejectFloatPin("Amount");
				if (!assignFloat("Bias", 0.0f, node.Bias)) return rejectFloatPin("Bias");
				if (!assignFloat("OutputGain", 1.0f, node.OutputGain)) return rejectFloatPin("OutputGain");
				if (!assignFloat("Attack", 0.01f, node.AttackSeconds)) return rejectFloatPin("Attack");
				if (!assignFloat("Decay", 0.1f, node.DecaySeconds)) return rejectFloatPin("Decay");
				node.DelaySeconds = std::clamp(NumericValue(*source, *definition, "Delay", 0.0), 0.0, 60.0);
				if (node.Operation == RuntimeOperation::WavePlayer)
				{
					if (!assignFloat("StartTime", 0.0f, node.StartTimeSeconds)) return rejectFloatPin("StartTime");
					if (!assignFloat("PitchSemitones", 0.0f, node.PitchSemitones)) return rejectFloatPin("PitchSemitones");
					if (!assignFloat("NearlyFinishedLead", 0.02f, node.NearlyFinishedLeadSeconds))
						return rejectFloatPin("NearlyFinishedLead");
					node.Loop = BooleanValue(*source, *definition, "Loop", false);
					const std::string assetRef(StringValue(*source, *definition, "Clip"));
					if (assetRef.empty())
						return reject("node '" + source->Id + "' requires a non-empty AssetRef on pin 'Clip'");
					if (!assetResolver)
						return reject("node '" + source->Id + "' requires an asset resolver for pin 'Clip'");
					try
					{
						node.Clip = assetResolver(assetRef);
					}
					catch (const std::exception& exception)
					{
						return reject("node '" + source->Id + "' failed to resolve pin 'Clip': " + exception.what());
					}
					catch (...)
					{
						return reject("node '" + source->Id + "' failed to resolve pin 'Clip'");
					}
					if (!node.Clip || !node.Clip->IsValid() || node.Clip->GetFrameCount() == 0 ||
					    node.Clip->GetSampleRate() <= 0 || node.Clip->GetSamples().size() != node.Clip->GetFrameCount())
						return reject("node '" + source->Id + "' resolved pin 'Clip' to a missing or invalid audio clip");
				}
				if (node.Operation == RuntimeOperation::Unsupported)
					return reject("runtime has no processor for node type '" + source->TypeId + "'");
				for (const AvbPinDef& pin : definition->Pins)
				{
					RuntimePin runtimePin;
					runtimePin.Definition = &pin;
					if (pin.Direction == AvbPinDir::Output && pin.Type == AvbPinType::Audio)
						runtimePin.Audio.assign(framesPerBlock, 0.0f);
					if (pin.Type == AvbPinType::Trigger)
						runtimePin.Triggers.Prepare(framesPerBlock, maxTriggersPerBlock);
					if (pin.Direction == AvbPinDir::Input)
						node.Inputs.push_back(std::move(runtimePin));
					else
						node.Outputs.push_back(std::move(runtimePin));
				}

				if (node.Operation == RuntimeOperation::Sine)
				{
					node.Sine = std::make_unique<Dsp::Sine>(sampleRate, node.FrequencyHz, node.Amplitude);
				}
				else if (node.Operation == RuntimeOperation::Noise)
				{
					const std::int64_t seed = IntegerValue(*source, *definition, "Seed", 0);
					node.NoiseSeed = static_cast<std::uint32_t>(seed);
					node.Noise = std::make_unique<Dsp::Noise>(sampleRate, node.NoiseSeed);
				}
				else if (node.Operation == RuntimeOperation::WavePlayer)
				{
					node.WavePlayer = std::make_unique<Dsp::WavePlayer>(sampleRate);
					node.WavePlayer->SetSource(node.Clip->GetSamples().data(), node.Clip->GetFrameCount(),
					                  static_cast<std::uint32_t>(node.Clip->GetSampleRate()));
					node.WavePlayer->SetPitchSemitones(node.PitchSemitones);
					node.WavePlayer->SetLoop(node.Loop);
					node.WavePlayer->SetNearlyFinishedLeadSeconds(node.NearlyFinishedLeadSeconds);
				}
				else if (node.Operation == RuntimeOperation::ADEnvelope)
				{
					node.Envelope = std::make_unique<Dsp::AdEnvelope>(sampleRate,
						node.AttackSeconds, node.DecaySeconds);
				}
				else if (node.Operation == RuntimeOperation::TriggerDelay)
					node.PendingTriggerFrames.resize(maxTriggersPerBlock);

				if (node.Operation == RuntimeOperation::AudioBusReader ||
				    node.Operation == RuntimeOperation::AudioBusWriter)
				{
					state->UsesBuses = true;
					if (!ParseBusId(StringValue(*source, *definition, "Bus"), node.Bus))
						return reject("node '" + source->Id + "' requires a numeric AudioBusId literal on pin 'Bus'");
					if (node.Operation == RuntimeOperation::AudioBusReader &&
					    !ParseBusTap(StringValue(*source, *definition, "Tap"), node.BusTap))
						return reject("node '" + source->Id + "' has an invalid AudioBusReader tap");
					if (node.Operation == RuntimeOperation::AudioBusWriter)
					{
						state->HasBusWriters = true;
						node.StereoScratch.assign(framesPerBlock * 2, 0.0f);
						state->BusWriterNodes.push_back(state->Nodes.size());
					}
				}
				if (!nodeIndices.emplace(source->Id, state->Nodes.size()).second)
					return reject("compiled graph contains duplicate node id '" + source->Id + "'");
				state->Nodes.push_back(std::move(node));
			}

			state->BusInputs.resize(state->BusWriterNodes.size());
			state->Links.reserve(graph.Links.size());
			state->Feedback.reserve(graph.Links.size());
			std::vector<std::size_t> indegree(state->Nodes.size(), 0);
			std::vector<std::vector<std::size_t>> outgoing(state->Nodes.size());
			for (const AvbLink& sourceLink : graph.Links)
			{
				const auto from = nodeIndices.find(sourceLink.FromNode);
				const auto to = nodeIndices.find(sourceLink.ToNode);
				if (from == nodeIndices.end() || to == nodeIndices.end())
					return reject("runtime link references a missing compiled node");
				RuntimeNode& fromNode = state->Nodes[from->second];
				RuntimeNode& toNode = state->Nodes[to->second];
				const std::size_t fromPin = FindPinIndex(fromNode.Outputs, sourceLink.FromPin);
				const std::size_t toPin = FindPinIndex(toNode.Inputs, sourceLink.ToPin);
				if (fromPin == kNoIndex || toPin == kNoIndex)
					return reject("runtime link references an unknown node pin");
				const AvbPinType type = fromNode.Outputs[fromPin].Definition->Type;
				if (sourceLink.Feedback && type != AvbPinType::Audio && type != AvbPinType::Trigger)
					return reject("feedback links require an Audio or Trigger pin with persistent runtime state");
				RuntimeLink link;
				link.FromNode = from->second;
				link.FromPin = fromPin;
				link.ToNode = to->second;
				link.ToPin = toPin;
				link.Feedback = sourceLink.Feedback;
				if (sourceLink.Feedback)
				{
					link.FeedbackIndex = state->Feedback.size();
					FeedbackState delayed;
					if (type == AvbPinType::Audio)
					{
						delayed.PreviousAudio.assign(framesPerBlock, 0.0f);
						delayed.NextAudio.assign(framesPerBlock, 0.0f);
					}
					else
					{
						delayed.PreviousTriggers.Prepare(framesPerBlock, maxTriggersPerBlock);
						delayed.NextTriggers.Prepare(framesPerBlock, maxTriggersPerBlock);
					}
					state->Feedback.push_back(std::move(delayed));
				}
				const std::size_t linkIndex = state->Links.size();
				toNode.Inputs[toPin].Links.push_back(linkIndex);
				state->Links.push_back(link);
				if (!sourceLink.Feedback)
				{
					outgoing[from->second].push_back(to->second);
					++indegree[to->second];
				}
			}

			// Keep compiler order as the deterministic tie-break while adding trigger
			// dependencies, which the data-only compiler intentionally ignores.
			std::priority_queue<std::size_t, std::vector<std::size_t>, std::greater<std::size_t>> ready;
			for (std::size_t i = 0; i < indegree.size(); ++i)
				if (indegree[i] == 0) ready.push(i);
			while (!ready.empty())
			{
				const std::size_t node = ready.top();
				ready.pop();
				state->Order.push_back(node);
				for (std::size_t next : outgoing[node])
					if (--indegree[next] == 0) ready.push(next);
			}
			if (state->Order.size() != state->Nodes.size())
				return reject("runtime dependencies contain an immediate cycle; declare delayed state with Feedback");

			m_Impl = std::move(state);
			if (error) error->clear();
			return true;
		}
		catch (const std::exception& exception)
		{
			return reject(std::string("AVB runtime preparation failed: ") + exception.what());
		}
		catch (...)
		{
			return reject("AVB runtime preparation failed with an unknown exception");
		}
	}

	void AvbRuntime::Reset() noexcept
	{
		if (!m_Impl)
			return;
		Impl& state = *m_Impl;
		state.SampleClock = 0;
		std::fill(state.m_Output.begin(), state.m_Output.end(), 0.0f);
		std::fill(state.BusOutput.begin(), state.BusOutput.end(), 0.0f);
		for (RuntimeNode& node : state.Nodes)
		{
			for (RuntimePin& pin : node.Inputs)
				if (pin.Definition && pin.Definition->Type == AvbPinType::Trigger)
					pin.Triggers.AdvanceBlock(state.Frames);
			for (RuntimePin& pin : node.Outputs)
				if (pin.Definition && pin.Definition->Type == AvbPinType::Trigger)
					pin.Triggers.AdvanceBlock(state.Frames);
				else if (pin.Definition && pin.Definition->Type == AvbPinType::Audio)
					std::fill(pin.Audio.begin(), pin.Audio.end(), 0.0f);
			if (node.Sine) node.Sine->Reset();
			if (node.Noise) node.Noise->Reset(node.NoiseSeed);
			if (node.WavePlayer)
			{
				node.WavePlayer->SetSource(node.Clip->GetSamples().data(), node.Clip->GetFrameCount(),
					                  static_cast<std::uint32_t>(node.Clip->GetSampleRate()));
				node.WavePlayer->SetPitchSemitones(node.PitchSemitones);
				node.WavePlayer->SetLoop(node.Loop);
				node.WavePlayer->SetNearlyFinishedLeadSeconds(node.NearlyFinishedLeadSeconds);
			}
			if (node.Envelope) node.Envelope->Reset();
			node.PendingTriggerCount = 0;
			node.HasFired = false;
			std::fill(node.StereoScratch.begin(), node.StereoScratch.end(), 0.0f);
		}
		for (FeedbackState& edge : state.Feedback)
		{
			std::fill(edge.PreviousAudio.begin(), edge.PreviousAudio.end(), 0.0f);
			std::fill(edge.NextAudio.begin(), edge.NextAudio.end(), 0.0f);
			edge.PreviousTriggers.AdvanceBlock(state.Frames);
			edge.NextTriggers.AdvanceBlock(state.Frames);
		}
	}

	bool AvbRuntime::QueueTrigger(const std::string& nodeId, const std::string& inputPin,
	                             int frame) noexcept
	{
		if (!m_Impl)
			return false;
		Impl& state = *m_Impl;
		const std::size_t nodeIndex = state.FindNode(nodeId);
		if (nodeIndex == kNoIndex)
			return false;
		RuntimeNode& node = state.Nodes[nodeIndex];
		const std::size_t pinIndex = FindPinIndex(node.Inputs, inputPin);
		if (pinIndex == kNoIndex || !node.Inputs[pinIndex].Definition ||
		    node.Inputs[pinIndex].Definition->Type != AvbPinType::Trigger)
			return false;
		return node.Inputs[pinIndex].Triggers.TriggerFrame(frame);
	}

	bool AvbRuntime::RenderBlock(float* monoOutput, std::size_t frames,
	                             AudioBusGraph* buses, AvbBusBlockMode busMode) noexcept
	{
		if (!m_Impl)
		{
			if (monoOutput)
				std::fill_n(monoOutput, frames, 0.0f);
			return false;
		}
		return m_Impl->Render(monoOutput, frames, buses, busMode, false);
	}

	bool AvbRuntime::RenderBlockStereo(float* interleavedStereoOutput, std::size_t frames,
	                                  AudioBusGraph* buses, AvbBusBlockMode busMode) noexcept
	{
		if (!m_Impl)
		{
			if (interleavedStereoOutput &&
			    frames <= (std::numeric_limits<std::size_t>::max)() / 2)
				std::fill_n(interleavedStereoOutput, frames * 2, 0.0f);
			return false;
		}
		return m_Impl->Render(interleavedStereoOutput, frames, buses, busMode, true);
	}

	const AvbTrigger* AvbRuntime::FindTriggerOutput(const std::string& nodeId,
	                                               const std::string& outputPin) const noexcept
	{
		if (!m_Impl)
			return nullptr;
		const std::size_t nodeIndex = m_Impl->FindNode(nodeId);
		if (nodeIndex == kNoIndex)
			return nullptr;
		const RuntimeNode& node = m_Impl->Nodes[nodeIndex];
		const std::size_t pinIndex = FindPinIndex(node.Outputs, outputPin);
		if (pinIndex == kNoIndex || !node.Outputs[pinIndex].Definition ||
		    node.Outputs[pinIndex].Definition->Type != AvbPinType::Trigger)
			return nullptr;
		return &node.Outputs[pinIndex].Triggers;
	}

	bool AvbRuntime::IsPrepared() const noexcept
	{
		return m_Impl != nullptr;
	}

	std::uint32_t AvbRuntime::SampleRate() const noexcept
	{
		return m_Impl ? m_Impl->SampleRate : 0;
	}

	std::size_t AvbRuntime::FramesPerBlock() const noexcept
	{
		return m_Impl ? m_Impl->Frames : 0;
	}

	double AvbRuntime::GetActualBlockRate() const noexcept
	{
		return m_Impl && m_Impl->Frames > 0
			? static_cast<double>(m_Impl->SampleRate) / static_cast<double>(m_Impl->Frames)
			: 0.0;
	}
}
