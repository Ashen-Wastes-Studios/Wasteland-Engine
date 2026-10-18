#include "Wasteland/Avb/AvbDsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <exception>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace Wasteland::Avb::Dsp;

namespace
{
	int g_Checks = 0;
	int g_Failures = 0;
	constexpr std::uint32_t kSampleRate = 48000;
	constexpr float kPi = 3.14159265358979323846f;

	void Check(bool condition, const char* message)
	{
		++g_Checks;
		if (!condition)
		{
			++g_Failures;
			std::printf("  FAIL: %s\n", message);
		}
	}

	void CheckNear(float actual, float expected, float tolerance, const char* message)
	{
		Check(std::isfinite(actual) && std::fabs(actual - expected) <= tolerance, message);
	}

	bool AllFinite(const float* samples, std::size_t count)
	{
		for (std::size_t i = 0; i < count; ++i)
			if (!std::isfinite(samples[i]))
				return false;
		return true;
	}

	float Rms(const std::vector<float>& samples, std::size_t skip = 0)
	{
		double sum = 0.0;
		std::size_t count = 0;
		for (std::size_t i = skip; i < samples.size(); ++i)
		{
			sum += static_cast<double>(samples[i]) * samples[i];
			++count;
		}
		return count ? static_cast<float>(std::sqrt(sum / count)) : 0.0f;
	}

	std::vector<float> MakeSine(std::size_t frames, float frequency, float amplitude = 0.5f)
	{
		std::vector<float> samples(frames);
		for (std::size_t i = 0; i < frames; ++i)
			samples[i] = amplitude * std::sin(2.0f * kPi * frequency * static_cast<float>(i) / kSampleRate);
		return samples;
	}

	void TestSampleRateContract()
	{
		Biquad valid(kSampleRate);
		Biquad minimum(kMinSampleRate), maximum(kMaxSampleRate);
		Check(valid.SampleRate() == kSampleRate, "DSP processors retain their explicit sample rate");
		Check(minimum.SampleRate() == kMinSampleRate && maximum.SampleRate() == kMaxSampleRate,
		      "the documented 8 kHz and 384 kHz sample-rate boundaries are accepted");
		bool rejected = false;
		try
		{
			Biquad invalid(0);
			(void)invalid;
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Check(rejected, "a zero sample rate is rejected instead of dividing by zero");
		rejected = false;
		try
		{
			Biquad aboveMaximum(kMaxSampleRate + 1);
			(void)aboveMaximum;
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Check(rejected, "a sample rate above 384 kHz is rejected");
	}

	void TestBiquadAndFilters()
	{
		Biquad filter(kSampleRate, BiquadType::LowPass, 1000.0f, 0.7071067f);
		auto lowTone = MakeSine(kSampleRate, 1000.0f);
		auto highTone = MakeSine(kSampleRate, 10000.0f);
		std::vector<float> lowOut(lowTone.size()), highOut(highTone.size());
		filter.Process(lowTone.data(), lowOut.data(), lowOut.size());
		filter.Reset();
		filter.Process(highTone.data(), highOut.data(), highOut.size());
		const float lowRms = Rms(lowOut, 1024);
		const float highRms = Rms(highOut, 1024);
		Check(lowRms < Rms(lowTone, 1024) * 0.8f,
		      "a 1 kHz low-pass attenuates a tone at its cutoff as specified");
		Check(lowRms > highRms * 5.0f, "a 1 kHz low-pass passes 1 kHz substantially better than 10 kHz");
		Check(highRms < 0.08f, "a 1 kHz low-pass attenuates a 10 kHz input in the real output buffer");

		StateVariableFilter svf(kSampleRate, 1200.0f, 0.7071067f);
		std::vector<StateVariableOutput> modes(2048);
		auto source = MakeSine(modes.size(), 500.0f);
		svf.Process(source.data(), modes.data(), modes.size());
		Check(AllFinite(source.data(), source.size()), "SVF receives the authored mono source buffer");
		bool sawBandEnergy = false;
		for (const auto& frame : modes)
		{
			Check(std::isfinite(frame.Low) && std::isfinite(frame.Band) &&
			      std::isfinite(frame.High) && std::isfinite(frame.Notch),
			      "SVF low/band/high/notch outputs remain finite");
			sawBandEnergy = sawBandEnergy || std::fabs(frame.Band) > 1.0e-4f;
		}
		Check(sawBandEnergy, "SVF band output carries real input energy");

		OnePole lowpass(kSampleRate, OnePoleType::LowPass, 500.0f);
		OnePole highpass(kSampleRate, OnePoleType::HighPass, 500.0f);
		auto lowFrequencySource = MakeSine(source.size(), 100.0f);
		std::vector<float> lp(source.size()), hp(source.size());
		lowpass.Process(lowFrequencySource.data(), lp.data(), source.size());
		highpass.Process(lowFrequencySource.data(), hp.data(), source.size());
		Check(Rms(lp, 128) > Rms(hp, 128) * 3.0f, "one-pole LP passes low-frequency input better than its HP counterpart");

		LadderFilter ladder(kSampleRate, 18000.0f, 3.8f);
		std::vector<float> hotInput(4096, 1000.0f), ladderOutput(hotInput.size());
		ladder.Process(hotInput.data(), ladderOutput.data(), ladderOutput.size());
		Check(AllFinite(ladderOutput.data(), ladderOutput.size()), "resonant ladder stays finite under extreme real input");
		Check(ladderOutput.back() != 0.0f, "ladder filter processes rather than discarding its input buffer");
	}

	void TestWaveShaper()
	{
		const float input[] = {-2.0f, -0.5f, 0.0f, 0.5f, 2.0f};
		float output[5] = {};
		WaveShaper shaper;
		shaper.SetParameters(WaveShape::HardClip, 1.0f, 0.0f, 1.0f);
		shaper.Process(input, output, 5);
		CheckNear(output[0], -1.0f, 1.0e-6f, "hard clip at drive 1 clamps negative full scale");
		CheckNear(output[4], 1.0f, 1.0e-6f, "hard clip at drive 1 clamps positive full scale");
		shaper.SetParameters(WaveShape::HardClip, 0.1f, 0.0f, 1.0f);
		shaper.Process(input, output, 5);
		CheckNear(output[1], -0.5f, 1.0e-5f, "hard clip at drive 0.1 is approximately identity for in-range samples");
		CheckNear(output[3], 0.5f, 1.0e-5f, "low-drive waveshaping does not normalize real input away");
		shaper.SetParameters(WaveShape::HardClip, 1.0f, 0.25f, 1.0f);
		const float zero = 0.0f;
		shaper.Process(&zero, output, 1);
		Check(output[0] > 0.2f, "waveshaper bias produces a measurable DC offset");

		const float curveInput[] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
		for (WaveShape shape : {WaveShape::Sine, WaveShape::InverseTangent,
		                        WaveShape::HyperbolicTangent, WaveShape::Cubic,
		                        WaveShape::HardClip})
		{
			shaper.SetParameters(shape, 1.0f, 0.0f, 1.0f);
			shaper.Process(curveInput, output, 5);
			Check(AllFinite(output, 5), "each waveshaper curve processes a real sample buffer finitely");
			CheckNear(output[0], -output[4], 1.0e-5f, "each waveshaper curve preserves odd symmetry at full scale");
			CheckNear(output[1], -output[3], 1.0e-5f, "each waveshaper curve preserves odd symmetry in-band");
		}
	}

	void TestEnvelopes()
	{
		AdEnvelope ad(kSampleRate, 0.01f, 0.02f);
		ad.Trigger();
		std::vector<float> attack(480), decay(1200);
		ad.Process(attack.data(), attack.size());
		Check(attack.back() >= 0.95f, "AD attack reaches its peak within 5% of authored 10 ms time");
		ad.Process(decay.data(), decay.size());
		CheckNear(decay.back(), 0.0f, 1.0e-7f, "AD decay reaches exact zero");
		ad.SetLooping(true);
		ad.Trigger();
		std::vector<float> loop(4000);
		ad.Process(loop.data(), loop.size());
		Check(AllFinite(loop.data(), loop.size()), "looping AD envelope remains finite across repeated cycles");
		Check(loop.back() > 0.0f, "looping AD envelope does not get stuck at its terminal zero");

		AdsrEnvelope adsr(kSampleRate);
		adsr.SetParameters(0.01f, 0.01f, 0.5f, 0.02f);
		adsr.TriggerAttack();
		std::vector<float> sustain(1200);
		adsr.Process(sustain.data(), sustain.size());
		CheckNear(sustain[479], 1.0f, 1.0e-6f, "ADSR attack reaches peak at the authored 10 ms sample count");
		CheckNear(sustain[1000], 0.5f, 0.01f, "ADSR transitions from attack through decay to authored sustain");
		adsr.TriggerRelease();
		std::vector<float> release(1200);
		adsr.Process(release.data(), release.size());
		CheckNear(release[release.size() - 1], 0.0f, 0.0f, "ADSR release returns to exactly zero");
		std::vector<float> silence(32);
		adsr.Process(silence.data(), silence.size());
		CheckNear(silence.back(), 0.0f, 0.0f, "ADSR remains exactly zero after release has completed");

		ad.SetParameters(0.0f, 0.0f);
		ad.SetLooping(false);
		ad.Trigger();
		std::vector<float> zeroTime(16);
		ad.Process(zeroTime.data(), zeroTime.size());
		Check(AllFinite(zeroTime.data(), zeroTime.size()) && zeroTime.back() == 0.0f,
		      "zero-duration AD phases terminate as exact silence without division by zero");
		adsr.SetParameters(0.0f, 0.0f, 0.4f, 0.0f);
		adsr.TriggerAttack();
		adsr.Process(zeroTime.data(), zeroTime.size());
		CheckNear(zeroTime.back(), 0.4f, 1.0e-6f, "zero ADSR attack/decay reaches sustain immediately");
		adsr.TriggerRelease();
		adsr.Process(zeroTime.data(), zeroTime.size());
		Check(zeroTime.back() == 0.0f, "zero-duration ADSR release is immediate and exact");
	}

	void TestCompressorAndLimiter()
	{
		Compressor compressor(kSampleRate);
		compressor.SetParameters(-20.0f, 8.0f, 0.0f, 0.005f, 0.05f, 0.0f, DetectorMode::Peak);
		std::vector<float> signal(4096, 0.1f), loudSidechain(signal.size(), 1.0f), quietSidechain(signal.size(), 0.0f);
		std::vector<float> ducked(signal.size()), unducked(signal.size()), duckGain(signal.size()), unityGain(signal.size());
		compressor.Process(signal.data(), loudSidechain.data(), ducked.data(), duckGain.data(), signal.size());
		compressor.Reset();
		compressor.Process(signal.data(), quietSidechain.data(), unducked.data(), unityGain.data(), signal.size());
		Check(ducked.back() < unducked.back() * 0.25f,
		      "compressor ducks the program input from an independent loud sidechain");
		CheckNear(unityGain.back(), 1.0f, 1.0e-3f,
		          "quiet sidechain leaves below-threshold program signal at unity gain");
		Check(AllFinite(duckGain.data(), duckGain.size()), "compressor GainEnvelope output is a real finite buffer");

		Limiter limiter(kSampleRate);
		limiter.SetParameters(0.8f, 0.05f, 0.0f);
		std::vector<float> overload(4096, 4.0f), limited(overload.size());
		limiter.Process(overload.data(), limited.data(), limited.size());
		Check(AllFinite(limited.data(), limited.size()), "limiter remains finite on overload input");
		float peak = 0.0f;
		for (float sample : limited)
			peak = std::max(peak, std::fabs(sample));
		Check(peak <= 0.80001f, "limiter enforces its declared output ceiling on actual samples");

		Compressor lookaheadCompressor(kSampleRate);
		lookaheadCompressor.SetParameters(0.0f, 4.0f, 0.0f, 0.0f, 0.0f,
		                                  2.0f / static_cast<float>(kSampleRate));
		float transient[8] = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
		float aligned[8] = {};
		lookaheadCompressor.Process(transient, nullptr, aligned, nullptr, 8);
		CheckNear(aligned[0], 0.0f, 1.0e-6f, "compressor lookahead delays the program path by its configured frame count");
		CheckNear(aligned[2], 0.5f, 1.0e-4f, "compressor lookahead returns the real transient at the aligned frame");

		Limiter lookaheadLimiter(kSampleRate);
		lookaheadLimiter.SetParameters(0.5f, 0.05f, 2.0f / static_cast<float>(kSampleRate));
		float limitedTransient[8] = {};
		lookaheadLimiter.Process(transient, limitedTransient, 8);
		CheckNear(limitedTransient[0], 0.0f, 1.0e-6f, "limiter lookahead has the documented sample delay");
		Check(limitedTransient[2] > 0.49f && limitedTransient[2] <= 0.50001f,
		      "limiter keeps the lookahead transient below its ceiling after alignment");
	}

	void TestDelayLines()
	{
		Delay delay(kSampleRate, 0.25f);
		delay.SetDelaySeconds(480.5f / static_cast<float>(kSampleRate));
		delay.SetFeedback(0.5f);
		delay.SetMix(0.0f, 1.0f);
		std::vector<float> impulse(2000, 0.0f), output(impulse.size(), 0.0f);
		impulse[0] = 1.0f;
		delay.Process(impulse.data(), output.data(), output.size());
		CheckNear(output[480], 0.5f, 1.0e-4f, "fractional delay interpolates the half-frame first echo");
		CheckNear(output[481], 0.5f, 1.0e-4f, "fractional delay distributes the impulse across adjacent samples");
		CheckNear(output[961], 0.25f, 1.0e-4f, "fractional-delay feedback produces the correctly interpolated second echo");
		delay.Reset();
		delay.SetDelaySeconds(0.01f);
		delay.SetFeedback(4.0f);
		Check(delay.Feedback() <= 0.99f, "feedback above the stability bound is caught by the parameter guard");
		std::fill(impulse.begin(), impulse.end(), 0.0f);
		impulse[0] = 1.0f;
		delay.Process(impulse.data(), output.data(), output.size());
		Check(AllFinite(output.data(), output.size()), "bounded feedback delay remains finite on an impulse buffer");
		Check(output[1920] > 0.9f && output[1920] < output[960],
		      "feedback guard makes successive real-buffer echoes decay rather than normalize away runaway state");
	}

	void TestStereoProcessing()
	{
		StereoPanner panner;
		const float mono[] = {0.5f, -0.25f};
		float stereo[4] = {};
		panner.Process(mono, stereo, 2, 0.0f, PanLaw::EqualPower);
		CheckNear(stereo[0] * stereo[0] + stereo[1] * stereo[1], 0.25f, 1.0e-6f,
		          "equal-power panner preserves energy at centre");
		panner.Process(mono, stereo, 2, -1.0f, PanLaw::Linear);
		CheckNear(stereo[0], mono[0], 1.0e-6f, "linear hard-left pan routes the mono buffer to left");
		CheckNear(stereo[1], 0.0f, 1.0e-6f, "linear hard-left pan silences right");

		StereoMixer mixer;
		const float a[] = {1.0f, -1.0f, 0.5f, 0.25f};
		const float b[] = {0.5f, 0.25f, -1.0f, 2.0f};
		float mixed[4] = {};
		mixer.Process(a, b, mixed, 2, -20.0f, 30.0f);
		CheckNear(mixed[0], -5.0f, 1.0e-6f, "stereo mixer preserves unclamped negative gain/inversion");
		CheckNear(mixed[1], 27.5f, 1.0e-6f, "stereo mixer honors over-unity gains without normalization");

		const std::size_t frames = 16;
		std::vector<float> stereoIn(frames * 2, 0.0f), normalOut(frames * 2), crossOut(frames * 2), pingOut(frames * 2);
		stereoIn[0] = 1.0f;
		StereoDelay normal(kSampleRate, 0.01f), cross(kSampleRate, 0.01f), ping(kSampleRate, 0.01f);
		const float delaySeconds = 4.0f / static_cast<float>(kSampleRate);
		for (StereoDelay* unit : {&normal, &cross, &ping})
		{
			unit->SetDelaySeconds(delaySeconds, delaySeconds);
			unit->SetFeedback(0.5f);
			unit->SetMix(0.0f, 1.0f);
		}
		normal.SetMode(StereoDelayMode::Normal);
		cross.SetMode(StereoDelayMode::Cross);
		ping.SetMode(StereoDelayMode::PingPong);
		normal.Process(stereoIn.data(), normalOut.data(), frames);
		cross.Process(stereoIn.data(), crossOut.data(), frames);
		ping.Process(stereoIn.data(), pingOut.data(), frames);
		CheckNear(normalOut[4 * 2], 1.0f, 1.0e-5f, "normal stereo delay preserves left-channel routing");
		CheckNear(crossOut[4 * 2 + 1], 1.0f, 1.0e-5f, "cross stereo delay routes the delayed left input to right");
		CheckNear(pingOut[8 * 2 + 1], 0.5f, 1.0e-5f, "ping-pong feedback alternates a left impulse into the right channel");
		Check(AllFinite(pingOut.data(), pingOut.size()), "stereo delay channels stay finite across feedback");
	}

	void TestGeneratorsAndEq()
	{
		Lfo first(kSampleRate, 2.0f, OscillatorWaveform::Triangle);
		Lfo second(kSampleRate, 2.0f, OscillatorWaveform::Triangle);
		std::vector<float> a(4096), b(a.size());
		first.Process(a.data(), a.size());
		second.Process(b.data(), b.size());
		Check(a == b, "LFO with identical rate/waveform/phase is bit-deterministic across real buffers");
		Check(AllFinite(a.data(), a.size()), "triangle LFO produces finite output");
		for (float sample : a)
			Check(std::fabs(sample) <= 1.0f, "LFO waveform remains in its documented unit range");

		Sine oscillator(kSampleRate, 1000.0f);
		std::vector<float> measuredTone(kSampleRate);
		oscillator.Process(measuredTone.data(), measuredTone.size());
		int positiveZeroCrossings = 0;
		for (std::size_t i = 1; i < measuredTone.size(); ++i)
			if (measuredTone[i - 1] <= 0.0f && measuredTone[i] > 0.0f)
				++positiveZeroCrossings;
		Check(std::abs(positiveZeroCrossings - 1000) <= 1,
		      "sine oscillator buffer has the requested 1 kHz zero-crossing rate at 48 kHz");
		oscillator.Reset();
		std::vector<float> tone(1024);
		std::vector<std::uint8_t> sync(tone.size(), 0);
		sync[137] = 1;
		oscillator.Process(tone.data(), tone.size(), sync.data());
		CheckNear(tone[137], 0.0f, 1.0e-6f, "sample-indexed Sine Sync resets phase on the requested frame");
		oscillator.SetFrequency(2000.0f, 0.01f);
		std::vector<float> glide(1024);
		oscillator.Process(glide.data(), glide.size());
		CheckNear(oscillator.CurrentFrequency(), 2000.0f, 1.0e-3f,
		          "sine glide reaches the target frequency after its sample-rate-derived duration");
		Check(AllFinite(glide.data(), glide.size()), "sine glide output remains finite");

		Noise noiseA(kSampleRate, 0x12345678u), noiseB(kSampleRate, 0x12345678u), noiseOther(kSampleRate, 0x87654321u);
		std::vector<float> n1(2048), n2(n1.size()), n3(n1.size());
		noiseA.Process(n1.data(), n1.size());
		noiseB.Process(n2.data(), n2.size());
		noiseOther.Process(n3.data(), n3.size());
		Check(n1 == n2, "seeded noise is bit-identical for the same seed and sample count");
		Check(n1 != n3, "different noise seeds produce distinct real sample buffers");
		for (float sample : n1)
			Check(sample >= -1.0f && sample < 1.0f, "seeded white noise stays in [-1, 1)");

		BiquadEq eq(kSampleRate);
		// Isolate the low shelf: a mid-band cut also attenuates this 250 Hz test tone.
		eq.SetParameters(250.0f, 6.0f, 1200.0f, 0.0f, 7000.0f, 0.0f);
		auto source = MakeSine(4096, 250.0f);
		std::vector<float> shaped(source.size());
		eq.Process(source.data(), shaped.data(), shaped.size());
		Check(AllFinite(shaped.data(), shaped.size()), "three-band biquad EQ processes finite actual samples");
		Check(Rms(shaped, 128) > Rms(source, 128) * 1.3f,
		      "low-shelf EQ gain measurably boosts its in-band buffer with other bands neutral");
	}

	void TestNonFiniteAndEmptyBuffers()
	{
		const float bad[] = {std::numeric_limits<float>::quiet_NaN(),
		                     std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
		                     0.25f, -0.25f};
		float output[5] = {};
		Biquad filter(kSampleRate);
		filter.Process(bad, output, 5);
		Check(AllFinite(output, 5), "biquad guards NaN/Inf input without poisoning later samples");
		WaveShaper shaper;
		shaper.Process(bad, output, 5);
		Check(AllFinite(output, 5), "waveshaper rejects non-finite samples at its input boundary");
		LadderFilter ladder(kSampleRate, 20000.0f, 3.8f);
		ladder.Process(bad, output, 5);
		Check(AllFinite(output, 5), "ladder's maximum-resonance state remains finite on malformed samples");
		StateVariableFilter svf(kSampleRate);
		StateVariableOutput svfOutput[5] = {};
		svf.Process(bad, svfOutput, 5);
		bool svfFinite = true;
		for (const auto& frame : svfOutput)
			svfFinite = svfFinite && std::isfinite(frame.Low) && std::isfinite(frame.Band) &&
			            std::isfinite(frame.High) && std::isfinite(frame.Notch);
		Check(svfFinite, "state-variable filter sanitizes non-finite inputs for all output modes");
		OnePole onePole(kSampleRate);
		onePole.Process(bad, output, 5);
		Check(AllFinite(output, 5), "one-pole filter remains finite on malformed samples");
		Compressor compressor(kSampleRate);
		float gain[5] = {};
		compressor.Process(bad, bad, output, gain, 5);
		Check(AllFinite(output, 5) && AllFinite(gain, 5), "compressor program, sidechain and gain envelope stay finite");
		Limiter limiter(kSampleRate);
		limiter.Process(bad, output, 5);
		Check(AllFinite(output, 5), "limiter sanitizes non-finite samples");
		Delay delay(kSampleRate, 0.001f);
		delay.Process(bad, output, 5);
		Check(AllFinite(output, 5), "delay state stays finite when input contains NaN and infinities");
		float stereoBad[10] = {};
		for (int i = 0; i < 5; ++i)
		{
			stereoBad[i * 2] = bad[i];
			stereoBad[i * 2 + 1] = bad[4 - i];
		}
		float stereoOut[10] = {};
		StereoDelay stereoDelay(kSampleRate, 0.001f);
		stereoDelay.Process(stereoBad, stereoOut, 5);
		Check(AllFinite(stereoOut, 10), "stereo delay sanitizes both interleaved channels");
		StereoPanner panner;
		panner.Process(bad, stereoOut, 5, std::numeric_limits<float>::quiet_NaN());
		Check(AllFinite(stereoOut, 10), "panner maps invalid pan and samples to finite stereo output");
		StereoMixer stereoMixer;
		stereoMixer.Process(stereoBad, stereoBad, stereoOut, 5,
		                    std::numeric_limits<float>::infinity(), -2.0f);
		Check(AllFinite(stereoOut, 10), "stereo mixer bounds gains and sanitizes non-finite input");
		Lfo lfo(kSampleRate);
		lfo.SetFrequency(std::numeric_limits<float>::quiet_NaN());
		lfo.Process(output, 5);
		Check(AllFinite(output, 5), "LFO handles a non-finite frequency without contaminating its phase");
		Sine oscillator(kSampleRate);
		oscillator.SetFrequency(std::numeric_limits<float>::infinity());
		oscillator.SetAmplitude(std::numeric_limits<float>::infinity());
		oscillator.Process(output, 5);
		Check(AllFinite(output, 5), "sine clamps invalid oscillator parameters to finite bounded values");
		BiquadEq eq(kSampleRate);
		eq.Process(bad, output, 5);
		Check(AllFinite(output, 5), "cascaded three-band EQ guards non-finite buffer input");
		AdEnvelope invalidAd(kSampleRate, std::numeric_limits<float>::quiet_NaN(),
		                     std::numeric_limits<float>::infinity());
		invalidAd.Trigger();
		invalidAd.Process(output, 5);
		Check(AllFinite(output, 5), "AD envelope replaces non-finite timing parameters with bounded defaults");
		AdsrEnvelope invalidAdsr(kSampleRate);
		invalidAdsr.SetParameters(std::numeric_limits<float>::infinity(),
		                         std::numeric_limits<float>::quiet_NaN(),
		                         std::numeric_limits<float>::quiet_NaN(),
		                         -std::numeric_limits<float>::infinity(),
		                         std::numeric_limits<float>::quiet_NaN());
		invalidAdsr.TriggerAttack();
		invalidAdsr.Process(output, 5);
		Check(AllFinite(output, 5), "ADSR bounds non-finite times, sustain and curve controls");
		Biquad control(kSampleRate), zeroFrameProbe(kSampleRate);
		const float first = 0.25f, next = -0.5f;
		float controlOut = 0.0f, probeOut = 0.0f;
		control.Process(&first, &controlOut, 1);
		zeroFrameProbe.Process(&first, &probeOut, 1);
		zeroFrameProbe.Process(nullptr, nullptr, 0);
		control.Process(&next, &controlOut, 1);
		zeroFrameProbe.Process(&next, &probeOut, 1);
		CheckNear(probeOut, controlOut, 0.0f,
		          "zero-frame processing accepts null buffers and leaves filter state unchanged");
	}
}

int main()
{
	TestSampleRateContract();
	TestBiquadAndFilters();
	TestWaveShaper();
	TestEnvelopes();
	TestCompressorAndLimiter();
	TestDelayLines();
	TestStereoProcessing();
	TestGeneratorsAndEq();
	TestNonFiniteAndEmptyBuffers();
	std::printf("test_avb_dsp: %d checks, %d failures\n", g_Checks, g_Failures);
	return g_Failures ? 1 : 0;
}
