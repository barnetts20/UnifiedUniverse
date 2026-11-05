#pragma once

// UniWebTexture.h
// D-164: the cosmic web as a baked 3D texture.
//
// WHY. The procedural web costs two or three 27-cell Worley searches per sample, and the
// background march takes ~100 of them per ray (11-15 ms per background face in the engine).
// A texture read is one fetch, and its mip chain is the prefilter: a distant sample reads the
// mip whose texels match its footprint, so the structure averages correctly instead of
// blending to a flat mean.
//
// WHAT. N^3 texels per period of the web (the field's [0, 1)^3), RGBA, full mip chain:
//   R walls and G filaments, each mixed over the web's two lattices; B nodes; A unused
//   (Uni_WebChannels in UniFields.ush, which this file bakes). The density is rebuilt from the
//   channels with the live weights (UniField_WebTex). Width, lattices and seed are baked: the
//   bake key.
//
// STAND-IN. Until the texture project delivers its adhesion-model web, this bakes today's
// procedural web, so the textured path can be checked against the procedural one on the same
// structure. A delivered texture replaces Mips (and the key) and nothing else.
//
// VALUES. Each mip-0 texel is the mean of Uni_WebChannels over its volume (SS^3 samples), so
// the texture's mean is the field's mean. Mip m + 1 is the exact mean of its 2 x 2 x 2
// children (periodic sizes are powers of two, so no edge handling is needed and the chain is
// wrap-aware). Every texel is rounded to half precision, the GPU format (RGBA16F), so the CPU
// sampler and the GPU read the same values.

#include "UniArchetype.h"
#include "UniShaderCores.h"
#include "UniVol.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace UniCore
{
	/** IEEE half from float, round to nearest even (denormals kept, overflow to infinity). */
	inline uint16_t FloatToHalf(float F)
	{
		uint32_t X; std::memcpy(&X, &F, 4);
		const uint32_t Sign = (X >> 16) & 0x8000u;
		const int32_t Exp = int32_t((X >> 23) & 0xFFu) - 127 + 15;
		uint32_t Man = X & 0x7FFFFFu;
		if (((X >> 23) & 0xFFu) == 0xFFu) { return uint16_t(Sign | 0x7C00u | (Man ? 0x200u : 0u)); }   // inf / nan
		if (Exp >= 31) { return uint16_t(Sign | 0x7C00u); }
		if (Exp <= 0)
		{
			if (Exp < -10) { return uint16_t(Sign); }
			Man |= 0x800000u;
			const uint32_t Shift = uint32_t(14 - Exp);
			uint32_t H = Man >> Shift;
			const uint32_t Rem = Man & ((1u << Shift) - 1u), Half = 1u << (Shift - 1u);
			if (Rem > Half || (Rem == Half && (H & 1u))) { ++H; }
			return uint16_t(Sign | H);
		}
		uint32_t H = (uint32_t(Exp) << 10) | (Man >> 13);
		const uint32_t Rem = Man & 0x1FFFu;
		if (Rem > 0x1000u || (Rem == 0x1000u && (H & 1u))) { ++H; }   // may carry into the exponent: correct
		return uint16_t(Sign | H);
	}

	/** The shim's decoder (UniHlslShim.h), so the CPU sampler and this file agree bit for bit. */
	inline float HalfToFloat(uint16_t H) { return UniHlsl::UniHalfToFloat(H); }

	inline float RoundToHalf(float F) { return HalfToFloat(FloatToHalf(F)); }

	/** The parameters a web texture is baked from (Uni_WebChannels' inputs), and its key. */
	struct FWebBakeKey
	{
		int G = 5;
		float Width = 0.12f;
		int G2 = 0;
		float W2 = 0.0f;
		uint32_t Seed = 0u;
		bool bNodes = true;
		uint32_t Size = 128u;        // texels per period
		uint32_t Supersample = 2u;   // samples per texel axis at mip 0

		static FWebBakeKey FromParams(const UniHlsl::FUniFieldParams& P, uint32_t InSize, uint32_t InSS)
		{
			FWebBakeKey K;
			K.G = std::max(int(P.A.x), 1);
			K.Width = P.A.y;
			K.G2 = int(P.B.y);
			K.W2 = P.B.z;
			K.Seed = UniHlsl::asuint(P.B.w);
			K.Size = InSize;
			K.Supersample = std::max(InSS, 1u);
			return K;
		}

		/** Does this key bake the web these parameters describe? (Weights, floor and power are live.) */
		bool Matches(const UniHlsl::FUniFieldParams& P) const
		{
			return G == std::max(int(P.A.x), 1) && Width == P.A.y && G2 == int(P.B.y) && W2 == P.B.z && Seed == UniHlsl::asuint(P.B.w);
		}

		UniHlsl::FUniFieldParams BakeParams() const
		{
			UniHlsl::FUniFieldParams P;
			P.A = UniHlsl::float4(float(G), Width, 0.0f, 1.0f);
			P.B = UniHlsl::float4(1.0f, float(G2), W2, UniHlsl::asfloat(Seed));
			P.C = UniHlsl::float4(0.0f, bNodes ? 1.0f : 0.0f, 0.0f, 0.0f);
			P.D = UniHlsl::float4(0.0f, 0.0f, 0.0f, 0.0f);
			return P;
		}

		/** Bump when Uni_WebChannels or the bake changes, so cached textures are rebaked. */
		static constexpr uint32_t BakeVersion = 1u;

		uint64_t Hash() const
		{
			uint32_t W; std::memcpy(&W, &Width, 4);
			uint32_t W2b; std::memcpy(&W2b, &W2, 4);
			const uint32_t V[9] = { BakeVersion, uint32_t(G), W, uint32_t(G2), W2b, Seed, bNodes ? 1u : 0u, Size, Supersample };
			uint64_t H = 1469598103934665603ull;
			for (uint32_t X : V) { for (int B = 0; B < 4; ++B) { H ^= (X >> (8 * B)) & 0xFFu; H *= 1099511628211ull; } }
			return H;
		}
	};

	class FWebTexture
	{
	public:
		FWebBakeKey Key;                 // the stand-in's bake key (unused for a delivered texture)
		std::string Name;                // D-165: a delivered texture's name ("" for the stand-in)
		uint32_t Size = 0u;
		/** Every mip as RGBA16F halves (4 per texel, x fastest, then y, then z): the GPU's format,
		 *  so the upload is a copy, and half the memory of floats (a 256^3 tile is 128 MB). */
		std::vector<std::vector<uint16_t>> Mips;

		uint32_t NumMips() const { return uint32_t(Mips.size()); }
		uint32_t MipSize(uint32_t M) const { return Size >> M; }
		size_t TexelCount(uint32_t M) const { return Mips[M].size() / 4u; }
		bool IsValid() const { return Size > 0u && !Mips.empty(); }

		UniHlsl::float4 TexelAt(uint32_t M, size_t I) const
		{
			const uint16_t* H = Mips[M].data() + I * 4u;
			return UniHlsl::float4(HalfToFloat(H[0]), HalfToFloat(H[1]), HalfToFloat(H[2]), HalfToFloat(H[3]));
		}
		UniHlsl::float4 Texel(uint32_t M, uint32_t X, uint32_t Y, uint32_t Z) const
		{
			const size_t S = MipSize(M);
			return TexelAt(M, (size_t(Z) * S + Y) * S + X);
		}
		static void PutTexel(std::vector<uint16_t>& Mip, size_t I, double R, double G, double B, double A)
		{
			Mip[I * 4 + 0] = FloatToHalf(float(R)); Mip[I * 4 + 1] = FloatToHalf(float(G)); Mip[I * 4 + 2] = FloatToHalf(float(B)); Mip[I * 4 + 3] = FloatToHalf(float(A));
		}

		/** Bake Uni_WebChannels at InKey (Size a power of two), on InThreads threads (0: all). */
		static FWebTexture Bake(const FWebBakeKey& InKey, unsigned InThreads = 0u)
		{
			FWebTexture T;
			T.Key = InKey;
			T.Size = InKey.Size;
			const uint32_t N = InKey.Size, SS = std::max(InKey.Supersample, 1u);
			const UniHlsl::FUniFieldParams P = InKey.BakeParams();
			std::vector<uint16_t> M0(size_t(N) * N * N * 4u);
			const unsigned Threads = InThreads ? InThreads : std::max(1u, std::thread::hardware_concurrency());
			std::atomic<uint32_t> NextZ{ 0u };
			auto Work = [&]()
			{
				for (uint32_t Z = NextZ.fetch_add(1u); Z < N; Z = NextZ.fetch_add(1u))
				{
					for (uint32_t Y = 0; Y < N; ++Y)
					{
						for (uint32_t X = 0; X < N; ++X)
						{
							double Acc[4] = { 0.0, 0.0, 0.0, 0.0 };
							for (uint32_t K = 0; K < SS; ++K) for (uint32_t J = 0; J < SS; ++J) for (uint32_t I = 0; I < SS; ++I)
							{
								const UniHlsl::float3 Q((float(X) + (float(I) + 0.5f) / float(SS)) / float(N),
									(float(Y) + (float(J) + 0.5f) / float(SS)) / float(N),
									(float(Z) + (float(K) + 0.5f) / float(SS)) / float(N));
								const UniHlsl::float4 C = UniHlsl::Uni_WebChannels(Q, P);
								Acc[0] += C.x; Acc[1] += C.y; Acc[2] += C.z; Acc[3] += C.w;
							}
							const double Inv = 1.0 / double(SS * SS * SS);
							PutTexel(M0, (size_t(Z) * N + Y) * N + X, Acc[0] * Inv, Acc[1] * Inv, Acc[2] * Inv, Acc[3] * Inv);
						}
					}
				}
			};
			std::vector<std::thread> Pool;
			for (unsigned I = 1; I < Threads; ++I) { Pool.emplace_back(Work); }
			Work();
			for (std::thread& Th : Pool) { Th.join(); }
			T.Mips.push_back(std::move(M0));
			T.BuildMips();
			return T;
		}

		/** Mips 1.. from mip 0: each texel the exact mean of its 2 x 2 x 2 children, half-rounded. */
		void BuildMips()
		{
			Mips.resize(1);
			for (uint32_t S = Size / 2u; S >= 1u; S /= 2u)
			{
				const uint32_t Mc = NumMips() - 1u;
				const uint32_t Sc = S * 2u;
				std::vector<uint16_t> M(size_t(S) * S * S * 4u);
				for (uint32_t Z = 0; Z < S; ++Z) for (uint32_t Y = 0; Y < S; ++Y) for (uint32_t X = 0; X < S; ++X)
				{
					double A[4] = { 0.0, 0.0, 0.0, 0.0 };
					for (uint32_t K = 0; K < 2u; ++K) for (uint32_t J = 0; J < 2u; ++J) for (uint32_t I = 0; I < 2u; ++I)
					{
						const UniHlsl::float4 V = TexelAt(Mc, (size_t(2u * Z + K) * Sc + (2u * Y + J)) * Sc + (2u * X + I));
						A[0] += V.x; A[1] += V.y; A[2] += V.z; A[3] += V.w;
					}
					PutTexel(M, (size_t(Z) * S + Y) * S + X, A[0] / 8.0, A[1] / 8.0, A[2] / 8.0, A[3] / 8.0);
				}
				Mips.push_back(std::move(M));
				if (S == 1u) { break; }
			}
		}

		/** Cache file: "UWEB", version 1, key hash, size, mip count, then every mip as RGBA16F. */
		bool Save(const std::string& Path) const
		{
			FILE* F = UniFOpen(Path.c_str(), "wb");
			if (!F) { return false; }
			const uint32_t Hdr[4] = { 0x42455755u /* "UWEB" */, 1u, Size, NumMips() };
			const uint64_t H = Key.Hash();
			bool bOk = std::fwrite(Hdr, sizeof(Hdr), 1, F) == 1 && std::fwrite(&H, sizeof(H), 1, F) == 1;
			for (uint32_t M = 0; bOk && M < NumMips(); ++M) { bOk = std::fwrite(Mips[M].data(), 2, Mips[M].size(), F) == Mips[M].size(); }
			std::fclose(F);
			return bOk;
		}

		/** Load a cache file baked at InKey; false (and invalid) if missing or for another key. */
		static bool Load(const std::string& Path, const FWebBakeKey& InKey, FWebTexture& Out)
		{
			Out = FWebTexture();
			FILE* F = UniFOpen(Path.c_str(), "rb");
			if (!F) { return false; }
			uint32_t Hdr[4] = {};
			uint64_t H = 0;
			bool bOk = std::fread(Hdr, sizeof(Hdr), 1, F) == 1 && std::fread(&H, sizeof(H), 1, F) == 1
				&& Hdr[0] == 0x42455755u && Hdr[1] == 1u && Hdr[2] == InKey.Size && H == InKey.Hash();
			if (bOk)
			{
				Out.Key = InKey;
				Out.Size = Hdr[2];
				for (uint32_t M = 0; bOk && M < Hdr[3]; ++M)
				{
					const size_t S = Out.MipSize(M);
					std::vector<uint16_t> Hm(S * S * S * 4u);
					bOk = std::fread(Hm.data(), 2, Hm.size(), F) == Hm.size();
					Out.Mips.push_back(std::move(Hm));
				}
			}
			std::fclose(F);
			if (!bOk) { Out = FWebTexture(); }
			return bOk;
		}

		/** D-165: a texture delivered by the texture project (NoiseVolumeTile): <Dir>/<Name>.bin is
		 *  mip 0 and <Name>_mips.bin levels 1.. concatenated, raw little-endian, x fastest, cubic,
		 *  power of two (the sidecar <Name>.json documents the same). D-179: RGBA16F (8 bytes a
		 *  texel), RGBA8_UNORM (4) or RGB8_UNORM (3, alpha 0), told apart by the size (a cube of
		 *  one is never a cube of another); UNORM texels become halves of k / 255, so the GPU and
		 *  CPU read the same values from the same halves. The size is taken from the file and the
		 *  mip chain must be complete and exact in length. Returns false with a reason. */
		static bool LoadDelivered(const std::string& Dir, const std::string& InName, FWebTexture& Out, std::string* Err = nullptr)
		{
			Out = FWebTexture();
			auto Fail = [&](const std::string& Why) { if (Err) { *Err = Why; } Out = FWebTexture(); return false; };
			auto ReadAll = [](const std::string& Path, std::vector<uint8_t>& Data) -> bool
			{
				FILE* F = UniFOpen(Path.c_str(), "rb");
				if (!F) { return false; }
				std::fseek(F, 0, SEEK_END);
				const long Bytes = std::ftell(F);
				std::fseek(F, 0, SEEK_SET);
				bool bOk = Bytes > 0;
				if (bOk) { Data.resize(size_t(Bytes)); bOk = std::fread(Data.data(), 1, Data.size(), F) == Data.size(); }
				std::fclose(F);
				return bOk;
			};
			auto IsCube = [](size_t Texels, uint32_t& N) { N = 1u; while (size_t(N) * N * N < Texels) { N *= 2u; } return size_t(N) * N * N == Texels; };
			std::vector<uint8_t> B0, BRest;
			const std::string Base = Dir + "/" + InName;
			if (!ReadAll(Base + ".bin", B0)) { return Fail("cannot read " + Base + ".bin"); }
			uint32_t N = 0u, Bpt = 0u;
			for (const uint32_t Try : { 8u, 4u, 3u })
			{
				uint32_t Nt = 0u;
				if (B0.size() % Try == 0u && IsCube(B0.size() / Try, Nt)) { N = Nt; Bpt = Try; break; }
			}
			if (Bpt == 0u) { return Fail(Base + ".bin is not a power-of-two cube of RGBA16F, RGBA8 or RGB8 texels"); }
			if (N > 1u && !ReadAll(Base + "_mips.bin", BRest)) { return Fail("cannot read " + Base + "_mips.bin"); }
			size_t Want = 0;
			for (uint32_t S = N / 2u; S >= 1u; S /= 2u) { Want += size_t(S) * S * S * Bpt; if (S == 1u) { break; } }
			if (BRest.size() != Want) { return Fail(Base + "_mips.bin has the wrong length for a full chain below " + std::to_string(N) + "^3"); }
			auto ToHalves = [Bpt](const uint8_t* Src, size_t Texels, std::vector<uint16_t>& Dst)
			{
				Dst.resize(Texels * 4u);
				if (Bpt == 8u) { std::memcpy(Dst.data(), Src, Texels * 8u); return; }
				uint16_t Lut[256];
				for (int K = 0; K < 256; ++K) { Lut[K] = FloatToHalf(float(K) / 255.0f); }
				for (size_t I = 0; I < Texels; ++I)
				{
					for (uint32_t C = 0; C < 4u; ++C) { Dst[I * 4u + C] = C < Bpt ? Lut[Src[I * Bpt + C]] : uint16_t(0u); }
				}
			};
			Out.Name = InName;
			Out.Size = N;
			Out.Key.Size = N;
			Out.Mips.emplace_back();
			ToHalves(B0.data(), size_t(N) * N * N, Out.Mips.back());
			size_t Off = 0;
			for (uint32_t S = N / 2u; S >= 1u; S /= 2u)
			{
				const size_t C = size_t(S) * S * S;
				Out.Mips.emplace_back();
				ToHalves(BRest.data() + Off, C, Out.Mips.back());
				Off += C * Bpt;
				if (S == 1u) { break; }
			}
			for (const std::vector<uint16_t>& V : Out.Mips)
			{
				for (uint16_t H : V) { if ((H & 0x7C00u) == 0x7C00u) { return Fail(InName + ": non-finite texel"); } }
			}
			return true;
		}

		/** The mean of each channel over the tile: the last mip's single texel (exact for box mips). */
		UniHlsl::float4 Mean() const { return IsValid() ? TexelAt(NumMips() - 1u, 0) : UniHlsl::float4(0.0f, 0.0f, 0.0f, 0.0f); }

		/** The mip pointer table the CPU sampler reads (BindWebTexture fills it). */
		mutable std::vector<const uint16_t*> MipPtrs;
	};

	/** D-167: one node of the texture project's node list (<name>_nodes.json): position in tile
	 *  units [0, 1)^3, mass as a share of the tile's mass. */
	struct FWebNode
	{
		double Pos[3] = { 0.0, 0.0, 0.0 };
		double Mass = 0.0;
	};

	/** Read <Dir>/<Name>_nodes.json: its "nodes" array of [x, y, z, mass, count] rows (heaviest
	 *  first, as delivered; sorted again here). A minimal reader for exactly that layout. */
	inline bool LoadWebNodes(const std::string& Dir, const std::string& Name, std::vector<FWebNode>& Out, std::string* Err = nullptr)
	{
		Out.clear();
		const std::string Path = Dir + "/" + Name + "_nodes.json";
		FILE* F = UniFOpen(Path.c_str(), "rb");
		if (!F) { if (Err) { *Err = "cannot read " + Path; } return false; }
		std::string Text;
		char Buf[65536];
		for (size_t N; (N = std::fread(Buf, 1, sizeof(Buf), F)) > 0;) { Text.append(Buf, N); }
		std::fclose(F);
		const size_t Key = Text.find("\"nodes\"");
		size_t I = Key == std::string::npos ? std::string::npos : Text.find('[', Key + 7);
		if (I == std::string::npos) { if (Err) { *Err = Path + ": no \"nodes\" array"; } return false; }
		++I;   // inside the outer array
		while (true)
		{
			while (I < Text.size() && (Text[I] == ' ' || Text[I] == ',' || Text[I] == '\n' || Text[I] == '\r' || Text[I] == '\t')) { ++I; }
			if (I >= Text.size() || Text[I] == ']') { break; }
			if (Text[I] != '[') { if (Err) { *Err = Path + ": unexpected text in \"nodes\""; } Out.clear(); return false; }
			++I;
			double V[5] = { 0, 0, 0, 0, 0 };
			int K = 0;
			while (I < Text.size() && Text[I] != ']')
			{
				if (Text[I] == ',' || Text[I] == ' ') { ++I; continue; }
				char* End = nullptr;
				const double X = std::strtod(Text.c_str() + I, &End);
				if (End == Text.c_str() + I) { if (Err) { *Err = Path + ": bad number"; } Out.clear(); return false; }
				if (K < 5) { V[K] = X; }
				++K;
				I = size_t(End - Text.c_str());
			}
			++I;   // past ']'
			if (K < 4) { if (Err) { *Err = Path + ": a node row has fewer than 4 values"; } Out.clear(); return false; }
			FWebNode Nd;
			Nd.Pos[0] = V[0]; Nd.Pos[1] = V[1]; Nd.Pos[2] = V[2]; Nd.Mass = V[3];
			Out.push_back(Nd);
		}
		std::stable_sort(Out.begin(), Out.end(), [](const FWebNode& A, const FWebNode& B) { return A.Mass > B.Mass; });
		return !Out.empty();
	}

	/** Point the shader cores' CPU sampler at T (nullptr unbinds: the textured web then reads NaN
	 *  and places nothing). T must outlive every sample, including async integral jobs. */
	inline void BindWebTexture(const FWebTexture* T)
	{
		if (!T || !T->IsValid()) { UniHlsl::UniWebTextureView = UniHlsl::FWebTexView(); return; }
		T->MipPtrs.clear();
		for (const auto& V : T->Mips) { T->MipPtrs.push_back(V.data()); }
		UniHlsl::FWebTexView View;
		View.Mips = T->MipPtrs.data();
		View.Size = T->Size;
		View.NumMips = T->NumMips();
		UniHlsl::UniWebTextureView = View;
	}

	/** D-179: the galaxy textures the galaxy field samples (UNI_GAL_WARP_FINE / _WARP_LARGE /
	 *  _DETAIL): the noise project's warp_fine and warp_large (curl displacement, RGB, RMS 1;
	 *  A fBm) and galaxy_detail (R ridged lanes, G star-forming knots, B fBm, A superbubbles).
	 *  nullptr unbinds one (a galaxy whose texture terms are on then reads NaN and places
	 *  nothing: fail closed). Each must outlive every sample. */
	inline void BindGalaxyTexture(int Slot, const FWebTexture* T)
	{
		if (Slot < 0 || Slot >= UniHlsl::UNI_GAL_TEX_COUNT) { return; }
		if (!T || !T->IsValid()) { UniHlsl::UniGalaxyTextureViews[Slot] = UniHlsl::FWebTexView(); return; }
		T->MipPtrs.clear();
		for (const auto& V : T->Mips) { T->MipPtrs.push_back(V.data()); }
		UniHlsl::FWebTexView View;
		View.Mips = T->MipPtrs.data();
		View.Size = T->Size;
		View.NumMips = T->NumMips();
		UniHlsl::UniGalaxyTextureViews[Slot] = View;
	}

	/** D-179: the delivered names of the galaxy textures, by slot. */
	inline const char* GalaxyTextureName(int Slot)
	{
		static const char* Names[] = { "warp_fine", "warp_large", "galaxy_detail" };
		return (Slot >= 0 && Slot < UniHlsl::UNI_GAL_TEX_COUNT) ? Names[Slot] : "";
	}

	/** Switch every web emitter of a universe that this texture bakes to the textured field
	 *  (C.z = 1, C.w = texels per period). Returns the number switched; emitters with another
	 *  web stay procedural. */
	inline int EnableWebTexture(FUniverseDef& D, const FWebBakeKey& K)
	{
		int N = 0;
		for (FUniEmitter& E : D.RootEmitters)
		{
			if (E.FieldId != UniHlsl::UNI_FIELD_WEB || !K.Matches(E.Exemplar.ToHlsl())) { continue; }
			E.Exemplar.V[10] = 1.0f;
			E.Exemplar.V[11] = float(K.Size);
			++N;
		}
		return N;
	}

	/** The bake key of a universe's web (its first web emitter), if any. */
	inline bool FindWebKey(const FUniverseDef& D, uint32_t Size, uint32_t SS, FWebBakeKey& Out)
	{
		for (const FUniEmitter& E : D.RootEmitters)
		{
			if (E.FieldId == UniHlsl::UNI_FIELD_WEB) { Out = FWebBakeKey::FromParams(E.Exemplar.ToHlsl(), Size, SS); return true; }
		}
		return false;
	}
}
