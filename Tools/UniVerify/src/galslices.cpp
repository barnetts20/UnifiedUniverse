// galslices.cpp
// `univerify galslices [--out DIR] [--quick]`: the sliced galaxy-map prototype (owner's idea,
// 2026-11-09). Today a galaxy sample reads warp_fine (3D) to displace the disc, then the 2D map at
// the displaced point (D-212). The prototype bakes the warp into the map instead: N slices through
// the disc's height, each texel the map at its own warped point, so a sample makes one trilinear
// read of a 3D map (x, y, slice) and no warp_fine read. The first bake of D-212 warped at the
// mid-plane only (one slice), which gave columns: every height had the same in-plane pattern.
//
// Per slice texel (x, y, k) at height z_k:
//   Pd = (x, y, z_k) displaced by warp_fine exactly as the field does (Uni_GalaxyTexTerms);
//   R  = the light's in-plane factor at Pd.xy, A = its arm part (Uni_GalaxyMapTexel);
//   G  = the disc's vertical centre seen from z_k: the mode warp's centre at Pd.xy minus the warp's
//        own vertical displacement (Pd.z - z_k), so the sample's |z - G| is the field's |Pd.z - centre|.
// Slices are either relative (z_k = centre(x, y) + u_k Span H flare(x, y): the mode warp's centre
// and the flare computed analytically at the sample before the lookup) or absolute (z_k = u_k Span,
// Span covering the mode warp). The detail texture (fBm, knots) and the bulge's warp_large are read
// as today. Optional jitter: the slice coordinate nudged by the detail fBm the sample already reads.
//
// Measured against UniField_Galaxy (the field the volume and placement must match), for three
// parameter sets (the home galaxy, a fast warp at the top of the rolled range, a thick dwarf-like
// disc): light-weighted per-point error (importance-sampled over the disc), and two perspective
// renders inside / just above the home disc (per-pixel p95 and total light), plus vertical
// cross-sections (the columns show there). Also the home galaxy with a smoother warp (warp_fine
// read at mip 1 or 2: the owner's "drop a warp octave"), the slices against that field, and that
// field against today's (the look it changes), and with the warp varying 4 or 8 times slower in
// height (its in-plane pattern kept). Writes
// metrics.json and float images (cross-sections and renders) for Tools/UniVerify/galslices_report.py.
// Prototype only: nothing in the engine or the shader cores changes.

#include "UniTest.h"
#include "UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniGalaxyMap.h"
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
namespace H = UniHlsl;

namespace
{
	using H::float3;
	using H::float4;

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

	H::FUniFieldParams ParOf(const FFieldParams& P)
	{
		const FFieldParamsGPU G = P.Pack();
		H::FUniFieldParams O;
		std::memcpy(static_cast<void*>(&O), &G, sizeof(O));
		return O;
	}

	struct FGalaxy
	{
		std::string Name;
		H::FUniFieldParams Par;      // as the field reads it
		H::FUniFieldParams NoWarp;   // G.x = 0: Uni_GalaxyTexTerms skips warp_fine (the sliced sample)
		H::FUniGalaxyConst K;
		float Slab = 0.0f;
		// A changed warp (the reference is then that field, FieldOf): warp_fine read at mip WarpMip
		// (a smoother warp; never finer than D-220's UNI_GAL_WARP_MIP_FLOOR), or with its frame's
		// height stretched ZStretch times (Fr.z = 2 z / ZStretch: the same in-plane warp, varying
		// ZStretch times slower with height). D-220's fine band is unchanged by either.
		float WarpMip = 0.0f;
		float ZStretch = 1.0f;
		bool Changed() const { return WarpMip > 0.0f || ZStretch != 1.0f; }
	};

	FGalaxy MakeGalaxy(const std::string& Name, const H::FUniFieldParams& P, float InWarpMip = 0.0f, float InZStretch = 1.0f)
	{
		FGalaxy G;
		G.Name = Name;
		G.WarpMip = InWarpMip;
		G.ZStretch = InZStretch;
		G.Par = P;
		G.NoWarp = P;
		G.NoWarp.G.x = 0.0f;
		G.K = H::Uni_GalaxyConstOf(P);
		G.Slab = H::Uni_GalaxyDiscSlab(P);
		return G;
	}

	/** The disc's warped point: Uni_GalaxyTexTerms' warp_fine branch (footprint 0), with the
	 *  galaxy's changed warp if it has one. */
	float3 WarpPd(const FGalaxy& G, const float3& P)
	{
		if (!G.Changed())
		{
			float3 Pd, Pb;
			float4 Dt;
			uint32_t bDt = 0u;
			float FineN = 0.0f;
			H::Uni_GalaxyTexTerms(P, G.Par, G.K, 0.0f, G.Slab, true, false, Pd, Pb, Dt, bDt, FineN);
			return Pd;
		}
		const H::FUniFieldParams& Par = G.Par;
		const uint32_t Ho = G.K.Ho;
		const float3 Off = float3(float(Ho & 0x3FFu), float((Ho >> 10u) & 0x3FFu), float((Ho >> 20u) & 0x3FFu)) * (1.0f / 1024.0f);
		const float R0 = std::sqrt(P.x * P.x + P.y * P.y);
		const float Rn0 = std::clamp(R0 / std::max(Par.D.x, 1.0e-6f), 0.0f, 1.0f);
		const float U0 = std::min(-std::log(std::max(R0, 1.0e-4f)), 6.0f);
		const float Tw = (U0 + 0.5f * std::max(Par.F.y, 0.0f) * U0 * U0) / std::max(Par.B.w, 1.0e-3f) * Rn0 * Par.H.z;
		const float St = std::sin(Tw), Ct = std::cos(Tw);
		const float3 Fr = float3(P.x * Ct - P.y * St, P.x * St + P.y * Ct, 2.0f * P.z / G.ZStretch);
		if (Par.G.x == 0.0f || std::abs(P.z) > G.Slab) { return P; }
		const float4 W = H::UniGalaxyTexSample(0, float3(Fr.x * Par.G.z + Off.x, Fr.y * Par.G.z + Off.y, Fr.z * Par.G.z + Off.z), std::max(G.WarpMip, float(UNI_GAL_WARP_MIP_FLOOR)));
		const float A = Par.G.x * H::Uni_GalaxyWarpInner(R0, Par);
		return float3(P.x + W.x * A, P.y + W.y * A, P.z + W.z * A);
	}

	/** D-220's fine band at P (footprint 0): Uni_GalaxyTexTerms' OutFineN with the galaxy's own
	 *  parameters (the band does not depend on the warp's mip or height stretch). */
	float FineNAt(const FGalaxy& G, const float3& P)
	{
		float3 Pd, Pb;
		float4 Dt;
		uint32_t bDt = 0u;
		float FineN = 0.0f;
		H::Uni_GalaxyTexTerms(P, G.Par, G.K, 0.0f, G.Slab, true, false, Pd, Pb, Dt, bDt, FineN);
		return FineN;
	}

	/** The galaxy's field; with a changed warp, Uni_GalaxySharedK's state rebuilt at that warp's
	 *  point (detail, bulge and D-220's fine band as the field reads them). */
	float FieldOf(const FGalaxy& G, const float3& P)
	{
		if (!G.Changed()) { return H::UniField_Galaxy(P, G.Par); }
		const float Cut = G.Par.D.x;
		if (P.x * P.x + P.y * P.y + P.z * P.z >= Cut * Cut) { return 0.0f; }
		const float3 Pd = WarpPd(G, P);
		const H::FUniGalaxyShared S0 = H::Uni_GalaxySharedK(P, G.NoWarp, G.K, 0.0f, G.Slab, true, H::Uni_GalaxyNeedsDetail(G.Par));   // detail, bulge
		H::FUniGalaxyShared S = H::Uni_GalaxySharedK(Pd, G.NoWarp, G.K, 0.0f, G.Slab, false, false);   // the in-plane state at Pd
		S.Pd = Pd;
		S.Pb = S0.Pb;
		S.Dt = S0.Dt;
		S.bDetail = S0.bDetail;
		S.FineN = FineNAt(G, P);
		return H::Uni_GalaxyFromShared(S, P, G.Par, 0.0f);
	}

	/** Side measurement: the field with the bulge's warp_large read only inside InRb (fading out
	 *  over [0.6, 1] InRb) instead of inside 0.5 (fading over [0.3, 0.5]). InRb = 0.5 is D-179's read (the field before D-218, which adopted InRb = 8 r_e). */
	float FieldBulgeWithin(const FGalaxy& G, const float3& P, float InRb)
	{
		const H::FUniFieldParams& Par = G.Par;
		const float Cut = Par.D.x;
		const float R2 = P.x * P.x + P.y * P.y + P.z * P.z;
		if (R2 >= Cut * Cut) { return 0.0f; }
		H::FUniFieldParams NoB = Par;
		NoB.G.y = 0.0f;
		H::FUniGalaxyShared S = H::Uni_GalaxySharedK(P, NoB, G.K, 0.0f, G.Slab, true, H::Uni_GalaxyNeedsDetail(Par));
		const float R = std::sqrt(R2);
		if (Par.G.y != 0.0f && Par.B.y > 0.0f && R < InRb)
		{
			const uint32_t Ho = G.K.Ho;
			const float3 Off = float3(float(Ho & 0x3FFu), float((Ho >> 10u) & 0x3FFu), float((Ho >> 20u) & 0x3FFu)) * (1.0f / 1024.0f);
			const float4 W = H::UniGalaxyTexSample(1, float3(P.x * 0.5f * Par.G.z + Off.z, P.y * 0.5f * Par.G.z + Off.x, P.z * 0.5f * Par.G.z + Off.y), 0.0f);
			const float Fb = 1.0f - std::clamp((R - 0.6f * InRb) / (0.4f * InRb), 0.0f, 1.0f);
			S.Pb = float3(P.x + W.x * Par.G.y * Fb, P.y + W.y * Par.G.y * Fb, P.z + W.z * Par.G.y * Fb);
		}
		return H::Uni_GalaxyFromShared(S, P, Par, 0.0f);
	}

	// ---------------------------------------------------------------------------------------
	// The sliced map.
	// ---------------------------------------------------------------------------------------
	struct FCfg
	{
		std::string Name;
		int N = 4;            // slices (0: the 2D map of today, warp_fine read per sample)
		bool bRel = true;     // slices relative to the disc's centre, in flared scale heights
		float SpanH = 3.0f;   // half-span in (flared) scale heights; absolute: plus the mode warp
		float Jitter = 0.0f;  // slice-coordinate nudge by the detail fBm, in slices
		u32 Size = 1024u;
	};

	/** The disc's frame at (X, Y): the slices' centre and half-span. */
	void FrameAt(const FGalaxy& G, const FCfg& C, float X, float Y, float& OutC, float& OutHalf)
	{
		const H::FUniFieldParams& P = G.Par;
		const float Hs = std::max(P.A.y, 1.0e-6f);
		if (C.bRel)
		{
			const H::FUniGalaxyShared S = H::Uni_GalaxySharedK(float3(X, Y, 0.0f), P, G.K, 0.0f, G.Slab, false, false);
			OutC = S.WarpZ;
			OutHalf = C.SpanH * Hs * S.Flare;
		}
		else
		{
			OutC = 0.0f;
			OutHalf = std::abs(P.E.x) + C.SpanH * Hs * (1.0f + std::max(P.E.w, 0.0f));
		}
	}

	float SliceU(int K, int N) { return N <= 1 ? 0.0f : -1.0f + 2.0f * float(K) / float(N - 1); }

	float Half(float V) { return H::UniHalfToFloat(FloatToHalf(V)); }

	struct FSliceMap
	{
		u32 Size = 0;
		int N = 0;
		std::vector<float> T;   // (R light, G centre seen from the slice, B 0, A arm), ((k * Size + y) * Size + x) * 4

		void Bake(const FGalaxy& G, const FCfg& C, int Threads)
		{
			Size = C.Size;
			N = std::max(C.N, 1);
			T.assign(size_t(N) * Size * Size * 4u, 0.0f);
			auto Rows = [&](u32 R0, u32 R1)
			{
				for (u32 R = R0; R < R1; ++R)
				{
					const int K = int(R / Size);
					const u32 Y = R % Size;
					for (u32 X = 0; X < Size; ++X)
					{
						const float Fx = (float(X) + 0.5f) / float(Size) * 2.0f - 1.0f;
						const float Fy = (float(Y) + 0.5f) / float(Size) * 2.0f - 1.0f;
						float Cz = 0.0f, Hh = 0.0f;
						FrameAt(G, C, Fx, Fy, Cz, Hh);
						const float Zk = Cz + Hh * SliceU(K, N);
						const float3 Pd = WarpPd(G, float3(Fx, Fy, Zk));
						const float4 M = H::Uni_GalaxyMapTexel(Pd.x, Pd.y, G.Par, G.Par, G.K, false);
						float* O = &T[((size_t(K) * Size + Y) * Size + X) * 4u];
						O[0] = Half(M.x);
						O[1] = Half(M.z - (Pd.z - Zk));
						O[2] = 0.0f;
						O[3] = Half(M.w);
					}
				}
			};
			const u32 Total = u32(N) * Size;
			std::vector<std::thread> Ts;
			for (int I = 0; I < Threads; ++I) { Ts.emplace_back(Rows, u32(u64(Total) * I / Threads), u32(u64(Total) * (I + 1) / Threads)); }
			for (auto& Th : Ts) { Th.join(); }
		}

		/** Trilinear: bilinear between texel centres (clamped), linear between slices (clamped). */
		float4 Sample(float U, float V, float Q) const
		{
			const float X = U * float(Size) - 0.5f, Y = V * float(Size) - 0.5f;
			const float Fx = std::floor(X), Fy = std::floor(Y);
			const float Ax = X - Fx, Ay = Y - Fy;
			auto Cl = [this](int I) { return std::clamp(I, 0, int(Size) - 1); };
			const int X0 = Cl(int(Fx)), X1 = Cl(int(Fx) + 1), Y0 = Cl(int(Fy)), Y1 = Cl(int(Fy) + 1);
			const float Qc = std::clamp(Q, 0.0f, float(N - 1));
			const int K0 = std::min(int(Qc), N - 1), K1 = std::min(K0 + 1, N - 1);
			const float Az = Qc - float(K0);
			float Out[4] = { 0, 0, 0, 0 };
			for (int Kk = 0; Kk < 2; ++Kk)
			{
				const int K = Kk == 0 ? K0 : K1;
				const float Wz = Kk == 0 ? 1.0f - Az : Az;
				if (Wz <= 0.0f) { continue; }
				auto At = [&](int I, int J, int Ch) { return T[((size_t(K) * Size + J) * Size + I) * 4u + Ch]; };
				for (int Ch = 0; Ch < 4; ++Ch)
				{
					const float A = At(X0, Y0, Ch) + (At(X1, Y0, Ch) - At(X0, Y0, Ch)) * Ax;
					const float B = At(X0, Y1, Ch) + (At(X1, Y1, Ch) - At(X0, Y1, Ch)) * Ax;
					Out[Ch] += Wz * (A + (B - A) * Ay);
				}
			}
			return float4(Out[0], Out[1], Out[2], Out[3]);
		}

		size_t Bytes() const { return T.size() / 4u * 8u; }   // RGBA16F, mip 0
	};

	/** A galaxy's light at InP from its sliced map (footprint 0): Uni_GalaxyMapEval's light with
	 *  the map read moved from (warped x, y) to (x, y, slice): no structural warp read. D-220's
	 *  fine band stays a per-sample read (a 3D multiplier the slices cannot hold). */
	float EvalSliced(const FGalaxy& G, const FCfg& C, const FSliceMap& M, const float3& P)
	{
		const H::FUniFieldParams& Par = G.Par;
		const float Cut = Par.D.x;
		const float R2 = P.x * P.x + P.y * P.y + P.z * P.z;
		if (R2 >= Cut * Cut) { return 0.0f; }
		float3 Pd, Pb;
		float4 Dt;
		uint32_t bDt = 0u;
		float FineN = 0.0f;
		H::Uni_GalaxyTexTerms(P, G.NoWarp, G.K, 0.0f, G.Slab, true, H::Uni_GalaxyNeedsDetail(Par), Pd, Pb, Dt, bDt, FineN);
		float DetailN = 0.0f, Knot = 0.0f, Cloud = 1.0f;
		if (bDt != 0u) { H::Uni_GalaxyDetailTerms(Dt, Par, DetailN, Knot, Cloud); }
		float Cz = 0.0f, Hh = 1.0f;
		FrameAt(G, C, P.x, P.y, Cz, Hh);
		float Q = M.N <= 1 ? 0.0f : ((P.z - Cz) / Hh + 1.0f) * 0.5f * float(M.N - 1);
		if (C.Jitter != 0.0f && bDt != 0u) { Q += C.Jitter * (2.0f * Dt.z - 1.0f); }
		const float4 T = M.Sample(P.x * 0.5f + 0.5f, P.y * 0.5f + 0.5f, Q);
		const float Rn = std::clamp(std::sqrt(P.x * P.x + P.y * P.y) / std::max(Cut, 1.0e-6f), 0.0f, 1.0f);
		const float Flare = Par.E.w > 0.0f ? 1.0f + Par.E.w * Rn : 1.0f;
		const float Z = std::abs(P.z - T.y);
		float Disc = 0.0f;
		if (Par.A.z > 0.0f)
		{
			const float Hs = std::max(Par.A.y, 1.0e-6f) * Flare;
			const float V = std::exp(-Z / Hs);
			const float Fd = Par.H.x != 0.0f ? std::max(1.0f + Par.H.x * DetailN, 0.0f) : 1.0f;
			const float Wk = Par.H.y != 0.0f ? std::max(1.0f + Par.H.y * Knot, 0.0f) : 1.0f;
			Disc = V * Fd * H::Uni_GalaxyFineMul(FineNAt(G, P), Par) * (std::max(T.x, 0.0f) + std::max(T.w, 0.0f) * (Wk - 1.0f));
		}
		const float Bulge = H::Uni_GalaxyBulgeAt(Pb, Par, 0.0f);
		const float R = std::sqrt(R2);
		return (Disc + Bulge) * std::clamp((Cut - R) / (0.05f * Cut), 0.0f, 1.0f);
	}

	// ---------------------------------------------------------------------------------------
	// A variant: the field, today's 2D map, or a sliced map.
	// ---------------------------------------------------------------------------------------
	struct FVariant
	{
		FCfg Cfg;
		bool bField = false;
		FGalaxyMap Map2D;
		FSliceMap Slices;
		double BakeMs = 0.0;

		float Eval(const FGalaxy& G, const float3& P) const
		{
			if (bField) { return FieldOf(G, P); }
			if (Cfg.N == 0)
			{
				float4 Comp;
				return H::Uni_GalaxyMapLight(P, G.Par, G.K, 0u, Comp);
			}
			return EvalSliced(G, Cfg, Slices, P);
		}

		size_t Bytes() const { return bField ? 0u : (Cfg.N == 0 ? Map2D.Bytes() : Slices.Bytes()); }
	};

	void ParallelRows(int Rows, int Threads, const std::function<void(int)>& Fn)
	{
		std::vector<std::thread> Ts;
		for (int T = 0; T < Threads; ++T)
		{
			Ts.emplace_back([&, T] { for (int R = T; R < Rows; R += Threads) { Fn(R); } });
		}
		for (auto& Th : Ts) { Th.join(); }
	}

	u64 SplitMix(u64& S) { u64 Z = (S += 0x9E3779B97F4A7C15ull); Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull; Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull; return Z ^ (Z >> 31); }
	double Unit(u64& S) { return (double(SplitMix(S) >> 11) + 0.5) * (1.0 / 9007199254740992.0); }

	// ---------------------------------------------------------------------------------------
	// Per-point error: points importance-sampled over the disc (radius ~ Gamma(2, h_R), height
	// ~ Laplace(2 H flare) about the centre), each weighted by 1 / pdf, so the sums are integrals
	// over the galaxy's volume: L1 = int |m - f| / int f (light-weighted mean relative error).
	// ---------------------------------------------------------------------------------------
	struct FPoint { float3 P; double W; bool bOff; };

	std::vector<FPoint> SamplePoints(const FGalaxy& G, int Count, u64 Seed)
	{
		std::vector<FPoint> Out;
		Out.reserve(Count);
		const double Hr = std::max(double(G.Par.A.x), 1e-3);
		const double Hs = std::max(double(G.Par.A.y), 1e-6);
		u64 S = Seed;
		FCfg Rel;
		while (int(Out.size()) < Count)
		{
			const double R = -Hr * std::log(Unit(S) * Unit(S));
			if (R >= 0.98) { continue; }
			const double Th = 6.283185307179586 * Unit(S);
			const float X = float(R * std::cos(Th)), Y = float(R * std::sin(Th));
			const H::FUniGalaxyShared Sh = H::Uni_GalaxySharedK(float3(X, Y, 0.0f), G.Par, G.K, 0.0f, G.Slab, false, false);
			const double B = 2.0 * Hs * double(Sh.Flare);
			const double U = Unit(S);
			const double V = U < 0.5 ? B * std::log(2.0 * U) : -B * std::log(2.0 * (1.0 - U));
			// pdf (x, y) = exp(-r / h) / (2 pi h^2); pdf (v) = exp(-|v| / b) / (2 b): constants cancel in ratios.
			const double W = std::exp(R / Hr) * 2.0 * B * std::exp(std::abs(V) / B);
			Out.push_back({ float3(X, Y, float(double(Sh.WarpZ) + V)), W, std::abs(V) > Hs * double(Sh.Flare) });
		}
		return Out;
	}

	struct FPointStats { double L1 = 0, L1Off = 0, Light = 0, P95 = 0, P50 = 0; };

	FPointStats PointStats(const std::vector<FPoint>& Pts, const std::vector<float>& F, const std::vector<float>& M)
	{
		FPointStats O;
		double Sf = 0, Sm = 0, Se = 0, SfOff = 0, SeOff = 0;
		std::vector<std::pair<double, double>> Rel;
		Rel.reserve(Pts.size());
		for (size_t I = 0; I < Pts.size(); ++I)
		{
			const double W = Pts[I].W, Fv = F[I], Mv = M[I];
			Sf += W * Fv; Sm += W * Mv; Se += W * std::abs(Mv - Fv);
			if (Pts[I].bOff) { SfOff += W * Fv; SeOff += W * std::abs(Mv - Fv); }
			if (Fv > 0.0) { Rel.push_back({ std::abs(Mv - Fv) / Fv, W * Fv }); }
		}
		O.L1 = Se / std::max(Sf, 1e-300);
		O.L1Off = SeOff / std::max(SfOff, 1e-300);
		O.Light = Sm / std::max(Sf, 1e-300) - 1.0;
		std::sort(Rel.begin(), Rel.end());
		double Tot = 0; for (auto& R : Rel) { Tot += R.second; }
		double Acc = 0; bool b50 = false;
		for (auto& R : Rel)
		{
			Acc += R.second;
			if (!b50 && Acc >= 0.5 * Tot) { O.P50 = R.first; b50 = true; }
			if (Acc >= 0.95 * Tot) { O.P95 = R.first; break; }
		}
		return O;
	}

	// ---------------------------------------------------------------------------------------
	// Images.
	// ---------------------------------------------------------------------------------------
	bool WriteF32(const std::string& Path, const std::vector<float>& V)
	{
		std::ofstream F(Path, std::ios::binary);
		if (!F) { return false; }
		F.write(reinterpret_cast<const char*>(V.data()), std::streamsize(V.size() * sizeof(float)));
		return bool(F);
	}

	/** A vertical cross-section: x in [X0, X1] at y = Y0, height v in [-V, V] flared scale heights
	 *  about the centre (so the mode warp does not tilt the picture). Row 0 is the top. */
	std::vector<float> CrossSection(const FGalaxy& G, const std::function<float(const float3&)>& Fn, int W, int Hpx, float X0, float X1, float Y0, float V, int Threads)
	{
		std::vector<float> Img(size_t(W) * Hpx, 0.0f);
		const float Hs = std::max(G.Par.A.y, 1.0e-6f);
		ParallelRows(W, Threads, [&](int I)
		{
			const float X = X0 + (X1 - X0) * (float(I) + 0.5f) / float(W);
			const H::FUniGalaxyShared Sh = H::Uni_GalaxySharedK(float3(X, Y0, 0.0f), G.Par, G.K, 0.0f, G.Slab, false, false);
			for (int J = 0; J < Hpx; ++J)
			{
				const float Vv = V * (1.0f - 2.0f * (float(J) + 0.5f) / float(Hpx));
				Img[size_t(J) * W + I] = Fn(float3(X, Y0, Sh.WarpZ + Vv * Hs * Sh.Flare));
			}
		});
		return Img;
	}

	struct FView { std::string Name; float3 Eye; float3 Look; float FovDeg; };

	/** Emission integrated along each pixel's ray (geometric steps from 0.0015 to 1.8 field units,
	 *  a fixed per-pixel jitter shared by every variant). Row 0 is the top. */
	std::vector<float> Render(const FView& Vw, const std::function<float(const float3&)>& Fn, int W, int Hpx, int Steps, int Threads)
	{
		std::vector<float> Img(size_t(W) * Hpx, 0.0f);
		auto Norm = [](float3 A) { const float L = std::sqrt(A.x * A.x + A.y * A.y + A.z * A.z); return float3(A.x / L, A.y / L, A.z / L); };
		auto Cross = [](float3 A, float3 B) { return float3(A.y * B.z - A.z * B.y, A.z * B.x - A.x * B.z, A.x * B.y - A.y * B.x); };
		const float3 F = Norm(Vw.Look);
		const float3 R = Norm(Cross(F, float3(0.0f, 0.0f, 1.0f)));
		const float3 U = Cross(R, F);
		const float Tx = std::tan(0.5f * Vw.FovDeg * 3.14159265f / 180.0f);
		const float Ty = Tx * float(Hpx) / float(W);
		const double T0 = 0.0015, T1 = 1.8;
		const double Ratio = std::pow(T1 / T0, 1.0 / double(Steps));
		ParallelRows(Hpx, Threads, [&](int J)
		{
			for (int I = 0; I < W; ++I)
			{
				const float Sx = (2.0f * (float(I) + 0.5f) / float(W) - 1.0f) * Tx;
				const float Sy = (1.0f - 2.0f * (float(J) + 0.5f) / float(Hpx)) * Ty;
				const float3 D = Norm(float3(F.x + R.x * Sx + U.x * Sy, F.y + R.y * Sx + U.y * Sy, F.z + R.z * Sx + U.z * Sy));
				u64 S = 0x51CEull * 1000003ull + u64(J) * 4099ull + u64(I);
				const double Jit = Unit(S);
				double Sum = 0.0;
				for (int K = 0; K < Steps; ++K)
				{
					const double A = T0 * std::pow(Ratio, double(K)), B = A * Ratio;
					const double T = A + (B - A) * Jit;
					const float3 P(Vw.Eye.x + D.x * float(T), Vw.Eye.y + D.y * float(T), Vw.Eye.z + D.z * float(T));
					Sum += double(Fn(P)) * (B - A);
				}
				Img[size_t(J) * W + I] = float(Sum);
			}
		});
		return Img;
	}

	void ImageStats(const std::vector<float>& A, const std::vector<float>& Ref, double& OutLight, double& OutP95, double& OutP50)
	{
		double Sa = 0.0, Sr = 0.0;
		for (size_t I = 0; I < Ref.size(); ++I) { Sa += A[I]; Sr += Ref[I]; }
		OutLight = Sa / std::max(Sr, 1e-300) - 1.0;
		const double Mean = Sr / double(Ref.size());
		std::vector<double> E;
		for (size_t I = 0; I < Ref.size(); ++I) { if (Ref[I] > 0.01 * Mean) { E.push_back(std::abs(double(A[I]) - Ref[I]) / Ref[I]); } }
		std::sort(E.begin(), E.end());
		OutP95 = E.empty() ? 0.0 : E[size_t(0.95 * double(E.size() - 1))];
		OutP50 = E.empty() ? 0.0 : E[size_t(0.50 * double(E.size() - 1))];
	}

	std::string J(double V) { char B[64]; std::snprintf(B, sizeof(B), "%.6g", std::isfinite(V) ? V : 0.0); return B; }
}

int UniGalSlicesCommand(int Argc, char** Argv)
{
	std::string Out = "Verification/galslices";
	bool bQuick = false;
	int Threads = int(std::max(1u, std::thread::hardware_concurrency()));
	for (int I = 2; I < Argc; ++I)
	{
		if (!std::strcmp(Argv[I], "--out") && I + 1 < Argc) { Out = Argv[++I]; }
		else if (!std::strcmp(Argv[I], "--quick")) { bQuick = true; }
		else if (!std::strcmp(Argv[I], "--threads") && I + 1 < Argc) { Threads = std::max(1, std::atoi(Argv[++I])); }
	}
	std::string BulgeJson;
	bool bBulgeOnly = false;
	for (int I = 2; I < Argc; ++I) { if (!std::strcmp(Argv[I], "--bulge-only")) { bBulgeOnly = true; } }
	const FTex Tx;
	if (!Tx.bOk) { std::fprintf(stderr, "galaxy textures not loaded: %s\n", Tx.Err.c_str()); return 2; }
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &Tx.T[I]); }

	// The home galaxy's parameters as the flythrough scene gives them (textures on).
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	H::FUniFieldParams Home{};
	bool bHome = false;
	for (const FAnchor& A : D.Anchors) { if (A.ArchetypeId == UniScenes::kGalaxy && !A.EmitterParams.empty()) { Home = ParOf(A.EmitterParams[0]); bHome = true; break; } }
	if (!bHome) { std::fprintf(stderr, "no home galaxy anchor\n"); return 2; }
	std::vector<FGalaxy> Gals;
	Gals.push_back(MakeGalaxy("home", Home));
	{
		H::FUniFieldParams P = Home;
		P.G.x = 0.022f; P.G.z = 1.6f;   // the top of the rolled disc warp and warp frequency ranges
		Gals.push_back(MakeGalaxy("fast warp", P));
	}
	{
		H::FUniFieldParams P = Home;
		P.A.y = 0.04f; P.G.x = 0.03f; P.G.z = 1.6f; P.H.x = 0.8f; P.H.y = 1.0f;   // thick, irregular (dwarf ranges)
		Gals.push_back(MakeGalaxy("thick disc", P));
	}
	if (!bQuick)
	{
		Gals.push_back(MakeGalaxy("home, warp at mip 3", Home, 3.0f));
		Gals.push_back(MakeGalaxy("home, warp 4x slower in height", Home, 0.0f, 4.0f));
		Gals.push_back(MakeGalaxy("home, warp 8x slower in height", Home, 0.0f, 8.0f));
	}

	{
		// The rebuilt field (FieldOf's changed-warp path) with the warp unchanged must be the field.
		FGalaxy G = MakeGalaxy("check", Home, 0.0f, 1.0000001f);
		FGalaxy G0 = MakeGalaxy("check0", Home);
		const std::vector<FPoint> Pts = SamplePoints(G0, 20000, 7u);
		double Worst = 0.0;
		for (const FPoint& P : Pts)
		{
			const double A = FieldOf(G, P.P), B = H::UniField_Galaxy(P.P, Home);
			if (B > 0.0) { Worst = std::max(Worst, std::abs(A - B) / B); }
		}
		std::printf("rebuilt field against UniField_Galaxy (unchanged warp): worst relative difference %.2e\n", Worst);
	}
	{
		// Side measurement: where the bulge's warp_large is read. It is read at r < 0.5 (a quarter
		// of the disc's area, ~90% of its light), while the bulge it moves is ~1% of the light at
		// r = 0.3. The field with the read limited to r < k r_e against the field.
		std::string Bj = "  \"bulge_read\": [\n";
		int Bn = 0;
		for (float Bw : { 0.6f, 1.5f })
		{
			H::FUniFieldParams P = Home;
			P.B.y = Bw;
			const FGalaxy G = MakeGalaxy("bulge", P);
			const std::vector<FPoint> Pts = SamplePoints(G, bQuick ? 40000 : 200000, 0xB0u);
			std::vector<float> F(Pts.size());
			ParallelRows(int(Pts.size()), Threads, [&](int I) { F[I] = FieldBulgeWithin(G, Pts[I].P, 0.5f); });   // D-179's read (D-218 changed the field)
			const float Re = P.A.w;
			for (float Rb : { 0.5f, 8.0f * Re, 6.0f * Re, 4.0f * Re })
			{
				std::vector<float> M(Pts.size());
				ParallelRows(int(Pts.size()), Threads, [&](int I) { M[I] = FieldBulgeWithin(G, Pts[I].P, Rb); });
				const FPointStats St = PointStats(Pts, F, M);
				double Wl = 0.0, Wt = 0.0;
				for (size_t I = 0; I < Pts.size(); ++I)
				{
					const float3& Q = Pts[I].P;
					Wt += Pts[I].W * F[I];
					if (Q.x * Q.x + Q.y * Q.y + Q.z * Q.z < Rb * Rb) { Wl += Pts[I].W * F[I]; }
				}
				std::printf("bulge weight %.1f, warp_large read inside r < %.2f: disc area read %5.1f%% (r < 0.5: 25.0%%), light read %5.1f%%, change L1 %.3f%% p95 %.3f%% light %+.4f%%\n",
					Bw, Rb, 100.0 * double(Rb * Rb), 100.0 * Wl / std::max(Wt, 1e-300), 100.0 * St.L1, 100.0 * St.P95, 100.0 * St.Light);
				Bj += std::string(Bn++ ? ",\n" : "") + "    {\"bulge_weight\": " + J(Bw) + ", \"radius\": " + J(Rb) + ", \"area\": " + J(double(Rb * Rb))
					+ ", \"light_read\": " + J(Wl / std::max(Wt, 1e-300)) + ", \"l1\": " + J(St.L1) + ", \"p95\": " + J(St.P95) + ", \"light\": " + J(St.Light) + "}";
			}
		}
		Bj += "\n  ],\n";
		if (bBulgeOnly)
		{
			std::printf("%s", Bj.c_str());
			for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
			return 0;
		}
		BulgeJson = Bj;
	}
	std::vector<FCfg> Cfgs;
	{
		auto Add = [&](const char* N, int Ns, bool bRel, float Span, float Jit, u32 Size) { FCfg C; C.Name = N; C.N = Ns; C.bRel = bRel; C.SpanH = Span; C.Jitter = Jit; C.Size = Size; Cfgs.push_back(C); };
		Add("2D map + warp read (today)", 0, true, 0.0f, 0.0f, 1024u);
		Add("1 slice (mid-plane warp)", 1, true, 3.0f, 0.0f, 1024u);
		Add("4 slices", 4, true, 3.0f, 0.0f, 1024u);
		Add("4 slices + jitter", 4, true, 3.0f, 0.5f, 1024u);
		Add("8 slices", 8, true, 3.0f, 0.0f, 1024u);
		Add("8 slices + jitter", 8, true, 3.0f, 0.5f, 1024u);
		if (!bQuick)
		{
			Add("16 slices", 16, true, 3.0f, 0.0f, 1024u);
			Add("32 slices", 32, true, 3.0f, 0.0f, 1024u);
			Add("16 slices +-4H", 16, true, 4.0f, 0.0f, 1024u);
			Add("8 slices absolute", 8, false, 3.0f, 0.0f, 1024u);
			Add("4 slices 512^2", 4, true, 3.0f, 0.0f, 512u);
			Add("8 slices 512^2", 8, true, 3.0f, 0.0f, 512u);
		}
	}
	const int NPts = bQuick ? 40000 : 300000;

	std::string Js = "{\n" + BulgeJson + "  \"galaxies\": [\n";
	const auto TStart = std::chrono::steady_clock::now();
	for (size_t Gi = 0; Gi < Gals.size(); ++Gi)
	{
		const FGalaxy& G = Gals[Gi];
		const bool bImages = Gi == 0;   // renders and cross-sections for the home galaxy only
		std::printf("== %s: H %.4f, warp %.3f at freq %.2f (warp_fine z period %.3f = %.1f H)\n", G.Name.c_str(), G.Par.A.y, G.Par.G.x, G.Par.G.z,
			0.25f / (2.0f * G.Par.G.z), 0.25f / (2.0f * G.Par.G.z) / G.Par.A.y);
		const std::vector<FPoint> Pts = SamplePoints(G, NPts, 0xC0FFEEull + Gi);
		std::vector<float> Fref(Pts.size());
		ParallelRows(int(Pts.size()), Threads, [&](int I) { Fref[I] = FieldOf(G, Pts[I].P); });
		u64 BulgeReads = 0;
		for (const FPoint& P : Pts) { if (P.P.x * P.P.x + P.P.y * P.P.y + P.P.z * P.P.z < 0.25f) { ++BulgeReads; } }
		const auto FieldFn = [&](const float3& P) { return FieldOf(G, P); };
		// A smoother warp changes the look: its field against the full-warp field.
		FPointStats Look;
		if (G.Changed())
		{
			std::vector<float> F0(Pts.size());
			ParallelRows(int(Pts.size()), Threads, [&](int I) { F0[I] = H::UniField_Galaxy(Pts[I].P, G.Par); });
			Look = PointStats(Pts, F0, Fref);
			std::printf("   look change against the full warp: L1 %.2f%%  p95 %.2f%%\n", 100.0 * Look.L1, 100.0 * Look.P95);
		}
		std::printf("   warp_large read at %.0f%% of sampled points\n", 100.0 * double(BulgeReads) / double(Pts.size()));

		// Views (home only): inside the disc at ~8 kpc looking along it, and low over the arms.
		const H::FUniGalaxyShared ShA = H::Uni_GalaxySharedK(float3(0.45f, -0.25f, 0.0f), G.Par, G.K, 0.0f, G.Slab, false, false);
		const H::FUniGalaxyShared ShB = H::Uni_GalaxySharedK(float3(0.38f, 0.12f, 0.0f), G.Par, G.K, 0.0f, G.Slab, false, false);
		std::vector<FView> Views = {
			{ "in_disc", float3(0.45f, -0.25f, ShA.WarpZ + 0.5f * G.Par.A.y), float3(-0.35f, 1.0f, -0.02f), 75.0f },
			{ "low_over", float3(0.38f, 0.12f, ShB.WarpZ + 4.0f * G.Par.A.y), float3(-0.25f, 1.0f, -0.22f), 75.0f },
		};
		const int Rw = bQuick ? 160 : 320, Rh = bQuick ? 80 : 160, Rs = bQuick ? 160 : 320;
		const int Cw = 720, Ch = 180;
		const float Cx0 = 0.12f, Cx1 = 0.72f, Cy = 0.03f, Cv = 5.0f;
		std::vector<std::vector<float>> RefImg;
		if (bImages)
		{
			WriteF32(Out + "/xsec_field.f32", CrossSection(G, FieldFn, Cw, Ch, Cx0, Cx1, Cy, Cv, Threads));
			for (const FView& Vw : Views)
			{
				const auto T0 = std::chrono::steady_clock::now();
				RefImg.push_back(Render(Vw, FieldFn, Rw, Rh, Rs, Threads));
				WriteF32(Out + "/view_" + Vw.Name + "_field.f32", RefImg.back());
				std::printf("   field render %s: %.1f s\n", Vw.Name.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count());
			}
		}
		else if (G.Changed())
		{
			// A changed warp's look: its field's cross-section (and its slices' below), file prefix g<index>_.
			WriteF32(Out + "/xsec_g" + std::to_string(Gi) + "_field.f32", CrossSection(G, FieldFn, Cw, Ch, Cx0, Cx1, Cy, Cv, Threads));
		}

		Js += "    {\"name\": \"" + G.Name + "\", \"H\": " + J(G.Par.A.y) + ", \"warp\": " + J(G.Par.G.x) + ", \"warp_freq\": " + J(G.Par.G.z)
			+ ", \"warp_z_period\": " + J(0.25 / (2.0 * G.Par.G.z)) + ", \"warp_mip\": " + J(G.WarpMip) + ", \"z_stretch\": " + J(G.ZStretch) + ", \"look_l1\": " + J(Look.L1) + ", \"look_p95\": " + J(Look.P95)
			+ ", \"bulge_read_share\": " + J(double(BulgeReads) / double(Pts.size())) + ", \"variants\": [\n";
		int Written = 0;
		for (size_t Ci = 0; Ci < Cfgs.size(); ++Ci)
		{
			const FCfg& C = Cfgs[Ci];
			if (!bImages && (C.Size != 1024u || C.N > 8 || !C.bRel)) { continue; }   // the full list for the home galaxy
			if (G.Changed() && C.N == 0) { continue; }   // today's map path reads the full warp
			FVariant V;
			V.Cfg = C;
			const auto Tb = std::chrono::steady_clock::now();
			if (C.N == 0)
			{
				FGalaxyMapRequest Rq;
				std::memcpy(static_cast<void*>(&Rq.Par), &G.Par, sizeof(Rq.Par));
				V.Map2D.Bake(Rq, C.Size, Threads);
				BindGalaxyMap(0u, &V.Map2D);
			}
			else { V.Slices.Bake(G, C, Threads); }
			V.BakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Tb).count();
			const auto Fn = [&](const float3& P) { return V.Eval(G, P); };
			std::vector<float> Mv(Pts.size());
			ParallelRows(int(Pts.size()), Threads, [&](int I) { Mv[I] = Fn(Pts[I].P); });
			const FPointStats St = PointStats(Pts, Fref, Mv);
			std::printf("   %-28s L1 %6.2f%%  off-plane L1 %6.2f%%  p50 %6.2f%%  p95 %6.2f%%  light %+6.2f%%  %6.1f MB  bake %.0f ms\n", C.Name.c_str(),
				100.0 * St.L1, 100.0 * St.L1Off, 100.0 * St.P50, 100.0 * St.P95, 100.0 * St.Light, double(V.Bytes()) / 1048576.0, V.BakeMs);
			Js += std::string(Written++ == 0 ? "" : ",\n") + "      {\"name\": \"" + C.Name + "\", \"slices\": " + std::to_string(C.N) + ", \"size\": " + std::to_string(C.Size)
				+ ", \"relative\": " + (C.bRel ? "true" : "false") + ", \"jitter\": " + J(C.Jitter)
				+ ", \"l1\": " + J(St.L1) + ", \"l1_off\": " + J(St.L1Off) + ", \"p50\": " + J(St.P50) + ", \"p95\": " + J(St.P95) + ", \"light\": " + J(St.Light)
				+ ", \"mb\": " + J(double(V.Bytes()) / 1048576.0) + ", \"bake_ms\": " + J(V.BakeMs);
			if (bImages)
			{
				const std::string Tag = std::to_string(Ci);
				WriteF32(Out + "/xsec_" + Tag + ".f32", CrossSection(G, Fn, Cw, Ch, Cx0, Cx1, Cy, Cv, Threads));
				Js += ", \"tag\": \"" + Tag + "\", \"views\": {";
				for (size_t Vi = 0; Vi < Views.size(); ++Vi)
				{
					const std::vector<float> Img = Render(Views[Vi], Fn, Rw, Rh, Rs, Threads);
					WriteF32(Out + "/view_" + Views[Vi].Name + "_" + Tag + ".f32", Img);
					double Le = 0, P95 = 0, P50 = 0;
					ImageStats(Img, RefImg[Vi], Le, P95, P50);
					std::printf("      view %-9s light %+6.2f%%  p50 %6.2f%%  p95 %6.2f%%\n", Views[Vi].Name.c_str(), 100.0 * Le, 100.0 * P50, 100.0 * P95);
					Js += std::string(Vi == 0 ? "" : ", ") + "\"" + Views[Vi].Name + "\": {\"light\": " + J(Le) + ", \"p50\": " + J(P50) + ", \"p95\": " + J(P95) + "}";
				}
				Js += "}";
			}
			else if (G.Changed() && (C.N == 8 || C.N == 4) && C.Jitter == 0.0f)
			{
				const std::string Tag = "g" + std::to_string(Gi) + "_" + std::to_string(Ci);
				WriteF32(Out + "/xsec_" + Tag + ".f32", CrossSection(G, Fn, Cw, Ch, Cx0, Cx1, Cy, Cv, Threads));
				Js += ", \"tag\": \"" + Tag + "\"";
			}
			Js += "}";
			if (C.N == 0) { BindGalaxyMap(0u, nullptr); }
			std::fflush(stdout);
		}
		Js += "\n    ]}" + std::string(Gi + 1 < Gals.size() ? "," : "") + "\n";
	}
	Js += "  ],\n  \"images\": {\"xsec\": [720, 180, 0.12, 0.72, 0.03, 5.0], \"view\": [" + std::to_string(bQuick ? 160 : 320) + ", " + std::to_string(bQuick ? 80 : 160) + "]},\n";
	Js += "  \"seconds\": " + J(std::chrono::duration<double>(std::chrono::steady_clock::now() - TStart).count()) + "\n}\n";
	{
		std::ofstream F(Out + "/metrics.json");
		F << Js;
	}
	for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); }
	std::printf("wrote %s/metrics.json\n", Out.c_str());
	return 0;
}
