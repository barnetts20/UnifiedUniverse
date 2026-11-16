// test_farcache.cpp
// D-228: the far cache (PARADIGMS #2; UniCore/UniFarCache.h, UniFarRef.h).
//   far_controller  the switching and fill rules on synthetic frames: settle, warm (no frame marches
//                   more than a face's share of rows), switch on with the history converted, faces
//                   in view refreshed first, out-of-view ones only when far off, off at speed, a jump,
//                   a lost map while on, a scene change.
//   far_split       at a still camera, near (stopped at R) + T_near x the far map = the whole march:
//                   light and the slices the sprites read.
//   far_flight      the solar camera drifting, then travelling, then drifting again, the renderer's
//                   rules end to end against a whole march every frame: the steps saved while on,
//                   the light at the switches (no jump: the history is converted), the error.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../src/UniFarRef.h"
#include "../scenes/UniScenes.h"
#include "UniFarCache.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

UNI_TEST(volume, far_controller)
{
	FFarCacheSettings S;
	S.StartCm = 2.0 * Units::Kpc;
	S.FaceSize = 64;
	S.OnFrames = 10;
	S.FillFrames = 4;
	const double Unit = S.TexelRad() * S.StartCm;
	FFarCacheControl C;
	std::string Detail;
	bool bOk = true;
	auto Fail = [&](const std::string& Why) { if (bOk) { Detail += Why; } bOk = false; };
	int MaxRows = 0, Frame = 0;
	auto Run = [&](double Motion, uint32_t InView, bool bStep = true, bool bScene = false, bool bReset = false, bool bEligible = true)
	{
		FFarCacheFrame Fr;
		Fr.bEligible = bEligible;
		Fr.bStepValid = bStep;
		Fr.Step[0] = Motion * Unit;
		Fr.FacesInView = InView;
		Fr.bSceneChanged = bScene;
		Fr.bReset = bReset;
		const FFarCacheDecision D = C.Update(S, Fr);
		if (D.FillFace >= 0) { MaxRows = std::max(MaxRows, D.RowEnd - D.RowBegin); }
		++Frame;
		return D;
	};
	const uint32_t View = (1u << 0) | (1u << 2);
	// 1. Settle and warm: fills start at once (in-view faces first), on once all six are marched and
	//    OnFrames have passed, with the history made near-only (-1).
	std::vector<int> Order;
	int OnAt = -1, SignOn = 0;
	for (int I = 0; I < 40 && OnAt < 0; ++I)
	{
		const FFarCacheDecision D = Run(0.005, View);
		if (D.bSnapshot) { Order.push_back(D.FillFace); }
		if (D.bUse) { OnAt = I; SignOn = D.HistSign; }
	}
	if (Order.size() != 6 || !((Order[0] == 0 && Order[1] == 2) || (Order[0] == 2 && Order[1] == 0))) { Fail("fill order not in-view first; "); }
	if (OnAt != 23 || SignOn != -1) { Fail(Uni::Fmt("on at frame %d (expected 23, six faces x 4 frames) sign %d; ", OnAt, SignOn)); }
	Detail += Uni::Fmt("on after %d frames (six faces x %d), history sign %d; ", OnAt + 1, S.FillFrames, SignOn);
	// 2. Drifting at 0.01 texel a frame: in-view faces re-marched past 0.3 texels, the others not.
	int InViewFills = 0, OtherFills = 0;
	for (int I = 0; I < 80; ++I)
	{
		const FFarCacheDecision D = Run(0.01, View);
		if (!D.bUse) { Fail("dropped out while drifting; "); }
		if (D.bSnapshot) { ((View >> D.FillFace) & 1u) ? ++InViewFills : ++OtherFills; }
	}
	if (InViewFills < 2 || OtherFills != 0) { Fail(Uni::Fmt("drift: %d in-view fills, %d others (expected >= 2, 0); ", InViewFills, OtherFills)); }
	Detail += Uni::Fmt("80 frames at 0.01 texel/frame: %d in-view refills, %d out of view; ", InViewFills, OtherFills);
	// 3. Off above OffMotion at once (history made whole: +1); between On and Off it stays off.
	{
		const FFarCacheDecision D = Run(0.05, View);
		if (D.bUse || D.HistSign != 1) { Fail("not off at 0.05 texel/frame with sign +1; "); }
		for (int I = 0; I < 20; ++I) { if (Run(0.03, View).bUse) { Fail("on between the thresholds; "); break; } }
	}
	// 4. Back to drifting after travelling ~0.65 texels: on again only once the in-view faces are
	//    re-marched (fresh), never on stale ones.
	{
		int Again = -1;
		for (int I = 0; I < 40 && Again < 0; ++I)
		{
			const FFarCacheDecision D = Run(0.005, View);
			if (D.bUse)
			{
				Again = I;
				if (D.HistSign != -1) { Fail("re-on without sign -1; "); }
				for (int F = 0; F < 6; ++F) { if (((View >> F) & 1u) && FFarCacheControl::Len(C.Moved[F]) > S.DueTexels * Unit) { Fail("re-on with a stale in-view face; "); } }
			}
		}
		if (Again < S.OnFrames - 1) { Fail("re-on before OnFrames; "); }
		Detail += Uni::Fmt("on again %d frames after slowing; ", Again + 1);
	}
	// 5. A scene change while on: stays on, every face re-marched, in-view first.
	{
		std::vector<int> Refill;
		for (int I = 0; I < 30; ++I)
		{
			const FFarCacheDecision D = Run(0.0, View, true, I == 0);
			if (!D.bUse) { Fail("off on a scene change; "); break; }
			if (D.bSnapshot) { Refill.push_back(D.FillFace); }
		}
		if (Refill.size() < 6 || !((View >> Refill[0]) & 1u) || !((View >> Refill[1]) & 1u)) { Fail("scene change refill not in view first; "); }
	}
	// 6. A jump: everything invalid, off, no conversion (the renderer drops the history on a jump).
	{
		const FFarCacheDecision D = Run(0.0, View, false);
		if (D.bUse || D.HistSign != 0 || D.bDropHistory) { Fail("jump: expected off, no sign, no drop; "); }
		for (int F = 0; F < 6; ++F) { if (C.bValid[F]) { Fail("jump left a face valid; "); break; } }
	}
	// 7. The map lost while on (re-created): the history is dropped, not converted.
	{
		int On = -1;
		for (int I = 0; I < 60 && On < 0; ++I) { if (Run(0.0, View).bUse) { On = I; } }
		const FFarCacheDecision D = Run(0.0, View, true, false, true);
		if (On < 0 || D.bUse || !D.bDropHistory || D.HistSign != 0) { Fail("lost map: expected off with the history dropped; "); }
	}
	// 8. Cells loading (their glow is per frame), then leaving the galaxy: off.
	{
		for (int I = 0; I < 60; ++I) { Run(0.0, View); }
		FFarCacheFrame Fr;
		Fr.bEligible = true; Fr.bStepValid = true; Fr.FacesInView = View; Fr.bLoading = true;
		const FFarCacheDecision Dl = C.Update(S, Fr);
		if (Dl.bUse || Dl.Reason != EFarReason::Loading || Dl.FillFace >= 0) { Fail("not off while cells load; "); }
		for (int I = 0; I < 60; ++I) { Run(0.0, View); }
		const FFarCacheDecision D = Run(0.0, View, true, false, false, false);
		if (D.bUse || D.Reason != EFarReason::Outside) { Fail("not off outside; "); }
	}
	if (MaxRows > S.RowsPerFrame()) { Fail("a frame marched more than a face's share of rows; "); }
	Detail += Uni::Fmt("at most %d rows a frame (a %d face over %d frames)", MaxRows, S.FaceSize, S.FillFrames);
	Uni::Check(1, "T1.vol.far_controller", "D-228 the far cache's rules: warms in-view faces first with at most a face's share of rows a frame, on once all six are marched (history made near-only) and the in-view faces fresh, refreshes in-view faces as the camera drifts and not out-of-view ones, off at speed (history made whole), stays off between the thresholds, invalid on a jump, drops the history if the map is lost while on, refills on a scene change in view first, off while cells load and outside a galaxy")
		.Metric("on_after_frames", double(OnAt + 1)).Metric("max_rows_frame", double(MaxRows)).Pass(bOk, Detail);
}

namespace
{
	/** Light-weighted relative error of A against B (luminance x pixel solid angle). */
	double LightError(const std::vector<H::float4>& A, const std::vector<H::float4>& B, const FCamera& Cm, double* OutSumA = nullptr, double* OutSumB = nullptr)
	{
		double Sa = 0, Sb = 0, Se = 0;
		for (size_t I = 0; I < B.size(); ++I)
		{
			const double Om = PixelSolidAngle(Cm, Cm.Width, Cm.Height, double(I % Cm.Width) + 0.5, double(I / Cm.Width) + 0.5);
			const double La = double(H::Uni_Luminance(H::float3(A[I].x, A[I].y, A[I].z))) * Om;
			const double Lb = double(H::Uni_Luminance(H::float3(B[I].x, B[I].y, B[I].z))) * Om;
			Sa += La; Sb += Lb; Se += std::abs(La - Lb);
		}
		if (OutSumA) { *OutSumA = Sa; }
		if (OutSumB) { *OutSumB = Sb; }
		return Se / std::max(Sb, 1e-300);
	}
}

UNI_TEST(volume, far_split)
{
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::SolarCamera();
	P.Update(Cam, FVec3f{});
	const int W = 160, Ht = 90;
	FCamera Cm; Cm.Coord = Cam; Cm.Width = W; Cm.Height = Ht; Cm.DeltaMag = 0.0;
	Cm.LookAlong(FV3{ -1, -0.12, -0.02 });
	const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
	std::string Detail;
	double WorstL = 0.0, WorstT = 0.0, WorstS = 0.0;
	for (double Rk : { 1.0, 2.0 })
	{
		const double R = Rk * Units::Kpc;
		// The whole march, and the near part, every pixel (no slicing).
		FTemporalVolume Whole, Near;
		Whole.Step(Sc, Cm, FV3{ 0, 0, 0 }, false, false, 2);
		FFarMap Map;
		Map.Reset(W);   // 90 degrees: a face spans the view's width
		const FVolumeScene Far = FarSplit(Sc, R, true);
		for (int F = 0; F < 6; ++F) { Map.MarchRows(Far, float(R), F, 0, Map.N); }
		Map.LayerBits = 0x3Fu;   // the spare layers were filled: show them
		Near.FarSlice = [&](const FV3& Dd, u32 K) { return Map.SliceAt(Dd, K); };
		Near.Step(FarSplit(Sc, R, false), Cm, FV3{ 0, 0, 0 }, false, false, 2);
		std::vector<H::float4> Out = Near.Vol;
		double Tsum = 0.0, Ssum = 0.0;
		for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
		{
			const size_t I = size_t(Y) * W + X;
			Out[I] = H::Uni_FarComposite(Out[I], Map.Sample(PixelDir(Cm, W, Ht, X + 0.5, Y + 0.5)));
			Tsum += std::abs(double(Out[I].w) - double(Whole.Vol[I].w));
			for (u32 K = 0; K < H::UNI_VOL_SLICES; ++K) { Ssum += std::abs(double(Near.Slices[I * H::UNI_VOL_SLICES + K]) - double(Whole.Slices[I * H::UNI_VOL_SLICES + K])); }
		}
		double Sa = 0, Sb = 0;
		const double E = LightError(Out, Whole.Vol, Cm, &Sa, &Sb);
		const double Tm = Tsum / double(W * Ht), Sm = Ssum / double(W * Ht * H::UNI_VOL_SLICES);
		WorstL = std::max(WorstL, E); WorstT = std::max(WorstT, Tm); WorstS = std::max(WorstS, Sm);
		Detail += Uni::Fmt("%sR %.0f kpc: light error %.2f%% (total %.3f%%), transmittance %.4f, slices %.4f mean abs", Detail.empty() ? "" : "; ", Rk, 100.0 * E, 100.0 * (Sa / Sb - 1.0), Tm, Sm);
	}
	Uni::Check(2, "T2.vol.far_split", "D-228 at a still camera the near part (stopped at R) composited with the far map (from R, a face per view width) is the whole march: light within 1% (the map's bilinear resampling), transmittance and the sprites' slices within 0.01 mean abs, at R = 1 and 2 kpc")
		.Metric("light_error", WorstL).Metric("transmittance_err", WorstT).Metric("slice_err", WorstS)
		.Pass(WorstL < 0.01 && WorstT < 0.01 && WorstS < 0.01, Detail);
}

UNI_TEST(volume, far_flight)
{
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const int W = 160, Ht = 90;
	FFarSim Sim;
	Sim.S.StartCm = 2.0 * Units::Kpc;
	Sim.S.FaceSize = W;
	Sim.S.OnFrames = 10;
	Sim.S.FillFrames = 4;
	FTemporalVolume Today;
	const double Unit = Sim.S.TexelRad() * Sim.S.StartCm;
	// Drift (0.005 texel/frame) 60 frames, travel (0.1 texel/frame) 12, drift again 50.
	struct FLeg { int Frames; double Motion; };
	const FLeg Legs[] = { { 60, 0.005 }, { 12, 0.1 }, { 50, 0.005 } };
	FCoord Cam = UniScenes::SolarCamera();
	FV3 Fwd = Norm(FV3{ -1, -0.12, -0.02 });
	const FV3 Dir = Norm(FV3{ -1.0, 0.13, 0.0 });
	const double Deg = 3.14159265358979 / 180.0;
	double SimOn = 0, TodayOn = 0, ErrOn = 0, ErrToday = 0, WorstJump = 0, WorstSwitchJump = 0;
	int OnFrames = 0, Switches = 0, F = 0;
	double PrevSum = -1.0;
	bool bPrevUse = false;
	std::string Detail;
	for (const FLeg& Lg : Legs)
	{
		for (int I = 0; I < Lg.Frames; ++I, ++F)
		{
			const FV3 Step = F > 0 ? Dir * (Lg.Motion * Unit) : FV3{ 0, 0, 0 };
			if (F > 0)
			{
				FVec3f Fr{};
				AddDisplacement(Cam, Fr, FVec3d{ Step.X, Step.Y, Step.Z });
				const FV3 K = Norm(FV3{ 0.2, 0, 1 });
				const double A = 0.01 * Deg, Cs = std::cos(A), Sn = std::sin(A);
				Fwd = Fwd * Cs + Cross(K, Fwd) * Sn + K * (Dot(K, Fwd) * (1.0 - Cs));
			}
			P.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = W; Cm.Height = Ht; Cm.DeltaMag = 0.0;
			Cm.LookAlong(Fwd);
			const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
			{
				// The renderer's interleave rule (D-208).
				const double ThetaPix = 2.0 * Cm.TanX() / double(W);
				const double StepLen = std::sqrt(Dot(Step, Step));
				const double Move = F > 0 && StepLen > 0.0 ? (Sc.NearCm > 0.0 ? StepLen / Sc.NearCm : 1.0e9) : 0.0;
				const u32 Il = H::Uni_TemporalInterleave(float(((F > 0 ? 0.01 * Deg : 0.0) + Move) / ThetaPix), 4u);
				Sim.Tv.Interleave = Today.Interleave = Il;
			}
			const size_t T0 = Today.Steps;
			Today.Step(Sc, Cm, Step, F > 0, true, 2);
			const double TodaySteps = double(Today.Steps - T0);
			Sim.Step(Sc, Cm, Step, F > 0, 2);
			// The reference: the whole march, every pixel.
			FTemporalVolume Ref;
			Ref.Step(Sc, Cm, FV3{ 0, 0, 0 }, false, false, 2);
			double Sa = 0, Sb = 0;
			const double E = LightError(Sim.Out, Ref.Vol, Cm, &Sa, &Sb);
			const double Et = LightError(Today.Vol, Ref.Vol, Cm);
			if (Sim.Last.bUse) { ++OnFrames; SimOn += double(Sim.StepsLast); TodayOn += TodaySteps; ErrOn = std::max(ErrOn, E); ErrToday = std::max(ErrToday, Et); }
			// The frame-to-frame change of the total light against the reference's own change: a
			// switch without the history converted jumps by the far part's share (tens of %).
			if (PrevSum > 0.0)
			{
				const double Jump = std::abs(Sa / Sb - PrevSum);
				if (Sim.Last.bUse != bPrevUse) { ++Switches; WorstSwitchJump = std::max(WorstSwitchJump, Jump); }
				else { WorstJump = std::max(WorstJump, Jump); }
			}
			PrevSum = Sa / Sb;
			bPrevUse = Sim.Last.bUse;
		}
	}
	const double Ratio = SimOn / std::max(TodayOn, 1.0);
	Detail = Uni::Fmt("on %d of %d frames, %d switches; steps while on %.2fx today's (march + fills); worst error while on %.2f%% (today %.2f%%) against the whole march; light jump at a switch %.2f%% (other frames %.2f%%); at most %d rows a frame; %d scene-key changes",
		OnFrames, F, Switches, Ratio, 100.0 * ErrOn, 100.0 * ErrToday, 100.0 * WorstSwitchJump, 100.0 * WorstJump, Sim.Map.MaxRowsInFrame, Sim.SceneChanges);
	const bool bOk = OnFrames > 40 && Switches >= 3 && Ratio < 0.7 && ErrOn < 0.01 && WorstSwitchJump < std::max(0.005, 2.0 * WorstJump) && Sim.Map.MaxRowsInFrame <= Sim.S.RowsPerFrame() && Sim.SceneChanges == 0;
	Uni::Check(2, "T2.vol.far_flight", "D-228 the solar camera drifting, travelling, drifting again with the renderer's far-cache rules: on while drifting at under 0.7x today's steps (march + face fills), within 1% of the whole march, switching off and on again without a light jump (history converted: under 0.5% or 2x the other frames' change), a face's share of rows a frame at most, the scene key (what the map's light depends on) steady throughout")
		.Metric("steps_ratio_on", Ratio).Metric("error_on", ErrOn).Metric("switch_jump", WorstSwitchJump).Metric("other_jump", WorstJump).Pass(bOk, Detail);
}

UNI_TEST(volume, far_split_nebulae)
{
	const UniVerify::FGalShapeOff ShapeOff;   // D-262 (B-041)
	// D-235 (owner, D-235 in the engine: "the nebula are not getting rendered consistently in all
	// clipmap bands... a nebula that is straddling bands and half of it is cut off"): the far map's
	// texels are coarser than the screen's pixels, and the nebulae blended to their mean at its
	// texel's footprint, so past the split they dimmed where the per-pixel march drew them. The
	// textured Milky Way with nebulae, from the solar camera along the disc and 1 kpc above it,
	// near (stopped at R) + the far map against the whole march, the map at a half and a quarter of
	// the view's resolution: the nebulae's added light (on - off) within 5%.
	FWebTexture Gt[3];
	bool bTex = true;
	for (int K = 0; K < 3; ++K) { std::string E; bTex = FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(K), Gt[K], &E) && bTex; BindGalaxyTexture(K, &Gt[K]); }
	struct FUnbind { ~FUnbind() { for (int K = 0; K < 3; ++K) { BindGalaxyTexture(K, nullptr); } } } Unbind;
	if (!bTex) { Uni::Check(2, "T2.vol.far_split_nebulae", "D-235 nebulae across the far split").Pass(false, "textures not loaded"); return; }
	FUniverseDef D = UniScenes::GalaxyToStar();
	UniScenes::UseGalaxyTextures(D, Gt[0].Size);
	FUniverseModel M(D);
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const int W = 160, Ht = 90;
	struct FShot { const char* Name; FVec3d Off; FV3 Look; };
	const FShot Shots[] = { { "along the disc", { 0, 0, 0 }, { -1, -0.12, -0.02 } }, { "1 kpc above, looking down", { 0, 0, 1.0 * Units::Kpc }, { -1, -0.1, -0.35 } } };
	std::string Detail;
	double Worst = 0.0;
	for (const FShot& Sh : Shots)
	{
		FCoord Cam = UniScenes::SolarCamera(); FVec3f Fr{};
		AddDisplacement(Cam, Fr, Sh.Off);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = W; Cm.Height = Ht; Cm.DeltaMag = 0.0;
		Cm.LookAlong(Sh.Look);
		FVolumeSettings Son; Son.NebulaGain = 1.0f; Son.NebulaReflect = 6.0f;
		const FVolumeScene On = VB.Build(P, Cam, FVec3f{}, Cm.View(), Son).Subset(false);
		const FVolumeScene Off = VB.Build(P, Cam, FVec3f{}, Cm.View()).Subset(false);
		FTemporalVolume WOn, WOff;
		WOn.Step(On, Cm, FV3{ 0, 0, 0 }, false, false, 2);
		WOff.Step(Off, Cm, FV3{ 0, 0, 0 }, false, false, 2);
		double Lw = 0.0;
		for (size_t I = 0; I < WOn.Vol.size(); ++I) { Lw += double(H::Uni_Luminance(H::float3(WOn.Vol[I].x, WOn.Vol[I].y, WOn.Vol[I].z))) - double(H::Uni_Luminance(H::float3(WOff.Vol[I].x, WOff.Vol[I].y, WOff.Vol[I].z))); }
		for (const int Div : { 2, 4 })
		{
			const double R = 2.0 * Units::Kpc;
			double Lc = 0.0;
			for (int Pass = 0; Pass < 2; ++Pass)
			{
				const FVolumeScene& Sc = Pass == 0 ? On : Off;
				FFarMap Map;
				Map.Reset(W / Div);
				const FVolumeScene Far = FarSplit(Sc, R, true);
				for (int F = 0; F < 6; ++F) { Map.MarchRows(Far, float(R), F, 0, Map.N); }
				Map.LayerBits = 0x3Fu;
				FTemporalVolume Near;
				Near.FarSlice = [&](const FV3& Dd, u32 K) { return Map.SliceAt(Dd, K); };
				Near.Step(FarSplit(Sc, R, false), Cm, FV3{ 0, 0, 0 }, false, false, 2);
				for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
				{
					const size_t I = size_t(Y) * W + X;
					const H::float4 C = H::Uni_FarComposite(Near.Vol[I], Map.Sample(PixelDir(Cm, W, Ht, X + 0.5, Y + 0.5)));
					Lc += (Pass == 0 ? 1.0 : -1.0) * double(H::Uni_Luminance(H::float3(C.x, C.y, C.z)));
				}
			}
			const double E = std::abs(Lc / std::max(Lw, 1e-300) - 1.0);
			Worst = std::max(Worst, E);
			Detail += Uni::Fmt("%s%s, map 1/%d: nebulae's light %+.1f%%", Detail.empty() ? "" : "; ", Sh.Name, Div, 100.0 * (Lc / std::max(Lw, 1e-300) - 1.0));
		}
	}
	Uni::Check(2, "T2.vol.far_split_nebulae", "D-235 (D-262: with D-258's galaxy shaping off, B-041) the textured Milky Way's nebulae across the far cache's split (R 2 kpc, the map at a half and a quarter of the view's resolution), two views: the nebulae's added light (on - off) in near + far map within 5% of the whole march's")
		.Metric("worst", Worst).Pass(Worst < 0.05, Detail);
}
