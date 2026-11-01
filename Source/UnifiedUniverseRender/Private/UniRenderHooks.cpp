// UniRenderHooks.cpp
// EVERY engine render hook of the plugin lives in this file (D-006), so an engine upgrade that
// moves a signature breaks one file.
//
// Placement (D-124, prototype): the backdrop is drawn in the post-processing callback that runs
// after motion blur. That is after TSR (sub-pixel stars are not smeared by temporal upscaling)
// and before bloom and tonemapping (stars still get bloom and auto-exposure), which is where the
// design puts the composite. The prototype merges "backdrop render" and "composite" into this
// one additive draw, testing scene depth itself; a separate backdrop target arrives with
// volumetrics. CHECK the pass order on your engine version (UE-5): if MotionBlur's callback runs
// before TSR there, stars will shimmer under TSR.
//
// Engine-version notes (D-106), all guarded below:
//   - UE 5.5+ passes the view to SubscribeToPostProcessingPass; earlier versions do not.
//   - UE 5.3+ hands post-process inputs as FScreenPassTextureSlice.
//   - FViewInfo (for the internal view rect and PreExposure) needs the Renderer's private
//     include path (UnifiedUniverseRender.Build.cs adds it).
//   - The draw goes into a copy of scene colour (or OverrideOutput), never into the input
//     (UE review R1), and uses the unjittered projection (W2, in UniRenderer.cpp).

#include "UniRenderHooks.h"

#include "UnifiedUniverseRender.h"
#include "UniRenderer.h"

#include "Misc/EngineVersionComparison.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "SceneRendering.h"          // FViewInfo (Renderer/Private)
#include "SceneTexturesConfig.h"
#include "SceneViewExtension.h"
#include "ScreenPass.h"

#include <atomic>

namespace
{
	std::atomic<bool> GUniActive{ false };

	class FUniSceneViewExtension : public FSceneViewExtensionBase
	{
	public:
		explicit FUniSceneViewExtension(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

		virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
		virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
		virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

#if UE_VERSION_NEWER_THAN(5, 4, 99)
		virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override
#else
		virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, FAfterPassCallbackDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override
#endif
		{
			if (Pass == EPostProcessingPass::MotionBlur)
			{
				InOutPassCallbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FUniSceneViewExtension::AfterMotionBlur_RenderThread));
			}
		}

	protected:
		virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
		{
			return GUniActive.load();
		}

	private:
		FScreenPassTexture AfterMotionBlur_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
		{
#if UE_VERSION_NEWER_THAN(5, 2, 99)
			FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
#else
			FScreenPassTexture SceneColor = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);
#endif
			const FViewInfo& ViewInfo = static_cast<const FViewInfo&>(View);

			// Only game views get the backdrop (W6): scene captures, reflection captures and
			// editor-only views pass through. OverrideOutput must still be honoured when set.
			if (View.bIsSceneCapture || View.bIsReflectionCapture || !View.bIsGameView)
			{
				if (Inputs.OverrideOutput.IsValid())
				{
					AddDrawTexturePass(GraphBuilder, ViewInfo, SceneColor, Inputs.OverrideOutput);
					return Inputs.OverrideOutput;
				}
				return SceneColor;
			}

			// R1: the post-process input is not guaranteed render-targetable (and must not be
			// written in place), so the sprites draw into a copy: the override output when the
			// chain hands one, otherwise a new target shaped like scene colour.
			FScreenPassRenderTarget Output = Inputs.OverrideOutput.IsValid()
				? Inputs.OverrideOutput
				: FScreenPassRenderTarget::CreateFromInput(GraphBuilder, SceneColor, ERenderTargetLoadAction::ENoAction, TEXT("UniSceneColor"));
			AddDrawTexturePass(GraphBuilder, ViewInfo, SceneColor, Output);

			FRDGTextureRef SceneDepth = nullptr;
			if (Inputs.SceneTextures.SceneTextures)
			{
				SceneDepth = Inputs.SceneTextures.SceneTextures->GetParameters()->SceneDepthTexture;
			}
			const FIntRect DepthRect = ViewInfo.ViewRect;   // internal (pre-upscale) rect
			// W3: scene colour is pre-exposed at this point; the sprite radiance must be too.
			const float PreExposure = ViewInfo.PreExposure > 0.0f ? ViewInfo.PreExposure : 1.0f;
			FUniRenderer::Get().RenderBackdrop_RenderThread(GraphBuilder, View, Output, SceneDepth, DepthRect, PreExposure);
			return MoveTemp(Output);
		}
	};

	TSharedPtr<FUniSceneViewExtension, ESPMode::ThreadSafe> GUniExtension;
}

namespace UniRenderHooks
{
	void EnsureRegistered()
	{
		check(IsInGameThread());
		if (!GUniExtension.IsValid())
		{
			GUniExtension = FSceneViewExtensions::NewExtension<FUniSceneViewExtension>();
			UE_LOG(LogUniRender, Log, TEXT("UnifiedUniverse: scene view extension registered (backdrop after MotionBlur)."));
		}
	}

	void SetActive(bool bActive)
	{
		GUniActive.store(bActive);
	}
}
