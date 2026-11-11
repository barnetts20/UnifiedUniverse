// test_superweight.cpp
// D-225 (owner: the hitch at some angles remained after D-224; uu.VolumeMaxSubsamples 1 removed it):
// a local carrier is supersampled with rays per axis x sqrt(its Weight). The owner's spot (mid-disc,
// 20 degrees below the plane looking at the core) at azimuths 60 and 90 has two distant clusters'
// glows (weight 0.03-0.05) behind the core, supersampled 3x3..8x8 before: up to ~600 steps a pixel
// at the auto budget's smallest march. Against that (the old rule, reproduced by scaling each
// carrier's ResolveScale by sqrt(Weight)): no pixel supersampled, the longest ray back to the
// galaxy's, the image within 3% in any pixel and 0.1% overall. And a carrier at full weight
// supersamples exactly as before.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FImg { std::vector<double> L; double Steps = 0.0; u32 Max = 0; double Super = 0.0; };

	FImg Render(const FVolumeScene& Sc, const FCamera& Cam)
	{
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		const H::FUniVolumeView V = ToHlsl(Sc.View);
		FImg O;
		O.L.resize(size_t(Cam.Width) * Cam.Height);
		for (int Y = 0; Y < Cam.Height; ++Y) for (int X = 0; X < Cam.Width; ++X)
		{
			bool bS = false;
			const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cam, Cam.Width, Cam.Height, X, Y, 3.0e38f, bS);
			O.L[size_t(Y) * Cam.Width + X] = H::Uni_Luminance(R.Radiance);
			O.Steps += R.Steps; O.Max = std::max(O.Max, R.Steps); O.Super += bS ? 1.0 : 0.0;
		}
		O.Steps /= double(O.L.size()); O.Super /= double(O.L.size());
		return O;
	}
}

UNI_TEST(volume, super_weight)
{
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const double Kpc = Units::Kpc, Deg = 3.14159265358979 / 180.0;
	std::string Detail;
	bool bOk = true;
	double WorstMax = 0.0, WorstL1 = 0.0, WorstSuper = 0.0;
	for (double Az : { 60.0, 90.0 })
	{
		const double X = 8.0 * Kpc * std::cos(Az * Deg), Y = 8.0 * Kpc * std::sin(Az * Deg), Z = -8.0 * Kpc * std::tan(20.0 * Deg);
		FCoord Cam = FCoord::Origin();
		FVec3f Fr{};
		AddDisplacement(Cam, Fr, FVec3d{ X, Y, Z });
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam, FVec3f{});
		FCamera Cm; Cm.Coord = Cam; Cm.Width = 93; Cm.Height = 62; Cm.DeltaMag = 0.0;   // the auto budget's smallest march, scaled
		Cm.LookAlong(FV3{ -X, -Y, -Z });
		const FVolumeSettings S;
		const FVolumeScene Now = VB.Build(P, Cam, FVec3f{}, Cm.View(), S).Subset(false);
		FVolumeScene Old = Now;
		for (FCarrierGPU& C : Old.Carriers) { if (C.ResolveScale > 0.0f) { C.ResolveScale *= std::sqrt(std::clamp(C.Weight, 1.0e-12f, 1.0f)); } }
		const FImg A = Render(Old, Cm), B = Render(Now, Cm);
		double Tot = 0.0, D = 0.0, Mx = 0.0;
		for (size_t I = 0; I < A.L.size(); ++I) { Tot += A.L[I]; D += std::abs(B.L[I] - A.L[I]); Mx = std::max(Mx, std::abs(B.L[I] - A.L[I]) / std::max(A.L[I], 1e-30)); }
		const double L1 = D / std::max(Tot, 1e-300);
		WorstMax = std::max(WorstMax, Mx); WorstL1 = std::max(WorstL1, L1); WorstSuper = std::max(WorstSuper, B.Super);
		if (!(A.Super > 0.0) || B.Max > 2u * 110u) { bOk = false; }   // the case is there, and the long rays are gone
		Detail += Uni::Fmt("%saz %.0f: old %.1f steps/px (max %u, %.2f%% supersampled), now %.1f (max %u, %.2f%%); per pixel at most %.2f%%, L1 %.4f%%",
			Detail.empty() ? "" : "; ", Az, A.Steps, A.Max, 100.0 * A.Super, B.Steps, B.Max, 100.0 * B.Super, 100.0 * Mx, 100.0 * L1);
	}
	// A carrier at full weight: the same rays per axis as the old rule, over feature sizes.
	int Diff = 0;
	for (float Rs : { 0.002f, 0.01f, 0.05f, 0.2f }) for (float Th : { 1.0e-4f, 1.0e-3f, 1.0e-2f })
	{
		H::FUniCarrier C{};
		C.CamLocal = H::float3(3.0f, 0.0f, 0.0f);
		C.ResolveScale = Rs;
		C.Weight = 1.0f;
		const float FeaturePx = Rs / 3.0f / Th;
		const H::uint Old = H::uint(std::clamp(std::ceil(0.5f / std::max(FeaturePx, 1.0e-6f)), 1.0f, 8.0f));
		if (H::Uni_CarrierSubsamples(C, Th) != Old) { ++Diff; }
	}
	if (Diff != 0 || !(WorstMax < 0.03) || !(WorstL1 < 0.001)) { bOk = false; }
	Detail += Uni::Fmt("; full weight: %d of 12 cases differ from the old rule", Diff);
	Uni::Check(2, "T2.vol.super_weight", "D-225 supersampling scales with sqrt(carrier weight): at the owner's hitch spot the faint cluster glows behind the core are no longer supersampled (longest ray back to the galaxy's), the image within 3% per pixel and 0.1% overall; full-weight carriers unchanged")
		.Metric("worst_pixel", WorstMax).Metric("worst_l1", WorstL1).Metric("supersampled_now", WorstSuper).Pass(bOk, Detail);
}
