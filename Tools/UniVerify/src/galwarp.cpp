// galwarp.cpp
// `univerify galwarp [--out DIR] [--quick]`: D-220 exploration (owner, 2026-11-10: the disc warp's
// finest level is too fine for the march, so it aliases and slides as the camera moves, and
// flying top-down toward a galaxy's centre shows concentric rings moving out; use a coarse warp
// for the structural variance and the finer level as a modulation, perceptually consistent).
//
// For each variant of the home galaxy's disc warp (warp_fine read at a mip floor; the fine band
// as a multiplier from warp_fine's A channel, an fBm with std 1: Uni_GalaxyFineMul), with the
// textures bound:
//  - aliasing: flights (top-down toward the centre; in the disc toward it) marched every pixel,
//    every frame against a fine reference of the same field (every step an eighth): each
//    frame's error and its crawl (how much the error pattern changes frame to frame);
//  - look: the face-on light (the field integrated through the disc) and a vertical
//    cross-section, written as float images; galwarp_report.py turns them into band powers and
//    pictures.
// Knobs: UniHlsl::GKnobWarpMipFloor / GKnobFineGain / GKnobFineFreq / GKnobFineZ (UniFields.ush's D-220 constants).

#include "UniTest.h"
#include "UniVerifyCommon.h"
#include "UniRenderRef.h"
#include "UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FVariant { std::string Name; float Floor = 2.0f; float Gain = 15.2f; float FineFreq = 3.3f; float FineZ = 4.0f; bool bNoWarp = false; };

	FUniverseDef SceneOf(const FTex& Tx, const FVariant& V)
	{
		FUniverseDef D = UniScenes::Flythrough();
		UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
		UniScenes::UseGalaxyPalettes(D);
		if (V.bNoWarp) { for (FAnchor& A : D.Anchors) { if (A.ArchetypeId == UniScenes::kGalaxy) { for (FFieldParams& P : A.EmitterParams) { P.V[24] = 0.0f; } } } }
		return D;
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

	void ParallelRows(int Rows, int Threads, const std::function<void(int)>& Fn)
	{
		std::vector<std::thread> Ts;
		for (int T = 0; T < Threads; ++T) { Ts.emplace_back([&, T] { for (int R = T; R < Rows; R += Threads) { Fn(R); } }); }
		for (auto& Th : Ts) { Th.join(); }
	}

	bool WriteF32(const std::string& Path, const std::vector<float>& V)
	{
		std::ofstream F(Path, std::ios::binary);
		if (!F) { return false; }
		F.write(reinterpret_cast<const char*>(V.data()), std::streamsize(V.size() * sizeof(float)));
		return bool(F);
	}

	FCoord CamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }

	/** Every pixel's luminance x solid angle, marched (fine: every step an eighth). */
	std::vector<double> MarchLum(const FVolumeScene& Sc, const FCamera& Cm, bool bFine, int Threads)
	{
		FVolumeScene F = Sc;
		if (bFine) { F.View.MarchFlags |= 2u; F.View.MaxSteps *= 8u; }
		const auto V = ToHlsl(F.View);
		const auto Cb = F.CarrierBuf(); const auto Pb = F.ParamBuf(); const auto Lb = F.LutBuf();
		std::vector<double> L(size_t(Cm.Width) * Cm.Height, 0.0);
		ParallelRows(Cm.Height, Threads, [&](int Y)
		{
			for (int X = 0; X < Cm.Width; ++X)
			{
				bool bS = false;
				const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bS);
				L[size_t(Y) * Cm.Width + X] = double(H::Uni_Luminance(R.Radiance)) * PixelSolidAngle(Cm, Cm.Width, Cm.Height, X + 0.5, Y + 0.5);
			}
		});
		return L;
	}

	struct FFlight { double Error = 0, Crawl = 0, P95 = 0; int Frames = 0; std::vector<double> LastA, LastR; };

	FFlight Fly(const FUniverseDef& D, FVec3d Pos, FV3 Look, FVec3d Step, int Frames, int W, int Ht, int Threads)
	{
		FUniverseModel M(D);
		FRefGenerator G(M);
		FVolumeBuilder VB(M, G);
		FFlight Out;
		std::vector<double> PrevE;
		for (int F = 0; F < Frames; ++F)
		{
			const FVec3d P{ Pos.X + Step.X * F, Pos.Y + Step.Y * F, Pos.Z + Step.Z * F };
			const FCoord Cam = CamAt(P);
			FRefPipeline Pl(G);
			Pl.bVelocityLod = false;
			Pl.Update(Cam, FVec3f{});
			FCamera Cm; Cm.Coord = Cam; Cm.Width = W; Cm.Height = Ht; Cm.DeltaMag = 0.0;
			Cm.LookAlong(Look);
			const FVolumeScene Sc = VB.Build(Pl, Cam, FVec3f{}, Cm.View()).Subset(false);
			const std::vector<double> A = MarchLum(Sc, Cm, false, Threads), R = MarchLum(Sc, Cm, true, Threads);
			std::vector<double> E(A.size());
			double Sr = 0, Se = 0;
			for (size_t I = 0; I < A.size(); ++I) { E[I] = A[I] - R[I]; Sr += R[I]; Se += std::abs(E[I]); }
			if (F > 0)
			{
				double Sc2 = 0;
				for (size_t I = 0; I < E.size(); ++I) { Sc2 += std::abs(E[I] - PrevE[I]); }
				Out.Crawl += Sc2 / std::max(Sr, 1e-300);
				Out.Error += Se / std::max(Sr, 1e-300);
				const double Mean = Sr / double(R.size());
				std::vector<double> Rel;
				for (size_t I = 0; I < R.size(); ++I) { if (R[I] > 0.01 * Mean) { Rel.push_back(std::abs(E[I]) / R[I]); } }
				std::sort(Rel.begin(), Rel.end());
				if (!Rel.empty()) { Out.P95 += Rel[size_t(0.95 * double(Rel.size() - 1))]; }
				++Out.Frames;
			}
			PrevE = E;
			Out.LastA = A; Out.LastR = R;
		}
		if (Out.Frames > 0) { Out.Error /= Out.Frames; Out.Crawl /= Out.Frames; Out.P95 /= Out.Frames; }
		return Out;
	}

	/** The face-on light: the field integrated over +-6 flared scale heights about the disc's
	 *  centre, on an N x N grid over [-Ext, Ext]^2 (field units). */
	std::vector<float> FaceOn(const H::FUniFieldParams& Par, int N, float Ext, int Threads)
	{
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(Par);
		const float Slab = H::Uni_GalaxyDiscSlab(Par);
		const float Hs = std::max(Par.A.y, 1.0e-6f);
		std::vector<float> Img(size_t(N) * N, 0.0f);
		const int Nz = 40;
		ParallelRows(N, Threads, [&](int J)
		{
			for (int I = 0; I < N; ++I)
			{
				const float X = -Ext + 2.0f * Ext * (float(I) + 0.5f) / float(N), Y = Ext - 2.0f * Ext * (float(J) + 0.5f) / float(N);
				const H::FUniGalaxyShared S = H::Uni_GalaxySharedK(H::float3(X, Y, 0.0f), Par, K, 0.0f, Slab, false, false);
				const float Half = 6.0f * Hs * S.Flare;
				double Sum = 0.0;
				for (int Z = 0; Z < Nz; ++Z)
				{
					const float Zz = S.WarpZ - Half + 2.0f * Half * (float(Z) + 0.5f) / float(Nz);
					Sum += double(H::UniField_Galaxy(H::float3(X, Y, Zz), Par));
				}
				Img[size_t(J) * N + I] = float(Sum * 2.0 * Half / Nz);
			}
		});
		return Img;
	}

	/** A vertical cross-section: x in [X0, X1] at y = Y0, height +-V flared scale heights about the
	 *  centre. Row 0 is the top. */
	std::vector<float> CrossSection(const H::FUniFieldParams& Par, int W, int Hpx, float X0, float X1, float Y0, float V, int Threads)
	{
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(Par);
		const float Slab = H::Uni_GalaxyDiscSlab(Par);
		const float Hs = std::max(Par.A.y, 1.0e-6f);
		std::vector<float> Img(size_t(W) * Hpx, 0.0f);
		ParallelRows(W, Threads, [&](int I)
		{
			const float X = X0 + (X1 - X0) * (float(I) + 0.5f) / float(W);
			const H::FUniGalaxyShared S = H::Uni_GalaxySharedK(H::float3(X, Y0, 0.0f), Par, K, 0.0f, Slab, false, false);
			for (int J = 0; J < Hpx; ++J)
			{
				const float Vv = V * (1.0f - 2.0f * (float(J) + 0.5f) / float(Hpx));
				Img[size_t(J) * W + I] = H::UniField_Galaxy(H::float3(X, Y0, S.WarpZ + Vv * Hs * S.Flare), Par);
			}
		});
		return Img;
	}

	std::vector<float> ToF(const std::vector<double>& V) { std::vector<float> O(V.size()); for (size_t I = 0; I < V.size(); ++I) { O[I] = float(V[I]); } return O; }

	std::string J(double V) { char B[64]; std::snprintf(B, sizeof(B), "%.6g", std::isfinite(V) ? V : 0.0); return B; }
}

int UniGalWarpCommand(int Argc, char** Argv)
{
	std::string Out = "Verification/galwarp";
	bool bQuick = false;
	bool bLookOnly = false;
	int Threads = int(std::max(1u, std::thread::hardware_concurrency()));
	std::vector<std::string> Only;
	for (int I = 2; I < Argc; ++I)
	{
		if (!std::strcmp(Argv[I], "--out") && I + 1 < Argc) { Out = Argv[++I]; }
		else if (!std::strcmp(Argv[I], "--quick")) { bQuick = true; }
		else if (!std::strcmp(Argv[I], "--only") && I + 1 < Argc) { Only.push_back(Argv[++I]); }
		else if (!std::strcmp(Argv[I], "--look-only")) { bLookOnly = true; }
	}
	const FTex Tx;
	if (!Tx.bOk) { std::fprintf(stderr, "galaxy textures not loaded: %s\n", Tx.Err.c_str()); return 2; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &Tx.T[I]); }

	// The D-220 values are UniFields.ush's (UniHlslShim.h's knob defaults); the others are the
	// exploration's record (DECISIONS D-220 has every variant tried and its numbers).
	std::vector<FVariant> Vars = {
		{ "before D-220 (full warp_fine)", 0.0f, 0.0f, 3.3f, 4.0f, false },
		{ "structural warp only (mip 2)", 2.0f, 0.0f, 3.3f, 4.0f, false },
		{ "D-220 (structural warp + fine band)", 2.0f, 15.2f, 3.3f, 4.0f, false },
		{ "fine band unstretched (x1 height)", 2.0f, 15.2f, 3.3f, 1.0f, false },
		{ "no disc warp", 0.0f, 0.0f, 3.3f, 4.0f, true },
	};

	const double Kpc = Units::Kpc;
	struct FPath { const char* Name; FVec3d Pos; FV3 Look; FVec3d Step; };
	const FPath Paths[] = {
		{ "top-down toward the centre", { 2.0 * Kpc, 1.2 * Kpc, 6.0 * Kpc }, { -0.25, -0.15, -1.0 }, { -0.03 * Kpc, -0.018 * Kpc, -0.12 * Kpc } },
		{ "in the disc toward the centre", { 8.0 * Kpc, 1.0 * Kpc, 0.03 * Kpc }, { -1.0, 0.05, 0.01 }, { -0.04 * Kpc, 0.0, 0.0 } },
	};
	const int W = bQuick ? 128 : 192, Ht = bQuick ? 80 : 120, Frames = bQuick ? 4 : 8;
	const int FaceN = bQuick ? 192 : 384;
	const float FaceExt = 0.75f;
	const int Xw = 720, Xh = 180;
	std::string Js = "{\n  \"view\": [" + std::to_string(W) + ", " + std::to_string(Ht) + "], \"face\": [" + std::to_string(FaceN) + ", " + J(FaceExt) + "], \"xsec\": [" + std::to_string(Xw) + ", " + std::to_string(Xh) + ", 0.12, 0.72, 0.03, 5.0],\n  \"variants\": [\n";
	int Written = 0;
	for (size_t Vi = 0; Vi < Vars.size(); ++Vi)
	{
		const FVariant& V = Vars[Vi];
		if (!Only.empty() && std::find(Only.begin(), Only.end(), std::to_string(Vi)) == Only.end()) { continue; }
		H::GKnobWarpMipFloor = V.Floor;
		H::GKnobFineGain = V.Gain;
		H::GKnobFineFreq = V.FineFreq;
		H::GKnobFineZ = V.FineZ;
		const FUniverseDef D = SceneOf(Tx, V);
		const H::FUniFieldParams Par = HomeOf(D);
		const auto T0 = std::chrono::steady_clock::now();
		const std::string Tag = std::to_string(Vi);
		WriteF32(Out + "/face_" + Tag + ".f32", FaceOn(Par, FaceN, FaceExt, Threads));
		WriteF32(Out + "/xsec_" + Tag + ".f32", CrossSection(Par, Xw, Xh, 0.12f, 0.72f, 0.03f, 5.0f, Threads));
		std::printf("== %s (look images %.0f s)\n", V.Name.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count());
		Js += std::string(Written++ ? ",\n" : "") + "    {\"name\": \"" + V.Name + "\", \"tag\": \"" + Tag + "\", \"floor\": " + J(V.Floor) + ", \"gain\": " + J(V.Gain) + ", \"fine_freq\": " + J(V.FineFreq) + ", \"fine_z\": " + J(V.FineZ) + ", \"paths\": [";
		for (size_t Pi = 0; Pi < (bLookOnly ? 0u : sizeof(Paths) / sizeof(Paths[0])); ++Pi)
		{
			const FPath& P = Paths[Pi];
			const auto T1 = std::chrono::steady_clock::now();
			const FFlight F = Fly(D, P.Pos, P.Look, P.Step, Frames, W, Ht, Threads);
			WriteF32(Out + "/view_" + Tag + "_" + std::to_string(Pi) + "_march.f32", ToF(F.LastA));
			WriteF32(Out + "/view_" + Tag + "_" + std::to_string(Pi) + "_fine.f32", ToF(F.LastR));
			std::printf("   %-30s march vs fine: error %6.2f%%  p95 %6.1f%%  crawl %6.2f%%   (%.0f s)\n", P.Name, 100.0 * F.Error, 100.0 * F.P95, 100.0 * F.Crawl,
				std::chrono::duration<double>(std::chrono::steady_clock::now() - T1).count());
			std::fflush(stdout);
			Js += std::string(Pi ? ", " : "") + "{\"name\": \"" + P.Name + "\", \"error\": " + J(F.Error) + ", \"p95\": " + J(F.P95) + ", \"crawl\": " + J(F.Crawl) + "}";
		}
		Js += "]}";
	}
	Js += "\n  ]\n}\n";
	{ std::ofstream F(Out + "/metrics.json"); F << Js; }
	H::GKnobWarpMipFloor = 2.0f;
	H::GKnobFineGain = 15.2f;
	H::GKnobFineFreq = 3.3f;
	H::GKnobFineZ = 4.0f;
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	std::printf("wrote %s/metrics.json\n", Out.c_str());
	return 0;
}
