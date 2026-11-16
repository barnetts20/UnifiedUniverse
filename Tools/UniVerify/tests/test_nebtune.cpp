// test_nebtune.cpp
// D-248 (owner 2026-11-15: "we want handles for all of those as well as the frequency/scale/
// prevalence of the nebulas themselves"): the nebulae's runtime tuning (UniCore::NebulaTune ->
// UniFields.ush UNI_NEB_TUNE / UNI_NEB_TUNE2; uu.Nebula.Scale, .Prevalence, .FineScale,
// .FineOctave, .FineLodBias), render only.
//  - the defaults are D-241..D-247's constants: NebulaTune's, FNebulaTune's, the shim's globals
//    and the HLSL macros' (parsed) all (1, 0, UNI_NEBULA_SHAPE_MEAN, UNI_NEB_SHAPE_MEAN2) and
//    (1, UNI_NEB_FINE_OCTAVE, 0, 0); the cores at those values are D-247's bit for bit (the other
//    checks hold);
//  - prevalence (x0.25, x0.5, x2, x4): over the Milky Way's gas layer (an annulus, samples apart
//    from the table's), the coverage that multiple of the default's (within 4%), and the near
//    detail's light the far blend's (the shape's mean at full detail over the mean the blend
//    goes to, within 7% of the default's ratio; x0.25 holds a quarter of the knots): one light
//    near and far. (The default's eroded mean over four independent sample sets of the D-256 Milky
//    Way spreads +-3.5%, x4's +-1%: the default's few nebulae carry it, so a set apart from the
//    table's is 3-6% off at the ends; 5% held on the earlier Milky Way.)
//  - scale (x0.5, x2): the coverage within 6% of the default's and the erosion's share of the
//    light (near over the structure's E[X^2 J], normalised by kNebulaErodeByScale) within 6% (two sample sets differ by 3-5%); the annulus holds 4x more or fewer
//    knots, so its near/far is reported, not held; the knots' blend moves with it;
//  - fine scale (x0.5, x2) and octave (2, 8): the near/far ratio within 7% (the same samples: 4% measured);
//  - the mip bias moves each detail layer's read by exactly its value;
//  - D-249: the nebulae's detail texture (slot 3): unbound, galaxy_detail's bit for bit; bound to
//    another texture, the nebulae's eroded shapes change and their knots and the field's light
//    (what placement reads) do not.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	struct FNtSample { H::float3 P; H::float4 Neb; };

	/** The Milky Way's gas layer: samples' knot terms at the current tuning. */
	std::vector<FNtSample> NtSamples(const H::FUniFieldParams& P, const H::FUniGalaxyConst& K, u32 Seed, int N)
	{
		std::vector<FNtSample> S;
		S.reserve(size_t(N));
		for (int I = 0; I < N; ++I)
		{
			const double R = 0.2 + 0.5 * H::Uni_DrawUnit(Seed, 2u, u32(3 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(Seed, 2u, u32(3 * I + 1));
			const double Z = (2.0 * H::Uni_DrawUnit(Seed, 2u, u32(3 * I + 2)) - 1.0) * 0.5 * double(P.A.y);
			const H::float3 Pt{ float(R * std::cos(Ph)), float(R * std::sin(Ph)), float(Z) };
			H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
			H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-6f, C, Nv);
			S.push_back({ Pt, Nv });
		}
		return S;
	}

	/** Coverage (X > 0), the near shape's mean (full detail) over the far blend's mean, and over
	 *  the structure's (E[X^2 J] x the blend's mean over its default value: the erosion's share
	 *  alone, apart from which knots the samples happen to hold). */
	void NtMeasure(const std::vector<FNtSample>& S, const H::FUniFieldParams& P, double& OutCover, double& OutRatio, double& OutErode)
	{
		double Cov = 0.0, Near = 0.0, Str = 0.0;
		for (const FNtSample& Q : S)
		{
			float X = 0.0f, Md = 1.0f;
			Near += double(H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, 0.0f, 1.0f, X, Md));
			Cov += X > 0.0f ? 1.0 : 0.0;
			Str += double(X) * X * std::clamp(1.0f + 2.0f * (Q.Neb.w - 0.493f), 0.2f, 1.8f);
		}
		OutCover = Cov / double(S.size());
		OutRatio = Near / double(S.size()) / double(H::GNebTune.z);
		OutErode = Near / std::max(Str, 1e-30);
	}

	struct FNtRestore
	{
		H::float4 A = H::GNebTune, B = H::GNebTune2;
		~FNtRestore() { H::GNebTune = A; H::GNebTune2 = B; }
	};
}

UNI_TEST(galaxy, nebula_tune)
{
	const FNtRestore Restore;
	// The defaults.
	size_t DefBad = 0;
	std::string DefText;
	{
		auto Eq4 = [&](const char* What, const H::float4& V, float A, float B, float C, float D)
		{
			if (!(V.x == A && V.y == B && V.z == C && V.w == D)) { ++DefBad; DefText += Uni::Fmt(" %s (%g %g %g %g);", What, V.x, V.y, V.z, V.w); }
		};
		const FNebulaTune Dt = NebulaTune(FVolumeSettings{});
		const FNebulaTune Ds;
		const float M = H::UNI_NEBULA_SHAPE_MEAN, M2 = float(UNI_NEB_SHAPE_MEAN2), Oc = float(UNI_NEB_FINE_OCTAVE);
		Eq4("NebulaTune", H::float4(Dt.T[0], Dt.T[1], Dt.T[2], Dt.T[3]), 1.0f, 0.0f, M, M2);
		Eq4("NebulaTune 2", H::float4(Dt.T2[0], Dt.T2[1], Dt.T2[2], Dt.T2[3]), 1.0f, Oc, 0.0f, 0.0f);
		Eq4("FNebulaTune", H::float4(Ds.T[0], Ds.T[1], Ds.T[2], Ds.T[3]), 1.0f, 0.0f, M, M2);
		Eq4("FNebulaTune 2", H::float4(Ds.T2[0], Ds.T2[1], Ds.T2[2], Ds.T2[3]), 1.0f, Oc, 0.0f, 0.0f);
		Eq4("shim", Restore.A, 1.0f, 0.0f, M, M2);
		Eq4("shim 2", Restore.B, 1.0f, Oc, 0.0f, 0.0f);
		std::string Src;
		{ std::ifstream F("Shaders/Private/Core/UniFields.ush"); std::stringstream B; B << F.rdbuf(); Src = B.str(); }
		auto Macro = [&](const char* Name, H::float4& Out)
		{
			const std::regex Re(std::string("#define\\s+") + Name + "\\s+float4\\(([-0-9.eE]+)f?,\\s*([-0-9.eE]+)f?,\\s*([-0-9.eE]+)f?,\\s*([-0-9.eE]+)f?\\)");
			std::smatch Mt;
			if (!std::regex_search(Src, Mt, Re)) { return false; }
			Out = H::float4(std::stof(Mt[1].str()), std::stof(Mt[2].str()), std::stof(Mt[3].str()), std::stof(Mt[4].str()));
			return true;
		};
		H::float4 Ha, Hb;
		if (!Macro("UNI_NEB_TUNE", Ha) || !Macro("UNI_NEB_TUNE2", Hb)) { ++DefBad; DefText += " HLSL defaults not found;"; }
		else { Eq4("HLSL", Ha, 1.0f, 0.0f, M, M2); Eq4("HLSL 2", Hb, 1.0f, Oc, 0.0f, 0.0f); }
	}

	FWebTexture T[3];
	bool bTex = true;
	for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bTex = false; } }
	if (!bTex) { Uni::Check(2, "T2.galaxy.nebula_tune", "D-248 the nebulae's tuning").Pass(false, "textures not loaded"); return; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const FFieldParams* Mw = nullptr;
	for (const FAnchor& A : D.Anchors) { if (!A.EmitterParams.empty()) { Mw = &A.EmitterParams[0]; } }
	if (!Mw) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } Uni::Check(2, "T2.galaxy.nebula_tune", "D-248").Pass(false, "no anchor"); return; }
	const H::FUniFieldParams P = Mw->ToHlsl();
	const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
	const int N = 200000;
	const u32 Seed = 0xB248u;   // apart from the table's samples (0xA11)

	ApplyNebulaTune(NebulaTune(1.0f, 1.0f, 1.0f, 4.0f, 0.0f));
	const std::vector<FNtSample> S0 = NtSamples(P, K, Seed, N);
	double Cov0 = 0.0, Rat0 = 0.0, Ero0 = 0.0;
	NtMeasure(S0, P, Cov0, Rat0, Ero0);

	std::string Detail = Uni::Fmt("default: cover %.2f%%, near/far %.4f", 100.0 * Cov0, Rat0);
	double WorstCover = 0.0, WorstRatio = 0.0, WorstScaleCover = 0.0, WorstErode = 0.0;
	for (float Pv : { 0.25f, 0.5f, 2.0f, 4.0f })
	{
		ApplyNebulaTune(NebulaTune(1.0f, Pv, 1.0f, 4.0f, 0.0f));
		double C = 0.0, R = 0.0, E = 0.0;
		NtMeasure(S0, P, C, R, E);
		WorstCover = std::max(WorstCover, std::abs(C / Cov0 / double(Pv) - 1.0));
		WorstRatio = std::max(WorstRatio, std::abs(R / Rat0 - 1.0));
		Detail += Uni::Fmt("; prevalence x%.2f: cover x%.3f, near/far x%.4f, light x%.2f", Pv, C / Cov0, R / Rat0, double(H::GNebTune.z) / double(H::UNI_NEBULA_SHAPE_MEAN));
	}
	bool bTexel = true;
	for (float Sc : { 0.5f, 2.0f })
	{
		ApplyNebulaTune(NebulaTune(Sc, 1.0f, 1.0f, 4.0f, 0.0f));
		const std::vector<FNtSample> Ss = NtSamples(P, K, Seed, N);
		double C = 0.0, R = 0.0, E = 0.0;
		NtMeasure(Ss, P, C, R, E);
		WorstScaleCover = std::max(WorstScaleCover, std::abs(C / Cov0 - 1.0));
		WorstErode = std::max(WorstErode, std::abs(E / Ero0 - 1.0));
		Detail += Uni::Fmt("; scale x%.1f: cover x%.3f, erosion's share x%.4f (near/far x%.4f: fewer knots in the annulus)", Sc, C / Cov0, E / Ero0, R / Rat0);
		// The knots' blend: full at twice UNI_NEBULA_FOOT texels a pixel, the texel x the scale.
		const float Tx = std::exp2(UNI_NEBULA_MIP) / (P.G.w * std::max(P.H.w, 1.0f)) * Sc;
		bTexel = bTexel && std::abs(H::Uni_GalaxyNebulaBlend(P, 2.0f * UNI_NEBULA_FOOT * Tx, 0.0f) - 1.0f) < 1e-4f && std::abs(H::Uni_GalaxyNebulaBlend(P, UNI_NEBULA_FOOT * Tx, 0.0f)) < 1e-4f;
	}
	for (int Kf = 0; Kf < 4; ++Kf)
	{
		const float Fs = Kf == 0 ? 0.5f : (Kf == 1 ? 2.0f : 1.0f);
		const float Oc = Kf == 2 ? 2.0f : (Kf == 3 ? 8.0f : 4.0f);
		ApplyNebulaTune(NebulaTune(1.0f, 1.0f, Fs, Oc, 0.0f));
		double C = 0.0, R = 0.0, E = 0.0;
		NtMeasure(S0, P, C, R, E);
		WorstRatio = std::max(WorstRatio, std::abs(R / Rat0 - 1.0));
		Detail += Uni::Fmt("; fine scale x%.1f octave %.0f: near/far x%.4f", Fs, Oc, R / Rat0);
	}
	bool bBias = true;
	{
		const float Foot = H::Uni_GalaxyNebulaFineTexel(P, 0.0f) * 2.0f;
		ApplyNebulaTune(NebulaTune(1.0f, 1.0f, 1.0f, 4.0f, 0.0f));
		const float L0 = H::Uni_GalaxyNebulaFineLod(P, 0.0f, Foot), L0t = H::Uni_GalaxyNebulaFineLod(P, 1.0f, Foot * 4.0f);
		ApplyNebulaTune(NebulaTune(1.0f, 1.0f, 1.0f, 4.0f, 1.0f));
		const float L1 = H::Uni_GalaxyNebulaFineLod(P, 0.0f, Foot), L1t = H::Uni_GalaxyNebulaFineLod(P, 1.0f, Foot * 4.0f);
		bBias = std::abs(L1 - L0 - 1.0f) < 1e-5f && std::abs(L1t - L0t - 1.0f) < 1e-5f;
		Detail += Uni::Fmt("; mip bias +1: walls' read %.2f -> %.2f, texture's %.2f -> %.2f", L0, L1, L0t, L1t);
	}
	// D-249: the nebulae's detail texture slot. Unbound: galaxy_detail's, bit for bit (bound to the
	// same texture); bound to another (warp_fine as a stand-in): the eroded shapes change, the knots'
	// structure X and the field's light (what placement reads) do not.
	size_t SlotSame = 0, SlotDiff = 0, SlotKnot = 0, SlotField = 0;
	{
		ApplyNebulaTune(NebulaTune(1.0f, 1.0f, 1.0f, 4.0f, 0.0f));
		const size_t Nq = 4000;
		std::vector<float> A(Nq), Ax(Nq), Af(Nq);
		for (size_t I = 0; I < Nq; ++I)
		{
			const FNtSample& Q = S0[I * 37 % S0.size()];
			float X = 0.0f, Md = 1.0f;
			A[I] = H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, 0.0f, 1.0f, X, Md);
			Ax[I] = X;
			H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
			Af[I] = H::UniField_GalaxyFilteredCKN(Q.P, P, K, 1e-6f, C, Nv);
		}
		BindGalaxyTexture(3, &T[2]);
		for (size_t I = 0; I < Nq; ++I)
		{
			const FNtSample& Q = S0[I * 37 % S0.size()];
			float X = 0.0f, Md = 1.0f;
			SlotSame += H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, 0.0f, 1.0f, X, Md) == A[I] ? 1 : 0;
		}
		BindGalaxyTexture(3, &T[0]);
		for (size_t I = 0; I < Nq; ++I)
		{
			const FNtSample& Q = S0[I * 37 % S0.size()];
			float X = 0.0f, Md = 1.0f;
			const float Sh = H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, 0.0f, 1.0f, X, Md);
			SlotDiff += (A[I] > 0.0f && Sh != A[I]) ? 1 : 0;
			SlotKnot += X == Ax[I] ? 1 : 0;
			H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
			SlotField += H::UniField_GalaxyFilteredCKN(Q.P, P, K, 1e-6f, C, Nv) == Af[I] ? 1 : 0;
		}
		BindGalaxyTexture(3, nullptr);
		size_t NonZero = 0;
		for (size_t I = 0; I < Nq; ++I) { NonZero += A[I] > 0.0f ? 1 : 0; }
		Detail += Uni::Fmt("; nebula detail slot: bound to galaxy_detail %zu of %zu shapes the same; to another texture %zu of %zu nebula shapes changed, knots %zu and field light %zu of %zu unchanged",
			SlotSame, Nq, SlotDiff, NonZero, SlotKnot, SlotField, Nq);
		SlotDiff = (NonZero > 0 && SlotDiff * 2 > NonZero) ? 1 : 0;
	}
	const bool bSlot = SlotSame == 4000 && SlotDiff == 1 && SlotKnot == 4000 && SlotField == 4000;
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	const bool bPass = bSlot && DefBad == 0 && WorstCover < 0.04 && WorstScaleCover < 0.06 && WorstRatio < 0.07 && WorstErode < 0.06 && bTexel && bBias;
	Uni::Check(2, "T2.galaxy.nebula_tune", "D-248 the nebulae's runtime tuning (uu.Nebula.Scale, .Prevalence, .FineScale, .FineOctave, .FineLodBias): the defaults are the constants (NebulaTune, the shim, the HLSL macros); over the Milky Way's gas layer prevalence x0.25..x4 covers that multiple of the default (4%) and keeps the near detail's light the far blend's as at the default (7%: an independent sample set's default spreads +-3.5% on this galaxy; fewer knots at x0.25), as do the fine scale and octave; scale x0.5 and x2 the default's coverage (6%) and the erosion's share of the light within 6% of the default's (kNebulaErodeByScale; two sample sets differ by 3-5%; the annulus holds 4x fewer or more knots, so its near/far is reported); the knots' blend moves with the scale; the mip bias moves the detail reads by its value; D-249: the nebulae's detail texture slot unbound is galaxy_detail's bit for bit, bound to another texture it changes the nebulae's shapes and neither their knots nor the field's light")
		.Metric("worst_cover", WorstCover).Metric("worst_scale_cover", WorstScaleCover).Metric("worst_near_far", WorstRatio).Metric("worst_erosion", WorstErode)
		.Pass(bPass, Uni::Fmt("defaults: %zu differ%s; %s; knots' blend %s, mip bias %s", DefBad, DefText.c_str(), Detail.c_str(), bTexel ? "ok" : "OFF", bBias ? "ok" : "OFF"));
}
