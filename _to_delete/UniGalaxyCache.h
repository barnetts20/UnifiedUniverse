// UniGalaxyCache.h
// D-211: the galaxy cache. A camera-centred clipmap of the camera's own galaxy (FVolumeScene::Home)
// in its field units: K nested levels of N^3 voxels, level L's voxel 2^L x level 0's, baked from
// the shader core's Uni_GalCacheBakeVoxel and read by the march (Uni_GalCacheSample, UniVolume.ush)
// in place of the field. This file decides what to bake each frame (FGalaxyCache::Update: the
// levels wrap toroidally, so a move rebakes slabs; a per-frame voxel budget, coarse levels first),
// writes the header the march reads (Publish), and bakes on the CPU for the reference
// (FGalaxyCacheTextures). The engine bakes the same boxes on the GPU (UniVolume.usf, CacheBakeCS).
// Engine-agnostic (no UE includes).

#pragma once

#include "UniVolume.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace UniCore
{
	/** One box of voxels to bake: a level and a range of world voxel indices (half-open). */
	struct FGalCacheBox
	{
		u32 Level = 0;
		int32_t Lo[3] = { 0, 0, 0 };
		int32_t Size[3] = { 0, 0, 0 };
		u64 Voxels() const { return u64(Size[0]) * u64(Size[1]) * u64(Size[2]); }
	};

	struct FGalCacheSettings
	{
		u32 N = 128;                   // voxels a level side (T2.vol.galaxy_cache: p95 3.1% at 128, 4.4% at 96, against the field)
		double FinestHalfPc = 512.0;   // level 0 reaches this far from the camera (pc): its voxel 2 x this / N (128 pc: the same light; one level more)
		double Reach = 2.0;            // the coarsest level reaches at least this far (field units: a ray from inside the cut sphere crosses at most 2)
		u64 Budget = 1u << 20;         // voxels baked a frame at most
		double FootScale = 1.0;        // a voxel is baked prefiltered over this x its (smallest) size
		double ZReach = 0.3;           // levels but the last reach this far in the field's z at least (field units): their z voxel is at most 2 ZReach / N
		u32 MaxLevels = 12u;           // = UNI_GAL_CACHE_MAX_LEVELS
	};

	class FGalaxyCache
	{
	public:
		FGalCacheSettings Settings;

		struct FLevel
		{
			bool bValid = false;
			int64_t Lo[3] = { 0, 0, 0 };   // baked window's first voxel (world voxel indices)
		};

		bool bActive = false;
		FObjectId Id;
		u32 K = 0;
		double V0 = 0.0;          // level 0 voxel (field units)
		float Scale = 1.0f;       // densities x this in the textures
		std::vector<FLevel> Levels;
		u64 BakedTotal = 0, Resets = 0;
		u64 LastBaked = 0;
		u32 LevelsBehind = 0;     // levels that could not follow the camera last frame

		double VoxelOf(u32 L) const { return std::ldexp(V0, int(L)); }
		/** The voxel along the field's z: capped but at the last level (isotropic: the far halo). */
		double VoxelZOf(u32 L) const
		{
			const double V = VoxelOf(L);
			if (L + 1u >= K) { return V; }
			return std::min(V, 2.0 * Settings.ZReach / double(std::max<u32>(Settings.N, 8u)));
		}

		/** The frame's work: which boxes to bake (to be baked before the march reads the header
		 *  Publish writes). Resets when the camera's galaxy changes (or leaves). */
		std::vector<FGalCacheBox> Update(const FVolumeScene& Sc)
		{
			std::vector<FGalCacheBox> Out;
			LastBaked = 0;
			LevelsBehind = 0;
			const int Home = Sc.Home.Carrier;
			const FCarrierGPU* Ca = HomeCarrier(Sc);
			if (!Ca) { if (bActive) { bActive = false; ++Resets; } return Out; }
			const u32 N = std::max<u32>(Settings.N, 8u);
			if (!bActive || !(Id == Sc.Home.Parent))
			{
				bActive = true;
				Id = Sc.Home.Parent;
				const double RadiusCm = 1.0 / double(Ca->InvRadius);
				V0 = 2.0 * Settings.FinestHalfPc * 3.0856775814913673e18 / double(N) / RadiusCm;
				const double Half0 = 0.5 * double(N) * V0;
				K = std::clamp<u32>(u32(std::ceil(std::log2(std::max(Settings.Reach / Half0, 1.0)))) + 1u, 1u, Settings.MaxLevels);
				Levels.assign(K, FLevel{});
				Scale = ScaleFor(Sc, *Ca);
				++Resets;
			}
			(void)Home;
			u64 Left = Settings.Budget;
			for (int Li = int(K) - 1; Li >= 0; --Li)   // coarse first: the fallback is ready first
			{
				FLevel& Lv = Levels[size_t(Li)];
				const double Vv[3] = { VoxelOf(u32(Li)), VoxelOf(u32(Li)), VoxelZOf(u32(Li)) };
				int64_t Want[3];
				for (int A = 0; A < 3; ++A) { Want[A] = int64_t(std::floor(Sc.Home.CamField[A] / Vv[A])) - int64_t(N / 2u); }
				if (Lv.bValid && Want[0] == Lv.Lo[0] && Want[1] == Lv.Lo[1] && Want[2] == Lv.Lo[2]) { continue; }
				std::vector<FGalCacheBox> Boxes;
				NewRegion(u32(Li), Lv, Want, N, Boxes);
				u64 Count = 0;
				for (const FGalCacheBox& B : Boxes) { Count += B.Voxels(); }
				if (Count > Left) { ++LevelsBehind; continue; }   // keeps its last window (coarser levels cover)
				Left -= Count;
				LastBaked += Count;
				Out.insert(Out.end(), Boxes.begin(), Boxes.end());
				Lv.bValid = true;
				for (int A = 0; A < 3; ++A) { Lv.Lo[A] = Want[A]; }
			}
			BakedTotal += LastBaked;
			return Out;
		}

		/** Forgets every level (the next Update re-initialises and bakes from scratch). */
		void Reset() { bActive = false; Levels.clear(); }

		/** The header the march reads, after the LUT's blocks and grids, and the view's fields.
		 *  InCarrier: the home carrier's index in the buffer the march reads (local carriers first). */
		void Publish(std::vector<HV::float4>& Lut, FVolumeViewGPU& View, int InCarrier) const
		{
			PublishAt(u32(Lut.size()), Lut, View, InCarrier);
		}

		/** Publish with the header's LUT index given (appended to Out, which the caller places there). */
		void PublishAt(u32 InBase, std::vector<HV::float4>& Lut, FVolumeViewGPU& View, int InCarrier) const
		{
			View.CacheBase = 0u; View.CacheCarrier = 0u; View.CacheN = 0u; View.CacheLevels = 0u;
			if (!bActive || InCarrier < 0 || K == 0u) { return; }
			bool bAny = false;
			for (const FLevel& Lv : Levels) { bAny = bAny || Lv.bValid; }
			if (!bAny) { return; }
			const u32 N = Settings.N;
			View.CacheBase = InBase;
			View.CacheCarrier = u32(InCarrier) + 1u;
			View.CacheN = N;
			View.CacheLevels = K;
			Lut.push_back(HV::float4(float(N), float(K), Scale, 1.0f / Scale));
			for (u32 L = 0; L < K; ++L)
			{
				const FLevel& Lv = Levels[L];
				const double V = VoxelOf(L), Vz = VoxelZOf(L);
				Lut.push_back(HV::float4(float(double(Lv.Lo[0]) * V), float(double(Lv.Lo[1]) * V), float(double(Lv.Lo[2]) * Vz), float(V)));
				Lut.push_back(HV::float4(float(double(Lv.Lo[0] + int64_t(N)) * V), float(double(Lv.Lo[1] + int64_t(N)) * V), float(double(Lv.Lo[2] + int64_t(N)) * Vz), Lv.bValid ? float(Vz) : 0.0f));
			}
		}

		static const FCarrierGPU* HomeCarrier(const FVolumeScene& Sc)
		{
			if (Sc.Home.Carrier < 0) { return nullptr; }
			int Local = 0;
			for (const FCarrierGPU& C : Sc.Carriers)
			{
				if ((C.Flags & HV::UNI_CARRIER_PERIODIC) != 0u) { continue; }
				if (Local == Sc.Home.Carrier) { return &C; }
				++Local;
			}
			return nullptr;
		}

	private:
		/** Densities x Scale fit half floats: the field's peak (the centre) near 2^13. */
		static float ScaleFor(const FVolumeScene& Sc, const FCarrierGPU& Ca)
		{
			HV::FUniFieldParams P;
			std::memcpy(static_cast<void*>(&P), &Sc.Params[Ca.ParamIndex], sizeof(P));
			double Peak = double(HV::UniField_GalaxyFiltered(HV::float3(0.0f, 0.0f, 0.0f), P, 0.0f));
			if ((Ca.Flags & HV::UNI_CARRIER_DUST) != 0u)
			{
				HV::FUniFieldParams D;
				std::memcpy(static_cast<void*>(&D), &Sc.Params[Ca.DustParamIndex], sizeof(D));
				Peak = std::max(Peak, double(HV::UniField_GalaxyFiltered(HV::float3(0.0f, 0.0f, 0.0f), D, 0.0f)));
			}
			if (!(Peak > 0.0) || !std::isfinite(Peak)) { return 1.0f; }
			return float(std::exp2(std::round(std::log2(8192.0 / Peak))));
		}

		/** Want's window minus Lv's (the whole window if Lv is not valid or moved a side or more):
		 *  at most three slabs. */
		static void NewRegion(u32 L, const FLevel& Lv, const int64_t Want[3], u32 N, std::vector<FGalCacheBox>& Out)
		{
			const int64_t Nn = int64_t(N);
			bool bWhole = !Lv.bValid;
			for (int A = 0; A < 3 && !bWhole; ++A) { bWhole = std::llabs(Want[A] - Lv.Lo[A]) >= Nn; }
			if (bWhole)
			{
				FGalCacheBox B; B.Level = L;
				for (int A = 0; A < 3; ++A) { B.Lo[A] = int32_t(Want[A]); B.Size[A] = int32_t(N); }
				Out.push_back(B);
				return;
			}
			// Axis by axis: the new part on this axis, the overlap on the axes before, the whole
			// new window on the axes after.
			int64_t OvLo[3], OvHi[3];
			for (int A = 0; A < 3; ++A) { OvLo[A] = std::max(Want[A], Lv.Lo[A]); OvHi[A] = std::min(Want[A], Lv.Lo[A]) + Nn; }
			for (int A = 0; A < 3; ++A)
			{
				const int64_t D = Want[A] - Lv.Lo[A];
				if (D == 0) { continue; }
				FGalCacheBox B; B.Level = L;
				for (int Q = 0; Q < 3; ++Q)
				{
					int64_t Lo, Hi;
					if (Q < A) { Lo = OvLo[Q]; Hi = OvHi[Q]; }
					else if (Q > A) { Lo = Want[Q]; Hi = Want[Q] + Nn; }
					else if (D > 0) { Lo = Lv.Lo[Q] + Nn; Hi = Want[Q] + Nn; }
					else { Lo = Want[Q]; Hi = Lv.Lo[Q]; }
					B.Lo[Q] = int32_t(Lo);
					B.Size[Q] = int32_t(Hi - Lo);
				}
				if (B.Voxels() > 0u) { Out.push_back(B); }
			}
		}
	};

	/** The reference's cache textures (CPU), baked with the shader core and bound to the shim's
	 *  sampler (BindGalaxyCache). Texels as the GPU stores them: A half floats, B unorm8. */
	struct FGalaxyCacheTextures
	{
		u32 N = 0, K = 0;
		std::vector<float> A, B;   // N x N x K (N + 2) x 4

		void Resize(u32 InN, u32 InK)
		{
			if (InN == N && InK == K && !A.empty()) { return; }
			N = InN; K = InK;
			const size_t T = size_t(N) * N * size_t(K) * (N + 2u) * 4u;
			A.assign(T, 0.0f);
			B.assign(T, 1.0f);
		}

		/** Bakes the boxes for the scene's home carrier (the frame Update ran on). */
		void Bake(const FGalaxyCache& C, const FVolumeScene& Sc, const std::vector<FGalCacheBox>& Boxes)
		{
			const FCarrierGPU* Ca = FGalaxyCache::HomeCarrier(Sc);
			if (!Ca || !C.bActive) { return; }
			Resize(C.Settings.N, C.K);
			HV::FUniCarrier Hc = ToHlsl(*Ca);
			HV::FUniFieldParams Par, Dust, Pal;
			std::memcpy(static_cast<void*>(&Par), &Sc.Params[Ca->ParamIndex], sizeof(Par));
			Dust = Par; Pal = Par;
			if ((Ca->Flags & HV::UNI_CARRIER_DUST) != 0u) { std::memcpy(static_cast<void*>(&Dust), &Sc.Params[Ca->DustParamIndex], sizeof(Dust)); }
			if ((Ca->Flags & HV::UNI_CARRIER_PALETTE) != 0u) { std::memcpy(static_cast<void*>(&Pal), &Sc.Params[Ca->PaletteIndex], sizeof(Pal)); }
			const HV::FUniGalaxyConst Kc = HV::Uni_GalaxyConstOf(Par);
			const int64_t Nn = int64_t(N);
			for (const FGalCacheBox& Bx : Boxes)
			{
				const double V = C.VoxelOf(Bx.Level), Vz = C.VoxelZOf(Bx.Level);
				for (int32_t Z = 0; Z < Bx.Size[2]; ++Z)
				{
					for (int32_t Y = 0; Y < Bx.Size[1]; ++Y)
					{
						for (int32_t X = 0; X < Bx.Size[0]; ++X)
						{
							const int64_t I[3] = { int64_t(Bx.Lo[0]) + X, int64_t(Bx.Lo[1]) + Y, int64_t(Bx.Lo[2]) + Z };
							const HV::float3 P(float((double(I[0]) + 0.5) * V), float((double(I[1]) + 0.5) * V), float((double(I[2]) + 0.5) * Vz));
							HV::float4 Ta, Tb;
							HV::Uni_GalCacheBakeVoxel(Hc, Par, Dust, Kc, Pal, P, float(C.Settings.FootScale * std::min(V, Vz)), C.Scale, Ta, Tb);
							const int64_t Tx = ((I[0] % Nn) + Nn) % Nn, Ty = ((I[1] % Nn) + Nn) % Nn, Tz = ((I[2] % Nn) + Nn) % Nn;
							const int64_t Zs = int64_t(Bx.Level) * (Nn + 2) + 1 + Tz;
							Store(Tx, Ty, Zs, Ta, Tb);
							if (Tz == Nn - 1) { Store(Tx, Ty, int64_t(Bx.Level) * (Nn + 2), Ta, Tb); }        // the copy before the level
							if (Tz == 0) { Store(Tx, Ty, int64_t(Bx.Level) * (Nn + 2) + Nn + 1, Ta, Tb); }   // and after it
						}
					}
				}
			}
		}

	private:
		static float Half(float F) { return UniHlsl::UniHalfToFloat(FloatToHalf(F)); }
		static float Unorm8(float F) { return std::round(std::clamp(F, 0.0f, 1.0f) * 255.0f) / 255.0f; }

		void Store(int64_t X, int64_t Y, int64_t Z, const HV::float4& Ta, const HV::float4& Tb)
		{
			const size_t I = ((size_t(Z) * N + size_t(Y)) * N + size_t(X)) * 4u;
			A[I] = Half(Ta.x); A[I + 1] = Half(Ta.y); A[I + 2] = 0.0f; A[I + 3] = 0.0f;
			B[I] = Unorm8(Tb.x); B[I + 1] = Unorm8(Tb.y); B[I + 2] = Unorm8(Tb.z); B[I + 3] = 1.0f;
		}
	};

	/** Binds (or, with null, unbinds) the reference's cache textures to the shim's sampler. */
	inline void BindGalaxyCache(const FGalaxyCacheTextures* T)
	{
		for (int W = 0; W < 2; ++W)
		{
			UniHlsl::FGalCacheTexView V;
			if (T && !T->A.empty())
			{
				V.Texels = W == 0 ? T->A.data() : T->B.data();
				V.W = int(T->N); V.H = int(T->N); V.D = int(T->K * (T->N + 2u));
			}
			UniHlsl::UniGalCacheViews[W] = V;
		}
	}
}
