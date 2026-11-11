// test_warpband.cpp
// D-220 (owner, 2026-11-10): the disc warp moves the disc at a structural level only (warp_fine at
// mip 2 at least) and its fine band comes back as a bounded multiplier (Uni_GalaxyFineMul). Checks:
//  - the C++ knobs (UniHlslShim.h) hold UniFields.ush's constants, so the shaders and the CPU agree;
//  - the look: the home galaxy's face-on structure (its light over the azimuthal mean, annulus
//    0.1-0.7) keeps the full warp's mix of scales: octave band powers 8-128 cycles a unit within
//    0.8-1.4 x, contrast within 10%;
//  - the aliasing: flying top-down toward the centre, the march against a fine reference of the
//    same field (every step an eighth): the error's crawl frame to frame falls at least 30%.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
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

	/** The knobs set for the warp before D-220 (full warp_fine, no fine band), restored after. */
	struct FBeforeD220
	{
		float F, G;
		FBeforeD220() : F(H::GKnobWarpMipFloor), G(H::GKnobFineGain) { H::GKnobWarpMipFloor = 0.0f; H::GKnobFineGain = 0.0f; }
		~FBeforeD220() { H::GKnobWarpMipFloor = F; H::GKnobFineGain = G; }
	};

	/** UniFields.ush's #define of Name, or NaN. */
	double ShaderConstant(const std::string& Text, const std::string& Name)
	{
		const std::regex Re("#define\\s+" + Name + "\\s+([-0-9.eE]+)f?");
		std::smatch M;
		return std::regex_search(Text, M, Re) ? std::stod(M[1].str()) : std::nan("");
	}

	H::FUniFieldParams HomeOf(const FUniverseDef& D)
	{
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

	/** The face-on structure's octave band powers (2^k to 2^(k+1) cycles a unit, k = 1..6) and its
	 *  RMS: the light integrated through the disc, over its azimuthal mean, minus 1, annulus 0.1-0.7. */
	void FaceBands(const H::FUniFieldParams& Par, int N, std::vector<double>& OutBands, double& OutRms)
	{
		const float Ext = 0.75f;
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(Par);
		const float Slab = H::Uni_GalaxyDiscSlab(Par);
		const float Hs = std::max(Par.A.y, 1.0e-6f);
		std::vector<double> L(size_t(N) * N, 0.0), R(size_t(N) * N, 0.0);
		for (int J = 0; J < N; ++J)
		{
			for (int I = 0; I < N; ++I)
			{
				const float X = -Ext + 2.0f * Ext * (float(I) + 0.5f) / float(N), Y = Ext - 2.0f * Ext * (float(J) + 0.5f) / float(N);
				const H::FUniGalaxyShared S = H::Uni_GalaxySharedK(H::float3(X, Y, 0.0f), Par, K, 0.0f, Slab, false, false);
				const float Half = 6.0f * Hs * S.Flare;
				double Sum = 0.0;
				for (int Z = 0; Z < 24; ++Z) { Sum += double(H::UniField_Galaxy(H::float3(X, Y, S.WarpZ - Half + 2.0f * Half * (float(Z) + 0.5f) / 24.0f), Par)); }
				L[size_t(J) * N + I] = Sum;
				R[size_t(J) * N + I] = std::sqrt(double(X) * X + double(Y) * Y);
			}
		}
		// The azimuthal mean in 160 radial bins.
		const int Bins = 160;
		std::vector<double> Sb(Bins, 0.0), Nb(Bins, 0.0);
		auto BinOf = [&](double Rr) { return std::min(int(Rr / (Ext * 1.42) * Bins), Bins - 1); };
		for (size_t I = 0; I < L.size(); ++I) { Sb[BinOf(R[I])] += L[I]; Nb[BinOf(R[I])] += 1.0; }
		std::vector<std::complex<double>> S(L.size());
		double Ss = 0.0, Ns = 0.0;
		for (size_t I = 0; I < L.size(); ++I)
		{
			const double Mean = Sb[BinOf(R[I])] / std::max(Nb[BinOf(R[I])], 1.0);
			const double V = Mean > 0.0 ? L[I] / Mean - 1.0 : 0.0;
			const bool bAnn = R[I] > 0.1 && R[I] < 0.7;
			const double Taper = std::clamp((R[I] - 0.1) / 0.05, 0.0, 1.0) * std::clamp((0.7 - R[I]) / 0.05, 0.0, 1.0);
			S[I] = bAnn ? V * Taper : 0.0;
			if (bAnn) { Ss += V * V; Ns += 1.0; }
		}
		OutRms = std::sqrt(Ss / std::max(Ns, 1.0));
		// Separable DFT.
		const double Tau = 6.283185307179586;
		std::vector<std::complex<double>> Tmp(S.size());
		for (int J = 0; J < N; ++J)
			for (int Kx = 0; Kx < N; ++Kx)
			{
				std::complex<double> A = 0.0;
				for (int I = 0; I < N; ++I) { A += S[size_t(J) * N + I] * std::polar(1.0, -Tau * Kx * I / N); }
				Tmp[size_t(J) * N + Kx] = A;
			}
		OutBands.assign(6, 0.0);
		const double Du = 2.0 * Ext / N;   // field units a pixel
		for (int Ky = 0; Ky < N; ++Ky)
			for (int Kx = 0; Kx < N; ++Kx)
			{
				std::complex<double> A = 0.0;
				for (int J = 0; J < N; ++J) { A += Tmp[size_t(J) * N + Kx] * std::polar(1.0, -Tau * Ky * J / N); }
				const double Fx = (Kx <= N / 2 ? Kx : Kx - N) / (N * Du), Fy = (Ky <= N / 2 ? Ky : Ky - N) / (N * Du);
				const double F = std::sqrt(Fx * Fx + Fy * Fy);
				for (int B = 0; B < 6; ++B) { if (F >= std::ldexp(2.0, B) && F < std::ldexp(4.0, B)) { OutBands[B] += std::norm(A); } }
			}
	}

	FCoord CamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }

	std::vector<double> MarchLum(const FVolumeScene& Sc, const FCamera& Cm, bool bFine)
	{
		FVolumeScene F = Sc;
		if (bFine) { F.View.MarchFlags |= 2u; F.View.MaxSteps *= 8u; }
		FTemporalVolume Full;
		Full.Step(F, Cm, FV3{ 0, 0, 0 }, false, false, 2);
		std::vector<double> L(Full.Vol.size());
		for (size_t I = 0; I < L.size(); ++I)
		{
			L[I] = double(H::Uni_Luminance(H::float3(Full.Vol[I].x, Full.Vol[I].y, Full.Vol[I].z))) * PixelSolidAngle(Cm, Cm.Width, Cm.Height, double(I % Cm.Width) + 0.5, double(I / Cm.Width) + 0.5);
		}
		return L;
	}

	/** Top-down toward the centre: the march's error against the fine reference, and its crawl. */
	void TopDown(const FUniverseDef& D, double& OutErr, double& OutCrawl)
	{
		FUniverseModel M(D);
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		const double Kpc = Units::Kpc;
		const int Frames = 5, W = 128, Ht = 80;
		std::vector<double> PrevE;
		OutErr = 0.0; OutCrawl = 0.0;
		for (int F = 0; F < Frames; ++F)
		{
			const FCoord Cam = CamAt(FVec3d{ (2.0 - 0.03 * F) * Kpc, (1.2 - 0.018 * F) * Kpc, (6.0 - 0.12 * F) * Kpc });
			FRefPipeline Pl(G);
			Pl.bVelocityLod = false;
			Pl.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = W; Cm.Height = Ht; Cm.DeltaMag = 0.0;
			Cm.LookAlong(FV3{ -0.25, -0.15, -1.0 });
			const FVolumeScene Sc = VB.Build(Pl, Cam, FVec3f{}, Cm.View()).Subset(false);
			const std::vector<double> A = MarchLum(Sc, Cm, false), R = MarchLum(Sc, Cm, true);
			std::vector<double> E(A.size());
			double Sr = 0.0, Se = 0.0;
			for (size_t I = 0; I < A.size(); ++I) { E[I] = A[I] - R[I]; Sr += R[I]; Se += std::abs(E[I]); }
			if (F > 0)
			{
				double Sc2 = 0.0;
				for (size_t I = 0; I < E.size(); ++I) { Sc2 += std::abs(E[I] - PrevE[I]); }
				OutCrawl += Sc2 / std::max(Sr, 1e-300) / double(Frames - 1);
				OutErr += Se / std::max(Sr, 1e-300) / double(Frames - 1);
			}
			PrevE = E;
		}
	}
}

UNI_TEST(galaxy, warp_band)
{
	const FTexSet Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.warp_band", "D-220 structural warp and fine band").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FBind Bind(Tx);
	// The shaders' constants against the C++ knobs.
	std::string Text;
	{
		std::ifstream F("Shaders/Private/Core/UniFields.ush");   // relative to the repo root, as Data/Noise is
		std::stringstream B; B << F.rdbuf(); Text = B.str();
	}
	const double Cf = ShaderConstant(Text, "UNI_GAL_WARP_MIP_FLOOR"), Cq = ShaderConstant(Text, "UNI_GAL_FINE_FREQ");
	const double Cz = ShaderConstant(Text, "UNI_GAL_FINE_Z"), Cg = ShaderConstant(Text, "UNI_GAL_FINE_GAIN");
	const bool bConst = Cf == double(H::GKnobWarpMipFloor) && std::abs(Cq - double(H::GKnobFineFreq)) < 1e-6 && Cz == double(H::GKnobFineZ) && std::abs(Cg - double(H::GKnobFineGain)) < 1e-6;

	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const H::FUniFieldParams Home = HomeOf(D);
	const int N = UniVerify::Options().FullScale ? 256 : 160;
	std::vector<double> Bn, Bo;
	double Rn = 0.0, Ro = 0.0, ErrN = 0.0, CrawlN = 0.0, ErrO = 0.0, CrawlO = 0.0;
	FaceBands(Home, N, Bn, Rn);
	TopDown(D, ErrN, CrawlN);
	{
		FBeforeD220 Old;
		FaceBands(Home, N, Bo, Ro);
		TopDown(D, ErrO, CrawlO);
	}
	std::string Bands;
	bool bBands = true;
	double Lo = 1e9, Hi = 0.0;
	for (int B = 2; B < 6; ++B)   // 8-16 .. 64-128 cycles a unit
	{
		const double Rt = Bn[B] / std::max(Bo[B], 1e-300);
		Lo = std::min(Lo, Rt); Hi = std::max(Hi, Rt);
		bBands = bBands && Rt >= 0.8 && Rt <= 1.4;
		Bands += Uni::Fmt("%s%d-%d: %.2f", Bands.empty() ? "" : ", ", 2 << B, 4 << B, Rt);
	}
	const double Contrast = Rn / std::max(Ro, 1e-300);
	const bool bLook = bBands && std::abs(Contrast - 1.0) < 0.1;
	const bool bAlias = CrawlN < 0.7 * CrawlO && ErrN < ErrO;
	Uni::Check(2, "T2.galaxy.warp_band", "D-220 the disc warp at a structural level (warp_fine mip 2 at least) and its fine band as a bounded multiplier: the shaders' constants equal the C++ knobs; the home galaxy's face-on structure keeps the full warp's octave band powers (8-128 cycles a unit, 0.8-1.4 x) and contrast (10%); flying top-down toward the centre the march's crawl against a fine reference falls at least 30% and its error falls")
		.Metric("band_lo", Lo).Metric("band_hi", Hi).Metric("contrast", Contrast).Metric("crawl_before", CrawlO).Metric("crawl_d220", CrawlN).Metric("error_before", ErrO).Metric("error_d220", ErrN)
		.Pass(bConst && bLook && bAlias, Uni::Fmt("constants %s (floor %g, freq %g, z %g, gain %g); bands against the full warp %s; contrast x%.3f; top-down march vs fine: before error %.2f%% crawl %.2f%%, D-220 error %.2f%% crawl %.2f%%",
			bConst ? "match" : "DIFFER", Cf, Cq, Cz, Cg, Bands.c_str(), Contrast, 100.0 * ErrO, 100.0 * CrawlO, 100.0 * ErrN, 100.0 * CrawlN));
}
