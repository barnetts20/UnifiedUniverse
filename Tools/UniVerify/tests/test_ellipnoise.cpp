// test_ellipnoise.cpp
// D-232 (owner, 2026-11-11: "globular galaxies are currently a simple falloff - they should also
// get noise based modulation so that they get some organic variance... should randomly pick
// channel weights to apply from a packed noise so that we have variance between globs"):
// early types' spheroids take galaxy_detail as a multiplier, mixed per galaxy.
//  - the shaders' mip floor equals the C++ knob; the constant block carries the weights' hash;
//  - only textured early types roll it (spirals, dwarfs, the home galaxy and untextured scenes
//    are unchanged), each with its own shape seed;
//  - the core (inside 0.2 r_e) is exactly the smooth profile; the light is kept;
//  - the projected images vary (residual RMS) and differ between galaxies (low correlation);
//  - the march's steps through a spheroid resolve it (no slide as the steps move);
//  - a contact sheet of rolled ellipticals with the noise and without.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
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

	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	void WriteFrame(const FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Base = RenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I) { std::fprintf(F, "%s\"%s\"", I ? "," : "", Hud[I].c_str()); }
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}

	double ShaderConstant(const std::string& Text, const std::string& Name)
	{
		const std::regex Re("#define\\s+" + Name + "\\s+([-0-9.eE]+)f?");
		std::smatch M;
		return std::regex_search(Text, M, Re) ? std::stod(M[1].str()) : std::nan("");
	}

	/** An early type's parameters as the generator rolls them (UniGenRef: early exemplar, then
	 *  each early range by name from the object seed). */
	FFieldParams RolledEarly(const FUniEmitter& E, u32 ObjSeed)
	{
		FFieldParams P = E.EarlyExemplar;
		for (const FParamRange& R : E.EarlyRanges) { P.V[R.Slot] = H::Uni_RollRange(ObjSeed, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		return P;
	}

	FFieldParams RolledLate(const FUniEmitter& E, u32 ObjSeed)
	{
		FFieldParams P = E.Exemplar;
		for (const FParamRange& R : E.Ranges) { P.V[R.Slot] = H::Uni_RollRange(ObjSeed, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		return P;
	}

	FFieldParams NoiseOff(FFieldParams P) { P.V[13] = 0.0f; return P; }

	/** D-238: the spheroid without its globular clusters (their own check is T2.galaxy.globulars;
	 *  noise off also halves an early type's count, so they would differ in the core). */
	float SphField(const H::float3& InP, const H::FUniFieldParams& InPar, float InFoot)
	{
		const u32 Hgc = H::Uni_SeedMix(H::Uni_GalaxyConstOf(InPar).Hs, 15u);
		return H::UniField_GalaxyFiltered(InP, InPar, InFoot) - H::Uni_GalaxyGcDensity(InP, InPar, Hgc, InFoot, 0.0f) * (H::dot(InP, InP) < InPar.D.x * InPar.D.x ? std::min(std::max((InPar.D.x - std::sqrt(H::dot(InP, InP))) / (0.05f * InPar.D.x), 0.0f), 1.0f) : 0.0f);
	}

	double Unit(u32 S, u32 K) { return double(H::Uni_DrawUnit(S, 0xE11u, K)); }

	/** Stratified Monte Carlo integral of the field over the unit ball's cube. */
	double Integral(const H::FUniFieldParams& P, int G, u32 Seed)
	{
		double Sum = 0.0;
		for (int X = 0; X < G; ++X) for (int Y = 0; Y < G; ++Y) for (int Z = 0; Z < G; ++Z)
		{
			const u32 K = u32((X * G + Y) * G + Z);
			const H::float3 Pt(float(-1.0 + 2.0 * (X + Unit(Seed, 3 * K)) / G), float(-1.0 + 2.0 * (Y + Unit(Seed, 3 * K + 1)) / G), float(-1.0 + 2.0 * (Z + Unit(Seed, 3 * K + 2)) / G));
			Sum += double(SphField(Pt, P, 0.0f));
		}
		return Sum * 8.0 / double(G) / double(G) / double(G);
	}

	/** A view: unit direction, right and up for inclination Inc and azimuth Az. */
	struct FView
	{
		double Dv[3], Rt[3], Up[3];
		FView(double Inc, double Az)
		{
			Dv[0] = std::sin(Inc) * std::cos(Az); Dv[1] = std::sin(Inc) * std::sin(Az); Dv[2] = -std::cos(Inc);
			Rt[0] = -std::sin(Az); Rt[1] = std::cos(Az); Rt[2] = 0.0;
			Up[0] = Dv[1] * Rt[2] - Dv[2] * Rt[1]; Up[1] = Dv[2] * Rt[0] - Dv[0] * Rt[2]; Up[2] = Dv[0] * Rt[1] - Dv[1] * Rt[0];
		}
	};

	/** The light through pixel (U, V) of a view, marched with the galaxy's own step hint (as the
	 *  volume march steps, HintScale 1) from a start shifted by Phase of the first step: the steps
	 *  move with Phase as they move with the camera. */
	double MarchHint(const H::FUniFieldParams& P, const FView& Vw, double U, double V, float Foot, double Phase, int* OutSteps = nullptr)
	{
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
		const H::float3 Dir(float(Vw.Dv[0]), float(Vw.Dv[1]), float(Vw.Dv[2]));
		double O[3];
		for (int Ax = 0; Ax < 3; ++Ax) { O[Ax] = U * Vw.Rt[Ax] + V * Vw.Up[Ax] - 1.0 * Vw.Dv[Ax]; }
		double T = 0.0, Sum = 0.0;
		int Steps = 0;
		{
			const H::float3 P0 = H::float3(float(O[0]), float(O[1]), float(O[2]));
			T = Phase * double(H::Uni_GalaxyStepHintFilteredK(P0, Dir, P, Foot, K.Hp));
		}
		while (T < 2.0 && Steps < 20000)
		{
			const H::float3 Pt(float(O[0] + T * Vw.Dv[0]), float(O[1] + T * Vw.Dv[1]), float(O[2] + T * Vw.Dv[2]));
			const double S = double(H::Uni_GalaxyStepHintFilteredK(Pt, Dir, P, Foot, K.Hp));
			const H::float3 Pm(float(O[0] + (T + 0.5 * S) * Vw.Dv[0]), float(O[1] + (T + 0.5 * S) * Vw.Dv[1]), float(O[2] + (T + 0.5 * S) * Vw.Dv[2]));
			if (H::dot(Pm, Pm) < 1.0f) { Sum += double(SphField(Pm, P, Foot)) * S; }
			T += S;
			++Steps;
		}
		if (OutSteps) { *OutSteps = Steps; }
		return Sum;
	}

	/** Fixed fine steps (the reference for the march's error). */
	double MarchFine(const H::FUniFieldParams& P, const FView& Vw, double U, double V, float Foot, int N)
	{
		double O[3];
		for (int Ax = 0; Ax < 3; ++Ax) { O[Ax] = U * Vw.Rt[Ax] + V * Vw.Up[Ax] - 1.0 * Vw.Dv[Ax]; }
		const double Dt = 2.0 / N;
		double Sum = 0.0;
		for (int S = 0; S < N; ++S)
		{
			const double T = (S + 0.5) * Dt;
			const H::float3 Pt(float(O[0] + T * Vw.Dv[0]), float(O[1] + T * Vw.Dv[1]), float(O[2] + T * Vw.Dv[2]));
			if (H::dot(Pt, Pt) < 1.0f) { Sum += double(SphField(Pt, P, Foot)) * Dt; }
		}
		return Sum;
	}

	/** Swim: the light-weighted mean over pixels of the spread (max - min) across step phases
	 *  over the mean, for one galaxy and view, Px pixels a side over [-Half, Half]. */
	double Swim(const H::FUniFieldParams& P, const FView& Vw, int Px, double Half, float Foot, double* OutFineErr = nullptr)
	{
		const double Ph[4] = { 0.0, 0.25, 0.5, 0.75 };
		double Wsum = 0.0, Ssum = 0.0, Esum = 0.0;
		for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
		{
			const double U = -Half + (X + 0.5) * 2.0 * Half / Px, V = Half - (Y + 0.5) * 2.0 * Half / Px;
			double Lo = 1e300, Hi = -1e300, Mean = 0.0;
			for (double F : Ph) { const double L = MarchHint(P, Vw, U, V, Foot, F); Lo = std::min(Lo, L); Hi = std::max(Hi, L); Mean += 0.25 * L; }
			if (!(Mean > 0.0)) { continue; }
			Wsum += Mean;
			Ssum += (Hi - Lo);
			if (OutFineErr) { Esum += std::abs(Mean - MarchFine(P, Vw, U, V, Foot, 4000)); }
		}
		if (OutFineErr) { *OutFineErr = Wsum > 0.0 ? Esum / Wsum : 0.0; }
		return Wsum > 0.0 ? Ssum / Wsum : 0.0;
	}

	/** The face-on light map (Px a side over [-Half, Half]), integrated along z. */
	std::vector<double> FaceOn(const H::FUniFieldParams& P, int Px, double Half, int Nz)
	{
		std::vector<double> L(size_t(Px) * Px, 0.0);
		const double Dz = 2.0 / Nz;
		for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
		{
			const double U = -Half + (X + 0.5) * 2.0 * Half / Px, V = -Half + (Y + 0.5) * 2.0 * Half / Px;
			double S = 0.0;
			for (int Z = 0; Z < Nz; ++Z)
			{
				const H::float3 Pt(float(U), float(V), float(-1.0 + (Z + 0.5) * Dz));
				S += double(SphField(Pt, P, 0.0f)) * Dz;
			}
			L[size_t(Y) * Px + X] = S;
		}
		return L;
	}

	/** A tile of a contact sheet: the light through each pixel, fine steps. */
	void Tile(FImage& Img, int Ox, int Oy, int Px, const H::FUniFieldParams& P, const FView& Vw, double Half)
	{
		std::vector<float> L(size_t(Px) * Px, 0.0f);
		float Peak = 0.0f;
		const float Foot = float(2.0 * Half / Px);
		for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
		{
			const double U = -Half + (X + 0.5) * 2.0 * Half / Px, V = Half - (Y + 0.5) * 2.0 * Half / Px;
			L[size_t(Y) * Px + X] = float(MarchFine(P, Vw, U, V, Foot, 400));
			Peak = std::max(Peak, L[size_t(Y) * Px + X]);
		}
		// Linear in the light (the PPM writer tonemaps 1 - exp(-I) with a 2.2 gamma).
		for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
		{
			const double V = L[size_t(Y) * Px + X] / std::max(double(Peak), 1e-30);
			const float I = float(3.0 * std::max(V, 0.0));
			Img.Add(Ox + X, Oy + Y, I * 1.0f, I * 0.86f, I * 0.66f);
		}
	}
}

UNI_TEST(galaxy, spheroid_noise)
{
	const FTexSet Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.spheroid_noise", "D-232 spheroid noise").Pass(false, "textures not loaded: " + Tx.Err); return; }
	FBind Bind(Tx);
	// The shaders' mip floor against the C++ knob.
	double Cf = std::nan("");
	{
		std::ifstream F("Shaders/Private/Core/UniFields.ush");
		std::stringstream B; B << F.rdbuf();
		Cf = ShaderConstant(B.str(), "UNI_GAL_SPH_MIP_FLOOR");
	}
	const bool bConst = Cf == double(H::GKnobSphMipFloor);

	// Who rolls it: textured early types only, each its own shape seed.
	FUniverseDef D = UniScenes::Base();
	FUniverseDef D0 = UniScenes::Base();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	const FUniEmitter& E0 = D0.Arch(UniScenes::kGalaxy).Emitters.at(0);
	size_t BadRoll = 0, LateNoise = 0, PlainNoise = 0;
	std::set<float> Seeds;
	for (u32 I = 0; I < 256u; ++I)
	{
		const u32 S = H::Uni_SeedMix(0xE111u, I);
		const FFieldParams Pe = RolledEarly(E, S), Pl = RolledLate(E, S), P0 = RolledEarly(E0, S);
		const bool bOk = Pe.V[13] >= 0.25f && Pe.V[13] <= 0.6f && Pe.V[27] >= 1.0f && Pe.V[27] <= 2.5f && Pe.V[25] > 0.0f && Pe.V[31] == float(Tx.T[0].Size)
			&& H::Uni_GalaxyTextured(Pe.ToHlsl()) && H::Uni_GalaxySpheroidNoise(Pe.ToHlsl()) > 0.0f;
		BadRoll += bOk ? 0 : 1;
		Seeds.insert(Pe.V[22]);
		LateNoise += H::Uni_GalaxySpheroidNoise(Pl.ToHlsl()) > 0.0f ? 1 : 0;
		PlainNoise += (H::Uni_GalaxySpheroidNoise(P0.ToHlsl()) > 0.0f || H::Uni_GalaxyTextured(P0.ToHlsl())) ? 1 : 0;
	}
	size_t OtherNoise = 0;
	for (const FArchetype& A : D.Archetypes)
	{
		for (const FUniEmitter& Em : A.Emitters)
		{
			if (Em.FieldId != H::UNI_FIELD_GALAXY) { continue; }
			for (u32 I = 0; I < 64u; ++I) { OtherNoise += H::Uni_GalaxySpheroidNoise(RolledLate(Em, H::Uni_SeedMix(0xE112u, I)).ToHlsl()) > 0.0f ? 1 : 0; }
			// A dust set (DustFrom: D.y its sigma, no bulge) never reads as noise.
			for (u32 I = 0; I < 16u; ++I) { OtherNoise += H::Uni_GalaxySpheroidNoise(DustFrom(RolledEarly(Em, H::Uni_SeedMix(0xE113u, I)), A.Volume).ToHlsl()) > 0.0f ? 1 : 0; }
		}
	}
	for (const FAnchor& A : D.Anchors) { for (const FFieldParams& P : A.EmitterParams) { OtherNoise += H::Uni_GalaxySpheroidNoise(P.ToHlsl()) > 0.0f ? 1 : 0; } }

	// The constant block carries the weights' hash.
	size_t PackBad = 0;
	for (u32 I = 0; I < 64u; ++I)
	{
		const H::FUniFieldParams P = RolledEarly(E, H::Uni_SeedMix(0xE114u, I)).ToHlsl();
		const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);
		const H::FUniGalaxyConst K2 = H::Uni_GalaxyConstUnpack(H::Uni_GalaxyConstPack(K));
		PackBad += (K2.Hn != K.Hn || K2.Ho != K.Ho || K2.Hs != K.Hs || K2.Hp != K.Hp || K2.ArmH.x != K.ArmH.x || K2.ArmH.w != K.ArmH.w) ? 1 : 0;
	}

	// Core exact, light kept, images vary and differ.
	const int NGal = 8;
	size_t CoreBad = 0, CoreN = 0;
	double WorstLight = 0.0, MinRms = 1e9, MeanRms = 0.0, MaxCorr = 0.0, MeanCorr = 0.0;
	std::vector<std::vector<double>> Res(NGal);
	std::vector<std::vector<char>> Mask(NGal);
	const int Px = 64;
	const double Half = 0.5;
	for (int Gi = 0; Gi < NGal; ++Gi)
	{
		const FFieldParams Pf = RolledEarly(E, H::Uni_SeedMix(0xE115u, u32(Gi)));
		const H::FUniFieldParams P = Pf.ToHlsl(), Pn = NoiseOff(Pf).ToHlsl();
		const double Re = double(P.A.w), Q = double(P.B.x);
		for (int I = 0; I < 400; ++I)
		{
			const double Rr = 0.19 * Re * std::cbrt(Unit(0x51u + Gi, 4 * I)), Ct = 2.0 * Unit(0x51u + Gi, 4 * I + 1) - 1.0, Ph = 6.2831853 * Unit(0x51u + Gi, 4 * I + 2);
			const double St = std::sqrt(std::max(0.0, 1.0 - Ct * Ct));
			// Inside 0.2 r_e of the (unwarped) spheroid's own coordinates, shrunk by the warp's reach.
			const H::float3 Pt(float(Rr * St * std::cos(Ph)), float(Rr * St * std::sin(Ph)), float(Rr * Ct * Q));
			// The warp moves the point: only points whose warped spheroid radius is inside count.
			H::float3 Pd, Pb; H::float4 Dt; u32 bDt = 0u; float Fn = 0.0f, Sm = 0.0f; H::float3 Nk(0.0f, 0.0f, 0.0f);
			H::Uni_GalaxyTexTerms(Pt, P, H::Uni_GalaxyConstOf(P), 0.0f, H::Uni_GalaxyDiscSlab(P), true, false, Pd, Pb, Dt, bDt, Fn, Sm, Nk);
			const double Rs = std::sqrt(double(Pb.x) * Pb.x + double(Pb.y) * Pb.y + double(Pb.z / P.B.x) * (Pb.z / P.B.x));
			if (Rs >= 0.2 * Re) { continue; }
			const float A = SphField(Pt, P, 0.0f), B = SphField(Pt, Pn, 0.0f);
			++CoreN;
			// D-238: the globulars taken out (SphField) leave a rounding (a few ulp) where one sits.
			if (std::abs(double(A) - double(B)) > 1.0e-6 * std::max(std::abs(double(A)), 1e-30) || Sm != 1.0f) { ++CoreBad; }
		}
		const double Ion = Integral(P, 56, 0x77u + Gi), Ioff = Integral(Pn, 56, 0x77u + Gi);
		WorstLight = std::max(WorstLight, std::abs(Ion / std::max(Ioff, 1e-300) - 1.0));
		const std::vector<double> Lon = FaceOn(P, Px, Half, 160), Loff = FaceOn(Pn, Px, Half, 160);
		Res[Gi].assign(Lon.size(), 0.0);
		Mask[Gi].assign(Lon.size(), 0);
		double S2 = 0.0; int N = 0;
		for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
		{
			const double U = -Half + (X + 0.5) * 2.0 * Half / Px, V = -Half + (Y + 0.5) * 2.0 * Half / Px;
			const double R = std::sqrt(U * U + V * V);
			const size_t K = size_t(Y) * Px + X;
			if (R < Re || R > 4.0 * Re || !(Loff[K] > 0.0)) { continue; }
			Res[Gi][K] = Lon[K] / Loff[K] - 1.0;
			Mask[Gi][K] = 1;
			S2 += Res[Gi][K] * Res[Gi][K]; ++N;
		}
		const double Rms = N ? std::sqrt(S2 / N) : 0.0;
		MinRms = std::min(MinRms, Rms);
		MeanRms += Rms / NGal;
	}
	int Pairs = 0;
	for (int A = 0; A < NGal; ++A) for (int B = A + 1; B < NGal; ++B)
	{
		double Sa = 0, Sb = 0, Saa = 0, Sbb = 0, Sab = 0; int N = 0;
		for (size_t K = 0; K < Res[A].size(); ++K)
		{
			if (!Mask[A][K] || !Mask[B][K]) { continue; }
			const double X = Res[A][K], Y = Res[B][K];
			Sa += X; Sb += Y; Saa += X * X; Sbb += Y * Y; Sab += X * Y; ++N;
		}
		if (N < 10) { continue; }
		const double Ca = Saa / N - (Sa / N) * (Sa / N), Cb = Sbb / N - (Sb / N) * (Sb / N);
		const double C = (Sab / N - (Sa / N) * (Sb / N)) / std::sqrt(std::max(Ca * Cb, 1e-300));
		MaxCorr = std::max(MaxCorr, std::abs(C));
		MeanCorr += std::abs(C);
		++Pairs;
	}
	MeanCorr = Pairs ? MeanCorr / Pairs : 1.0;

	// The march: steps from the galaxy's hint slide through the spheroid as the camera moves; the
	// noise must not add a slide of its own (spread across step phases, with and without), and the
	// march's error against fine steps stays the smooth profile's.
	double SwimOn = 0.0, SwimOff = 0.0, ErrOn = 0.0, ErrOff = 0.0;
	{
		const int Ng = 3, Pxs = 20;
		for (int Gi = 0; Gi < Ng; ++Gi)
		{
			const FFieldParams Pf = RolledEarly(E, H::Uni_SeedMix(0xE116u, u32(Gi)));
			const H::FUniFieldParams P = Pf.ToHlsl(), Pn = NoiseOff(Pf).ToHlsl();
			const FView Vw(0.4 + 0.5 * Gi, 0.9 * Gi);
			double Eo = 0.0, Ef = 0.0;
			// Close: a pixel 0.002 of the radius (mip 0, the floor binds).
			SwimOn += Swim(P, Vw, Pxs, 0.35, 0.002f, &Eo) / Ng;
			SwimOff += Swim(Pn, Vw, Pxs, 0.35, 0.002f, &Ef) / Ng;
			ErrOn += Eo / Ng; ErrOff += Ef / Ng;
		}
	}

	// Contact sheet: nine rolled ellipticals with the noise (left) and without (right), log stretch.
	{
		const int T = 120, Gn = 3;
		FImage Img(T * Gn * 2, T * Gn);
		for (int Gi = 0; Gi < Gn * Gn; ++Gi)
		{
			const FFieldParams Pf = RolledEarly(E, H::Uni_SeedMix(0x9A1Au, u32(Gi)));
			const FView Vw((20.0 + 7.0 * Gi) * 3.14159265358979 / 180.0, 0.7 * Gi);
			const int Ox = (Gi % Gn) * T, Oy = (Gi / Gn) * T;
			Tile(Img, Ox, Oy, T, Pf.ToHlsl(), Vw, 0.45);
			Tile(Img, Ox + T * Gn, Oy, T, NoiseOff(Pf).ToHlsl(), Vw, 0.45);
		}
		WriteFrame(Img, "still_galaxy_ellipticals", { "D-232: nine rolled early types with the spheroid noise (left) and without (right)",
			"light along each ray, 3 x each tile's peak into the PPM tonemap" });
	}

	const bool bPass = bConst && BadRoll == 0 && Seeds.size() >= 250 && LateNoise == 0 && PlainNoise == 0 && OtherNoise == 0 && PackBad == 0
		&& CoreBad == 0 && CoreN > 400 && WorstLight < 0.06 && MinRms > 0.03 && MeanCorr < 0.25 && SwimOn < SwimOff + 0.003 && ErrOn < ErrOff + 0.005;
	Uni::Check(2, "T2.galaxy.spheroid_noise", "D-232 spheroid noise: the shaders' mip floor is the C++ knob's; textured early types (only) roll it, each its own shape seed; the constant block carries its hash; the core (inside 0.2 r_e) is the smooth profile exactly; the light is kept within 6% (a low-frequency noise over one spheroid does not average to its mean); projected images vary (residual RMS over 3% in 1-4 r_e) and differ between galaxies (mean |correlation| under 0.25); the march's slide across step phases and its error against fine steps are the smooth spheroid's within 0.3% and 0.5% (textured spirals slide 0.09-0.13%)")
		.Metric("light_worst", WorstLight).Metric("rms_min", MinRms).Metric("rms_mean", MeanRms).Metric("corr_mean", MeanCorr).Metric("corr_max", MaxCorr)
		.Metric("swim_on", SwimOn).Metric("swim_off", SwimOff).Metric("err_on", ErrOn).Metric("err_off", ErrOff)
		.Pass(bPass, Uni::Fmt("floor %.2f (knob %.2f); 256 rolls: %zu bad, %zu shape seeds, late %zu / untextured %zu / others %zu with noise; pack %zu bad; core %zu of %zu differ; 8 ellipticals: light within %.2f%%, residual RMS %.1f%% min %.1f%% mean, |corr| %.3f mean %.3f max; march (close, 3 views): slide %.3f%% vs %.3f%% smooth, error vs fine %.3f%% vs %.3f%%",
			Cf, double(H::GKnobSphMipFloor), BadRoll, Seeds.size(), LateNoise, PlainNoise, OtherNoise, PackBad, CoreBad, CoreN, 100.0 * WorstLight, 100.0 * MinRms, 100.0 * MeanRms,
			MeanCorr, MaxCorr, 100.0 * SwimOn, 100.0 * SwimOff, 100.0 * ErrOn, 100.0 * ErrOff));
}
