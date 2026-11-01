// test_render.cpp
// Tier 3: rendering behaviour through the shared project-and-cull core.
//   jitter      camera parked at 1e30 / 1e35 / 2^127 cm, 0.001 cm steps, vs a long-double reference
//   ordering    monotonic f(r) keeps depth order; per-depth and per-layer schemes do not
//   occlusion   scene-depth test against a promoted true-scale occluder
//   flythrough  10^28 cm to 1 m from a star: 48-frame contact sheet, zero pops
//   frames      reference stills of both scenes (inspected by the session, OBSERVATIONS.md)
// Images go to Verification/work/render/*.ppm with a .hud.json sidecar each; verify_all
// converts them to PNG with the HUD drawn and assembles contact sheets.

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../scenes/UniScenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	std::string Esc(const std::string& S) { std::string O; for (char C : S) { if (C == '"' || C == '\\') { O += '\\'; } O += C; } return O; }

	void WriteFrame(const FImage& Img, const std::string& Name, const std::vector<std::string>& Hud)
	{
		const std::string Base = RenderDir() + "/" + Name;
		Img.WritePpm(Base + ".ppm");
		FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb");
		if (!F) { return; }
		std::fprintf(F, "{\"lines\":[");
		for (size_t I = 0; I < Hud.size(); ++I) { std::fprintf(F, "%s\"%s\"", I ? "," : "", Esc(Hud[I]).c_str()); }
		std::fprintf(F, "]}\n");
		std::fclose(F);
	}

	std::vector<std::string> HudLines(const FRefPipeline& P, const FCamera& Cam, const FFrameStats& St, const std::string& Title)
	{
		std::vector<std::string> L;
		L.push_back(Title);
		const FDelta FromOrigin = Sub(Cam.Coord, FCoord::Origin());
		L.push_back("cam " + ToHexString(Cam.Coord).substr(0, 24) + "..  |origin| " + FormatDistance(Length(ToDouble(FromOrigin))));
		L.push_back(Uni::Fmt("resident %zu  visible %zu  promoted %zu  occluded %zu", St.Resident, St.Visible, St.Promoted, St.Occluded));
		std::string Lv = "visible by level:";
		for (const auto& KV : St.VisibleByLevel) { Lv += Uni::Fmt(" L%d:%zu%s", KV.first, KV.second, P.LevelSuspended(KV.first) ? "s" : ""); }
		L.push_back(Lv);
		return L;
	}

	FCoord Moved(const FCoord& C, const FV3& D)
	{
		FCoord R = C;
		FVec3f F;
		AddDisplacement(R, F, FVec3d{ D.X, D.Y, D.Z });
		return R;
	}
}

// ==========================================================================================

UNI_TEST(render, jitter)
{
	// Static objects at distances 10^1 .. 10^26 cm, each stored in the cell of the level its
	// distance implies (as generation would place it), seen from a camera parked far out and
	// stepped by 0.001 cm per frame for 100 frames.
	const double W = 1920.0;
	double WorstPx = 0.0, WorstStepPx = 0.0, WorstHiLoPx = 0.0, WorstHiLoNearPx = 0.0;
	long long Samples = 0;
	const double Offsets[3] = { 1.0e30, 1.0e35, std::ldexp(1.0, 127) };
	FCamera Cam;
	Cam.Width = int(W); Cam.Height = 1080;
	Cam.LookAlong({ 1.0, 0.13, -0.07 });
	const H::FUniView V0 = Cam.View();
	for (double Off : Offsets)
	{
		FCoord Base = FCoord::Origin();
		FVec3f BF;
		AddDisplacement(Base, BF, FVec3d{ Off * 0.8, Off * 0.6, 0.0 });   // |offset| = Off
		struct FObj { FCellKey K; u32 O[3]; FCoord Exact; };
		std::vector<FObj> Objs;
		Uni::FTestRng R{ u64(Off) };
		for (int E = 1; E <= 26; ++E)
		{
			for (int N = 0; N < 6; ++N)
			{
				const double Dist = std::pow(10.0, E + R.Unit());
				const FV3 Dir = Norm(Cam.Forward + Cam.Right * (R.Unit() - 0.5) + Cam.Up * (R.Unit() - 0.5) * 0.5);
				const FCoord P = Moved(Base, Dir * Dist);
				FObj Ob;
				Ob.K = CellOf(P, LevelForRelevance(Dist * 1.5));
				const FCoord O = CellOrigin(Ob.K);
				for (int A = 0; A < 3; ++A)
				{
					const FU128 Rel = Sub(P.Axis(A), O.Axis(A));
					Ob.O[A] = Ob.K.Level >= 32 ? Shr(Rel, Ob.K.Level - 32).L[3] : Shl(Rel, 32 - Ob.K.Level).L[3];
				}
				Ob.Exact = ObjectCoord(Ob.K, Ob.O);   // the stored (quantised) position is the truth
				Objs.push_back(Ob);
			}
		}
		// The D-104 case: objects much closer than their cell is wide, as a bright star is when
		// approached (a 10 kpc cell at L75, seen from 10^9 .. 10^13 cm, around the r0 boundary).
		const size_t NumRegular = Objs.size();
		for (int L : { 60, 68, 75 })
		{
			for (int E = 9; E <= 13; ++E)
			{
				const double Dist = std::pow(10.0, E + R.Unit());
				const FV3 Dir = Norm(Cam.Forward + Cam.Right * (R.Unit() - 0.5) + Cam.Up * (R.Unit() - 0.5) * 0.5);
				const FCoord P = Moved(Base, Dir * Dist);
				FObj Ob;
				Ob.K = CellOf(P, L);
				const FCoord O = CellOrigin(Ob.K);
				for (int A = 0; A < 3; ++A) { Ob.O[A] = Shr(Sub(P.Axis(A), O.Axis(A)), L - 32).L[3]; }
				Ob.Exact = ObjectCoord(Ob.K, Ob.O);
				Objs.push_back(Ob);
			}
		}
		std::vector<double> PrevCore(Objs.size() * 2), PrevRef(Objs.size() * 2);
		FCoord CamC = Base;
		FVec3f Frac{};
		for (int F = 0; F < 100; ++F)
		{
			H::FUniView V = V0;
			V.CameraFraction = H::float3(Frac.X, Frac.Y, Frac.Z);
			for (size_t I = 0; I < Objs.size(); ++I)
			{
				H::uint4 D[3];
				CellDelta(Objs[I].K, CamC, D);
				const H::float3 Rel = H::Uni_ObjectRelative(D[0], D[1], D[2], H::uint3(Objs[I].O[0], Objs[I].O[1], Objs[I].O[2]), u32(Objs[I].K.Level), V.CameraFraction);
				const double R2 = std::sqrt(double(Rel.x) * Rel.x + double(Rel.y) * Rel.y + double(Rel.z) * Rel.z);
				// Core: the float direction the GPU would compute, projected.
				const H::float3 Dir = Rel * (1.0f / float(R2));
				const double Fz = H::dot(Dir, V.Forward);
				const double Cx = H::dot(Dir, V.Right) / (Fz * V.TanHalfFovX), Cy = H::dot(Dir, V.Up) / (Fz * V.TanHalfFovY);
				// Reference: exact integer delta in long double, minus the fraction.
				const FDelta Ex = Sub(Objs[I].Exact, CamC);
				long double Rx = (long double)U128ToDouble(Ex.Mag[0]) * (Ex.Neg[0] ? -1 : 1) - Frac.X;
				long double Ry = (long double)U128ToDouble(Ex.Mag[1]) * (Ex.Neg[1] ? -1 : 1) - Frac.Y;
				long double Rz = (long double)U128ToDouble(Ex.Mag[2]) * (Ex.Neg[2] ? -1 : 1) - Frac.Z;
				const long double Rf = Rx * Cam.Forward.X + Ry * Cam.Forward.Y + Rz * Cam.Forward.Z;
				const long double Rr = Rx * Cam.Right.X + Ry * Cam.Right.Y + Rz * Cam.Right.Z;
				const long double Ru = Rx * Cam.Up.X + Ry * Cam.Up.Y + Rz * Cam.Up.Z;
				const double RefX = double(Rr / (Rf * (long double)Cam.TanX())), RefY = double(Ru / (Rf * (long double)Cam.TanY()));
				const double ErrPx = std::max(std::fabs(Cx - RefX), std::fabs(Cy - RefY)) * W * 0.5;
				WorstPx = std::max(WorstPx, ErrPx);
				if (F > 0)
				{
					const double StepCore = std::max(std::fabs((Cx - PrevCore[2 * I]) - (RefX - PrevRef[2 * I])), std::fabs((Cy - PrevCore[2 * I + 1]) - (RefY - PrevRef[2 * I + 1])));
					WorstStepPx = std::max(WorstStepPx, StepCore * W * 0.5);
				}
				PrevCore[2 * I] = Cx; PrevCore[2 * I + 1] = Cy; PrevRef[2 * I] = RefX; PrevRef[2 * I + 1] = RefY;

				// The design's original form, for the record (D-104): hi/lo cell delta + float offset.
				const FHiLo HL = ToHiLo(Sub(CellOrigin(Objs[I].K), CamC));
				const double Scale = std::ldexp(1.0, Objs[I].K.Level - 32);
				const float Hx = HL.Hi.X + (HL.Lo.X + float(double(Objs[I].O[0]) * Scale)) - Frac.X;
				const float Hy = HL.Hi.Y + (HL.Lo.Y + float(double(Objs[I].O[1]) * Scale)) - Frac.Y;
				const float Hz = HL.Hi.Z + (HL.Lo.Z + float(double(Objs[I].O[2]) * Scale)) - Frac.Z;
				const double Hf = Hx * Cam.Forward.X + Hy * Cam.Forward.Y + Hz * Cam.Forward.Z;
				const double HxN = (Hx * Cam.Right.X + Hy * Cam.Right.Y + Hz * Cam.Right.Z) / (Hf * Cam.TanX());
				if (I < NumRegular) { WorstHiLoPx = std::max(WorstHiLoPx, std::fabs(HxN - RefX) * W * 0.5); }
				else { WorstHiLoNearPx = std::max(WorstHiLoNearPx, std::fabs(HxN - RefX) * W * 0.5); }
				++Samples;
			}
			AddDisplacement(CamC, Frac, FVec3d{ 0.001, 0.0, 0.0 });
		}
	}
	Uni::Check(3, "T3.jitter", "Camera parked at 1e30 / 1e35 / 2^127 cm, 0.001 cm per frame for 100 frames: max screen error of static objects vs long-double reference < 0.01 px")
		.Metric("samples", double(Samples)).Metric("worst_px", WorstPx).Metric("worst_frame_step_px", WorstStepPx)
		.Metric("hilo_design_worst_px", WorstHiLoPx).Metric("hilo_design_near_in_coarse_cell_px", WorstHiLoNearPx)
		.Pass(WorstPx < 0.01 && WorstStepPx < 0.01,
			Uni::Fmt("%lld samples (objects 10 cm .. 10^27 cm away, plus objects 10^9..10^14 cm away stored in L60/68/75 cells; 1920 px over 90 deg): "
				"worst error %.2e px, worst frame-to-frame jitter %.2e px. For the record, the design's hi/lo + float-offset form: %.2e px when cells match "
				"distance, %.3g px for near objects in coarse cells (D-104)", Samples, WorstPx, WorstStepPx, WorstHiLoPx, WorstHiLoNearPx));
}

UNI_TEST(render, ordering)
{
	Uni::FTestRng R(0x0DE7ull);
	long long Pairs = 0, Reversed = 0, Ties = 0, DepthWrong = 0, LayerWrong = 0;
	for (int I = 0; I < 100000; ++I)
	{
		auto Draw = [&](double& Rr, FV3& Dir, int& Level)
		{
			Rr = std::exp2(R.Unit() * 127.0);
			Dir = Norm({ 1.0, (R.Unit() - 0.5) * 1.6, (R.Unit() - 0.5) * 0.9 });
			Level = LevelForRelevance(Rr * (1.0 + R.Unit()));
		};
		double R1, R2; FV3 D1, D2; int L1, L2;
		Draw(R1, D1, L1); Draw(R2, D2, L2);
		if (R1 == R2) { continue; }
		++Pairs;
		const bool TrueLess = R1 < R2;
		// Unified Universe: radial f(r) in float, as the GPU computes it.
		const float F1 = H::Uni_Compress(float(R1)), F2 = H::Uni_Compress(float(R2));
		if (F1 == F2) { ++Ties; }
		else if ((F1 < F2) != TrueLess) { ++Reversed; }
		// Per-depth compression: compress view-space z, keep the projection (x, y scaled by f(z)/z),
		// then compare render-space distances.
		auto PerDepth = [](double Rr, FV3 D)
		{
			const double Z = Rr * D.X;
			const double Fz = double(H::Uni_Compress(float(Z)));
			const double S = Fz / Z;
			const double X = Rr * D.Y * S, Y = Rr * D.Z * S;
			return std::sqrt(X * X + Y * Y + Fz * Fz);
		};
		const double P1 = PerDepth(R1, D1), P2 = PerDepth(R2, D2);
		if (P1 != P2 && ((P1 < P2) != TrueLess)) { ++DepthWrong; }
		// Fixed layer compositing (UltraLargeScale-style): coarser layers always behind finer.
		const int Layer1 = L1 / 8, Layer2 = L2 / 8;
		const bool LayerLess = Layer1 != Layer2 ? Layer1 < Layer2 : TrueLess;
		if (LayerLess != TrueLess) { ++LayerWrong; }
	}
	Uni::Check(3, "T3.ordering", "Compressed render depth order equals true distance order for 10^5 random pairs; per-depth compression shown to fail on the same set")
		.Metric("pairs", double(Pairs)).Metric("reversed", double(Reversed)).Metric("ties", double(Ties))
		.Metric("per_depth_wrong", double(DepthWrong)).Metric("per_layer_wrong", double(LayerWrong))
		.Pass(Reversed == 0 && DepthWrong > 0,
			Uni::Fmt("%lld pairs: radial f(r) reverses %lld (float ties %lld); per-depth z compression misorders %lld; fixed layer compositing misorders %lld",
				Pairs, Reversed, Ties, DepthWrong, LayerWrong));
}

UNI_TEST(render, occlusion)
{
	// A promoted Earth-sized occluder 5e9 cm ahead. Backdrop objects in front of it, behind it
	// and beside it; the sprite's scene-depth test must keep exactly the visible ones.
	FCamera Cam;
	Cam.Width = 480; Cam.Height = 270;
	Cam.Coord = FCoord::Origin();
	Cam.LookAlong({ 1, 0, 0 });
	FImage Img(Cam.Width, Cam.Height);
	const double OccDist = 5.0e9, OccR = 6.4e8;
	DrawPromotedSphere(Img, Cam, { OccDist, 0, 0 }, OccR, H::float3(0.3f, 0.5f, 1.0f));
	const H::FUniView V = Cam.View();
	Uni::FTestRng R(0x0CC1ull);
	long long Placements = 0, Wrong = 0, Behind = 0, Front = 0, Beside = 0;
	for (int I = 0; I < 20000; ++I)
	{
		const double Ang = (R.Unit() - 0.5) * 2.0 * std::asin(OccR / OccDist) * 2.0;
		const double Ang2 = (R.Unit() - 0.5) * 2.0 * std::asin(OccR / OccDist) * 2.0;
		const FV3 Dir = Norm({ 1.0, std::tan(Ang), std::tan(Ang2) });
		const double Dist = std::exp(std::log(1.0e9) + R.Unit() * (std::log(1.0e14) - std::log(1.0e9)));
		// Exact geometric truth: does the ray hit the sphere before Dist?
		const double B = Dir.X * OccDist;
		const double Disc = B * B - (OccDist * OccDist - OccR * OccR);
		const double THit = Disc >= 0 ? B - std::sqrt(Disc) : std::numeric_limits<double>::infinity();
		if (std::fabs(Dist - THit) < 1e-4 * Dist) { continue; }   // grazing the surface within float precision
		if (Disc >= 0 && std::sqrt(Disc) < 1e-3 * OccR) { continue; } // at the silhouette, where pixel sampling decides
		const bool Visible = Dist < THit;
		// Through the core: the object's projected sprite against the scene distance at its pixel.
		const FCoord P = Moved(Cam.Coord, Dir * Dist);
		const int L = LevelForRelevance(Dist * 1.5);
		const FCellKey K = CellOf(P, L);
		const FCoord O = CellOrigin(K);
		u32 Off[3];
		for (int A = 0; A < 3; ++A) { const FU128 Rl = Sub(P.Axis(A), O.Axis(A)); Off[A] = L >= 32 ? Shr(Rl, L - 32).L[3] : Shl(Rl, 32 - L).L[3]; }
		H::uint4 D[3];
		CellDelta(K, Cam.Coord, D);
		const H::FUniObject HO = H::Uni_MakeObject(H::uint3(Off[0], Off[1], Off[2]), UniScenes::kStar, 0u, 1u, 30.0f, -15.0f, 0.0f, H::float3(1, 1, 1));
		const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(L), 1.0f, HO, V, false);
		if (!(S.Flags & H::UNI_SPRITE_VISIBLE)) { continue; }
		const int Px = int(std::floor((double(S.NdcX) * 0.5 + 0.5) * Img.W)), Py = int(std::floor((0.5 - double(S.NdcY) * 0.5) * Img.H));
		if (Px < 0 || Py < 0 || Px >= Img.W || Py >= Img.H) { continue; }
		// The pixel's scene distance is for the pixel centre ray; use the object's own ray for the
		// truth to avoid testing pixel quantisation, by recasting at the sprite direction.
		const double Bs = double(S.RenderPos.x) / H::Uni_Compress(S.TrueDistance) * OccDist;
		const double Ds = Bs * Bs - (OccDist * OccDist - OccR * OccR);
		const double SceneAtRay = Ds >= 0 && Bs > 0 ? Bs - std::sqrt(Ds) : std::numeric_limits<double>::infinity();
		const bool Kept = H::Uni_SceneDepthKeep(S.TrueDistance, float(std::min(SceneAtRay, 3.0e38)));
		++Placements;
		if (Kept != Visible) { ++Wrong; }
		if (!Visible) { ++Behind; } else if (Disc >= 0) { ++Front; } else { ++Beside; }
	}
	Uni::Check(3, "T3.occlusion", "Synthetic promoted occluder: backdrop objects behind it are culled, objects in front kept, for all test placements")
		.Metric("placements", double(Placements)).Metric("wrong", double(Wrong)).Metric("behind", double(Behind)).Metric("front", double(Front)).Metric("beside", double(Beside))
		.Pass(Wrong == 0 && Behind > 1000 && Front > 100,
			Uni::Fmt("%lld placements (%lld behind, %lld in front on the disc, %lld beside), %lld wrong", Placements, Behind, Front, Beside, Wrong));
}

UNI_TEST(render, frames)
{
	// Reference stills: the Milky-Way-like anchor seen from 8 kpc out (towards the centre and
	// out of the plane), and the cosmic web of galaxies from intergalactic space.
	int Written = 0;
	std::string Detail;
	{
		FUniverseModel M(UniScenes::GalaxyToStar());
		FRefGenerator G(M);
		FRefPipeline P(G);
		const FCoord C = UniScenes::SolarCamera();
		P.Update(C, FVec3f{});
		struct FShot { const char* Name; FV3 Dir; };
		for (const FShot& S : { FShot{ "still_galaxy_towards_centre", { -1, -0.12, -0.02 } }, FShot{ "still_galaxy_out_of_plane", { -0.3, 0.1, 1 } } })
		{
			FCamera Cam; Cam.Coord = C; Cam.LookAlong(S.Dir);
			FImage Img(Cam.Width, Cam.Height);
			const FFrameStats St = RenderFrame(P, Cam, Img);
			WriteFrame(Img, S.Name, HudLines(P, Cam, St, std::string("galaxy_to_star: ") + S.Name));
			Detail += Uni::Fmt("%s: %zu visible of %zu resident; ", S.Name, St.Visible, St.Resident);
			++Written;
		}
	}
	{
		FUniverseModel M(UniScenes::WebToGalaxy());
		FRefGenerator G(M);
		FRefPipeline P(G);
		FCamera Cam; Cam.Coord = UniScenes::VoidCamera(); Cam.LookAlong({ 1, 0.3, 0.1 });
		P.DeltaMag = 5.0;   // a sensitive scan: windows N = 6, m_lim 11.5, mu_lim 30
		Cam.DeltaMag = 5.0;
		P.Update(Cam.Coord, FVec3f{});
		FImage Img(Cam.Width, Cam.Height);
		const FFrameStats St = RenderFrame(P, Cam, Img);
		WriteFrame(Img, "still_web_sensitive_dm5", HudLines(P, Cam, St, "web_to_galaxy at dm = +5 (sensitive scan)"));
		Detail += Uni::Fmt("web dm+5: %zu visible of %zu resident", St.Visible, St.Resident);
		++Written;
	}
	Uni::Check(3, "T3.frames", "Reference stills written for visual inspection (galaxy interior x2, cosmic web)")
		.Metric("frames", Written).Pass(Written == 3, Detail);
}

UNI_TEST(render, flythrough)
{
	// From 10^28 cm (3 Gpc) above a bright star in the anchored galaxy down to 1 m from its
	// surface, exponentially in surface distance over 2,400 simulated frames. 48 are rendered for
	// the contact sheet; every frame is checked for pops: objects entering residency (generated,
	// restored or regenerated) that are already visible above the fade threshold.
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);

	// Pick the target: the brightest star within 100 pc of the solar camera.
	FCoord Target; double TargetR = 7e10; double BestMag = 99;
	{
		FRefPipeline P0(G);
		P0.Update(UniScenes::SolarCamera(), FVec3f{});
		P0.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float, u64)
		{
			if ((O.A[3] & 0xFFFFu) != UniScenes::kStar) { return; }
			const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
			const FCoord C = ObjectCoord(K, Off);
			const double D = Length(ToDouble(Sub(C, UniScenes::SolarCamera())));
			const float Mag = H::Uni_ObjectAbsMag(UniCore::ToHlsl(O));
			if (D < 100.0 * Units::Pc && Mag < BestMag) { BestMag = Mag; Target = C; TargetR = std::exp2(double(H::Uni_ObjectRadiusLog2(UniCore::ToHlsl(O)))); }
		});
	}

	FRefPipeline P(G);
	const int Frames = 2400, Shots = 48;
	const FV3 U = Norm({ 0.35, 0.25, 0.9 });
	const double S0 = 1.0e28, S1 = 100.0;
	long long Pops = 0, StrictPops = 0, EnteringObjects = 0;
	double WorstPopIntensity = 0.0;
	FCamera Cam; Cam.Width = 320; Cam.Height = 180;
	const float VisibleRef = std::pow(10.0f, -0.4f * ((H::UNI_M_REF - 0.5f) - (H::UNI_M_REF - 1.0f)));   // a just-faded-in star
	const float Threshold = 0.01f * VisibleRef;
	int Shot = 0;
	std::string Notes;
	H::FUniView View;
	bool bCounting = false;   // frame 0 is the initial fill: there is no previous frame to pop against
	P.OnEnterWindow = [&](const FCellContent& C, float Fade)
	{
		if (!bCounting) { return; }
		for (const FObjectGPU& O : C.Objects)
		{
			++EnteringObjects;
			H::uint4 D[3];
			CellDelta(C.Key, Cam.Coord, D);
			const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(C.Key.Level), Fade, UniCore::ToHlsl(O), View, false);
			if (!(S.Flags & H::UNI_SPRITE_VISIBLE)) { continue; }
			const float I = std::max(S.Radiance.x, std::max(S.Radiance.y, S.Radiance.z));
			if (I > 0.0f) { ++StrictPops; }
			if (I > Threshold) { ++Pops; WorstPopIntensity = std::max(WorstPopIntensity, double(I)); }
		}
	};
	for (int F = 0; F < Frames; ++F)
	{
		const double T = double(F) / double(Frames - 1);
		const double S = S0 * std::pow(S1 / S0, T);
		Cam.Coord = Moved(Target, U * (TargetR + S));
		Cam.LookAlong(U * -1.0, { 0, 0, 1 });
		View = Cam.View();   // the pop check projects from this frame's camera
		bCounting = F > 0;
		P.Update(Cam.Coord, FVec3f{});
		const bool bShot = Shot < Shots && F >= int(std::lround(double(Shot) * double(Frames - 1) / double(Shots - 1)));
		if (bShot)
		{
			FImage Img(Cam.Width, Cam.Height);
			const FFrameStats St = RenderFrame(P, Cam, Img);
			std::vector<std::string> Hud = { Uni::Fmt("%02d  %s", Shot, FormatDistance(S).c_str()), Uni::Fmt("vis %zu res %zu", St.Visible, St.Resident) };
			WriteFrame(Img, Uni::Fmt("fly_%02d", Shot), Hud);
			if (Shot % 8 == 0 || Shot == Shots - 1) { Notes += Uni::Fmt("[%02d %s: %zu vis] ", Shot, FormatDistance(S).c_str(), St.Visible); }
			++Shot;
		}
	}
	Uni::Check(3, "T3.flythrough", "Scripted path 10^28 cm to 1 m from an object: 48-frame contact sheet; objects entering above the fade threshold = 0 (frames 1..N; frame 0 is the initial fill)")
		.Metric("frames", Frames).Metric("shots", Shot).Metric("entering_objects", double(EnteringObjects)).Metric("pops", double(Pops))
		.Metric("strict_pops", double(StrictPops)).Metric("target_abs_mag", BestMag)
		.Pass(Pops == 0 && Shot == Shots && EnteringObjects > 0,
			Uni::Fmt("%d frames, %lld objects entered residency, %lld above threshold (1%% of a just-visible star), %lld with any non-zero intensity; target M = %.2f. %s",
				Frames, EnteringObjects, Pops, StrictPops, BestMag, Notes.c_str()));
}

UNI_TEST(render, sensitivity)
{
	// Global sensitivity sweep at the solar camera: dm ramps 0 -> +2.5 -> 0 over 120 frames
	// (a scanner pulse). Windows grow and shrink; the limiting magnitude follows. Pass: no
	// entering object is visible above threshold (pops), no level's fade is reset while it is
	// drawing (blinks), and the visible count rises with dm as the luminosity function predicts.
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	FCamera Cam; Cam.Coord = UniScenes::SolarCamera(); Cam.LookAlong({ -1, -0.12, -0.02 }); Cam.Width = 480; Cam.Height = 270;
	H::FUniView View;
	long long Pops = 0, Entering = 0;
	bool bCounting = false;
	const float VisibleRef = std::pow(10.0f, -0.4f * 0.5f);
	P.OnEnterWindow = [&](const FCellContent& C, float Fade)
	{
		if (!bCounting) { return; }
		for (const FObjectGPU& O : C.Objects)
		{
			++Entering;
			H::uint4 D[3];
			CellDelta(C.Key, Cam.Coord, D);
			const H::FUniSprite S = H::Uni_ProjectObject(D[0], D[1], D[2], u32(C.Key.Level), Fade, UniCore::ToHlsl(O), View, false);
			if ((S.Flags & H::UNI_SPRITE_VISIBLE) && std::max(S.Radiance.x, std::max(S.Radiance.y, S.Radiance.z)) > 0.01f * VisibleRef) { ++Pops; }
		}
	};
	long long Blinks = 0;
	size_t Vis0 = 0, VisPeak = 0, VisEnd = 0;
	std::map<int, float> PrevFade;
	for (int F = 0; F < 120; ++F)
	{
		const double Dm = 2.5 * std::sin(3.14159265358979 * double(F) / 119.0);
		P.DeltaMag = Dm; Cam.DeltaMag = Dm;
		View = Cam.View();
		bCounting = F > 0;
		P.Update(Cam.Coord, FVec3f{});
		for (int L : P.LevelList())
		{
			const float Fd = P.LevelFade(L);
			if (F > 0 && PrevFade.count(L) && PrevFade[L] > 0.5f && Fd < PrevFade[L] - 0.3f) { ++Blinks; }
			PrevFade[L] = Fd;
		}
		if (F == 0 || F == 60 || F == 119)
		{
			FImage Img(Cam.Width, Cam.Height);
			const FFrameStats St = RenderFrame(P, Cam, Img);
			(F == 0 ? Vis0 : (F == 60 ? VisPeak : VisEnd)) = St.Visible;
		}
	}
	Uni::Check(3, "T3.sensitivity", "Global sensitivity sweep dm 0 -> +2.5 -> 0: no pops, no level blinks, visible count grows with dm")
		.Metric("pops", double(Pops)).Metric("blinks", double(Blinks)).Metric("entering", double(Entering))
		.Metric("visible_dm0", double(Vis0)).Metric("visible_peak", double(VisPeak)).Metric("visible_end", double(VisEnd))
		.Pass(Pops == 0 && Blinks == 0 && VisPeak > 2 * Vis0 && VisEnd < VisPeak,
			Uni::Fmt("120 frames: %lld entering objects, %lld pops, %lld level blinks; visible %zu at dm 0, %zu at dm +2.5, %zu back at 0", Entering, Pops, Blinks, Vis0, VisPeak, VisEnd));
}
