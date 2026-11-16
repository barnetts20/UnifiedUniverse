// test_nebdetail.cpp
// D-241 (owner 2026-11-14: the nebulae's detail layers sliced and looked poor; "take a look at the
// way the cloud atmosphere layer handles its warp, structure, detail layers and its march"; then
// "the detail layer is too fine"): a spiral's nebulae as one field, CloudAtmosphere's way
// (Uni_GalaxyNebulaShapeAt): the knot read's structure, eroded by two warped detail reads.
//  - the constants: the shaders' defines equal the shim's knobs (the explorations sweep those);
//  - the light: over the Milky Way's gas layer, the shape's mean with both detail layers, with the
//    texture faded, with both faded and fully blended are one (the fade means hold it), and it is
//    D-234's (the scale);
//  - the dust: its multiplier's mean 1 where the structure is, and 1 outside it and once faded;
//  - the level of detail: by footprint alone (each layer full at its read's mip 2, out by mip 5),
//    detail 0 reads nothing and is bit for bit the faded shape at any footprint, the knots' blend
//    has no step term, early types keep D-236's shape;
//  - stills: a slice through the brightest knot (800 pc and 200 pc) and, from 250 pc back, the
//    mean of jittered frames (what the temporal pass converges to) against one unjittered frame.
//    The jittered mean per pixel within 1 point of the unjittered frame's error against the fine
//    reference and its light as near (D-256 moved the brightest knot: the jittered mean's per-pixel
//    error 0.7 points above one unjittered frame's there, with every D-258 term off too; 32 frames
//    the same: the steps' one-sample bias, not noise).

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FNdTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FNdTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FNdBind
	{
		explicit FNdBind(const FNdTex& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FNdBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	struct FNdSample { H::float3 P; H::float4 Neb; };

	FCoord NdCamAt(const FVec3d& P) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, P); return C; }

	std::string NdRenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	std::vector<float> NdMarch(const FVolumeScene& Sc, const FCamera& Cm, u32 Flags, float Shift)
	{
		auto V = ToHlsl(Sc.View);
		V.MarchFlags |= Flags; V.JitterShift = Shift;
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		std::vector<float> Rgb(size_t(Cm.Width) * Cm.Height * 3, 0.0f);
		std::atomic<int> Next{ 0 };
		auto Work = [&]()
		{
			for (int Y = Next++; Y < Cm.Height; Y = Next++) for (int X = 0; X < Cm.Width; ++X)
			{
				bool bS = false;
				const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cm, Cm.Width, Cm.Height, X, Y, 3.0e38f, bS);
				const size_t I = (size_t(Y) * Cm.Width + X) * 3;
				Rgb[I] = R.Radiance.x; Rgb[I + 1] = R.Radiance.y; Rgb[I + 2] = R.Radiance.z;
			}
		};
		std::vector<std::thread> Ts; for (int T = 1; T < 2; ++T) { Ts.emplace_back(Work); }
		Work(); for (std::thread& T : Ts) { T.join(); }
		return Rgb;
	}

	double NdLum(const std::vector<float>& F, size_t I) { return 0.2126 * F[3 * I] + 0.7152 * F[3 * I + 1] + 0.0722 * F[3 * I + 2]; }
}

UNI_TEST(galaxy, nebula_detail)
{
	const UniVerify::FGalShapeOff ShapeOff;   // D-262 (B-041)
	const FNdTex Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.nebula_detail", "D-241 the nebulae's detail").Pass(false, "textures not loaded: " + Tx.Err); return; }
	const FNdBind Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const FFieldParams* Mw = nullptr;
	for (const FAnchor& A : D.Anchors) { if (!A.EmitterParams.empty()) { Mw = &A.EmitterParams[0]; } }
	if (!Mw) { Uni::Check(2, "T2.galaxy.nebula_detail", "D-241").Pass(false, "no anchor"); return; }
	const H::FUniFieldParams P = Mw->ToHlsl();
	const H::FUniGalaxyConst K = H::Uni_GalaxyConstOf(P);

	// The constants: the shaders' defines against the shim's knobs.
	std::string Src;
	{
		std::ifstream F("Shaders/Private/Core/UniFields.ush");
		std::stringstream B; B << F.rdbuf(); Src = B.str();
	}
	auto Def = [&](const char* Name) { const std::regex Re(std::string("#define\\s+") + Name + "\\s+([-0-9.eE]+)f?"); std::smatch M; return std::regex_search(Src, M, Re) ? std::stod(M[1].str()) : std::nan(""); };
	struct FKn { const char* Name; double Knob; };
	const FKn Kn[] = { { "UNI_NEB_FINE_FREQ", H::GKnobNebFreq }, { "UNI_NEB_FINE_OCTAVE", H::GKnobNebOctave }, { "UNI_NEB_EROSION1", H::GKnobNebErosion1 },
		{ "UNI_NEB_EROSION2", H::GKnobNebErosion2 }, { "UNI_NEB_WARP1", H::GKnobNebWarp1 }, { "UNI_NEB_WARP2", H::GKnobNebWarp2 }, { "UNI_NEB_FM1", H::GKnobNebFm1 },
		{ "UNI_NEB_FM2", H::GKnobNebFm2 }, { "UNI_NEB_LANE_MEAN", H::GKnobNebLaneMean }, { "UNI_NEB_SHAPE_MEAN2", H::GKnobNebShapeMean2 }, { "UNI_NEB_CORE", H::GKnobNebCore } };
	size_t ConstBad = 0;
	std::string ConstText;
	for (const FKn& C : Kn)
	{
		const double V = Def(C.Name);
		if (!(std::abs(V - C.Knob) <= 1e-6 * std::max(std::abs(C.Knob), 1e-6))) { ++ConstBad; ConstText += Uni::Fmt(" %s %g vs %g;", C.Name, V, C.Knob); }
	}

	// The light: the gas layer of an annulus (the knot terms from the field, footprint ~0).
	std::vector<FNdSample> S;
	const int N = 240000;
	for (int I = 0; I < N; ++I)
	{
		const double R = 0.2 + 0.5 * H::Uni_DrawUnit(0xA11u, 2u, u32(3 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(0xA11u, 2u, u32(3 * I + 1));
		const double Z = (2.0 * H::Uni_DrawUnit(0xA11u, 2u, u32(3 * I + 2)) - 1.0) * 0.5 * double(P.A.y);
		const H::float3 Pt{ float(R * std::cos(Ph)), float(R * std::sin(Ph)), float(Z) };
		H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
		H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-6f, C, Nv);
		S.push_back({ Pt, Nv });
	}
	const float FootTexOff = H::Uni_GalaxyNebulaFineTexel(P, 1.0f) * std::exp2(5.0f - UNI_NEB_FINE_MIP);   // the texture's read at mip 5, the walls' near mip 3
	const float FootOff = H::Uni_GalaxyNebulaFineTexel(P, 0.0f) * std::exp2(5.0f - UNI_NEB_FINE_MIP);      // the walls' at mip 5
	const float FootBlend = 2.0f * UNI_NEBULA_FOOT * std::exp2(UNI_NEBULA_MIP) / (P.G.w * std::max(P.H.w, 1.0f));   // the knots fully blended
	double Mf = 0, Mt = 0, Mo = 0, Md0 = 0, Mb = 0, Dust = 0, DustOut = 0, DustOff = 0;
	size_t NIn = 0, NOut = 0, Bit = 0;
	for (const FNdSample& Q : S)
	{
		float X = 0.0f, Md = 1.0f;
		Mf += double(H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, 0.0f, 1.0f, X, Md));
		if (X > 0.0f) { Dust += Md; ++NIn; } else { DustOut += Md; ++NOut; }
		float X2 = 0.0f, Md2 = 1.0f;
		Mt += double(H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, FootTexOff, 1.0f, X2, Md2));
		Mo += double(H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, FootOff, 1.0f, X2, Md2));
		DustOff += Md2;
		const float Sh0 = H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, 0.0f, 0.0f, X2, Md2);
		Md0 += double(Sh0);
		// Detail 0 is the faded shape at any footprint, bit for bit.
		const float ShF = H::Uni_GalaxyNebulaShapeAt(Q.P, Q.Neb, P, FootOff, 0.0f, X2, Md2);
		Bit += (ShF != Sh0) ? 1 : 0;
		if (Q.Neb.x > 0.0f) { float Xb = 0.0f, Mdb = 1.0f, Mrb = 1.0f; Mb += double(H::Uni_GalaxyNebulaDensityAt(Q.P, Q.Neb, P, FootBlend, 0.0f, 1.0f, -1.0f, Xb, Mdb, Mrb)) / (double(Q.Neb.x) * UNI_NEBULA_SCALE); }
		else { Mb += double(H::UNI_NEBULA_SHAPE_MEAN); }
	}
	Mf /= N; Mt /= N; Mo /= N; Md0 /= N; Mb /= N; Dust /= std::max<size_t>(NIn, 1); DustOut /= std::max<size_t>(NOut, 1); DustOff /= N;
	const double Ref = H::UNI_NEBULA_SHAPE_MEAN;
	const double LightWorst = std::max({ std::abs(Mf / Ref - 1.0), std::abs(Mt / Ref - 1.0), std::abs(Mo / Ref - 1.0), std::abs(Md0 / Ref - 1.0), std::abs(Mb / Ref - 1.0) });

	// The level of detail.
	const float L1n = H::Uni_GalaxyNebulaFineLod(P, 0.0f, 0.0f), L1f = H::Uni_GalaxyNebulaFineLod(P, 0.0f, FootOff);
	const bool bLod = L1n == UNI_NEB_FINE_MIP && H::Uni_GalaxyNebulaFineStrength(L1n) == 1.0f && std::abs(L1f - 5.0f) < 1e-3f && H::Uni_GalaxyNebulaFineStrength(L1f) == 0.0f
		&& H::Uni_GalaxyNebulaBlend(P, 0.0f, 0.0f) == 0.0f && H::Uni_GalaxyNebulaBlend(P, FootBlend, 0.0f) == 1.0f;
	size_t EarlyBad = 0;
	{
		const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
		for (u32 I = 0; I < 32u; ++I)
		{
			FFieldParams Pf = E.EarlyExemplar;
			for (const FParamRange& R : E.EarlyRanges) { Pf.V[R.Slot] = H::Uni_RollRange(H::Uni_SeedMix(0xF373u, I), ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
			Pf.V[20] = 1.0f;
			const H::FUniFieldParams Pq = Pf.ToHlsl();
			const H::float4 Nv(1.0f, 0.567f * 1.3f, 0.5f, 0.6f);
			float X = 0.0f, Md = 0.0f;
			const float Sh = H::Uni_GalaxyNebulaShapeAt(H::float3{ 0.01f, 0.02f, 0.0f }, Nv, Pq, 0.0f, 1.0f, X, Md);
			const float Xe = H::Uni_GalaxyNebulaX(Nv, Pq);
			EarlyBad += (Sh != Xe * Xe * std::clamp(1.0f + 2.0f * (Nv.w - 0.493f), 0.2f, 1.8f) || Md != 1.0f) ? 1 : 0;
		}
	}

	// The brightest knot, for the stills.
	double Hx = 0.0, Hy = 0.0, Hb = -1.0;
	for (int I = 0; I < 60000; ++I)
	{
		const double R = 0.3 + 0.4 * H::Uni_DrawUnit(0xF374u, 2u, u32(2 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(0xF374u, 2u, u32(2 * I + 1));
		const H::float3 Pt{ float(R * std::cos(Ph)), float(R * std::sin(Ph)), 0.0f };
		H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
		const float Rho = H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-5f, C, Nv);
		const double Sc = double(H::Uni_GalaxyNebulaX(Nv, P)) * double(H::Uni_GalaxyNebulaDensity(Nv, P, 1e-5f)) / std::max(double(Rho), 1e-30);
		if (Sc > Hb) { Hb = Sc; Hx = Pt.x; Hy = Pt.y; }
	}
	// Slices: D-234's shape and the rebuilt one, 800 pc and 200 pc a side, the mid-plane.
	{
		const int W = 256;
		FImage Img(W * 2, W * 2);
		const double UnitPc = 24000.0;
		for (int Row = 0; Row < 2; ++Row)
		{
			const double Span = (Row == 0 ? 800.0 : 200.0) / UnitPc;
			const float Foot = float(Span / W);
			for (int Y = 0; Y < W; ++Y) for (int X = 0; X < W; ++X)
			{
				const H::float3 Pt{ float(Hx + ((X + 0.5) / W - 0.5) * Span), float(Hy + (0.5 - (Y + 0.5) / W) * Span), 0.0f };
				H::float4 C(0, 0, 0, 0), Nv(0, 0, 0, 0);
				H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-6f, C, Nv);
				const float Xx = H::Uni_GalaxyNebulaX(Nv, P);
				const float V0 = Xx * Xx * std::clamp(1.0f + 2.0f * (Nv.w - 0.493f), 0.2f, 1.8f);
				float Xs = 0.0f, Md = 1.0f;
				const float V1 = H::Uni_GalaxyNebulaShapeAt(Pt, Nv, P, Foot, 1.0f, Xs, Md) / (H::UNI_NEBULA_SHAPE_MEAN / UNI_NEB_SHAPE_MEAN2);
				const float G0 = std::sqrt(std::min(V0 / 1.2f, 1.0f)), G1 = std::sqrt(std::min(V1 / 1.2f, 1.0f));
				const float Dk = std::min(std::max(Md - 1.0f, 0.0f) * 0.15f, 0.5f);
				Img.Add(X, Row * W + Y, G0, 0.55f * G0, 0.65f * G0);
				Img.Add(W + X, Row * W + Y, G1, 0.55f * G1 + 0.2f * Dk, 0.65f * G1 + 0.35f * Dk);
			}
		}
		const std::string Base = NdRenderDir() + "/still_nebula_detail";
		Img.WritePpm(Base + ".ppm");
		if (FILE* Fh = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(Fh, "{\"lines\":[\"D-241: a slice through the Milky Way's brightest disc nebula, 800 pc (top) and 200 pc (bottom): D-234's shape (left), the rebuilt field (right: structure eroded by the warped walls and texture; dust lanes tinted)\"]}\n");
			std::fclose(Fh);
		}
	}
	// The march: from 250 pc back, one unjittered frame and the mean of 12 jittered ones.
	double ErrUn = 0.0, ErrJit = 0.0, LightUn = 0.0, LightJit = 0.0;
	{
		FUniverseModel Mo2(D);
		FRefGenerator G(Mo2);
		FVolumeBuilder VB(Mo2, G);
		double Radius = 0.0;
		auto Build = [&](const FCoord& Cam, const FCamera& Cm)
		{
			FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cam, FVec3f{});
			FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f;
			FVolumeScene Sc = VB.Build(Pp, Cam, FVec3f{}, Cm.View(), Sv).Subset(false);
			for (const FCarrierGPU& Cg : Sc.Carriers)
			{
				const double Rl = std::sqrt(double(Cg.CamLocal[0]) * Cg.CamLocal[0] + double(Cg.CamLocal[1]) * Cg.CamLocal[1] + double(Cg.CamLocal[2]) * Cg.CamLocal[2]);
				if (Cg.FieldId == H::UNI_FIELD_GALAXY && Rl < 1.0) { Radius = 1.0 / double(Cg.InvRadius); }
			}
			return Sc;
		};
		{ FCamera Cm; Cm.Coord = NdCamAt({ 8.0 * Units::Kpc, 0.0, 0.0 }); Cm.Width = 4; Cm.Height = 4; Cm.LookAlong({ 0.0, 1.0, 0.0 }); Build(Cm.Coord, Cm); }
		const int W = 192, Ht = 108;
		FCamera Cm; Cm.Coord = NdCamAt({ Hx * Radius, Hy * Radius - 250.0 * Units::Pc, 5.0 * Units::Pc }); Cm.Width = W; Cm.Height = Ht; Cm.LookAlong({ 0.3, 1.0, 0.05 });
		const FVolumeScene Sc = Build(Cm.Coord, Cm);
		const std::vector<float> A = NdMarch(Sc, Cm, 0u, 0.0f);
		FVolumeScene Sf = Sc; Sf.View.MaxSteps = 16384u;
		const std::vector<float> Ref2 = NdMarch(Sf, Cm, 2u, 0.0f);
		std::vector<float> Jm(A.size(), 0.0f);
		const int Nj = 12;
		for (int Kj = 0; Kj < Nj; ++Kj)
		{
			const std::vector<float> J = NdMarch(Sc, Cm, 8u, float(std::fmod(Kj * 0.6180339887, 1.0)));
			for (size_t I = 0; I < J.size(); ++I) { Jm[I] += J[I] / float(Nj); }
		}
		const size_t Np = size_t(W) * Ht;
		double M = 0.0;
		for (size_t I = 0; I < Np; ++I) { M += NdLum(Ref2, I) / Np; }
		for (size_t I = 0; I < Np; ++I) { LightUn += NdLum(A, I) / std::max(M, 1e-300) / Np; LightJit += NdLum(Jm, I) / std::max(M, 1e-300) / Np; }
		for (size_t I = 0; I < Np; ++I) { ErrUn += std::abs(NdLum(A, I) - NdLum(Ref2, I)) / std::max(M, 1e-300) / Np; ErrJit += std::abs(NdLum(Jm, I) - NdLum(Ref2, I)) / std::max(M, 1e-300) / Np; }
		std::vector<double> L; for (size_t I = 0; I < Np; ++I) { L.push_back(NdLum(Ref2, I)); }
		std::sort(L.begin(), L.end());
		const double Top = L[size_t(0.995 * (L.size() - 1))];
		FImage Img(W * 3, Ht);
		const std::vector<float>* Fr[3] = { &A, &Jm, &Ref2 };
		for (int Q = 0; Q < 3; ++Q) for (int Y = 0; Y < Ht; ++Y) for (int X = 0; X < W; ++X)
		{
			const std::vector<float>& F = *Fr[Q];
			const size_t I = size_t(Y) * W + X;
			const double Lr = NdLum(F, I) / std::max(Top, 1e-300);
			const double Sc2 = Lr > 0.0 ? (Lr / (1.0 + 0.25 * Lr)) / Lr / std::max(Top, 1e-300) : 0.0;
			Img.Add(Q * W + X, Y, float(F[3 * I] * Sc2), float(F[3 * I + 1] * Sc2), float(F[3 * I + 2] * Sc2));
		}
		const std::string Base = NdRenderDir() + "/still_nebula_march";
		Img.WritePpm(Base + ".ppm");
		if (FILE* Fh = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(Fh, "{\"lines\":[\"D-240/D-241: 250 pc from the Milky Way's brightest disc nebula: one unjittered march (left), the mean of 12 jittered (centre: what the temporal pass converges to), the fine reference (right)\"]}\n");
			std::fclose(Fh);
		}
	}

	const bool bPass = ConstBad == 0 && LightWorst < 0.02 && std::abs(Dust - 1.0) < 0.02 && std::abs(DustOut - 1.0) < 1e-6 && std::abs(DustOff - 1.0) < 1e-6 && Bit == 0
		&& bLod && EarlyBad == 0 && ErrJit < ErrUn + 0.01 && std::abs(LightJit - 1.0) <= std::abs(LightUn - 1.0) + 0.002;
	Uni::Check(2, "T2.galaxy.nebula_detail", "D-241 (D-262: with D-258's galaxy shaping off, B-041) the nebulae as one field, CloudAtmosphere's way: the shaders' constants equal the shim's knobs; over the Milky Way's gas layer the shape's mean with both detail layers, the texture faded, both faded and fully blended within 2% of D-234's; the dust multiplier's mean 1 (2%) where the structure is, exactly 1 outside it and faded; each layer full at its read's mip 2 and out by mip 5, the knots' blend by footprint alone; detail 0 the faded shape bit for bit at any footprint; early types D-236's shape; from 250 pc the mean of 12 jittered frames per pixel within 1 point of one unjittered march's error against the fine reference and its frame's light as near (0.2%) (D-256's Milky Way: 5.0% / 5.7%, light x0.989 / x0.995; before it 5.9% / 5.4%); still_nebula_detail, still_nebula_march")
		.Metric("light_worst", LightWorst).Metric("dust_mean", Dust).Metric("err_unjittered", ErrUn).Metric("err_jittered", ErrJit)
		.Pass(bPass, Uni::Fmt("constants: %zu differ%s; shape mean x %.4f full, %.4f texture faded, %.4f both, %.4f blended of D-234's; dust mean %.4f inside (%zu samples), %.6f outside, %.6f faded; %zu faded shapes differ; lod %s; %zu early types off; from 250 pc vs the fine reference: unjittered %.2f%% (light x%.4f), jittered mean %.2f%% (x%.4f)",
			ConstBad, ConstText.c_str(), Mf / Ref, Mt / Ref, Mo / Ref, Mb / Ref, Dust, NIn, DustOut, DustOff, Bit, bLod ? "ok" : "off", EarlyBad, 100.0 * ErrUn, LightUn, 100.0 * ErrJit, LightJit));
}
