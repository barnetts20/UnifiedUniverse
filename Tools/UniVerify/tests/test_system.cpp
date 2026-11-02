// test_system.cpp
// Star systems and planets (B-002, D-142..D-145): orbits, photometry, determinism, the
// activation hysteresis, and the end-to-end path through the shared projection core that the
// GPU runs (dynamic pseudo-cells, exact offsets, effective magnitudes).

#include "../src/UniTest.h"
#include "../scenes/UniScenes.h"
#include "UniSystem.h"

#include <cstring>
#include <map>
#include <set>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	/** Real star records from the reference pipeline at the solar camera, with their cells. */
	struct FStarRef { FCellKey Cell; FObjectGPU O; FCoord Coord; };
	const std::vector<FStarRef>& SolarStars()
	{
		static std::vector<FStarRef> Out;
		if (!Out.empty()) { return Out; }
		static FUniverseModel M(UniScenes::GalaxyToStar());
		static FRefGenerator G(M);
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(UniScenes::SolarCamera(), FVec3f{});
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float, u64)
		{
			if ((O.A[3] & 0xFFFFu) != UniScenes::kStar) { return; }
			if ((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN) { return; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			Out.push_back({ K, O, ObjectCoord(K, Off) });
		});
		return Out;
	}

	FObjectGPU SunLikeRecord(u32 Seed)
	{
		return FromHlsl(H::Uni_MakeObject(H::uint3(0u, 0u, 0u), UniScenes::kStar, 0u, Seed, float(std::log2(6.957e10)), 4.83f, 0.0f, H::float3(1.0f, 0.95f, 0.9f)));
	}
}

UNI_TEST(system, kepler)
{
	// Kepler's equation residual, orbit closure after one period, and vis-viva speed.
	double MaxResidual = 0.0, MaxClosure = 0.0, MaxVisViva = 0.0;
	u32 S = 12345u;
	for (int I = 0; I < 200000; ++I)
	{
		const double M = (H::Uni_DrawUnit(S, 1u, u32(I)) * 2.0 - 1.0) * 20.0;
		const double E0 = 0.95 * H::Uni_DrawUnit(S, 2u, u32(I));
		const double E = KeplerSolve(M, E0);
		MaxResidual = std::max(MaxResidual, std::abs(std::remainder(E - E0 * std::sin(E) - M, SysConst::TwoPi)));
	}
	const FSystem Sys = BuildSystem(SunLikeRecord(777u));
	int Planets = 0;
	for (int K = 0; K < 2000; ++K)
	{
		const FSystem Sx = BuildSystem(SunLikeRecord(1000u + u32(K)));
		for (const FPlanet& P : Sx.Planets)
		{
			++Planets;
			const double T = 1.0e9 + 3.7e7 * K;
			const FVec3d A = PlanetOffset(P, T), B = PlanetOffset(P, T + P.PeriodS);
			MaxClosure = std::max(MaxClosure, Length(FVec3d{ A.X - B.X, A.Y - B.Y, A.Z - B.Z }) / P.SemiMajorCm);
			const double Dt = P.PeriodS * 1.0e-6;
			const FVec3d C = PlanetOffset(P, T + Dt);
			const double V = Length(FVec3d{ C.X - A.X, C.Y - A.Y, C.Z - A.Z }) / Dt;
			const double R = Length(A);
			const double Want = std::sqrt(SysConst::GMSun * Sx.MassSun * (2.0 / R - 1.0 / P.SemiMajorCm));
			MaxVisViva = std::max(MaxVisViva, std::abs(V / Want - 1.0));
		}
	}
	// A 1 AU orbit around one solar mass: one sidereal year.
	FPlanet Earth;
	Earth.SemiMajorCm = Units::Au;
	const double Year = SysConst::TwoPi * std::sqrt(Units::Au * Units::Au * Units::Au / SysConst::GMSun);
	(void)Sys; (void)Earth;
	Uni::Check(1, "T1.system.kepler", "Orbits: Kepler residual < 1e-12 rad, closure after one period < 1e-9 a, vis-viva speed within 1e-4, 1 AU period = 365.256 d")
		.Metric("max_residual", MaxResidual).Metric("max_closure", MaxClosure).Metric("max_visviva", MaxVisViva).Metric("year_days", Year / 86400.0)
		.Pass(MaxResidual < 1.0e-12 && MaxClosure < 1.0e-9 && MaxVisViva < 1.0e-4 && std::abs(Year / 86400.0 - 365.256) < 0.01 && Planets > 1000,
			Uni::Fmt("200k solves: worst residual %.2e rad; %d planets: worst closure %.2e a, worst vis-viva error %.2e; 1 AU period %.3f d", MaxResidual, Planets, MaxClosure, MaxVisViva, Year / 86400.0));
}

UNI_TEST(system, photometry)
{
	// Lambert phase function against a numerical integral over a lit sphere, and two known
	// planets at mean opposition with the Sun as the star.
	double MaxPhaseErr = 0.0;
	auto Integral = [](double Alpha)
	{
		// Flux to the observer from a Lambert sphere lit along S, seen along V: integral of
		// (n.s)(n.v) over the lit and visible part of the sphere.
		const double Sx = std::sin(Alpha), Sz = std::cos(Alpha);   // light direction, observer along +z
		const int N = 600;
		double Sum = 0.0;
		for (int I = 0; I < N; ++I)
		{
			const double Th = (I + 0.5) * SysConst::Pi / N;
			for (int J = 0; J < 2 * N; ++J)
			{
				const double Ph = (J + 0.5) * SysConst::Pi / N;
				const double Nx = std::sin(Th) * std::cos(Ph), Nz = std::cos(Th);
				const double Ns = Nx * Sx + Nz * Sz, Nv = Nz;
				if (Ns > 0.0 && Nv > 0.0) { Sum += Ns * Nv * std::sin(Th); }
			}
		}
		return Sum;
	};
	const double I0 = Integral(0.0);
	for (double A : { 0.3, 0.8, 1.2, 1.5707963, 2.0, 2.6, 3.0 })
	{
		MaxPhaseErr = std::max(MaxPhaseErr, std::abs(Integral(A) / I0 - LambertPhase(A)));
	}
	auto Opposition = [](double RadiusCm, double Albedo, double RAu, double DeltaAu)
	{
		FPlanet P;
		P.RadiusCm = RadiusCm;
		P.Albedo = Albedo;
		// Sun at origin, planet on +x, observer between them on the line (phase angle 0).
		const FVec3d StarToPlanet{ RAu * Units::Au, 0.0, 0.0 };
		const FVec3d PlanetToCamera{ -DeltaAu * Units::Au, 0.0, 0.0 };
		return PlanetPhotometry(P, 4.83, StarToPlanet, PlanetToCamera).AppMag;
	};
	const double Jupiter = Opposition(6.9911e9, 0.538, 5.203, 4.203);   // literature: about -2.7
	const double Mars = Opposition(3.3895e8, 0.170, 1.524, 0.524);      // literature: about -2.0
	Uni::Check(1, "T1.system.photometry", "Reflected light: Lambert phase function within 1e-3 of a numerical sphere integral; Jupiter and Mars at mean opposition within 0.3 mag of -2.7 and -2.0")
		.Metric("phase_err", MaxPhaseErr).Metric("jupiter", Jupiter).Metric("mars", Mars)
		.Pass(MaxPhaseErr < 1.0e-3 && std::abs(Jupiter + 2.7) < 0.3 && std::abs(Mars + 2.0) < 0.3,
			Uni::Fmt("phase function worst error %.1e over 7 angles; Jupiter %.2f, Mars %.2f at mean opposition", MaxPhaseErr, Jupiter, Mars));
}

UNI_TEST(system, determinism)
{
	// Systems built from real star records: identical when rebuilt, distinct between stars,
	// planets inside their limits, and a sane population.
	const std::vector<FStarRef>& Stars = SolarStars();
	size_t Systems = 0, Planets = 0, WithGiant = 0, Mismatch = 0, OutOfRange = 0, Empty = 0;
	std::set<std::vector<u32>> Distinct;
	for (size_t I = 0; I < Stars.size() && Systems < 20000; I += 3)
	{
		const FSystem A = BuildSystem(Stars[I].O), B = BuildSystem(Stars[I].O);
		++Systems;
		bool Same = A.Planets.size() == B.Planets.size();
		for (size_t K = 0; Same && K < A.Planets.size(); ++K) { Same = std::memcmp(&A.Planets[K], &B.Planets[K], sizeof(FPlanet)) == 0; }
		Mismatch += Same ? 0 : 1;
		Planets += A.Planets.size();
		Empty += A.Planets.empty() ? 1 : 0;
		bool Giant = false;
		std::vector<u32> Sig;
		const double SqrtL = std::sqrt(A.LumSun);
		for (const FPlanet& P : A.Planets)
		{
			Giant = Giant || P.Type == EPlanetType::GasGiant;
			Sig.push_back(P.Seed);
			const bool bIn = P.SemiMajorCm >= 0.7 * SysConst::InnerAu * Units::Au * SqrtL * 0.999 && P.SemiMajorCm <= std::min(SysConst::OuterLimitAu * Units::Au * SqrtL, 0.6 * A.ActivationCm) * 1.001
				&& P.Ecc >= 0.0 && P.Ecc < 0.26 && A.SpanCm < A.ActivationCm;
			OutOfRange += bIn ? 0 : 1;
		}
		WithGiant += Giant ? 1 : 0;
		if (!Sig.empty()) { Distinct.insert(Sig); }
	}
	const size_t NonEmpty = Systems - Empty;
	Uni::Check(2, "T2.system.determinism", "Systems from real star records: rebuilt bit-identically, distinct per star, every planet inside its limits and inside the activation radius")
		.Metric("systems", double(Systems)).Metric("planets", double(Planets)).Metric("mismatch", double(Mismatch)).Metric("out_of_range", double(OutOfRange))
		.Metric("distinct", double(Distinct.size())).Metric("with_gas_giant", double(WithGiant))
		.Pass(Systems > 1000 && Mismatch == 0 && OutOfRange == 0 && Distinct.size() == NonEmpty,
			Uni::Fmt("%zu systems (%zu without planets), %zu planets (%.2f per system), %zu with a gas giant; %zu rebuilt differently, %zu planets out of range, %zu distinct planet sets of %zu",
				Systems, Empty, Planets, double(Planets) / double(std::max<size_t>(Systems, 1)), WithGiant, Mismatch, OutOfRange, Distinct.size(), NonEmpty));
}

UNI_TEST(system, pipeline)
{
	// End to end through the shared projection core, as the GPU runs it: for real stars, put
	// the camera inside the system, build the dynamic cells and records, and compare what
	// Uni_ObjectRelative / Uni_ApparentMag recover with the exact double-precision answer, over
	// sampled times covering the outermost orbit. No offset may saturate.
	const std::vector<FStarRef>& Stars = SolarStars();
	double MaxAngle = 0.0, MaxDistRel = 0.0, MaxMag = 0.0;
	size_t Records = 0, Saturated = 0, Systems = 0;
	for (size_t I = 0; I < Stars.size() && Systems < 300; I += 37)
	{
		FSystemManager Mgr;
		const FSystem S = BuildSystem(Stars[I].O);
		if (S.Planets.empty()) { continue; }
		++Systems;
		FCoord Cam = Stars[I].Coord;
		FVec3f F{};
		AddDisplacement(Cam, F, FVec3d{ 0.31 * S.ActivationCm, -0.22 * S.ActivationCm, 0.05 * S.ActivationCm });
		Mgr.Report(Stars[I].Cell, Stars[I].O, Cam, 0);
		const FActiveSystem& A = Mgr.ActiveSystems().begin()->second;
		const double Outer = S.Planets.back().PeriodS;
		for (int Ts = 0; Ts < 24; ++Ts)
		{
			const double T = 1.0e8 + Outer * double(Ts) / 24.0;
			std::vector<FCellGPU> Cells;
			std::vector<FObjectGPU> Recs;
			Mgr.Build(Cam, T, 1000u, 256u, Cells, Recs);
			for (size_t K = 0; K < Recs.size(); ++K)
			{
				const FCellGPU& C = Cells[K];   // one cell per planet (D-143 amendment)
				const H::uint4 Dx(C.DeltaX[0], C.DeltaX[1], C.DeltaX[2], C.DeltaX[3]);
				const H::uint4 Dy(C.DeltaY[0], C.DeltaY[1], C.DeltaY[2], C.DeltaY[3]);
				const H::uint4 Dz(C.DeltaZ[0], C.DeltaZ[1], C.DeltaZ[2], C.DeltaZ[3]);
				const FObjectGPU& O = Recs[C.PoolStart - 1000u];
				++Records;
				for (int Ax = 0; Ax < 3; ++Ax) { Saturated += O.A[Ax] == 0xFFFFFFFFu ? 1 : 0; }
				const H::FUniObject HO = ToHlsl(O);
				const H::float3 Rel = H::Uni_ObjectRelative(Dx, Dy, Dz, H::uint3(O.A[0], O.A[1], O.A[2]), C.Level, H::float3(0.0f, 0.0f, 0.0f));
				// Exact: star + orbit - camera.
				const FPlanet& P = S.Planets[K];
				FCoord Pc = A.Star;
				FVec3f Pf{};
				AddDisplacement(Pc, Pf, PlanetOffset(P, T));
				const FVec3d Want = ToDouble(Sub(Pc, Cam));
				const FVec3d Got{ Rel.x, Rel.y, Rel.z };
				const double Lw = Length(Want), Lg = Length(Got);
				const double Cos = (Want.X * Got.X + Want.Y * Got.Y + Want.Z * Got.Z) / (Lw * Lg);
				MaxAngle = std::max(MaxAngle, std::acos(std::clamp(Cos, -1.0, 1.0)));
				MaxDistRel = std::max(MaxDistRel, std::abs(Lg / Lw - 1.0));
				if (!((O.A[3] >> 16) & H::UNI_OBJ_FLAG_DELTA_HIDDEN))
				{
					const FPlanetPhotometry Ph = PlanetPhotometry(P, double(S.StarAbsMag), PlanetOffset(P, T), ToDouble(Sub(Cam, Pc)));
					const double GpuMag = double(H::Uni_ApparentMag(H::Uni_ObjectAbsMag(HO), float(Lg)));
					MaxMag = std::max(MaxMag, std::abs(GpuMag - Ph.AppMag));
				}
			}
		}
	}
	Uni::Check(2, "T2.system.pipeline", "Planets through the GPU's projection core: direction within 1e-6 rad and distance within 1e-6 of exact, apparent magnitude within 0.05, no saturated offsets, over whole outer orbits")
		.Metric("systems", double(Systems)).Metric("records", double(Records)).Metric("max_angle", MaxAngle).Metric("max_dist_rel", MaxDistRel).Metric("max_mag", MaxMag).Metric("saturated", double(Saturated))
		.Pass(Systems > 50 && MaxAngle < 1.0e-6 && MaxDistRel < 1.0e-6 && MaxMag < 0.05 && Saturated == 0,
			Uni::Fmt("%zu systems, %zu planet records over 24 times per outer orbit: worst direction %.1e rad, distance %.1e, magnitude %.3f; %zu saturated offsets",
				Systems, Records, MaxAngle, MaxDistRel, MaxMag, Saturated));
}

UNI_TEST(system, activation)
{
	// Fly straight through systems: the GPU's float test (Uni_SystemActivates) reports every
	// frame the star is inside; the manager deactivates with exact hysteresis. Each system must
	// activate exactly once and deactivate exactly once: no flicker at the edge, even when the
	// path grazes the activation radius.
	const std::vector<FStarRef>& Stars = SolarStars();
	size_t Paths = 0, BadPaths = 0, MaxActive = 0;
	for (size_t I = 0; I < Stars.size() && Paths < 200; I += 53)
	{
		const FStarRef& St = Stars[I];
		const FSystem S = BuildSystem(St.O);
		const double Ra = S.ActivationCm;
		const double Miss = Ra * (Paths % 4 == 0 ? 0.999 : (Paths % 4 == 1 ? 0.9 : (Paths % 4 == 2 ? 0.5 : 0.05)));
		FSystemManager Mgr;
		std::vector<int> Transitions;
		bool Was = false;
		const int Frames = 2000;
		for (int Fr = 0; Fr <= Frames; ++Fr)
		{
			const double X = -3.0 * Ra + 6.0 * Ra * double(Fr) / Frames;
			// Jitter along the path, as a real camera would wobble across the edge.
			const double Wob = 1.0e-3 * Ra * std::sin(double(Fr) * 0.7);
			FCoord Cam = St.Coord;
			FVec3f F{};
			AddDisplacement(Cam, F, FVec3d{ X, Miss + Wob, 0.0 });
			const float Dist = float(Length(ToDouble(Sub(St.Coord, Cam))));
			if (H::Uni_SystemActivates(UniScenes::kStar, 1u << UniScenes::kStar, H::Uni_ObjectAbsMag(ToHlsl(St.O)), Dist)) { Mgr.Report(St.Cell, St.O, Cam, u64(Fr)); }
			Mgr.Update(Cam);
			const bool Is = !Mgr.ActiveSystems().empty();
			if (Is != Was) { Transitions.push_back(Fr); }
			Was = Is;
			MaxActive = std::max(MaxActive, Mgr.ActiveSystems().size());
		}
		++Paths;
		BadPaths += Transitions.size() == 2 ? 0 : 1;
	}
	Uni::Check(2, "T2.system.activation", "Activation hysteresis: every path through a system (grazing to central, with camera wobble) activates once and deactivates once")
		.Metric("paths", double(Paths)).Metric("bad_paths", double(BadPaths)).Metric("max_active", double(MaxActive))
		.Pass(Paths > 50 && BadPaths == 0 && MaxActive == 1, Uni::Fmt("%zu paths, %zu with other than one activation and one deactivation", Paths, BadPaths));
}

#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include <sys/stat.h>

namespace
{
	void WriteSystemFrame(const UniRender::FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Dir = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(Dir.c_str(), 0755);
		Img.WritePpm(Dir + "/" + Name + ".ppm");
		FILE* F = std::fopen((Dir + "/" + Name + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I) { std::fprintf(F, "%s\"%s\"", I ? "," : "", Hud[I].c_str()); }
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}
}

UNI_TEST(system, stills)
{
	// Two reference frames through the same cores the GPU uses: a Sun-like system seen from
	// above its plane, and a close pass of its largest planet. Planets go through the dynamic
	// cell path (FSystemManager::Build + Uni_ProjectObject), the stars through the pipeline.
	using namespace UniRender;
	const std::vector<FStarRef>& Stars = SolarStars();
	const FStarRef* Pick = nullptr;
	FSystem S;
	for (const FStarRef& St : Stars)
	{
		const FSystem C = BuildSystem(St.O);
		bool Giant = false;
		for (const FPlanet& P : C.Planets) { Giant = Giant || P.Type == EPlanetType::GasGiant; }
		if (Giant && C.Planets.size() >= 4 && std::abs(C.StarAbsMag - 4.8f) < 1.0f) { Pick = &St; S = C; break; }
	}
	if (!Pick)
	{
		Uni::Check(3, "T3.system.stills", "Reference frames of a star system render").Pass(false, "no Sun-like star with a gas giant among the solar stars");
		return;
	}
	static FUniverseModel M(UniScenes::GalaxyToStar());
	static FRefGenerator G(M);
	const double T = 3.0e7;
	int Drawn[2] = { 0, 0 };
	for (int Shot = 0; Shot < 2; ++Shot)
	{
		FCamera Cam;
		Cam.Width = 960; Cam.Height = 540;
		FVec3d Look;
		if (Shot == 0)
		{
			Cam.Coord = Pick->Coord;
			const double Sp = S.SpanCm;
			AddDisplacement(Cam.Coord, Cam.Frac, FVec3d{ 0.25 * Sp, -0.9 * Sp, 0.7 * Sp });
			Look = ToDouble(Sub(Pick->Coord, Cam.Coord));
		}
		else
		{
			const FPlanet* Big = &S.Planets[0];
			for (const FPlanet& P : S.Planets) { if (P.RadiusCm > Big->RadiusCm) { Big = &P; } }
			FCoord Pc = Pick->Coord;
			FVec3f Pf{};
			AddDisplacement(Pc, Pf, PlanetOffset(*Big, T));
			const FVec3d O = PlanetOffset(*Big, T);
			const double L = Length(O);
			// 40 planet radii out on the day side, slightly above the plane, looking back at it.
			Cam.Coord = Pc;
			AddDisplacement(Cam.Coord, Cam.Frac, FVec3d{ -O.X / L * 40.0 * Big->RadiusCm, -O.Y / L * 40.0 * Big->RadiusCm, 8.0 * Big->RadiusCm });
			Look = ToDouble(Sub(Pc, Cam.Coord));
		}
		Cam.LookAlong(FV3{ Look.X, Look.Y, Look.Z });
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cam.Coord, Cam.Frac);
		FImage Img(Cam.Width, Cam.Height);
		const FFrameStats St = RenderFrame(P, Cam, Img);
		FSystemManager Mgr;
		Mgr.Report(Pick->Cell, Pick->O, Cam.Coord, 0);
		std::vector<FCellGPU> Cells;
		std::vector<FObjectGPU> Recs;
		Mgr.Build(Cam.Coord, T, 0u, 256u, Cells, Recs);
		const H::FUniView V = Cam.View();
		for (const FCellGPU& C : Cells)
		{
			const H::uint4 Dx(C.DeltaX[0], C.DeltaX[1], C.DeltaX[2], C.DeltaX[3]);
			const H::uint4 Dy(C.DeltaY[0], C.DeltaY[1], C.DeltaY[2], C.DeltaY[3]);
			const H::uint4 Dz(C.DeltaZ[0], C.DeltaZ[1], C.DeltaZ[2], C.DeltaZ[3]);
			for (u32 I = 0; I < C.PoolCount; ++I)
			{
				const H::FUniSprite Sp = H::Uni_ProjectObject(Dx, Dy, Dz, C.Level, 1.0f, ToHlsl(Recs[C.PoolStart + I]), V, false);
				if ((Sp.Flags & H::UNI_SPRITE_VISIBLE) && Splat(Img, Cam, Sp)) { ++Drawn[Shot]; }
			}
		}
		std::vector<std::string> Hud = {
			Shot == 0 ? "Star system from above its plane (B-002, D-142..D-145)" : "Close pass of the system's largest planet",
			Uni::Fmt("star M = %.2f, %zu planets, span %.1f AU, activation radius %.0f AU", S.StarAbsMag, S.Planets.size(), S.SpanCm / Units::Au, S.ActivationCm / Units::Au),
			Uni::Fmt("t = %.3g s; planets drawn %d; stars visible %zu", T, Drawn[Shot], St.Visible),
		};
		for (const FPlanet& Pl : S.Planets)
		{
			if (Hud.size() > 9) { break; }
			Hud.push_back(Uni::Fmt("  %s a = %.2f AU, R = %.1f R_earth, e = %.2f, P = %.2f yr", Pl.Type == EPlanetType::Rocky ? "rocky" : (Pl.Type == EPlanetType::GasGiant ? "gas giant" : "ice giant"),
				Pl.SemiMajorCm / Units::Au, Pl.RadiusCm / SysConst::EarthRadius, Pl.Ecc, Pl.PeriodS / 3.15576e7));
		}
		WriteSystemFrame(Img, Shot == 0 ? "still_system_overview" : "still_system_planet_close", Hud);
	}
	Uni::Check(3, "T3.system.stills", "Reference frames of a star system (overview, close planet) render, with planets through the dynamic-cell path")
		.Metric("planets_overview", Drawn[0]).Metric("planets_close", Drawn[1])
		.Pass(Drawn[0] >= 2 && Drawn[1] >= 1, Uni::Fmt("overview: %d planets drawn; close pass: %d", Drawn[0], Drawn[1]));
}
