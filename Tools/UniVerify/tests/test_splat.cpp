// test_splat.cpp
// D-223 (owner: "scatter + quads"): plain point sprites drawn by atomic adds into fixed-point
// buffers and one shared gather, instead of a quad each. The splat path against today's quad
// (both the CPU references in UniSplatRef.h, which use the shaders' core):
//  - one star at every sub-pixel phase: its shape against the quad's, and its peak's swing
//    (twinkle as it drifts) against the quad's;
//  - a real star field (the flythrough scene from the solar camera, three views): which sprites
//    take the splat path, their light, per-pixel luminance and colour against the quads;
//  - order independence (the same buffers bit for bit in reverse order), the fixed point's
//    resolution against the sprite floor, no overflow;
//  - D-223 amendment, tile skipping: at 1920 x 1080 the tiled gather and resolve give the
//    untiled image bit for bit (no skipped row read: they hold NaN), the zero pass leaves every
//    buffer zero, and how much of the screen each level covers.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniSplatRef.h"
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
	H::FUniSprite PlainStar(float InNdcX, float InNdcY, float InCorePx, float InPpr, H::float3 InRad)
	{
		H::FUniSprite S{};
		S.RenderPos = H::float3(0.0f, 0.0f, 1.0f);
		S.AngularRadius = InCorePx / InPpr;
		S.Radiance = InRad;
		S.TrueDistance = 1.0e20f;
		S.NdcX = InNdcX;
		S.NdcY = InNdcY;
		S.Flags = H::UNI_SPRITE_VISIBLE;
		return S;
	}

	double Lum3(const float* P) { return 0.2126 * P[0] + 0.7152 * P[1] + 0.0722 * P[2]; }
}

UNI_TEST(sprite, splat)
{
	std::string Detail;
	bool bOk = true;
	const float Floor = H::UNI_SPRITE_FLOOR, Extent = 3.0f;

	// 1. One star at 11 x 11 sub-pixel phases, for three core sigmas.
	double Worst1 = 0.0;
	for (float Core : { 1.0f, 0.75f, 1.5f })
	{
		const int W = 48, Ht = 48;
		const float Ppr = 1000.0f;
		double Worst = 0.0, PqMin = 1e30, PqMax = 0.0, PsMin = 1e30, PsMax = 0.0, EnQ = 0.0, EnS = 0.0;
		for (int A = 0; A <= 10; ++A) for (int B = 0; B <= 10; ++B)
		{
			const double Px = 24.0 + A / 10.0, Py = 24.0 + B / 10.0;
			const H::FUniSprite S = PlainStar(float(Px / W * 2.0 - 1.0), float(1.0 - Py / Ht * 2.0), Core, Ppr, H::float3(1.0f, 1.0f, 1.0f));
			if (!H::Uni_SpriteSplattable(S, Ppr, Core, 0.0f)) { bOk = false; Detail += "; a plain star was not splattable"; }
			std::vector<float> Q(size_t(W) * Ht * 3, 0.0f), Sp(size_t(W) * Ht * 3, 0.0f);
			QuadSprite(W, Ht, Q, S, Ppr, 1.0f, Floor, Extent);
			FSplatRef R(W, Ht);
			R.Deposit(S, 1.0f);
			R.Resolve(Core, Sp);
			double Pq = 0.0, Ps = 0.0, D = 0.0, Eq = 0.0, Es = 0.0;
			for (size_t I = 0; I < size_t(W) * Ht; ++I)
			{
				const double Lq = Lum3(&Q[I * 3]), Ls = Lum3(&Sp[I * 3]);
				Pq = std::max(Pq, Lq); Ps = std::max(Ps, Ls); D = std::max(D, std::abs(Lq - Ls)); Eq += Lq; Es += Ls;
			}
			Worst = std::max(Worst, D / Pq);
			PqMin = std::min(PqMin, Pq); PqMax = std::max(PqMax, Pq); PsMin = std::min(PsMin, Ps); PsMax = std::max(PsMax, Ps);
			EnQ += Eq; EnS += Es;
		}
		const double SwQ = PqMax / PqMin - 1.0, SwS = PsMax / PsMin - 1.0;
		const double Lim = Core < 1.0f ? 0.05 : 0.03;
		if (!(Worst <= Lim) || std::abs(SwS - SwQ) > 0.05) { bOk = false; }
		if (Core == 1.0f) { Worst1 = Worst; }
		Detail += Uni::Fmt("%score %.2f px: shape within %.2f%% of the quad's peak (limit %.0f%%), peak swing %.1f%% (quad %.1f%%), light %.4f of the quad's",
			Detail.empty() ? "" : "; ", Core, 100.0 * Worst, 100.0 * Lim, 100.0 * SwS, 100.0 * SwQ, EnS / EnQ);
	}

	// 2. A real star field: the flythrough scene from the solar camera.
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord CamC = UniScenes::SolarCamera();
	P.Update(CamC, FVec3f{});
	struct FView { const char* Name; FV3 Look; };
	const FView Views[] = { { "toward the centre", { -1.0, 0.05, 0.02 } }, { "along the disc", { 0.05, 1.0, 0.02 } }, { "out of the plane", { 0.3, 0.2, 1.0 } } };
	double WorstLight = 0.0, WorstL1 = 0.0, WorstCol = 0.0;
	size_t TotalVis = 0, TotalSplat = 0;
	bool bSame = true, bTiledSame = true, bZeroed = true;
	uint64_t Overflow = 0;
	double WorstActive = 0.0, WorstWritten = 0.0;
	std::string TileDetail;
	for (const FView& Vw : Views)
	{
		FCamera Cm; Cm.Coord = CamC; Cm.Width = 480; Cm.Height = 270; Cm.DeltaMag = 0.0;
		Cm.LookAlong(Vw.Look);
		const H::FUniView V = Cm.View();
		const float Ppr = float(Cm.FocalPx());
		const float Core = H::Uni_SpriteCorePx(V);
		std::vector<H::FUniSprite> Splat;
		size_t Vis = 0;
		double LightVis = 0.0, LightSplat = 0.0;
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float Fade, u64)
		{
			H::uint4 D[3];
			CellDelta(K, CamC, D);
			const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(K.Level), Fade, UniCore::ToHlsl(O), V, false);
			if ((S.Flags & H::UNI_SPRITE_PROMOTE_CANDIDATE) != 0u || (S.Flags & H::UNI_SPRITE_VISIBLE) == 0u) { return; }
			++Vis;
			const double L = H::Uni_Luminance(S.Radiance) * (1.0 + double(S.HaloRatio));
			LightVis += L;
			if (H::Uni_SpriteSplattable(S, Ppr, Core, 0.0f)) { Splat.push_back(S); LightSplat += L; }
		});
		TotalVis += Vis; TotalSplat += Splat.size();
		// The splattable sprites both ways (the rest are quads either way).
		const int W = Cm.Width, Ht = Cm.Height;
		std::vector<float> Q(size_t(W) * Ht * 3, 0.0f), Sp(size_t(W) * Ht * 3, 0.0f);
		for (const H::FUniSprite& S : Splat) { QuadSprite(W, Ht, Q, S, Ppr, 1.0f, Floor, Extent); }
		FSplatRef R(W, Ht);
		for (const H::FUniSprite& S : Splat) { R.Deposit(S, 1.0f); }
		R.Resolve(Core, Sp);
		Overflow += R.Overflow;
		FSplatRef Rr(W, Ht);
		for (size_t I = Splat.size(); I-- > 0;) { Rr.Deposit(Splat[I], 1.0f); }
		if (Rr.Lum2x != R.Lum2x || Rr.Col != R.Col) { bSame = false; }
		double Lq = 0.0, Ls = 0.0, L1 = 0.0, Ce = 0.0;
		for (size_t I = 0; I < size_t(W) * Ht; ++I)
		{
			const double A = Lum3(&Q[I * 3]), B = Lum3(&Sp[I * 3]);
			Lq += A; Ls += B; L1 += std::abs(A - B);
			if (A > 0.0 && B > 0.0)
			{
				double E = 0.0;
				for (int K = 0; K < 3; ++K) { E += std::abs(Q[I * 3 + K] / A - Sp[I * 3 + K] / B); }
				Ce += A * E / 3.0;
			}
		}
		const double Light = Ls / std::max(Lq, 1e-300) - 1.0, L1r = L1 / std::max(Lq, 1e-300), Cr = Ce / std::max(Lq, 1e-300);
		// Tiles at an engine-sized view (the same sprites: their NDC does not depend on the size).
		{
			const int Wt = 1920, Htt = 1080;
			FSplatRef T(Wt, Htt);
			for (const H::FUniSprite& S : Splat) { T.Deposit(S, 1.0f); }
			std::vector<float> Full(size_t(Wt) * Htt * 3, 0.0f), Tiled(size_t(Wt) * Htt * 3, 0.0f);
			T.Resolve(Core, Full);
			const std::vector<uint8_t> Lv = T.TileLevels();
			T.Resolve(Core, Tiled, &Lv);
			for (size_t I = 0; I < Full.size(); ++I) { if (!(Full[I] == Tiled[I])) { bTiledSame = false; break; } }
			size_t N2 = 0, N1 = 0, No = 0;
			for (size_t I = 0; I < Lv.size(); ++I) { N2 += Lv[I] == 2u; N1 += Lv[I] == 1u; No += T.Occ[I] != 0u; }
			T.ZeroTouched(Lv);
			for (uint32_t U : T.Lum2x) { if (U != 0u) { bZeroed = false; break; } }
			for (uint32_t U : T.Col) { if (U != 0u) { bZeroed = false; break; } }
			const double Nt = double(Lv.size());
			WorstActive = std::max(WorstActive, double(N2) / Nt);
			WorstWritten = std::max(WorstWritten, double(N2 + N1) / Nt);
			TileDetail += Uni::Fmt("%s%s %.1f%% gathered and resolved, %.1f%% zero rows, %.1f%% marked", TileDetail.empty() ? "" : "; ", Vw.Name,
				100.0 * double(N2) / Nt, 100.0 * double(N1) / Nt, 100.0 * double(No) / Nt);
		}
		WorstLight = std::max(WorstLight, std::abs(Light)); WorstL1 = std::max(WorstL1, L1r); WorstCol = std::max(WorstCol, Cr);
		Detail += Uni::Fmt("; %s: %zu sprites, %zu splatted (%.1f%%, %.1f%% of the light), light %+.3f%%, per-pixel L1 %.2f%%, colour %.2f%%",
			Vw.Name, Vis, Splat.size(), 100.0 * double(Splat.size()) / double(std::max<size_t>(Vis, 1)), 100.0 * LightSplat / std::max(LightVis, 1e-300), 100.0 * Light, 100.0 * L1r, 100.0 * Cr);
	}
	if (!(WorstLight < 0.005 && WorstL1 < 0.05 && WorstCol < 0.03 && bSame && Overflow == 0 && TotalSplat > 0 && bTiledSame && bZeroed)) { bOk = false; }
	Detail += Uni::Fmt("; tiles (1920 x 1080, %d px): tiled = untiled bit for bit: %s; buffers zero after the zero pass: %s; %s", H::UNI_SPLAT_TILE,
		bTiledSame ? "yes" : "NO", bZeroed ? "yes" : "NO", TileDetail.c_str());

	// 3. The fixed point against the sprite floor.
	const double Units = double(H::UNI_SPLAT_SCALE) * double(H::UNI_SPRITE_FLOOR);
	if (!(Units >= 100.0)) { bOk = false; }
	Detail += Uni::Fmt("; order independent: %s; the floor is %.0f units; overflow %llu", bSame ? "yes" : "NO", Units, (unsigned long long)Overflow);

	Uni::Check(2, "T2.sprite.splat", "D-223 plain point sprites by atomic adds and one shared gather match today's quads: one star within 3% of the quad's peak at a 1 px core (5% at 0.75 px) at every sub-pixel phase, its twinkle within 5 points of the quad's; a real star field's splatted sprites within 0.5% in light, 5% per-pixel L1 and 3% in colour; the same buffers in any order; the floor at least 100 fixed-point units; no overflow; tiled = untiled bit for bit and the buffers zero after the zero pass")
		.Metric("shape_worst_1px", Worst1).Metric("field_light", WorstLight).Metric("field_l1", WorstL1).Metric("field_colour", WorstCol)
		.Metric("splat_share", double(TotalSplat) / double(std::max<size_t>(TotalVis, 1))).Metric("sprites", double(TotalVis))
		.Metric("tiles_active", WorstActive).Metric("tiles_written", WorstWritten)
		.Pass(bOk, Detail);
}
