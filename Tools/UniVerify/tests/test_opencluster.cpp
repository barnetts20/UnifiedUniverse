// test_opencluster.cpp
// D-239 (owner, B-031: a galactic star-cluster layer; globular and open clusters): open clusters
// as a field (Uni_GalaxyOc* in UniFields.ush) and their placement (UniGenTask.ush).
//  - gating: textured spirals with arms and knots only; untextured scenes, early types and dust
//    sets none;
//  - the lattice: occupancy UNI_OC_OCC, the mass draw's mean 1, sigmas in range, centres within
//    the height scatter of the analytic warped mid-plane;
//  - their size: each cluster's stars (all levels) and the share of the disc's light they hold;
//  - the light moved, not added: in two patches of the Milky Way's disc the clusters' gathered
//    light (each its weight x the saturated young light at its centre) against UNI_OC_FRAC of
//    the saturated young light's integral there;
//  - the volume: one cluster's light kept as the footprint widens, and blended to the mean;
//  - placement, the real generator on the CPU (probes, counts, candidates from the map): around
//    six bright clusters at L64-L67 (listed, small boxes; L67 drawn by cell), and every cluster of
//    an 8 x 8 cell patch at L68 and L70 (drawn by cell): the stars inside 3 halo sigmas (less the
//    field's expected there) against the clusters' expected counts, the radial profile, the
//    stars' centroid against the centre, candidates clipped and per star;
//  - stills: the patch face-on (the disc's light at the mid-plane, clusters widened over a
//    pixel), the stars placed around one cluster at L64, and L68's stars over the patch.

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
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	struct FOcTex
	{
		FWebTexture T[3];
		bool bOk = true;
		std::string Err;
		FOcTex()
		{
			for (int S = 0; S < 3; ++S)
			{
				std::string E;
				if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bOk = false; Err += E + "; "; }
			}
		}
	};

	struct FOcBind
	{
		explicit FOcBind(const FOcTex& S) { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &S.T[I]); } }
		~FOcBind() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } }
	};

	FFieldParams OcRoll(const FUniEmitter& E, u32 S, bool bEarly)
	{
		FFieldParams P = bEarly ? E.EarlyExemplar : E.Exemplar;
		for (const FParamRange& R : bEarly ? E.EarlyRanges : E.Ranges) { P.V[R.Slot] = H::Uni_RollRange(S, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize); }
		return P;
	}

	double OcChi3(double X) { return std::erf(X / std::sqrt(2.0)) - std::sqrt(2.0 / 3.14159265358979) * X * std::exp(-0.5 * X * X); }

	/** The cumulative mass share of an open cluster's two Gaussians (core InS1, widened by InW2) inside radius InR (3D). */
	double OcCdf(double InR, double InS1, double InW2 = 0.0)
	{
		const double S1 = std::sqrt(InS1 * InS1 + InW2), S2 = std::sqrt(double(H::UNI_OC_HALO) * H::UNI_OC_HALO * InS1 * InS1 + InW2);
		return (1.0 - double(H::UNI_OC_HALO_SHARE)) * OcChi3(InR / S1) + double(H::UNI_OC_HALO_SHARE) * OcChi3(InR / S2);
	}

	std::string OcRenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	void OcWriteFrame(const FImage& Img, const std::string& Name, const char* Hud)
	{
		const std::string Base = OcRenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb")) { std::fprintf(F, "{\"lines\":[\"%s\"]}\n", Hud); std::fclose(F); }
	}

	/** The field's parts at InP: the density, the saturated young light, the clusters' part. */
	double OcParts(const H::float3& InP, const H::FUniFieldParams& P, const H::FUniGalaxyConst& K, double& OutYs, double& OutOc)
	{
		H::float4 Comp(0, 0, 0, 0);
		H::float2 Oc(0.0f, 0.0f);
		const double Rho = H::UniField_GalaxyO(InP, P, K, Comp, Oc);
		OutYs = Oc.x; OutOc = Oc.y;
		return Rho;
	}

	struct FOcInfo { int X, Y; H::FUniOc O; double Ys, M; };
}

UNI_TEST(galaxy, open_clusters)
{
	const FOcTex Tx;
	if (!Tx.bOk) { Uni::Check(2, "T2.galaxy.open_clusters", "D-239 open clusters").Pass(false, "textures not loaded: " + Tx.Err); return; }
	const FOcBind Bind(Tx);
	FUniverseDef D = UniScenes::Flythrough();
	UniScenes::UseGalaxyTextures(D, Tx.T[0].Size);
	UniScenes::UseGalaxyPalettes(D);
	const FUniEmitter& E = D.Arch(UniScenes::kGalaxy).Emitters.at(0);
	const FUniverseDef D0 = UniScenes::Base();
	const FUniEmitter& E0 = D0.Arch(UniScenes::kGalaxy).Emitters.at(0);

	// Gating.
	size_t SpiralOn = 0, EarlyOn = 0, PlainOn = 0, DustOn = 0, ArmsKnots = 0;
	for (u32 I = 0; I < 128u; ++I)
	{
		const u32 S = H::Uni_SeedMix(0x0C01u, I);
		const H::FUniFieldParams Ps = OcRoll(E, S, false).ToHlsl(), Pe = OcRoll(E, S, true).ToHlsl();
		SpiralOn += H::Uni_GalaxyOcOn(Ps) ? 1 : 0;
		ArmsKnots += (Ps.C.x > 0.0f && Ps.B.z > 0.0f && Ps.H.y > 0.0f) ? 1 : 0;
		EarlyOn += H::Uni_GalaxyOcOn(Pe) ? 1 : 0;
		PlainOn += H::Uni_GalaxyOcOn(OcRoll(E0, S, false).ToHlsl()) ? 1 : 0;
		DustOn += H::Uni_GalaxyOcOn(DustFrom(OcRoll(E, S, false), D.Arch(UniScenes::kGalaxy).Volume).ToHlsl()) ? 1 : 0;
	}

	// The Milky Way's contributor (the generator's, with its frame).
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
	if (!Mw) { Uni::Check(2, "T2.galaxy.open_clusters", "D-239").Pass(false, "no Milky Way contributor"); return; }
	const FContributor Ct = *Mw;
	const H::FUniFieldParams P = Ct.Params.ToHlsl();
	if (!H::Uni_GalaxyOcOn(P)) { Uni::Check(2, "T2.galaxy.open_clusters", "D-239").Pass(false, "the Milky Way has no open clusters"); return; }
	const H::FUniGalaxyConst Kg = H::Uni_GalaxyConstOf(P);
	const u32 Hoc = H::Uni_SeedMix(Kg.Hs, 16u);
	const float L = H::Uni_GalaxyOcLattice(P);
	const double Cut = P.D.x;
	const double Rf = Ct.InfluenceCm;
	const double PcPerUnit = Rf / 3.0857e18;
	const double StarsPerMass = Ct.NTotal * Ct.Keep / G.IntegralAll(Ct);   // stars (all levels) per unit of field mass

	// Two patches of the disc (60 x 60 lattice cells, centred at 0.3 and 0.5 of the cut): the lattice
	// census, the clusters' sizes, and their gathered light against frac of the saturated young
	// light's integral.
	const int Np = 60;
	size_t Cells = 0, Occupied = 0, SigmaBad = 0;
	double Wsum = 0.0, MidWorst = 0.0;
	double ConsRatio[2] = { 0.0, 0.0 }, ConsSe[2] = { 0.0, 0.0 };   // D-259: and the clusters' sampling error (relative)
	std::vector<FOcInfo> Patch0;   // the first patch's clusters
	std::vector<double> StarsOf;   // each cluster's stars, all levels (both patches)
	int Pc0x = 0, Pc0y = 0;
	for (int Pi = 0; Pi < 2; ++Pi)
	{
		const double Rc = (Pi == 0 ? 0.3 : 0.5) * Cut, Ang = Pi == 0 ? 0.6 : 2.3;
		const int Cx = int(std::floor(Rc * std::cos(Ang) / L)) - Np / 2, Cy = int(std::floor(Rc * std::sin(Ang) / L)) - Np / 2;
		if (Pi == 0) { Pc0x = Cx; Pc0y = Cy; }
		double Gathered = 0.0, Gathered2 = 0.0;
		for (int Y = Cy; Y < Cy + Np; ++Y) for (int X = Cx; X < Cx + Np; ++X)
		{
			++Cells;
			const H::FUniOc O = H::Uni_GalaxyOcAt(H::int2(X, Y), P, Hoc, Kg.Hp, L);
			if (!(O.Wm > 0.0f)) { continue; }
			++Occupied;
			const double Rn = std::min(std::sqrt(double(O.C.x) * O.C.x + double(O.C.y) * O.C.y) / Cut, 1.0);
			const double Hh = double(P.A.y) * (P.E.w > 0.0f ? 1.0 + double(P.E.w) * Rn : 1.0);
			const double Dz = double(O.C.z) - double(H::Uni_GalaxyWarpZAt(H::float2(O.C.x, O.C.y), P, Kg.Hp));
			MidWorst = std::max(MidWorst, std::abs(Dz) / Hh);
			// The draw: Wm = frac / occ x W x L^2 x 2 h e^(|dz| / h).
			Wsum += double(O.Wm) / (double(H::UNI_OC_FRAC) / double(H::UNI_OC_OCC) * double(L) * L * 2.0 * Hh * std::exp(std::abs(Dz) / Hh));
			const double S1n = double(O.S1) / (double(H::UNI_OC_SIGMA) * double(P.A.x));
			SigmaBad += (S1n < 0.599 || S1n > 1.601) ? 1 : 0;
			double Ys = 0.0, Oc = 0.0;
			OcParts(O.C, P, Kg, Ys, Oc);
			Gathered += double(O.Wm) * Ys; Gathered2 += double(O.Wm) * Ys * double(O.Wm) * Ys;
			StarsOf.push_back(StarsPerMass * double(O.Wm) * Ys);
			if (Pi == 0) { Patch0.push_back({ X, Y, O, Ys, double(O.Wm) * Ys }); }
		}
		// The saturated young light's integral over the patch, z over the analytic mid-plane +- 12 h.
		const int Gs = 96, Gz = 24;
		const double Zr = 12.0 * double(P.A.y) * (1.0 + std::max(double(P.E.w), 0.0));
		double Sum = 0.0;
		for (int Iy = 0; Iy < Gs; ++Iy) for (int Ix = 0; Ix < Gs; ++Ix)
		{
			const u32 Kk = u32(Iy * Gs + Ix);
			const double Xd = (Cx + Np * (Ix + H::Uni_DrawUnit(0x0C02u + Pi, 1u, 2 * Kk)) / Gs) * L, Yd = (Cy + Np * (Iy + H::Uni_DrawUnit(0x0C02u + Pi, 1u, 2 * Kk + 1)) / Gs) * L;
			const double Zw = H::Uni_GalaxyWarpZAt(H::float2(float(Xd), float(Yd)), P, Kg.Hp);
			for (int Iz = 0; Iz < Gz; ++Iz)
			{
				const double Zd = Zw - Zr + 2.0 * Zr * (Iz + H::Uni_DrawUnit(0x0C04u + Pi, 1u, Kk * Gz + Iz)) / Gz;
				double Ys = 0.0, Oc = 0.0;
				OcParts(H::float3{ float(Xd), float(Yd), float(Zd) }, P, Kg, Ys, Oc);
				Sum += Ys;
			}
		}
		const double YsInt = Sum / (double(Gs) * Gs * Gz) * (double(Np) * L) * (double(Np) * L) * 2.0 * Zr;
		ConsRatio[Pi] = Gathered / std::max(double(H::UNI_OC_FRAC) * YsInt, 1e-300);
		// The clusters are a Monte Carlo estimate of the integral (a lattice of random draws): its
		// relative standard error from their spread (a few thousand clusters; spiky young light).
		ConsSe[Pi] = std::sqrt(std::max(Gathered2 - Gathered * Gathered / double(Np * Np), 0.0)) / std::max(Gathered, 1e-300);   // every cell a draw (empty ones 0)
	}
	const double OccFrac = double(Occupied) / std::max<double>(Cells, 1.0);
	const double Wmean = Wsum / std::max<double>(Occupied, 1.0);
	std::sort(StarsOf.begin(), StarsOf.end());
	auto StarsQ = [&](double F) { return StarsOf.empty() ? 0.0 : StarsOf[size_t(F * double(StarsOf.size() - 1))]; };
	// The share of the disc's light the clusters hold (an annulus 0.2-0.6 of the cut, z +- 10 h).
	double HeldShare = 0.0;
	{
		double Held = 0.0, All = 0.0;
		const int Ns = 100000;
		for (int I = 0; I < Ns; ++I)
		{
			const double R = Cut * std::sqrt(0.04 + 0.32 * H::Uni_DrawUnit(0x0C07u, 1u, 3 * I)), Th = 6.2831853 * H::Uni_DrawUnit(0x0C07u, 1u, 3 * I + 1);
			const double Z = (2.0 * H::Uni_DrawUnit(0x0C07u, 1u, 3 * I + 2) - 1.0) * 10.0 * double(P.A.y);
			double Ys = 0.0, Oc = 0.0;
			const double Rho = OcParts(H::float3{ float(R * std::cos(Th)), float(R * std::sin(Th)), float(Z) }, P, Kg, Ys, Oc);
			Held += double(H::UNI_OC_FRAC) * Ys; All += Rho - Oc + double(H::UNI_OC_FRAC) * Ys;
		}
		HeldShare = Held / std::max(All, 1e-300);
	}

	// The volume: one isolated cluster's C about it as the footprint widens (W2 the widening's
	// variance), less the blend's mean, against (1 - blend) x its weight.
	double WidenWorst = 0.0;
	std::string WidenText;
	{
		const FOcInfo* Iso = nullptr;   // no other cluster within a cell
		for (const FOcInfo& I : Patch0)
		{
			if (I.X < Pc0x + 2 || I.X >= Pc0x + Np - 2 || I.Y < Pc0y + 2 || I.Y >= Pc0y + Np - 2) { continue; }
			bool bAlone = true;
			for (const FOcInfo& J : Patch0)
			{
				if (&I == &J) { continue; }
				const H::float3 Dd = I.O.C - J.O.C;
				if (double(Dd.x) * Dd.x + double(Dd.y) * Dd.y < double(L) * L) { bAlone = false; break; }
			}
			if (bAlone) { Iso = &I; break; }
		}
		if (Iso)
		{
			const double Smax = double(H::UNI_OC_HALO) * H::UNI_OC_SIGMA * 1.6 * P.A.x;
			for (double Wf : { 0.0, 0.02, 0.06, 0.2 })
			{
				const double W2 = (Wf * L) * (Wf * L);
				const double Tb = double(H::smoothstep(float(L / 12.0), float(L / 8.0), float(std::sqrt(Smax * Smax + W2))));
				if (Tb < 0.99)
				{
					const double S2e = std::sqrt(double(H::UNI_OC_HALO) * H::UNI_OC_HALO * Iso->O.S1 * Iso->O.S1 + W2);
					const double Rmax = std::min(8.0 * S2e, 0.45 * L), Rmin = 0.01 * double(Iso->O.S1);
					const int Nd = 256, Nr = 500;
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
							const H::float3 Q = Iso->O.C + H::float3{ float(Dv[0] * R), float(Dv[1] * R), float(Dv[2] * R) };
							Ray += (double(H::Uni_GalaxyOcClump(Q, P, Hoc, Kg.Hp, float(W2))) - double(H::UNI_OC_FRAC) * Tb) * R * R * Dr;
						}
						Sum += Ray * 4.0 * 3.14159265358979 / Nd;
					}
					Sum += (double(H::Uni_GalaxyOcClump(Iso->O.C, P, Hoc, Kg.Hp, float(W2))) - double(H::UNI_OC_FRAC) * Tb) * (4.0 / 3.0) * 3.14159265358979 * Rmin * Rmin * Rmin;
					const double Want = double(Iso->O.Wm) * (1.0 - Tb) * OcCdf(Rmax, double(Iso->O.S1), W2);
					WidenWorst = std::max(WidenWorst, std::abs(Sum / std::max(Want, 1e-300) - 1.0));
					WidenText += Uni::Fmt("%s%.2fL: %.3f (blend %.2f)", WidenText.empty() ? "" : ", ", Wf, Sum / std::max(Want, 1e-300), Tb);
				}
				else
				{
					const double C0 = double(H::Uni_GalaxyOcClump(Iso->O.C, P, Hoc, Kg.Hp, float(W2)));
					WidenWorst = std::max(WidenWorst, std::abs(C0 / double(H::UNI_OC_FRAC) - 1.0));
					WidenText += Uni::Fmt("%s%.2fL: the mean (C/frac %.4f)", WidenText.empty() ? "" : ", ", Wf, C0 / double(H::UNI_OC_FRAC));
				}
			}
		}
		else { WidenWorst = 1.0; WidenText = "no isolated cluster"; }
	}

	// Placement.
	auto WorldOf = [&](const H::float3& Pf)
	{
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
	// The field's mass in a cluster's ball without the clusters (sampled), per cluster.
	std::map<std::pair<int, int>, double> BaseOf;
	auto BaseMass = [&](const FOcInfo& Oi, double Rz)
	{
		const auto Key = std::make_pair(Oi.X, Oi.Y);
		const auto It = BaseOf.find(Key);
		if (It != BaseOf.end()) { return It->second; }
		double Sum = 0.0;
		int Nin = 0;
		for (int I = 0; I < 600; ++I)
		{
			const H::float3 U{ 2.0f * H::Uni_DrawUnit(0x0C06u, 2u, u32(3 * I)) - 1.0f, 2.0f * H::Uni_DrawUnit(0x0C06u, 2u, u32(3 * I + 1)) - 1.0f, 2.0f * H::Uni_DrawUnit(0x0C06u, 2u, u32(3 * I + 2)) - 1.0f };
			if (H::dot(U, U) > 1.0f) { continue; }
			double Ys = 0.0, Oc = 0.0;
			Sum += OcParts(Oi.O.C + U * float(Rz), P, Kg, Ys, Oc) - Oc;
			++Nin;
		}
		const double B = Sum / std::max(Nin, 1) * (4.0 / 3.0 * 3.14159265358979) * Rz * Rz * Rz;
		BaseOf[Key] = B;
		return B;
	};
	struct FRun { double Kappa = 0.0, Cand = 0.0, Acc = 0.0; int Mode = -1; std::vector<H::float3> Stars; };
	double Clip = 0.0, CandAll = 0.0;
	int Truncated = 0, TasksRun = 0;
	auto RunCell = [&](const FCellKey& Kc, FRun& R)
	{
		if (!G.Overlaps(Ct, Kc)) { return; }
		FGenTaskGPU T;
		if (!G.BuildTask(Ct, Kc, T) || !G.ResolveMap(T, Ct)) { return; }
		if (R.Mode < 0) { R.Mode = int(H::Uni_TaskEvalOf(FRefGenerator::ToHlsl(T)).OcMode); }
		const FTaskCountGPU C = G.RunProbe(T);
		T.PoolCapacity = std::min(C.Count, G.MaxPerCell);
		if (T.PoolCapacity < C.Count) { ++Truncated; }
		R.Kappa = double(T.LambdaScale) / (double(T.Frame.SizeP) * double(T.Frame.SizeP) * double(T.Frame.SizeP));
		++TasksRun;
		if (T.PoolCapacity == 0u) { return; }
		std::vector<FObjectGPU> Recs;
		FTaskStats St;
		G.RunPlace(T, C, Recs, St);
		R.Cand += St.Candidates; R.Acc += St.Accepted; Clip += St.Clipped; CandAll += St.Candidates;
		for (const FObjectGPU& O : Recs)
		{
			if (((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) != 0u) { continue; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			R.Stars.push_back(FieldOf(ObjectCoord(Kc, Off)));
		}
	};
	const char* ModeName[4] = { "none", "listed", "by cell", "mean" };
	double ObsSum[2] = { 0, 0 }, ExpSum[2] = { 0, 0 }, VarSum[2] = { 0, 0 };   // [listed, by cell]
	double ProfObs[4] = { 0, 0, 0, 0 }, ProfExp[4] = { 0, 0, 0, 0 }, ProfN = 0.0;
	double CentWorst = 0.0, CandBig = 0.0, AccBig = 0.0;
	std::string LevelText;
	auto Score = [&](const FOcInfo& Oi, const FRun& R, int Lv, std::vector<H::float3>* Keep)
	{
		const double S1 = Oi.O.S1;
		const double Rz = 3.0 * double(H::UNI_OC_HALO) * S1;
		const double Rs[4] = { S1, 2.0 * S1, double(H::UNI_OC_HALO) * S1, 2.0 * double(H::UNI_OC_HALO) * S1 };
		double Obs = 0.0, Cx = 0.0, Cy = 0.0, Cz = 0.0, Cn = 0.0, Pb[4] = { 0, 0, 0, 0 };
		for (const H::float3& Pf : R.Stars)
		{
			const H::float3 Dd = Pf - Oi.O.C;
			const double Rr = std::sqrt(double(H::dot(Dd, Dd)));
			if (!(Rr < Rz)) { continue; }
			Obs += 1.0;
			for (int B = 0; B < 4; ++B) { Pb[B] += Rr < Rs[B] ? 1.0 : 0.0; }
			if (Rr < 2.0 * S1) { Cx += Dd.x; Cy += Dd.y; Cz += Dd.z; Cn += 1.0; }
			if (Keep) { Keep->push_back(Dd); }
		}
		const double ExpOc = R.Kappa * Oi.M * OcCdf(Rz, S1);
		const double ExpBase = R.Kappa * BaseMass(Oi, Rz);
		const int Bk = R.Mode == 1 ? 0 : 1;
		ObsSum[Bk] += Obs - ExpBase; ExpSum[Bk] += ExpOc; VarSum[Bk] += ExpOc + ExpBase;
		// The profile and centroid where the cluster dominates its ball.
		if (ExpOc > 2.0 * ExpBase)
		{
			for (int B = 0; B < 4; ++B) { ProfObs[B] += Pb[B]; ProfExp[B] += Obs * OcCdf(Rs[B], S1) / OcCdf(Rz, S1); }
			ProfN += Obs;
			if (Cn >= 20.0) { CentWorst = std::max(CentWorst, std::sqrt(Cx * Cx + Cy * Cy + Cz * Cz) / Cn / S1 * std::sqrt(Cn) / 1.5); }
		}
		(void)Lv;
		return std::make_pair(Obs - ExpBase, ExpOc);
	};

	// Around the six brightest clusters of the first patch's inner 40 x 40 cells (L64: two).
	std::vector<FOcInfo> Pick;
	{
		std::vector<FOcInfo> Inner;
		for (const FOcInfo& I : Patch0) { if (I.X >= Pc0x + 10 && I.X < Pc0x + 50 && I.Y >= Pc0y + 10 && I.Y < Pc0y + 50) { Inner.push_back(I); } }
		std::sort(Inner.begin(), Inner.end(), [](const FOcInfo& A, const FOcInfo& B) { return A.M > B.M; });
		for (size_t I = 0; I < Inner.size() && Pick.size() < 6u; ++I) { Pick.push_back(Inner[I]); }
	}
	std::vector<H::float3> Stars0;   // around the first pick at L64 (the still)
	for (int Lv : { 64, 66, 67 })
	{
		if (!M.SlicesOf(UniScenes::kStar).ForLevel(Lv)) { LevelText += Uni::Fmt("%sL%d none", LevelText.empty() ? "" : ", ", Lv); continue; }
		double ObsL = 0.0, ExpL = 0.0, CandL = 0.0, AccL = 0.0;
		int Mode = -1;
		const size_t Ng = Lv == 64 ? 2u : Pick.size();
		for (size_t Gi = 0; Gi < Ng && Gi < Pick.size(); ++Gi)
		{
			const FOcInfo& Oi = Pick[Gi];
			const double Rz = 3.0 * double(H::UNI_OC_HALO) * double(Oi.O.S1);
			const FCellKey K0 = CellOf(WorldOf(Oi.O.C), Lv);
			const int Nn = int(std::ceil(Rz / (std::ldexp(1.0, Lv) / Rf)));
			FRun R;
			for (int Dz = -Nn; Dz <= Nn; ++Dz) for (int Dy = -Nn; Dy <= Nn; ++Dy) for (int Dx = -Nn; Dx <= Nn; ++Dx) { RunCell(Neighbour(K0, Dx, Dy, Dz), R); }
			Mode = R.Mode;
			const auto Oe = Score(Oi, R, Lv, (Lv == 64 && Gi == 0) ? &Stars0 : nullptr);
			ObsL += Oe.first; ExpL += Oe.second; CandL += R.Cand; AccL += R.Acc;
		}
		LevelText += Uni::Fmt("%sL%d %s %.0f/%.0f (%.2f cand/star)", LevelText.empty() ? "" : ", ", Lv, Mode >= 0 ? ModeName[Mode] : "?", ObsL, ExpL, CandL / std::max(AccL, 1.0));
	}

	// Every cluster of an 8 x 8 cell patch about the brightest at L68 and L70: the cells about it (the disc's slab).
	const int Sn = 8, Sx0 = Pick.empty() ? Pc0x + 26 : Pick[0].X - Sn / 2, Sy0 = Pick.empty() ? Pc0y + 26 : Pick[0].Y - Sn / 2;   // about the brightest cluster
	const double Mx = (Sx0 + 0.5 * Sn) * double(L), My = (Sy0 + 0.5 * Sn) * double(L);
	const double Mz = H::Uni_GalaxyWarpZAt(H::float2(float(Mx), float(My)), P, Kg.Hp);
	std::vector<H::float3> Stars68;
	for (int Lv : { 68, 70 })
	{
		if (!M.SlicesOf(UniScenes::kStar).ForLevel(Lv)) { LevelText += Uni::Fmt(", L%d none", Lv); continue; }
		const double CellF = std::ldexp(1.0, Lv) / Rf;
		const double Half = 0.5 * Sn * L + 0.87 * CellF + 0.3 * L;
		const double Zh = 1.2 * double(P.A.y) * (1.0 + std::max(double(P.E.w), 0.0)) + 0.87 * CellF + 0.3 * L;
		const FCellKey K0 = CellOf(WorldOf(H::float3{ float(Mx), float(My), float(Mz) }), Lv);
		const int Nn = int(std::ceil(std::sqrt(2.0 * Half * Half + Zh * Zh) / CellF));
		FRun R;
		const u32 Mid[3] = { 0x80000000u, 0x80000000u, 0x80000000u };
		for (int Dz = -Nn; Dz <= Nn; ++Dz) for (int Dy = -Nn; Dy <= Nn; ++Dy) for (int Dx = -Nn; Dx <= Nn; ++Dx)
		{
			const FCellKey Kc = Neighbour(K0, Dx, Dy, Dz);
			const H::float3 Q = FieldOf(ObjectCoord(Kc, Mid));
			if (std::abs(Q.x - Mx) > Half || std::abs(Q.y - My) > Half || std::abs(Q.z - Mz) > Zh) { continue; }
			RunCell(Kc, R);
		}
		double ObsL = 0.0, ExpL = 0.0;
		size_t Nc = 0;
		for (const FOcInfo& Oi : Patch0)
		{
			if (Oi.X < Sx0 || Oi.X >= Sx0 + Sn || Oi.Y < Sy0 || Oi.Y >= Sy0 + Sn) { continue; }
			const auto Oe = Score(Oi, R, Lv, nullptr);
			ObsL += Oe.first; ExpL += Oe.second; ++Nc;
		}
		CandBig += R.Cand; AccBig += R.Acc;
		if (Lv == 68) { Stars68 = R.Stars; }
		LevelText += Uni::Fmt(", L%d %s %zu clusters %.1f/%.1f (%.2f cand/star)", Lv, R.Mode >= 0 ? ModeName[R.Mode] : "?", Nc, ObsL, ExpL, R.Cand / std::max(R.Acc, 1.0));
	}
	const double RatioS = ObsSum[0] / std::max(ExpSum[0], 1e-300), RatioB = ObsSum[1] / std::max(ExpSum[1], 1e-300);
	const double ZS = (ObsSum[0] - ExpSum[0]) / std::sqrt(std::max(VarSum[0], 1e-300)), ZB = (ObsSum[1] - ExpSum[1]) / std::sqrt(std::max(VarSum[1], 1e-300));
	double ProfWorst = 0.0;
	for (int B = 0; B < 4; ++B) { ProfWorst = std::max(ProfWorst, std::abs(ProfObs[B] - ProfExp[B]) / std::max(ProfN, 1.0)); }
	const double ClipRate = Clip / std::max(CandAll, 1.0), CandPerBig = CandBig / std::max(AccBig, 1.0);

	// Stills: the 8 x 8 patch face-on, the disc's light over +-2 h of the analytic mid-plane with
	// the clusters widened over a pixel; the stars around the first pick at L64; L68's stars.
	{
		const int W = 300;
		FImage Img(W * 3, W);
		const double X0 = Sx0 * double(L), Y0 = Sy0 * double(L), Span = Sn * double(L), Px = Span / W;
		std::vector<double> Lum(size_t(W) * W, 0.0);
		double Lmax = 0.0;
		for (int Y = 0; Y < W; ++Y) for (int X = 0; X < W; ++X)
		{
			const double Xd = X0 + (X + 0.5) * Px, Yd = Y0 + Span - (Y + 0.5) * Px;
			const double Zw = H::Uni_GalaxyWarpZAt(H::float2(float(Xd), float(Yd)), P, Kg.Hp);
			double V = 0.0;
			for (int Iz = -2; Iz <= 2; ++Iz)
			{
				const H::float3 Q{ float(Xd), float(Yd), float(Zw + Iz * double(P.A.y)) };
				double Ys = 0.0, Oc = 0.0;
				const double Rho = OcParts(Q, P, Kg, Ys, Oc);
				V += Rho - Oc + Ys * double(H::Uni_GalaxyOcClump(Q, P, Hoc, Kg.Hp, float(Px * Px)));
			}
			Lum[size_t(Y) * W + X] = V; Lmax = std::max(Lmax, V);
		}
		auto Tone = [&](double V) { return float(std::log(1.0 + 30.0 * V / std::max(Lmax, 1e-300)) / std::log(31.0)); };
		for (int Y = 0; Y < W; ++Y) for (int X = 0; X < W; ++X)
		{
			const float V = Tone(Lum[size_t(Y) * W + X]);
			Img.Add(X, Y, 0.8f * V, 0.88f * V, V);
			Img.Add(2 * W + X, Y, 0.28f * V, 0.31f * V, 0.35f * V);
		}
		if (!Pick.empty())
		{
			std::vector<float> Hd(size_t(W) * W, 0.0f);
			const double Sp = 3.0 * double(H::UNI_OC_HALO) * double(Pick[0].O.S1);
			for (const H::float3& Dd : Stars0)
			{
				const int X1 = int((Dd.x / Sp * 0.5 + 0.5) * W), Y1 = int((0.5 - Dd.y / Sp * 0.5) * W);
				if (X1 >= 0 && X1 < W && Y1 >= 0 && Y1 < W) { Hd[size_t(Y1) * W + X1] += 1.0f; }
			}
			float Hm = 1.0f; for (float V : Hd) { Hm = std::max(Hm, V); }
			for (int Y = 0; Y < W; ++Y) for (int X = 0; X < W; ++X)
			{
				const float V = Hd[size_t(Y) * W + X] > 0.0f ? std::log(1.0f + Hd[size_t(Y) * W + X]) / std::log(1.0f + Hm) : 0.0f;
				Img.Add(W + X, Y, V, 0.9f * V, 0.75f * V);
			}
		}
		{
			// L68's stars face-on (all heights), log count per pixel over the dimmed light.
			std::vector<float> Hd(size_t(W) * W, 0.0f);
			for (const H::float3& Pf : Stars68)
			{
				const int X1 = int((Pf.x - X0) / Px), Y1 = int((Y0 + Span - Pf.y) / Px);
				if (X1 >= 0 && X1 < W && Y1 >= 0 && Y1 < W) { Hd[size_t(Y1) * W + X1] += 1.0f; }
			}
			float Hm = 1.0f; for (float V : Hd) { Hm = std::max(Hm, V); }
			for (int Y = 0; Y < W; ++Y) for (int X = 0; X < W; ++X)
			{
				const float V = Hd[size_t(Y) * W + X] > 0.0f ? std::log(1.0f + Hd[size_t(Y) * W + X]) / std::log(1.0f + Hm) : 0.0f;
				Img.Add(2 * W + X, Y, V, 0.8f * V, 0.5f * V);
			}
		}
		const std::string Hud = Uni::Fmt("D-239: a patch of the Milky Way's disc face-on (%.0f pc), its light within 2 h of the mid-plane (clusters widened over a pixel); the stars placed around one cluster at L64 (+-3 halo sigmas, log count); L68's stars over the patch (log count)", Span * PcPerUnit);
		OcWriteFrame(Img, "still_open_clusters", Hud.c_str());
	}

	const bool bPass = SpiralOn > 60 && SpiralOn == ArmsKnots && EarlyOn == 0 && PlainOn == 0 && DustOn == 0
		&& std::abs(OccFrac - double(H::UNI_OC_OCC)) < 0.03 && std::abs(Wmean - 1.0) < 0.04 && SigmaBad == 0 && MidWorst <= 3.0 * double(H::UNI_OC_Z) + 1e-3
		&& StarsQ(0.5) > 100.0 && StarsQ(0.5) < 5000.0 && StarsQ(0.99) < 2.0e5 && StarsQ(1.0) < 3.0e5 && HeldShare > 0.01 && HeldShare < 0.06
		&& std::abs(ConsRatio[0] - 1.0) < std::max(0.25, 3.0 * ConsSe[0]) && std::abs(ConsRatio[1] - 1.0) < std::max(0.25, 3.0 * ConsSe[1])
		&& WidenWorst < 0.05
		&& Pick.size() >= 4u && std::abs(RatioS - 1.0) < 0.08 && std::abs(ZS) < 4.0 && std::abs(RatioB - 1.0) < 0.12 && std::abs(ZB) < 4.0
		&& ProfWorst < 0.04 && CentWorst < 4.0 && ClipRate < 0.01 && Truncated == 0;
	Uni::Check(2, "T2.galaxy.open_clusters", "D-239 open clusters: textured spirals with arms and knots only (none untextured, early or on dust sets); the lattice's occupancy within 3 points of its knob, the mass draw's mean within 4%, sigmas in range, centres within 3 x 0.25 h of the analytic mid-plane; a median cluster 100-5000 stars (all levels; the mass follows the young light, so arm knots hold the largest), the 99th percentile under 2 x 10^5 (10^5 before D-256: its barred arms concentrate the young light; 1.4 x 10^5 on its Milky Way; B-035's top-heavy spectrum), none over 3 x 10^5, together 1-6% of the disc's light; their gathered light within 25% (or 3 of its sampling errors: the clusters a Monte Carlo estimate) of frac x the saturated young light in two disc patches (B-035: the clusters on the analytic mid-plane, the young light on the texture-warped one; 0.98 / 0.94 before D-256, 0.86 / 0.76 on its Milky Way, 4.5 and 3.1 sampling errors: a bias); one cluster's light kept within 5% as the footprint widens and blends; the real generator around six bright clusters at L64-L67 and every cluster of an 8 x 8 cell patch at L68 and L70: stars within 3 halo sigmas (less the field's) within 8% of the expected where listed, 12% where drawn by cell (|z| < 4 each), the profile within 4 points of the two Gaussians, the centroid within 4 sigma of its noise, under 1% of candidates clipped, no cell truncated")
		.Metric("spirals_on", double(SpiralOn)).Metric("occupancy", OccFrac).Metric("w_mean", Wmean).Metric("stars_median", StarsQ(0.5)).Metric("stars_max", StarsQ(1.0)).Metric("held_share", HeldShare)
		.Metric("conservation_0", ConsRatio[0]).Metric("conservation_1", ConsRatio[1]).Metric("widen_worst", WidenWorst)
		.Metric("placed_ratio_listed", RatioS).Metric("placed_z_listed", ZS).Metric("placed_ratio_cell", RatioB).Metric("placed_z_cell", ZB)
		.Metric("profile_worst", ProfWorst).Metric("centroid_worst", CentWorst).Metric("clipped", ClipRate).Metric("candidates_per_star_big", CandPerBig)
		.Pass(bPass, Uni::Fmt("128 rolls: %zu spirals with clusters (%zu with arms and knots), %zu early / %zu untextured / %zu dust sets; Milky Way lattice %.0f pc: occupancy %.3f, draw mean %.3f, %zu sigmas out of range, height worst %.2f h; stars a cluster p10 %.0f median %.0f p90 %.0f p99 %.0f max %.0f, %.2f%% of the disc's light; gathered / frac x young %.3f, %.3f (sampling error %.3f, %.3f); widening %s; placed (%d tasks; obs/exp %s): listed x %.3f (z %+.2f), by cell x %.3f (z %+.2f), profile worst %.3f, centroid worst %.2f, clipped %.3f%%, %d truncated",
			SpiralOn, ArmsKnots, EarlyOn, PlainOn, DustOn, double(L) * PcPerUnit, OccFrac, Wmean, SigmaBad, MidWorst, StarsQ(0.1), StarsQ(0.5), StarsQ(0.9), StarsQ(0.99), StarsQ(1.0), 100.0 * HeldShare,
			ConsRatio[0], ConsRatio[1], ConsSe[0], ConsSe[1], WidenText.c_str(), TasksRun, LevelText.c_str(), RatioS, ZS, RatioB, ZB, ProfWorst, CentWorst, 100.0 * ClipRate, Truncated));
}
