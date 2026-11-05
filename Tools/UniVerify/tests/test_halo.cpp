// test_halo.cpp
// D-185 (amended) and D-188: the halo's colour, and the sprite quad's reach. Owner, in the
// engine: a bright star's halo was cut into a square at the quad's edge (a fixed 3 sigmas). The
// quad now reaches until the core and halo fall below a floor (3 to 8 sigmas) with a soft edge;
// the halo chroma keeps luminance. Spikes were withdrawn (owner: the engine's bloom and lens
// flares instead).

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	double Unit(u32 S, u32 K) { return double(H::Uni_DrawUnit(S, 0x4A10u, K)); }
}

UNI_TEST(sprite, reach)
{
	// 1. Reach: at the computed radius the sprite's light (core + halo, as the pixel shader
	//    draws it) is at or below the floor unless the reach is at its cap; never under 3 sigmas
	//    of the core or the halo; the light the soft edge removes stays small.
	const float Floor = H::UNI_SPRITE_FLOOR;
	int Cases = 0, AboveFloor = 0, UnderMin = 0, Capped = 0;
	double WorstEdgeLoss = 0.0, WorstCutRatio = 0.0;
	for (u32 I = 0; I < 2000; ++I)
	{
		const float Sigma = float(0.5 + 6.0 * Unit(I, 0));
		const bool bHalo = Unit(I, 1) < 0.7;
		const float HaloPx = bHalo ? float(Sigma * (1.0 + 4.0 * Unit(I, 2))) : 0.0f;
		const float Ratio = bHalo ? float(0.05 + 3.0 * Unit(I, 3)) : 0.0f;
		const float Lum = float(std::pow(10.0, -3.0 + 11.0 * Unit(I, 4)));   // 1e-3 .. 1e8 scene units
		const float CoreW = Unit(I, 5) < 0.1 ? 0.0f : 1.0f;                  // promoted stars: halo only
		const float R = H::Uni_SpriteReachPx(Lum, Sigma, 0.0f, HaloPx, Ratio, CoreW, Floor, 3.0f);
		const bool bCap = R >= 0.999f * H::UNI_SPRITE_REACH_MAX * std::max(Sigma, HaloPx);
		if (bCap) { ++Capped; }
		const double AtR = double(Lum) * double(H::Uni_SpriteProfileHalo(R, Sigma, 0.0f, HaloPx, Ratio, CoreW));
		if (!bCap && AtR > 1.0001 * Floor) { ++AboveFloor; }
		WorstCutRatio = std::max(WorstCutRatio, AtR / Floor * (bCap ? 0.0 : 1.0));
		if (R < 2.999f * Sigma || (bHalo && R < 2.999f * HaloPx)) { ++UnderMin; }
		// Light the edge window removes: integrate the profile over the plane, with and without it.
		if (I < 300 && (CoreW > 0.0f || bHalo))
		{
			double All = 0.0, Kept = 0.0;
			const int N = 400;
			const double Rmax = 1.2 * R;
			for (int K = 0; K < N; ++K)
			{
				const double Rr = (K + 0.5) * Rmax / N;
				const double W = H::Uni_SpriteProfileHalo(float(Rr), Sigma, 0.0f, HaloPx, Ratio, CoreW) * 6.283185307179586 * Rr * (Rmax / N);
				All += W;
				Kept += W * double(H::Uni_SpriteEdgeWindow(float(Rr), R));
			}
			WorstEdgeLoss = std::max(WorstEdgeLoss, 1.0 - Kept / std::max(All, 1e-300));
		}
		++Cases;
	}
	// 2. Halo chroma: luminance kept, more vivid, identity at 1.
	double WorstLum = 0.0, WorstIdentity = 0.0;
	int NotMoreVivid = 0;
	for (u32 I = 0; I < 1000; ++I)
	{
		const H::float3 C(float(0.05 + Unit(I, 10)), float(0.05 + Unit(I, 11)), float(0.05 + Unit(I, 12)));
		const H::float3 B = H::Uni_SpriteChroma(C, 1.4f), Id = H::Uni_SpriteChroma(C, 1.0f);
		WorstLum = std::max(WorstLum, double(std::abs(H::Uni_Luminance(B) / H::Uni_Luminance(C) - 1.0f)));
		WorstIdentity = std::max(WorstIdentity, double(std::max({ std::abs(Id.x - C.x), std::abs(Id.y - C.y), std::abs(Id.z - C.z) })));
		const float Sc = (std::max({ C.x, C.y, C.z }) - std::min({ C.x, C.y, C.z })) / std::max({ C.x, C.y, C.z });
		const float Sb = (std::max({ B.x, B.y, B.z }) - std::min({ B.x, B.y, B.z })) / std::max({ B.x, B.y, B.z });
		if (Sc > 0.01f && Sb < Sc) { ++NotMoreVivid; }
	}
	Uni::Check(1, "T1.sprite.reach", "D-188 sprite reach: over 2,000 sprites (cores 0.5-6.5 px, halos to 5x, luminance 1e-3..1e8) the quad reaches until core and halo fall below the floor (unless at its 8-sigma cap), never under 3 sigmas of either; the light beyond its soft edge (and past the quad) under 2.5% (the fixed 3-sigma quad lost 1.1%). D-185 halo chroma keeps luminance (1e-5), is more vivid, identity at 1")
		.Metric("cases", Cases).Metric("above_floor", AboveFloor).Metric("under_min", UnderMin).Metric("capped", Capped).Metric("edge_loss", WorstEdgeLoss).Metric("chroma_lum", WorstLum)
		.Pass(AboveFloor == 0 && UnderMin == 0 && WorstEdgeLoss < 0.025 && WorstLum < 1e-5 && WorstIdentity < 1e-6 && NotMoreVivid == 0,
			Uni::Fmt("%d sprites: %d above the floor at their reach (worst %.3fx the floor), %d under 3 sigmas, %d at the cap; the soft edge removes at most %.2f%% of the light; chroma: luminance %.1e, identity %.1e, %d not more vivid",
				Cases, AboveFloor, WorstCutRatio, UnderMin, Capped, 100.0 * WorstEdgeLoss, WorstLum, WorstIdentity, NotMoreVivid));
}
