// D-218 (B-024, owner's choice from D-217): a galaxy's bulge warp (warp_large) is read only
// where the bulge is, inside Rb = min(0.5, 8 sqrt(r_e^2 + footprint^2)), instead of inside 0.5.
// Checked against the D-179 read (inside 0.5, fading over [0.3, 0.5]) rebuilt here.
#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	using H::float3;
	using H::float4;

	struct FTexSet
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FTexSet()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FBind
	{
		explicit FBind(const FTexSet& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	/** The field with D-179's bulge read: warp_large inside r < 0.5, fading over [0.3, 0.5]. */
	float FieldD179(const H::FUniFieldParams& Par, const H::FUniGalaxyConst& K, const float3& P)
	{
		const float Cut = Par.D.x;
		const float R2 = P.x * P.x + P.y * P.y + P.z * P.z;
		if (R2 >= Cut * Cut) { return 0.0f; }
		H::FUniFieldParams NoB = Par;
		NoB.G.y = 0.0f;   // no bulge read in the shared terms; it is done below as D-179 did
		const float Slab = H::Uni_GalaxyDiscSlab(Par);
		H::FUniGalaxyShared S = H::Uni_GalaxySharedK(P, NoB, K, 0.0f, Slab, true, H::Uni_GalaxyNeedsDetail(Par));
		if (Par.G.y != 0.0f && Par.B.y > 0.0f && R2 < 0.25f)
		{
			const uint32_t Ho = K.Ho;
			const float3 Off = float3(float(Ho & 0x3FFu), float((Ho >> 10u) & 0x3FFu), float((Ho >> 20u) & 0x3FFu)) * (1.0f / 1024.0f);
			const float4 W = H::UniGalaxyTexSample(1, float3(P.x * 0.5f * Par.G.z + Off.z, P.y * 0.5f * Par.G.z + Off.x, P.z * 0.5f * Par.G.z + Off.y), 0.0f);
			const float Fb = 1.0f - std::clamp((std::sqrt(R2) - 0.3f) * 5.0f, 0.0f, 1.0f);
			S.Pb = float3(P.x + W.x * Par.G.y * Fb, P.y + W.y * Par.G.y * Fb, P.z + W.z * Par.G.y * Fb);
		}
		return H::Uni_GalaxyFromShared(S, P, Par, 0.0f);
	}

	u64 SplitMix(u64& S) { u64 Z = (S += 0x9E3779B97F4A7C15ull); Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull; Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull; return Z ^ (Z >> 31); }
	double Unit(u64& S) { return (double(SplitMix(S) >> 11) + 0.5) * (1.0 / 9007199254740992.0); }

	struct FCmp { double L1 = 0, P95 = 0, Light = 0, MaxRel = 0; };

	/** New field against D-179's over points importance-sampled through the disc (radius ~
	 *  Gamma(2, h_R), height ~ Laplace(2 H) about the mode warp's centre), weighted by 1 / pdf:
	 *  light-weighted mean relative difference, its light-weighted p95, the total light's change. */
	FCmp Compare(const H::FUniFieldParams& Par, int N, u64 Seed)
	{
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(Par);
		const double Hr = std::max(double(Par.A.x), 1e-3), Hs = std::max(double(Par.A.y), 1e-6);
		u64 S = Seed;
		double Sf = 0, Sm = 0, Se = 0;
		std::vector<std::pair<double, double>> Rel;
		FCmp O;
		for (int I = 0; I < N;)
		{
			const double R = -Hr * std::log(Unit(S) * Unit(S));
			if (R >= 0.98) { continue; }
			++I;
			const double Th = 6.283185307179586 * Unit(S);
			const float X = float(R * std::cos(Th)), Y = float(R * std::sin(Th));
			const H::FUniGalaxyShared Sh = H::Uni_GalaxySharedK(float3(X, Y, 0.0f), Par, K, 0.0f, H::Uni_GalaxyDiscSlab(Par), false, false);
			const double B = 2.0 * Hs * double(Sh.Flare);
			const double U = Unit(S);
			const double V = U < 0.5 ? B * std::log(2.0 * U) : -B * std::log(2.0 * (1.0 - U));
			const double W = std::exp(R / Hr) * 2.0 * B * std::exp(std::abs(V) / B);
			const float3 P(X, Y, float(double(Sh.WarpZ) + V));
			const double F = FieldD179(Par, K, P), M = H::UniField_Galaxy(P, Par);
			Sf += W * F; Sm += W * M; Se += W * std::abs(M - F);
			if (F > 0.0) { Rel.push_back({ std::abs(M - F) / F, W * F }); O.MaxRel = std::max(O.MaxRel, std::abs(M - F) / F); }
		}
		O.L1 = Se / std::max(Sf, 1e-300);
		O.Light = Sm / std::max(Sf, 1e-300) - 1.0;
		std::sort(Rel.begin(), Rel.end());
		double Tot = 0; for (auto& E : Rel) { Tot += E.second; }
		double Acc = 0;
		for (auto& E : Rel) { Acc += E.second; if (Acc >= 0.95 * Tot) { O.P95 = E.first; break; } }
		return O;
	}

	H::FUniFieldParams HomeParams(const FTexSet& Tx)
	{
		FUniverseDef D = UniScenes::Flythrough();
		UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
		H::FUniFieldParams O{};
		for (const FAnchor& A : D.Anchors)
		{
			if (A.ArchetypeId == UniScenes::kGalaxy && !A.EmitterParams.empty())
			{
				const FFieldParamsGPU G = A.EmitterParams[0].Pack();
				std::memcpy(static_cast<void*>(&O), &G, sizeof(O));
				break;
			}
		}
		return O;
	}
}

UNI_TEST(galaxy, bulge_read)
{
	const FTexSet Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.bulge_read", "D-218 bulge warp read").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FBind Bind(Tx);
	const H::FUniFieldParams Home = HomeParams(Tx);
	const int N = UniVerify::Options().FullScale ? 200000 : 60000;
	// A spiral's bulge (r_e 0.04: Rb 0.32) at the home galaxy's weight and the rolled maximum.
	H::FUniFieldParams Heavy = Home;
	Heavy.B.y = 1.5f;
	const FCmp A = Compare(Home, N, 0xB0u), B = Compare(Heavy, N, 0xB1u);
	// An early type's large bulge (r_e 0.08: Rb = 0.5): the D-179 read to rounding.
	H::FUniFieldParams Early = Home;
	Early.A.w = 0.08f;
	const FCmp E = Compare(Early, N / 4, 0xB2u);
	const double Rb = std::min(0.5, 8.0 * double(Home.A.w));
	std::string Detail = Uni::Fmt("spiral r_e %.2f: read inside %.2f (%.0f%% of the disc's area, was 25%%); weight %.1f: L1 %.3f%% p95 %.3f%% light %+.4f%%; weight 1.5: L1 %.3f%% p95 %.3f%% light %+.4f%%; early type r_e 0.08: max point difference %.1e",
		double(Home.A.w), Rb, 100.0 * Rb * Rb, double(Home.B.y), 100.0 * A.L1, 100.0 * A.P95, 100.0 * A.Light, 100.0 * B.L1, 100.0 * B.P95, 100.0 * B.Light, E.MaxRel);
	Uni::Check(2, "T2.galaxy.bulge_read", "D-218 the bulge's warp_large read only inside 8 effective radii (at most 0.5): a spiral's field within 0.2% per point (light-weighted; p95 under 1%; light within 0.01%) of D-179's read inside 0.5, at the home galaxy's bulge weight and the rolled maximum 1.5; an early type (Rb 0.5) the same to rounding (1e-5)")
		.Metric("l1_home", A.L1).Metric("p95_home", A.P95).Metric("l1_heavy", B.L1).Metric("p95_heavy", B.P95).Metric("light_heavy", B.Light).Metric("early_max", E.MaxRel).Metric("read_area", Rb * Rb)
		.Pass(A.L1 < 0.002 && B.L1 < 0.002 && A.P95 < 0.01 && B.P95 < 0.01 && std::abs(A.Light) < 1e-4 && std::abs(B.Light) < 1e-4 && E.MaxRel < 1e-5, Detail);
}
