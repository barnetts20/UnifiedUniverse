// UniGalaxyMap.h
// D-212: the galaxy map. A 2D RGBA16F map with mips over a galaxy's field x, y in [-1, 1], baked
// once from the shader core (Uni_GalaxyMapTexel): (light's in-plane factor, dust's, the disc's
// vertical centre, the light's arm part). The march evaluates the camera's own galaxy from it
// (Uni_GalaxyMapEval). The CPU bake here is the reference's and the engine's (baked on a worker,
// uploaded once): the same texels on both sides. Engine-agnostic (no UE includes).

#pragma once

#include "UniVolume.h"
#include "UniWebTexture.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

namespace UniCore
{
	struct FGalaxyMap
	{
		u32 Size = 0;
		std::vector<std::vector<float>> Mips;   // RGBA per texel, half-rounded; Size >> m a side
		mutable std::vector<const float*> MipPtrs;

		bool IsValid() const { return Size > 0u && !Mips.empty(); }
		size_t Bytes() const { size_t B = 0; for (const auto& M : Mips) { B += M.size() / 4u * 8u; } return B; }

		/** Bakes the carrier's map (InSize a power of two) on InThreads threads. */
		void Bake(const FCarrierGPU& Ca, const std::vector<FFieldParamsGPU>& Params, u32 InSize, int InThreads = 1)
		{
			Size = InSize;
			HV::FUniFieldParams Par, Dust;
			std::memcpy(static_cast<void*>(&Par), &Params[Ca.ParamIndex], sizeof(Par));
			Dust = Par;
			const bool bDust = (Ca.Flags & HV::UNI_CARRIER_DUST) != 0u;
			if (bDust) { std::memcpy(static_cast<void*>(&Dust), &Params[Ca.DustParamIndex], sizeof(Dust)); }
			const HV::FUniGalaxyConst K = HV::Uni_GalaxyConstOf(Par);
			Mips.clear();
			Mips.emplace_back(size_t(Size) * Size * 4u, 0.0f);
			std::vector<float>& M0 = Mips[0];
			auto Rows = [&](u32 Y0, u32 Y1)
			{
				for (u32 Y = Y0; Y < Y1; ++Y)
				{
					for (u32 X = 0; X < Size; ++X)
					{
						const float Fx = (float(X) + 0.5f) / float(Size) * 2.0f - 1.0f;
						const float Fy = (float(Y) + 0.5f) / float(Size) * 2.0f - 1.0f;
						const HV::float4 T = HV::Uni_GalaxyMapTexel(Fx, Fy, Par, Dust, K, bDust);
						float* O = &M0[(size_t(Y) * Size + X) * 4u];
						O[0] = T.x; O[1] = T.y; O[2] = T.z; O[3] = T.w;
					}
				}
			};
			const int Nt = std::max(InThreads, 1);
			if (Nt == 1) { Rows(0, Size); }
			else
			{
				std::vector<std::thread> Ts;
				for (int T = 0; T < Nt; ++T) { Ts.emplace_back(Rows, u32(u64(Size) * T / Nt), u32(u64(Size) * (T + 1) / Nt)); }
				for (auto& T : Ts) { T.join(); }
			}
			// Mips: 2 x 2 means of the unrounded level above (the vertical centre too: it is smooth).
			for (u32 S = Size / 2u; S >= 1u; S /= 2u)
			{
				const std::vector<float>& Up = Mips.back();
				std::vector<float> Dn(size_t(S) * S * 4u, 0.0f);
				const u32 Su = S * 2u;
				for (u32 Y = 0; Y < S; ++Y)
				{
					for (u32 X = 0; X < S; ++X)
					{
						for (int C = 0; C < 4; ++C)
						{
							const float A = Up[((size_t(2 * Y) * Su) + 2 * X) * 4u + C], B = Up[((size_t(2 * Y) * Su) + 2 * X + 1) * 4u + C];
							const float Cc = Up[((size_t(2 * Y + 1) * Su) + 2 * X) * 4u + C], D = Up[((size_t(2 * Y + 1) * Su) + 2 * X + 1) * 4u + C];
							Dn[(size_t(Y) * S + X) * 4u + C] = 0.25f * (A + B + Cc + D);
						}
					}
				}
				Mips.push_back(std::move(Dn));
				if (S == 1u) { break; }
			}
			for (auto& M : Mips) { for (float& V : M) { V = UniHlsl::UniHalfToFloat(FloatToHalf(V)); } }   // as the GPU stores them
		}
	};

	/** Binds (or, with null, unbinds) a map to the shim's sampler. */
	inline void BindGalaxyMap(const FGalaxyMap* M)
	{
		UniHlsl::FGalMapView V;
		if (M && M->IsValid())
		{
			M->MipPtrs.clear();
			for (const auto& L : M->Mips) { M->MipPtrs.push_back(L.data()); }
			V.Mips = M->MipPtrs.data();
			V.Size = int(M->Size);
			V.NumMips = int(M->Mips.size());
		}
		UniHlsl::UniGalMapView = V;
	}

	/** The view's map fields for the scene's home carrier (local index; 0: none). */
	inline void PublishGalaxyMap(FVolumeViewGPU& View, int InCarrier, u32 InSize)
	{
		View.MapCarrier = InCarrier >= 0 && InSize > 0u ? u32(InCarrier) + 1u : 0u;
		View.MapSize = InSize;
		View.MapPad0 = 0u; View.MapPad1 = 0u;
	}
}
