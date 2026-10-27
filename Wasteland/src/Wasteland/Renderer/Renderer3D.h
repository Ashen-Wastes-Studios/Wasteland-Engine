#pragma once

#include "Wasteland/Renderer/Camera.h"
#include "Wasteland/Renderer/EditorCamera.h"
#include "Wasteland/Renderer/Shader.h"
#include "Wasteland/Renderer/VertexArray.h"
#include "Wasteland/Renderer/ProceduralMeshes.h"

#include "Wasteland/Renderer/Texture.h"
#include <Wasteland/Scene/Components.h>
#include <Wasteland/Scene/Entity.h>

#include <glm/glm.hpp>

// For the std::vector out-parameter on ReadbackRayTraceTarget below. Declared
// here rather than relying on a transitive include, so the header stands alone.
#include <vector>

namespace Wasteland::Terrain
{
    struct TerrainChunkMesh;
}

namespace Wasteland
{

    // Explicit :int underlying type so the value can cross the Python binding
    // and any serialised settings blob as a plain integer, with no enum-name
    // coupling on the far side. The four values are ordered deliberately --
    // the dynamic-resolution scaler compares presets by magnitude.
    enum class QualityPreset : int
    {
        Low = 0,
        Medium = 1,
        High = 2,
        Ultra = 3
    };

    // Anti-aliasing, split into two orthogonal axes.
    // SamplePattern: where the sub-pixel offset comes from (a CPU-side choice
    // expressed through the existing u_Jitter uniform -- no new pass needed).
    // ReconstructionFilter: how samples are combined into the final image.
    // They are separate enums because the combinations are meaningful
    // (RGSS + TAA and RGSS + FSR1 are both valid); folding them into one enum
    // would force a cross-product into the UI.
    enum class SamplePattern : int
    {
        BlueNoise = 0,   // current default: STBN spatio-temporal blue noise
        Halton = 1,      // classic radical inverse (code still present, currently unused)
        RotatedGrid = 2, // RGSS, the classic 4x pattern
        Quincunx = 3,    // 5-sample: 4 corners + centre
        OrderedGrid = 4, // 2x2 axis-aligned
        Centre = 5       // no jitter
    };

    enum class ReconstructionFilter : int
    {
        None = 0,          // raw output at RenderScale 1.0, no upscale, no accumulation
        Nearest = 1,       // 1 tap, point sampling
        Bilinear = 2,      // 4 tap -- the current implicit default
        Box = 3,           // 4 tap unweighted
        Bicubic = 4,       // 16 tap Catmull-Rom
        Lanczos = 5,       // windowed sinc
        FXAA = 6,          // post-process edge AA, cheap fallback for old GPUs
        SMAA = 7,          // post-process edge AA, better quality than FXAA
        FSR1 = 8,          // EASU spatial upscaler. RCAS is NOT implemented: it
                          // would need a second full-res pass over EASU's
                          // output, and there is no scratch image to run it in.
        TSR = 9,           // temporal super resolution. DECLARED BUT NOT
                          // WIRED: passes 15/16 exist in the shader, are never
                          // dispatched, and u_TSREnabled/u_TSRSharpenStrength
                          // are never uploaded. It needs a cross-frame history
                          // buffer and the image binding budget is exhausted,
                          // so the editor refuses the selection rather than
                          // accept a filter that cannot run.
        Temporal = 10,     // TAA. NOT consumed here -- pass 2 (RunTemporal-
                          // Accumulation) always runs and is gated by the
                          // separate RayTracingAccumulate flag (the editor's
                          // "Accumulate" checkbox) and u_AccumulationAlpha.
                          // Selecting this in the filter dropdown is a no-op.
        SpatialDenoise = 11, // ATrous wavelet only, no temporal
        Wlaa = 12            // WLAA: native-resolution AA = the temporal
                             // accumulation (pass 2) plus the RCAS sharpen
                             // folded into the composite at RenderScale 1.0. A
                             // real branch, not a fall-through: RunComposite
                             // reads this value and sharpens the resolved image.
    };

    // Entirely static: one process-wide renderer, no instance state. That is
    // why Init/Shutdown are explicit static calls rather than a ctor/dtor pair.
    class Renderer3D
    {
    public:
        // Creates native GPU resources and reads the driver renderer string to
        // classify the GPU tier (see GetGPUTierName). Must precede any Draw*.
        static void Init();
        // Releases what Init created. The editor switches rendering APIs at
        // runtime, which is why RecreateResources exists as a separate step.
        static void Shutdown();
        // Rebuild native GPU resources after switching rendering APIs.
        static void RecreateResources();

        // Overload pair rather than a Camera* : the editor camera is a different
        // type, so the call site's static type picks the view/proj source and no
        // runtime branch is needed inside the renderer.
        static void BeginScene(const Camera &camera, const glm::mat4 &transform);
        static void BeginScene(const EditorCamera &camera);
        // Publishes the scene's transform epoch for THIS frame. Must be called
        // BEFORE BeginScene, because BeginScene's flush consults it: the gate
        // compares the epoch sampled now against the one recorded when the
        // previous frame's instances were accepted.
        //
        // The renderer cannot see the Scene (BeginScene takes a camera, not a
        // scene), so the Scene publishes its own stamp. See Scene::BumpTransformEpoch
        // for why the stamp is conservative rather than precise.
        static void SetSceneTransformEpoch(uint32_t epoch);
        // Closes the frame's submission. Not the same as Flush(): EndScene ends
        // recording, Flush is what actually drains the accumulated batch to the GPU.
        static void EndScene();
        // Drains the shared dynamic batch. Draw* only record into it, so anything
        // that must be on screen this frame has to be followed by a Flush.
        static void Flush();

        // Primitive 3D drawing functions mimicking 2D
        // entityID defaults to -1 ("no owning entity"), which the submit path
        // treats as a scene-level rather than per-entity draw.
        static void DrawCube(const glm::mat4 &transform, const glm::vec4 &color, MaterialComponent &material, int entityID = -1);

        // sectors/stacks are tessellation counts, so this also costs geometry:
        // the caller owns the trade-off between a smooth silhouette and vertex
        // count. The Python/Cube-era 2D path had no equivalent knob.
        static void DrawSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int sectors, int stacks, MaterialComponent &material, int entityID = -1);

        // Blender "Add > Mesh" primitives, generated by ProceduralMeshes.
        // Each follows DrawCube/DrawSphere: rasterised into the shared dynamic
        // vertex/index batch, and pushed to the Nova path tracer as an analytic
        // instance carrying its PrimitiveShape discriminator.
        // The eight Draw* primitives below share one implementation
        // (DrawProceduralPrimitive) that both rasterises into the dynamic batch
        // and, when ray tracing is on, pushes the matching Nova analytic instance.
        static void DrawPlane(const glm::mat4 &transform, const glm::vec4 &color, int subdivisions, MaterialComponent &material, int entityID = -1);
        static void DrawCircle(const glm::mat4 &transform, const glm::vec4 &color, float radius, int segments, MaterialComponent &material, int entityID = -1);
        static void DrawIcoSphere(const glm::mat4 &transform, const glm::vec4 &color, float radius, int subdivisions, MaterialComponent &material, int entityID = -1);
        static void DrawCylinder(const glm::mat4 &transform, const glm::vec4 &color, float radius, float height, int segments, MaterialComponent &material, int entityID = -1);
        static void DrawCone(const glm::mat4 &transform, const glm::vec4 &color, float radius, float height, int segments, MaterialComponent &material, int entityID = -1);
        static void DrawTorus(const glm::mat4 &transform, const glm::vec4 &color, float majorRadius, float minorRadius, int majorSegments, int minorSegments, MaterialComponent &material, int entityID = -1);
        static void DrawGrid(const glm::mat4 &transform, const glm::vec4 &color, int xSubdivisions, int ySubdivisions, MaterialComponent &material, int entityID = -1);

        // Identity default: a shader+VAO pair that is already in world space can
        // be submitted with no matrix argument at all.
        static void Submit(const Ref<Shader> &shader, const Ref<VertexArray> &vertexArray, const glm::mat4 &transform = glm::mat4(1.0f));

        // Stats
        // Plain aggregate with in-class defaults, so a default-constructed
        // Statistics already reads as zero -- no ctor needed and no chance of an
        // uninitialised counter being reported.
        struct Statistics
        {
            uint32_t DrawCalls = 0;
            uint32_t QuadCount = 0;

            // Derived from QuadCount rather than counted independently, so the three
            // numbers can never disagree. QuadCount * 4 is exact in uint32 for any
            // plausible frame, but nothing here guards the multiply -- see the
            // GetStats() getter, which is the one place the values are exposed.
            uint32_t GetTotalVertexCount() { return QuadCount * 4; }
            uint32_t GetTotalIndexCount() { return QuadCount * 6; }

            // Cumulative count of CPU tessellation batch overflows that fired
            // mid-frame (a Draw* pushed past MaxVertices/MaxIndices and had to
            // reset the batch before EndScene). NOT reset per frame: it is a
            // scene-capacity signal, so a monotonic total is the useful form.
            // A rising value means the batch is too small for the scene.
            uint32_t MidFrameFlushes = 0;

            // Instances dropped by the screen-space cull (WASTELAND_SCREEN_CULL)
            // rather than submitted to the BVH. Per-frame, and reset by
            // ResetStats(): it is a tuning readout for that cull, not a
            // scene-capacity signal like MidFrameFlushes above. A high value means
            // the threshold is eating visible geometry; zero with the cull enabled
            // means nothing was small enough to drop.
            uint32_t ScreenCulledInstances = 0;

            // The PREVIOUS frame's ScreenCulledInstances, snapshotted at the end of
            // Flush. Exists because the frame-time log runs early in Flush (from
            // UpdateDynamicResolution) while the cull that populates
            // ScreenCulledInstances runs at the end -- so logging the live counter
            // reported 0 on every frame, no matter how many instances were dropped.
            // Measured: a 100k-entity scene at an 8px threshold went 24,576 -> 1,912
            // instances while the log still said screenCulled 0. Read THIS field for
            // reporting; read ScreenCulledInstances only from inside the cull.
            uint32_t LastFrameScreenCulled = 0;

            // Instances that reached CullInstanceForTrace with MaxDistance <= 0.
            // Must be zero. A non-zero value means a submission path built its
            // instance without assigning MaxDistance: the CPU cull then drops
            // every instance from that path (`camDist >= 0.0f`), and the shader
            // would reject them too. Per-frame, reset by ResetStats().
            uint32_t ZeroMaxDistanceInstances = 0;

            // Mean of the per-pixel gate over the last frame, written by the
            // neural stage's pass 19 and read back by the renderer. 0.0 when the
            // stage is disabled or no weights are loaded.
            //
            // Deliberately a MEASURED value, never a constant. The editor needs
            // to tell "the network decided not to enhance this frame" (gate near
            // 0, frame unchanged) from "the stage silently isn't running" (frame
            // also unchanged) -- those are indistinguishable on screen, and only
            // the gate separates them. An earlier draft of this panel would have
            // printed a hardcoded 0.42 here, which is exactly the kind of
            // invented number that makes a broken stage look healthy.
            float NeuralGateMean = 0.0f;

            // --- instance-delta accounting (Phase 2 of the million-entity plan) ---
            // All four are CUMULATIVE, not per-frame: a per-frame counter reads 0 on
            // a static scene and cannot distinguish "nothing changed" from "the gate
            // was never consulted".
            //
            // InstancesSubmitted is the number of ray-traced instances the flush
            // accepted on the most recent frame that submitted any -- i.e. the size
            // of m_SceneInstances after the capacity clamp, so it is comparable
            // against MaxRayTracingInstances.
            uint32_t InstancesSubmitted = 0;
            // Instances the flush had to DROP because the scene exceeded
            // MaxRayTracingInstances. Non-zero means geometry is missing from the
            // frame -- a correctness failure, not a budget note. Reset to 0 on any
            // frame that fits, so it reads as "what is being lost right now".
            uint32_t InstancesTruncated = 0;
            // How many times the CPU BVH was rebuilt. The interesting assertion is
            // a DELTA across a window (a static scene must not add to it), not the
            // absolute value, because scene load and camera moves legitimately
            // rebuild.
            uint32_t BvhRebuilds = 0;
            // How many times SceneInstancesChanged() returned true. Kept alongside
            // BvhRebuilds because a flush can be skipped for a reason other than
            // "unchanged" (FrameIndex==0 forces a rebuild).
            uint32_t InstanceInvalidations = 0;
            // How many times the transform-epoch gate alone proved the scene
            // unchanged and short-circuited the memcmp. This is the counter that
            // says whether the fast path is earning its keep: zero means every
            // frame is still paying the memcmp.
            uint32_t EpochShortCircuits = 0;
            // The epoch sampled from the Scene on the most recent frame.
            uint32_t SceneTransformEpoch = 0;
        };
        static void ResetStats();
        static Statistics GetStats();
        // Cumulative mid-frame batch-overflow count, for the editor stats panel
        // and any tooling that wants to know whether the tessellation batch is
        // sized for the current scene.
        static uint32_t GetMidFrameFlushCount();
        // The raster path is always live; this gates the Nova path tracer only.
        static bool IsRayTracingEnabled();
        static void SetRayTracingEnabled(bool enabled);
        static QualityPreset GetQualityPreset();
        static void SetQualityPreset(QualityPreset preset);
        static SamplePattern GetSamplePattern();
        static void SetSamplePattern(SamplePattern pattern);
        static ReconstructionFilter GetReconstructionFilter();
        static void SetReconstructionFilter(ReconstructionFilter filter);
        static bool IsRayTracingAccumulate();
        static void SetRayTracingAccumulate(bool enabled);
        static uint32_t GetRayTraceTargetID();
        // Opaque output handle: DX11 returns an SRV, DX12 returns an ImGui GPU
        // descriptor handle, and Vulkan returns the raw nvrhi::ITexture* from
        // VulkanNova::Output(). The receiving backend must interpret it; Vulkan
        // does not return an ImGui descriptor here.
        static void* GetRayTraceTargetNativeSRV();
        // GPU -> CPU capture of the active Nova output image, as tightly packed
        // top-down 8-bit RGBA. This is the DX12 counterpart of reading the
        // default framebuffer: the Nova pass writes to its own UAV image rather
        // than to the window, so the only thing to read is that image, and the
        // only backend that can read it back without stalling is the one that
        // owns it. Returns false when the active backend has no readback (the
        // DX11 and GL Nova instances, and NVRHI raster) or when the image has
        // not been rendered yet.
        static bool ReadbackRayTraceTarget(std::vector<unsigned char> &rgba, uint32_t &width, uint32_t &height);
        static void ResizeRayTraceTarget(uint32_t width, uint32_t height);
        static float GetRenderScale();
        static void SetRenderScale(float scale);

        // Dynamic resolution: holds a target framerate (default 30fps) by
        // moving RenderScale between [DynamicMinScale, preset scale] based on
        // measured GPU time. This is what lets Ultra stay above 30fps on
        // older hardware (RTX 20-series): the preset stays at full quality,
        // the tracer transparently drops resolution when the GPU can't keep
        // up, and the composite upscale + shader perf tiers hide the change.
        // Independent of IsRayTracingEnabled(): the scaler moves RenderScale for
        // the raster path too, so a machine with ray tracing off still benefits.
        static bool IsDynamicResolutionEnabled();
        static void SetDynamicResolutionEnabled(bool enabled);
        static float GetTargetFPS();
        static void SetTargetFPS(float fps);
        static float GetDynamicMinScale();
        static void SetDynamicMinScale(float scale);
        // Smoothed measured framerate / GPU frame cost. 0 until measured.
        // Both return 0 until the first sample lands. Callers must not treat 0 as
        // a real measurement -- it is "not measured yet", not "infinitely fast".
        static float GetCurrentFPS();
        static float GetGPUFrameTimeMs();
        // True while the scaler is holding RenderScale below the preset max.
        static bool IsDynamicResolutionActive();
        // Classified from the GL renderer string at Init
        // ("RTX 20-series", "RTX 30-series or newer", ...). Empty if unknown.
        static const char *GetGPUTierName();
        // Feeds the application's real frame time (ms) into the scaler.
        // Call once per frame (EditorLayer::OnUpdate does). The scaler uses
        // the slowest available signal (GPU timer vs app frame), so it still
        // converges when the bottleneck sits outside the compute chain.
        // Fed from the app's real frame time by EditorLayer::OnUpdate. Using the
        // slowest available signal (GPU timer vs app frame) means the scaler still
        // converges when the bottleneck is outside the compute chain entirely.
        static void NotifyFrameTime(double ms);

        // Ray Tracing Settings
        // D3: BOTH OF THESE ARE CURRENTLY NO-OPS ON RENDERING. The value is
        // stored and uploaded as u_SamplesPerPixel, which no shader pass reads
        // (there is no per-pixel sample loop). The editor slider that drove this
        // has been removed. Retained because this is a public API with a Python
        // binding; real multi-sampling arrives with Phase 4 temporal work.
        // DEFECT (documented upstream in the block comment above, kept here
        // because the declaration pair is the only place a caller can see it):
        // the value is stored and uploaded as u_SamplesPerPixel, but no shader
        // pass reads it -- there is no per-pixel sample loop. Not fixed here
        // because it is public API with a Python binding; real multi-sampling
        // arrives with Phase 4 temporal work.
        static uint32_t GetSamplesPerPixel();
        static void SetSamplesPerPixel(uint32_t samples);
        // Movement threshold, the second half of the same no-op pair. Its consumer
        // is not named in this header, and unlike the sample count there is no
        // accompanying note saying whether anything reads it -- appears unused.
        static float GetMovementThreshold();
        static void SetMovementThreshold(float threshold);

        // Sky Settings
        // Sky is a single global gradient (two colours), not per-scene -- a second
        // sky would need a second pass and a second uniform block.
        static glm::vec3 GetSkyBottomColor();
        static void SetSkyBottomColor(const glm::vec3 &color);
        static glm::vec3 GetSkyTopColor();
        static void SetSkyTopColor(const glm::vec3 &color);

        // Volumetric atmosphere (fog + clouds). Volumes are submitted per-frame
        // by the Scene from VolumetricFog/VolumetricClouds components; the
        // entity Transform defines the volume box (center = Translation,
        // full size = Scale). Raymarched in both Nova and raster paths.
        // Fixed capacity, checked at submission: volumes beyond the 4th are
        // dropped rather than growing the uniform block. The same fixed-array
        // approach is used for clouds (2), analytic lights (8) and water (8).
        static constexpr int MaxFogVolumes = 4;
        static constexpr int MaxCloudVolumes = 2;
        // Per-frame submission, not a retained registration: the Scene re-sends
        // its VolumetricFog/VolumetricClouds components every frame and the
        // renderer keeps only the current set. The entity Transform supplies the
        // volume box (Translation = centre, Scale = full size).
        static void SubmitFogVolume(const glm::mat4 &transform, const VolumetricFogComponent &fog);
        static void SubmitCloudVolume(const glm::mat4 &transform, const VolumetricCloudsComponent &clouds);
        static bool IsVolumetricFogEnabled();
        static void SetVolumetricFogEnabled(bool enabled);
        static bool IsVolumetricCloudsEnabled();
        static void SetVolumetricCloudsEnabled(bool enabled);
        // One sun for the whole frame, shared by fog, clouds and god rays. Any
        // second directional light is a separate analytic light and does not
        // affect the atmospheric passes.
        static glm::vec3 GetSunDirection();
        static void SetSunDirection(const glm::vec3 &direction);
        // Global step size for both volume raymarches; scales the quality/cost of
        // the march rather than the volume extents.
        static float GetVolumetricStepScale();
        static void SetVolumetricStepScale(float scale);

        // Global atmospheric fog: distance + height haze applied to all pixels,
        // independent of box fog volumes. Gives distant terrain a natural fade.
        // Global fog is distance+height haze applied to every pixel, deliberately
        // separate from the per-volume fog above: a volume is a box in the world,
        // this is a screen-wide effect. The split is why terrain can fade without
        // anyone authoring a volume for it.
        static bool IsGlobalFogEnabled();
        static void SetGlobalFogEnabled(bool enabled);
        static float GetGlobalFogDensity();
        static void SetGlobalFogDensity(float density);
        static float GetGlobalFogHeightFalloff();
        static void SetGlobalFogHeightFalloff(float falloff);
        static glm::vec3 GetGlobalFogColor();
        static void SetGlobalFogColor(const glm::vec3 &color);
        static float GetGlobalFogBaseHeight();
        static void SetGlobalFogBaseHeight(float height);
        static float GetGlobalFogDistanceAttenuation();
        static void SetGlobalFogDistanceAttenuation(float atten);

        // Screen-space god rays: radial blur from the sun's screen position.
        // Computes light shafts from any directional light (the primary sun).
        // Radial blur from the sun's screen position, so it is a post-process and
        // runs after the scene is resolved into the target it samples.
        static bool IsGodRaysEnabled();
        static void SetGodRaysEnabled(bool enabled);
        static float GetGodRayIntensity();
        static void SetGodRayIntensity(float intensity);
        static float GetGodRayDecay();
        static void SetGodRayDecay(float decay);
        // Unlike u_SamplesPerPixel, this count is a real loop length in the blur
        // pass, so it has an honest cost/quality trade-off.
        static int GetGodRaySamples();
        static void SetGodRaySamples(int samples);
        static float GetGodRayDensity();
        static void SetGodRayDensity(float density);
        static glm::vec3 GetGodRayColor();
        static void SetGodRayColor(const glm::vec3 &color);

        // Camera / Post-Processing Effects (all backends: OpenGL, DX11, DX12, Vulkan)
        // Post effects apply across all four backends (GL/DX11/DX12/Vulkan);
        // there is no per-backend opt-in, so a backend that lacks one of these
        // passes has to accept the no-op rather than fail to compile.
        static bool IsBloomEnabled();
        static void SetBloomEnabled(bool enabled);
        static bool GetBloomEnabled();
        static float GetBloomIntensity();
        static void SetBloomIntensity(float intensity);
        static float GetBloomThreshold();
        static void SetBloomThreshold(float threshold);
        static bool IsBilateralBlurEnabled();
        static void SetBilateralBlurEnabled(bool enabled);
        static int GetBilateralBlurPasses();
        static void SetBilateralBlurPasses(int passes);
        // Experimental compute effects remain disabled by default. Toggling
        // either resets temporal accumulation so history is not mixed across modes.
        static bool IsSVGFEnabled();
        static void SetSVGFEnabled(bool enabled);
        // Clean-room reconstruction stack (WLSR / WLAA / WLRR / WLFG) master
        // switch. Off by default; see Renderer3D.cpp's ReconstructionEnabled.
        static bool IsReconstructionEnabled();
        static void SetReconstructionEnabled(bool enabled);
        // WLSR sharpen stage (RCAS, pass 15). Off by default, like the mask: a
        // sharpen stage that runs unconditionally is a quality change nobody asked
        // for, and it must be addable without disturbing the existing pipeline.
        static bool IsRCASEnabled();
        static void SetRCASEnabled(bool enabled);
        // The sharpen strength handed to the shader. The plan's "proportional to
        // the reactive mask" is implemented in the shader, which scales this by
        // (1 - mask); this is the ceiling the shader can reach.
        static void SetRCASharpness(float strength);
        static float GetRCASharpness();
        static bool IsReSTIREnabled();
        static void SetReSTIREnabled(bool enabled);

        static bool IsVignetteEnabled();
        static void SetVignetteEnabled(bool enabled);
        static bool GetVignetteEnabled();
        static float GetVignetteIntensity();
        static void SetVignetteIntensity(float intensity);
        static float GetVignetteRadius();
        static void SetVignetteRadius(float radius);
        static float GetVignetteSoftness();
        static void SetVignetteSoftness(float softness);

        static bool IsChromaticAberrationEnabled();
        static void SetChromaticAberrationEnabled(bool enabled);
        static bool GetChromaticAberrationEnabled();
        static float GetChromaticAberrationIntensity();
        static void SetChromaticAberrationIntensity(float intensity);

        static bool IsColorGradingEnabled();
        static void SetColorGradingEnabled(bool enabled);
        static bool GetColorGradingEnabled();
        static float GetColorTemperature();
        static void SetColorTemperature(float temperature);
        static float GetColorTint();
        static void SetColorTint(float tint);
        static float GetColorGamma();
        static void SetColorGamma(float gamma);

        static bool IsFilmGrainEnabled();
        static void SetFilmGrainEnabled(bool enabled);
        static bool GetFilmGrainEnabled();
        static float GetFilmGrainIntensity();
        static void SetFilmGrainIntensity(float intensity);

        // Analytic lights (directional/spot/point/area components). Submitted
        // per-frame by the Scene. Evaluated with shadows in the Nova path
        // alongside emissive-geometry lights.
        //
        // Two caps, deliberately separate:
        //
        //   MaxAnalyticLights  -- how many lights a SCENE may hold. This is a
        //                         collection cap only; it does not make the
        //                         shader slower. A city can submit a few
        //                         thousand street lights without any of them
        //                         being evaluated unless selected.
        //
        //   MaxRenderedLights -- how many lights the SHADER evaluates per
        //                         pixel. The shader's light arrays are fixed at
        //                         this size, so it is also the upload cap. Each
        //                         frame the nearest MaxRenderedLights to the
        //                         camera are selected from the collected list,
        //                         so a 10k-light city only ever shades the
        //                         lights that are actually near the viewer.
        //
        // Admission to the rendered set is DISTANCE-based (nearest wins), not
        // type-quota-based: with thousands of lights a type quota is both
        // meaningless (every type is over-represented) and harmful (it would
        // force a distant spot light into the set over a near point light).
        static constexpr int MaxAnalyticLights = 12000;
        static constexpr int MaxRenderedLights = 32;
        static void SubmitDirectionalLight(const glm::mat4 &transform, const DirectionalLightComponent &light);
        static void SubmitPointLight(const glm::mat4 &transform, const PointLightComponent &light);
        static void SubmitSpotLight(const glm::mat4 &transform, const SpotLightComponent &light);
        static void SubmitAreaLight(const glm::mat4 &transform, const AreaLightComponent &light);

        // Water surfaces (WaterComponent). Submitted per-frame by the Scene;
        // the entity Transform defines the surface: Translation.y is the still
        // water level, Scale.x/z are the surface extents (1x1 XZ plane at
        // local y=0). The raster path draws a CPU-displaced grid using the
        // component's wave field (matches gameplay sampling exactly); the
        // Nova path-tracer gets a thin-box instance with water material
        // (flat surface, wave displacement is raster-only).
        static constexpr int MaxWaterSurfaces = 8;
        // The two backends do not agree on what a water surface IS: raster draws
        // a CPU-displaced grid from the component's wave field (so gameplay
        // sampling and the visible surface match exactly), while the path tracer
        // gets a thin box with a flat surface. Wave displacement is raster-only.
        static void DrawWaterPlane(const glm::mat4 &transform, const WaterComponent &water, int entityID = -1);

        // DX12 Nova-only terrain submission. The Scene calls this once per
        // loaded streamed chunk; other backends and raster-only frames ignore it.
        static void DrawTerrainChunk(const Terrain::TerrainChunkMesh &mesh);

        // Neural Rendering (OpenGL GLSL MLP: neural texture detail + neural indirect lighting)
        static bool IsNeuralRenderingEnabled();
        static void SetNeuralRenderingEnabled(bool enabled);
        static float GetNeuralTextureStrength();
        static void SetNeuralTextureStrength(float strength);
        static float GetNeuralLightStrength();
        static void SetNeuralLightStrength(float strength);
        static float GetNeuralMaterialStrength();
        static void SetNeuralMaterialStrength(float strength);

        // Trained end-of-frame neural stage (docs/NEURAL_RENDERING.md).
        // Distinct from the toggle above: that one is the in-estimator MLP that
        // shapes shading, this one is the CPU-trained MLP that runs as compute
        // passes 17/18/19 at the end of the frame. Off by default, because it is
        // a change to the final image. WL_NEURAL_STAGE overrides this at runtime
        // for verification.
        static bool IsNeuralStageEnabled();
        static void SetNeuralStageEnabled(bool enabled);
        // False when assets/neural/neural_weights.bin is missing or failed
        // validation, in which case the stage is skipped whatever the toggle says.
        static bool IsNeuralStageAvailable();

        // Halton low-discrepancy sequence, the classic TAA jitter source:
        // stratified sample positions that are far more even than random ones.
        // index is the sequence number, base the radical base (2 or 3 typically).
        static float Halton(int index, int base);
        static glm::vec2 GetCurrentJitter(int frameIndex, glm::vec2 viewportSize);

        // Discovers/normalises derived maps for a texture path on disk; the two
        // float params bias the generated normal strength and roughness. Kept on
        // the renderer rather than a loader because it needs the backend's
        // texture creation path to make the results usable immediately.
        static void GenerateMaterialMaps(const std::string &texturePath, float normalStrength, float roughBias);

        /// Runs the offline material bake and reads back what it actually wrote into
        /// GeneratedTextureID, so "the maps were baked" can be told apart from "the bake
        /// silently wrote nothing".
        ///
        /// This exists because the GL bake had three independent defects, all silent:
        /// u_PassID was never set (so it ran an arbitrary pass), it bound image unit 8
        /// which does not exist (GL_MAX_IMAGE_UNITS is 8, so valid units are 0..7), and
        /// the dispatch used truncating division. With the unit-8 bug the bake wrote
        /// nothing at all -- and neither the renderer, the shader compiler, the link
        /// step nor any log line said so. Only reading the texture back can.
        ///
        /// Returns false when the GL path is unavailable. outNonZero is the fraction of
        /// texels with any of .xy/.z set (.xy is the packed normal, .z the packed AO).
        static bool ProbeMaterialBake(const std::string &texturePath, float normalStrength,
                                     float roughBias, float *outMean, float *outMax,
                                     float *outNonZero);

    private:
        // Private: the flush+clear pair the frame loop uses, kept out of the public
        // surface so callers cannot leave the counters mid-frame out of step with
        // the GPU.
        static void FlushAndReset();

        // Rasterises a ProceduralMeshes primitive into the shared dynamic batch
        // and, when ray tracing is on, submits its Nova analytic instance.
        // A private member (not a free function) so it can reach FlushAndReset.
        // shape is the PrimitiveShape discriminator handed to the Nova instance;
        // localMin/localMax are the mesh bounds for that analytic primitive, and
        // shapeParam/shapeParam2 carry the shape-specific dimensions (e.g. the
        // two radii of a torus) that the instance also needs. It is a member
        // rather than a free function purely so it can reach FlushAndReset.
        static void DrawProceduralPrimitive(const glm::mat4 &transform, const ProceduralMesh &mesh,
                                           const glm::vec4 &color, MaterialComponent &material,
                                           int shape, const glm::vec3 &localMin, const glm::vec3 &localMax,
                                           float shapeParam, float shapeParam2, int entityID);
    };

}