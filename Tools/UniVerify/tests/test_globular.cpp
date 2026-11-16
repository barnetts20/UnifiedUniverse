// test_globular.cpp
// D-238 (owner, B-031: a galactic star-cluster layer; globular and open clusters): globular
// clusters as a field (Uni_GalaxyGc* in UniFields.ush) and their exact placement (UniGenTask.ush).
//  - gating: textured galaxies with a bulge only (spirals, early types x 2, the home galaxy);
//    untextured scenes and dust sets none;
//  - the Milky Way's census: about UNI_GC_N0 x its bulge weight globulars, their masses' sum the
//    system's (the lattice's occupancy capped at one a cell), their radii the halo's;
//  - placement, the real generator on the CPU (probes, counts, candidates from the map) in cells
//    around six globulars at four star levels: the stars inside 3 halo sigmas (less the field's
//    expected there) against the globular's expected count (its mass x the level's stars per unit
//    mass), its radial profile against the two Gaussians, candidates clipped under 1%;
//  - the volume: its light kept as it widens (footprint, step) and blends to the smooth halo;
//  - stills: the Milky Way's globulars face-on and edge-on, and the stars placed around one.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniGalaxyMap.h"
#include "UniGenRef.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FGcTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FGcTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FGcBind
	{
		explicit FGcBind(const FGcTex& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FGcBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	FFieldParams GcRoll(const FUniEmitter& E, u32 S, bool bEarly)
	{
		FFieldParams P = bEarly ? E.EarlyExemplar : E.Exemplar;
		for (const FParamRange& R : bEarly ? E.EarlyRanges : E.Ranges) { P.V[R.Slot] = H::Uni_RollRange(S, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		return P;
	}

	/** The cumulative mass share of the two Gaussians inside radius InR (3D). */
	double GcCdf(double InR, double InS1)
	{
		auto Chi3 = [](double X) { return std::erf(X / std::sqrt(2.0)) - std::sqrt(2.0 / 3.14159265358979) * X * std::exp(-0.5 * X * X); };
		const double S2 = double(H::UNI_GC_HALO) * InS1;
		return (1.0 - double(H::UNI_GC_HALO_SHARE)) * Chi3(InR / InS1) + double(H::UNI_GC_HALO_SHARE) * Chi3(InR / S2);
	}

	std::string GcRenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	void GcWriteFrame(const FImage& Img, const std::string& Name, const char* Hud)
	{
		const std::string Base = GcRenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb")) { std::fprintf(F, "{\"lines\":[\"%s\"]}\n", Hud); std::fclose(F); }
	}

	struct FGcInfo { int X, Y, Z; H::FUniGc G; double R; };
}

UNI_TEST(galaxy, globulars)
{
	const FGcTex Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.globulars", "D-238 globular clusters").Pass(false, "textures not loaded: " + Tx.Err); return; }
	const FGcBind Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	const FUniverseDef D0 = UniScenes::Base();
	const FUniEmitter& E0 = D0.Arch(UniScenes::kGalaxy).Emitters.at(0);

	// Gating.
	size_t SpiralN = 0, EarlyN = 0, PlainBad = 0, DustBad = 0;
	double SpiralMean = 0.0, EarlyMean = 0.0;
	for (u32 I = 0; I < 128u; ++I)
	{
		const u32 S = H::Uni_SeedMix(0x6C01u, I);
		const H::FUniFieldParams Ps = GcRoll(E, S, false).ToHlsl(), Pe = GcRoll(E, S, true).ToHlsl();
		const float Ns = H::Uni_GalaxyGcCount(Ps, H::Uni_SeedMix(H::Uni_GalaxyConstOf(Ps).Hs, 15u)), Ne = H::Uni_GalaxyGcCount(Pe, H::Uni_SeedMix(H::Uni_GalaxyConstOf(Pe).Hs, 15u));
		SpiralN += Ns > 0.0f ? 1 : 0; EarlyN += Ne > 0.0f ? 1 : 0; SpiralMean += Ns / 128.0; EarlyMean += Ne / 128.0;
		const H::FUniFieldParams P0 = GcRoll(E0, S, false).ToHlsl();
		PlainBad += H::Uni_GalaxyGcCount(P0, 1u) > 0.0f ? 1 : 0;
		const H::FUniFieldParams Pd = DustFrom(GcRoll(E, S, false), D.Arch(UniScenes::kGalaxy).Volume).ToHlsl();
		DustBad += H::Uni_GalaxyGcCount(Pd, 1u) > 0.0f ? 1 : 0;
	}

	// The Milky Way's contributor (the generator's, with its frame), its census.
	FUniverseModel M(D);
	FGalaxyMapCache Cache;
	FCpuGalaxyMaps Host(Cache, 4u, 512u, 4);
	FRefGenerator G(M);
	G.Maps = &Cache;
	FRefPipeline Pp(G);
	Pp.bVelocityLod = false;
	Pp.Update(UniScenes::SolarCamera(), FVec3f{});
	const FContributor* Mw = nullptr;
	for (const auto& KV : Pp.Contributors())
	{
		if (KV.second.Emitter && KV.second.Emitter->FieldId == H::UNI_FIELD_GALAXY && KV.second.InfluenceCm > 1.0e21 && KV.second.Emitter->ChildArchetypeId == UniScenes::kStar) { Mw = &KV.second; break; }
	}
	if (!Mw) { Uni::Check(2, "T2.galaxy.globulars", "D-238").Pass(false, "no Milky Way contributor"); return; }
	const FContributor Ct = *Mw;
	const H::FUniFieldParams P = Ct.Params.ToHlsl();
	const u32 Hgc = H::Uni_SeedMix(H::Uni_GalaxyConstOf(P).Hs, 15u);
	const float N = H::Uni_GalaxyGcCount(P, Hgc);
	const float L = H::Uni_GalaxyGcLattice(P);
	std::vector<FGcInfo> All;
	double MassSum = 0.0;
	{
		const int R = int(std::ceil(double(P.D.x) / double(L))) + 1;
		for (int Z = -R; Z <= R; ++Z) for (int Y = -R; Y <= R; ++Y) for (int X = -R; X <= R; ++X)
		{
			const H::FUniGc Gc = H::Uni_GalaxyGcAt(H::int3(X, Y, Z), P, Hgc, N, L);
			if (Gc.M > 0.0f) { All.push_back({ X, Y, Z, Gc, std::sqrt(double(H::dot(Gc.C, Gc.C))) }); MassSum += Gc.M; }
		}
	}
	const double MassExp = double(H::UNI_GC_MASS) * double(H::Uni_GalaxyBulgeIntegral(P));
	// Radii against the halo: the share inside the core radius and inside 4 core radii.
	auto PdfShare = [&](double Rmax)
	{
		double S = 0.0, T = 0.0;
		const int Ns = 4000;
		for (int I = 0; I < Ns; ++I)
		{
			const double R = (I + 0.5) / Ns * double(P.D.x) * 0.95;
			const double W = 4.0 * 3.14159265358979 * R * R * double(H::Uni_GalaxyGcPdf(float(R), P));
			T += W; if (R < Rmax) { S += W; }
		}
		return S / T;
	};
	const double Rc = double(H::UNI_GC_CORE) * double(P.A.w);
	double In1 = 0.0, In4 = 0.0;
	for (const FGcInfo& I : All) { In1 += I.R < Rc ? 1.0 : 0.0; In4 += I.R < 4.0 * Rc ? 1.0 : 0.0; }
	In1 /= std::max<double>(All.size(), 1.0); In4 /= std::max<double>(All.size(), 1.0);
	const double Exp1 = PdfShare(Rc), Exp4 = PdfShare(4.0 * Rc);

	// Placement around six globulars between 0.1 and 0.4 of the radius, at four star levels.
	std::vector<FGcInfo> Pick;
	for (const FGcInfo& I : All) { if (I.R > 0.1 && I.R < 0.4 && Pick.size() < 6u) { Pick.push_back(I); } }
	const double Rf = Ct.InfluenceCm;
	auto WorldOf = [&](const H::float3& Pf)
	{
		// field = Rot (world - centre) / influence: world = centre + Rot^T field x influence
		const double F[3] = { Pf.x, Pf.y, Pf.z };
		FVec3d W;
		W.X = (Ct.Rot[0] * F[0] + Ct.Rot[3] * F[1] + Ct.Rot[6] * F[2]) * Rf;
		W.Y = (Ct.Rot[1] * F[0] + Ct.Rot[4] * F[1] + Ct.Rot[7] * F[2]) * Rf;
		W.Z = (Ct.Rot[2] * F[0] + Ct.Rot[5] * F[1] + Ct.Rot[8] * F[2]) * Rf;
		FCoord C = Ct.Centre; FVec3f Fr; AddDisplacement(C, Fr, W);
		return C;
	};
	auto FieldOf = [&](const FCoord& C)
	{
		const FVec3d D3 = ToDouble(Sub(C, Ct.Centre));
		const double V[3] = { D3.X / Rf, D3.Y / Rf, D3.Z / Rf };
		return H::float3{ float(Ct.Rot[0] * V[0] + Ct.Rot[1] * V[1] + Ct.Rot[2] * V[2]), float(Ct.Rot[3] * V[0] + Ct.Rot[4] * V[1] + Ct.Rot[5] * V[2]), float(Ct.Rot[6] * V[0] + Ct.Rot[7] * V[1] + Ct.Rot[8] * V[2]) };
	};
	const int Levels[4] = { 64, 66, 68, 70 };
	double ObsSum = 0.0, ExpSum = 0.0, VarSum = 0.0;
	double Cand = 0.0, Acc = 0.0, Clip = 0.0;
	double ProfObs[4] = { 0, 0, 0, 0 }, ProfExp[4] = { 0, 0, 0, 0 }, ProfN = 0.0;
	int Truncated = 0, TasksRun = 0;
	std::string LevelText;
	std::vector<H::float3> Stars0;   // around the first globular at L64 (the still)
	for (int Li = 0; Li < 4; ++Li)
	{
		const int Lv = Levels[Li];
		const FSlice* Sl = M.SlicesOf(UniScenes::kStar).ForLevel(Lv);
		if (!Sl) { continue; }
		double ObsL = 0.0, ExpL = 0.0;
		const size_t Ng = Lv == 64 ? 2u : Pick.size();
		for (size_t Gi = 0; Gi < Ng && Gi < Pick.size(); ++Gi)
		{
			const H::FUniGc& Gc = Pick[Gi].G;
			const double Rz = 3.0 * double(H::UNI_GC_HALO) * double(Gc.S1);   // field units
			const FCoord Cc = WorldOf(Gc.C);
			const FCellKey K0 = CellOf(Cc, Lv);
			const double CellF = std::ldexp(1.0, Lv) / Rf;
			const int Nn = int(std::ceil(Rz / CellF));
			double Kappa = 0.0;
			double Obs = 0.0;
			for (int Dz = -Nn; Dz <= Nn; ++Dz) for (int Dy = -Nn; Dy <= Nn; ++Dy) for (int Dx = -Nn; Dx <= Nn; ++Dx)
			{
				const FCellKey Kc = Neighbour(K0, Dx, Dy, Dz);
				if (!G.Overlaps(Ct, Kc)) { continue; }
				FGenTaskGPU T;
				if (!G.BuildTask(Ct, Kc, T) || !G.ResolveMap(T, Ct)) { continue; }
				const FTaskCountGPU C = G.RunProbe(T);
				T.PoolCapacity = std::min(C.Count, G.MaxPerCell);
				if (T.PoolCapacity < C.Count) { ++Truncated; }
				const double Vb = double(T.Frame.SizeP) * double(T.Frame.SizeP) * double(T.Frame.SizeP);
				Kappa = double(T.LambdaScale) / Vb;   // stars per unit (density x volume) at this level
				++TasksRun;
				if (T.PoolCapacity == 0u) { continue; }
				std::vector<FObjectGPU> Recs;
				FTaskStats St;
				G.RunPlace(T, C, Recs, St);
				Cand += St.Candidates; Acc += St.Accepted; Clip += St.Clipped;
				for (const FObjectGPU& O : Recs)
				{
					if (((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) != 0u) { continue; }
					const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
					const H::float3 Pf = FieldOf(ObjectCoord(Kc, Off));
					const H::float3 Dd = Pf - Gc.C;
					const double R = std::sqrt(double(H::dot(Dd, Dd)));
					if (R < Rz)
					{
						Obs += 1.0;
						const double Rs[4] = { double(Gc.S1), 2.0 * double(Gc.S1), double(H::UNI_GC_HALO) * double(Gc.S1), 2.0 * double(H::UNI_GC_HALO) * double(Gc.S1) };
						for (int B = 0; B < 4; ++B) { ProfObs[B] += R < Rs[B] ? 1.0 : 0.0; }
						ProfN += 1.0;
						if (Lv == 64 && Gi == 0) { Stars0.push_back(Dd); }
					}
				}
			}
			// The field's expected stars in the sphere (its density without the globular, sampled).
			double Base = 0.0;
			{
				const H::FUniGalaxyConst Kg = H::Uni_GalaxyConstOf(P);
				double Sum = 0.0;
				int Nin = 0;
				for (int I = 0; I < 800; ++I)
				{
					const H::float3 U{ 2.0f * H::Uni_DrawUnit(0x6C02u, 2u, u32(3 * I)) - 1.0f, 2.0f * H::Uni_DrawUnit(0x6C02u, 2u, u32(3 * I + 1)) - 1.0f, 2.0f * H::Uni_DrawUnit(0x6C02u, 2u, u32(3 * I + 2)) - 1.0f };
					if (H::dot(U, U) > 1.0f) { continue; }
					const H::float3 Q = Gc.C + U * float(Rz);
					H::float4 Comp(0, 0, 0, 0);
					Sum += double(H::UniField_GalaxyFilteredCK(Q, P, Kg, 0.0f, Comp)) - double(H::Uni_GalaxyGcDensity(Q, P, Hgc, 0.0f, 0.0f));
					++Nin;
				}
				Base = Sum / std::max(Nin, 1) * (4.0 / 3.0 * 3.14159265358979) * Rz * Rz * Rz;   // the field's mass in the ball
			}
			const double ExpGc = Kappa * double(Gc.M) * GcCdf(Rz, double(Gc.S1));
			const double ExpBase = Kappa * Base;
			ObsL += Obs - ExpBase; ExpL += ExpGc;
			ObsSum += Obs - ExpBase; ExpSum += ExpGc; VarSum += ExpGc + ExpBase;
			for (int B = 0; B < 4; ++B)
			{
				const double Rs[4] = { double(Gc.S1), 2.0 * double(Gc.S1), double(H::UNI_GC_HALO) * double(Gc.S1), 2.0 * double(H::UNI_GC_HALO) * double(Gc.S1) };
				ProfExp[B] += Obs * GcCdf(Rs[B], double(Gc.S1)) / GcCdf(Rz, double(Gc.S1));
			}
		}
		LevelText += Uni::Fmt("%sL%d %.0f/%.0f", LevelText.empty() ? "" : ", ", Lv, ObsL, ExpL);
	}
	const double Ratio = ObsSum / std::max(ExpSum, 1e-300);
	const double Z = (ObsSum - ExpSum) / std::sqrt(std::max(VarSum, 1e-300));
	double ProfWorst = 0.0;
	for (int B = 0; B < 4; ++B) { ProfWorst = std::max(ProfWorst, std::abs(ProfObs[B] - ProfExp[B]) / std::max(ProfN, 1.0)); }
	const double ClipRate = Clip / std::max(Cand, 1.0), CandPer = Cand / std::max(Acc, 1.0);

	// The volume: one globular's light over its cell as the footprint widens (exact, the blend, the
	// smooth halo), against its mass; and the smooth halo's mean over many cells against the lattice.
	double WidenWorst = 0.0;
	std::string WidenText;
	{
		const FGcInfo& I0 = Pick.empty() ? All.front() : Pick.front();
		const H::float3 Lo{ float(I0.X) * L, float(I0.Y) * L, float(I0.Z) * L };
		for (float Wf : { 0.0f, 0.01f, 0.03f, 0.1f })
		{
			// Radial quadrature about the centre over 256 directions (the profile is isotropic; points
			// past its cell look up their own cells, as the volume does), less the smooth halo's blend.
			const float W = Wf * L;
			const float Se0 = std::sqrt(std::pow(H::UNI_GC_HALO * H::UNI_GC_SIGMA * 1.6f * P.A.w, 2.0f) + 0.5f * W * W);
			const double Tb0 = double(H::smoothstep(0.26f * H::UNI_GC_MARGIN * L, 0.5f * H::UNI_GC_MARGIN * L, Se0));
			const double S2e = std::sqrt(double(H::UNI_GC_HALO) * H::UNI_GC_HALO * I0.G.S1 * I0.G.S1 + 0.5 * double(W) * W);
			const double Rmax = 8.0 * S2e, Rmin = 0.01 * double(I0.G.S1);
			const int Nd = 256, Nr = 600;
			double Sum = 0.0;
			for (int Di = 0; Di < Nd; ++Di)
			{
				const double Cz = 1.0 - 2.0 * (Di + 0.5) / Nd, Sz = std::sqrt(std::max(0.0, 1.0 - Cz * Cz)), Ph = 2.399963 * Di;
				const double Dv[3] = { Sz * std::cos(Ph), Sz * std::sin(Ph), Cz };
				double Ray = 0.0;
				for (int Ri = 0; Ri < Nr; ++Ri)
				{
					const double T0 = std::log(Rmin) + (std::log(Rmax) - std::log(Rmin)) * Ri / Nr, T1 = std::log(Rmin) + (std::log(Rmax) - std::log(Rmin)) * (Ri + 1) / Nr;
					const double R = std::exp(0.5 * (T0 + T1)), Dr = std::exp(T1) - std::exp(T0);
					const H::float3 Q = I0.G.C + H::float3{ float(Dv[0] * R), float(Dv[1] * R), float(Dv[2] * R) };
					const double Rho = double(H::Uni_GalaxyGcDensity(Q, P, Hgc, W, W)) - double(H::Uni_GalaxyGcSmooth(Q, P, N, L)) * Tb0;
					Ray += Rho * R * R * Dr;
				}
				Sum += Ray * 4.0 * 3.14159265358979 / Nd;
			}
			// The core inside Rmin (its density there about the centre's).
			Sum += double(H::Uni_GalaxyGcDensity(I0.G.C, P, Hgc, W, W)) * (4.0 / 3.0) * 3.14159265358979 * Rmin * Rmin * Rmin;
			const float Se = std::sqrt(std::pow(H::UNI_GC_HALO * H::UNI_GC_SIGMA * 1.6f * P.A.w, 2.0f) + 0.5f * W * W);
			const double Tb = double(H::smoothstep(0.26f * H::UNI_GC_MARGIN * L, 0.5f * H::UNI_GC_MARGIN * L, Se));
			const double Want = double(I0.G.M) * (1.0 - Tb);
			const double Err = std::abs(Sum / std::max(Want, 1e-300) - 1.0);
			if (Tb < 0.99) { WidenWorst = std::max(WidenWorst, Err); }
			if (Tb < 0.99) { WidenText += Uni::Fmt("%s%.2fL: %.3f (blend %.2f)", WidenText.empty() ? "" : ", ", double(Wf), Sum / std::max(Want, 1e-300), Tb); }
			else { WidenText += Uni::Fmt("%s%.2fL: the smooth halo (blend 1)", WidenText.empty() ? "" : ", ", double(Wf)); }
		}
	}
	// The smooth halo's mass against the lattice's in a shell of cells (0.15-0.35 of the radius).
	double SmoothRatio = 0.0;
	{
		double Lat = 0.0, Smo = 0.0;
		for (const FGcInfo& I : All) { if (I.R > 0.15 && I.R < 0.35) { Lat += I.G.M; } }
		const int Ns = 400000;
		for (int K = 0; K < Ns; ++K)
		{
			const H::float3 Q{ 0.7f * (2.0f * H::Uni_DrawUnit(0x6C05u, 2u, u32(3 * K)) - 1.0f), 0.7f * (2.0f * H::Uni_DrawUnit(0x6C05u, 2u, u32(3 * K + 1)) - 1.0f), 0.7f * (2.0f * H::Uni_DrawUnit(0x6C05u, 2u, u32(3 * K + 2)) - 1.0f) };
			const double R = std::sqrt(double(H::dot(Q, Q)));
			if (R > 0.15 && R < 0.35) { Smo += double(H::Uni_GalaxyGcSmooth(Q, P, N, L)) * (1.4 * 1.4 * 1.4) / Ns; }
		}
		SmoothRatio = Smo / std::max(Lat, 1e-300);
	}

	// Stills: the Milky Way's globulars face-on (x, y) and edge-on (x, z), dot size by mass; the
	// stars placed around the first picked globular at L64 (x, y), log density.
	{
		const int W = 360;
		FImage Img(W * 3, W);
		for (const FGcInfo& I : All)
		{
			const double Sx = (I.G.C.x / 0.7 * 0.5 + 0.5) * W, Sy = (0.5 - I.G.C.y / 0.7 * 0.5) * W, Sz = (0.5 - I.G.C.z / 0.7 * 0.5) * W;
			const float B = float(std::min(3.0, 0.6 * std::sqrt(double(I.G.M) / std::max(MassSum / std::max<double>(All.size(), 1.0), 1e-300))));
			for (int Dy = -1; Dy <= 1; ++Dy) for (int Dx = -1; Dx <= 1; ++Dx)
			{
				const int X1 = int(Sx) + Dx, Y1 = int(Sy) + Dy, Y2 = int(Sz) + Dy;
				if (X1 >= 0 && X1 < W && Y1 >= 0 && Y1 < W) { Img.Add(X1, Y1, B, 0.85f * B, 0.6f * B); }
				if (X1 >= 0 && X1 < W && Y2 >= 0 && Y2 < W) { Img.Add(W + X1, Y2, B, 0.85f * B, 0.6f * B); }
			}
		}
		if (!Pick.empty())
		{
			std::vector<float> Hd(size_t(W) * W, 0.0f);
			const double Span = 3.0 * double(H::UNI_GC_HALO) * double(Pick[0].G.S1);
			for (const H::float3& Dd : Stars0)
			{
				const int X1 = int((Dd.x / Span * 0.5 + 0.5) * W), Y1 = int((0.5 - Dd.y / Span * 0.5) * W);
				if (X1 >= 0 && X1 < W && Y1 >= 0 && Y1 < W) { Hd[size_t(Y1) * W + X1] += 1.0f; }
			}
			float Mx = 1.0f; for (float V : Hd) { Mx = std::max(Mx, V); }
			for (int Y = 0; Y < W; ++Y) for (int X = 0; X < W; ++X)
			{
				const float V = Hd[size_t(Y) * W + X] > 0.0f ? std::log(1.0f + Hd[size_t(Y) * W + X]) / std::log(1.0f + Mx) : 0.0f;
				Img.Add(2 * W + X, Y, V, 0.9f * V, 0.75f * V);
			}
		}
		GcWriteFrame(Img, "still_globulars", "D-238: the Milky Way's globular clusters face-on and edge-on (+-0.7 of the field unit, dot size by mass); the stars placed around one at L64 (+-3 halo sigmas, log count)");
	}

	const double GcN = double(All.size());
	const bool bPass = SpiralN > 100 && EarlyN > 100 && PlainBad == 0 && DustBad == 0 && EarlyMean > 1.5 * SpiralMean
		&& GcN > 0.6 * N && GcN < 1.3 * N && std::abs(MassSum / MassExp - 1.0) < 0.35
		&& std::abs(In1 - Exp1) < 0.08 && std::abs(In4 - Exp4) < 0.1
		&& Pick.size() >= 4u && std::abs(Ratio - 1.0) < 0.08 && std::abs(Z) < 4.0 && ProfWorst < 0.04 && ClipRate < 0.01 && Truncated == 0
		&& WidenWorst < 0.05 && std::abs(SmoothRatio - 1.0) < 0.15;
	Uni::Check(2, "T2.galaxy.globulars", "D-238 globular clusters: textured galaxies with a bulge only (early types about twice the spirals), none untextured or on dust sets; the Milky Way's census within 0.6-1.3x its expected number, the masses' sum within 35% of the system's (occupancy capped), their radii the halo's (within 8-10 points); the real generator around picked globulars at L64-L70: stars within 3 halo sigmas (less the field's) within 8% of the expected and |z| < 4, their radial profile within 4 points of the two Gaussians, under 1% of candidates clipped, no cell truncated; the volume's light kept within 5% as it widens; the smooth halo the lattice's mass within 15%")
		.Metric("mw_count", GcN).Metric("mw_expected", double(N)).Metric("mass_ratio", MassSum / MassExp).Metric("placed_ratio", Ratio).Metric("placed_z", Z)
		.Metric("profile_worst", ProfWorst).Metric("clipped", ClipRate).Metric("candidates_per_star", CandPer).Metric("widen_worst", WidenWorst).Metric("smooth_ratio", SmoothRatio)
		.Pass(bPass, Uni::Fmt("128 rolls: %zu spirals (mean %.0f) / %zu early types (mean %.0f) with globulars, %zu untextured / %zu dust sets; Milky Way: %.0f globulars (N %.0f), mass x %.3f of the system's, inside the core %.2f (halo %.2f), inside 4 cores %.2f (%.2f); placed (%d tasks; obs/exp %s): x %.3f, z %+.2f, profile worst %.3f, clipped %.3f%%, candidates %.2f a star, %d truncated; widening %s; smooth halo x %.3f of the lattice",
			SpiralN, SpiralMean, EarlyN, EarlyMean, PlainBad, DustBad, GcN, double(N), MassSum / MassExp, In1, Exp1, In4, Exp4, TasksRun, LevelText.c_str(), Ratio, Z, ProfWorst, 100.0 * ClipRate, CandPer, Truncated, WidenText.c_str(), SmoothRatio));
}
