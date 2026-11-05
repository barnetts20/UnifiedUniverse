// test_warp.cpp
// D-187 (WARP_PLAN E1, E2): warp streaks and arrival. The streak profile keeps a sprite's light
// at any length, sits along its tail, and is the plain profile at zero length; the tail follows
// the camera's move (towards the vanishing point when flying forward, against a sideways move)
// and nothing at rest; a new cell's age reaches the project pass, so its objects can arrive.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include <sys/stat.h>
#include "../scenes/UniScenes.h"
#include "UniStreamer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>

using namespace UniCore;
namespace H = UniHlsl;

namespace
{
	double Unit(u32 S, u32 K) { return double(H::Uni_DrawUnit(S, 0x3A2Bu, K)); }

	/** Stills: 600 stars ahead, flying forward, at warp 0, 0.5 and 1 (left to right), drawn with
	 *  the pixel shader's streak profile (the camera's basis is the quad's here). */
	void WarpStill()
	{
		const int W = 320, Hh = 200;
		UniRender::FImage Img(W * 3, Hh);
		const double Ppr = W * 0.5;   // 90 degrees across
		const H::float3 Side(0, 1, 0), Up(0, 0, 1);
		for (int Panel = 0; Panel < 3; ++Panel)
		{
			const H::float4 Warp(float(0.5 * Panel), 1.0f, 64.0f, 6.0f);
			for (u32 I = 0; I < 600; ++I)
			{
				const double Ty = 2.0 * Unit(I, 30) - 1.0, Tz = 0.625 * (2.0 * Unit(I, 31) - 1.0);
				const double N = std::sqrt(1.0 + Ty * Ty + Tz * Tz);
				const H::float3 Dir(float(1.0 / N), float(Ty / N), float(Tz / N));
				const float Dist = float(1.0e21 * std::pow(10.0, 1.5 * Unit(I, 32)));
				const float Bright = float(0.3 + 3.0 * Unit(I, 33) * Unit(I, 33));
				const H::float3 S = H::Uni_SpriteStreak(Dir, Dist, H::float3(2.0e20f, 0, 0), Side, Up, float(Ppr), Warp, 1.0f);
				const double Cx = Panel * W + W * 0.5 + Ty * Ppr, Cy = Hh * 0.5 - Tz * Ppr;   // image y down; quad Up = +z
				for (int Y = int(Cy) - 70; Y <= int(Cy) + 70; ++Y) for (int X = int(Cx) - 70; X <= int(Cx) + 70; ++X)
				{
					if (X < Panel * W || X >= (Panel + 1) * W || Y < 0 || Y >= Hh) { continue; }
					const float Px = float((X + 0.5) - Cx), Py = float(-((Y + 0.5) - Cy));
					const bool bS = S.z > 0.0f;   // no streak: the plain profile (the shader's other branch)
					const float Wt = H::Uni_SpriteProfileStreak(Px, Py, 1.0f, 0.0f, 0.0f, 1.0f, bS ? S.x : 1.0f, bS ? S.y : 0.0f, bS ? S.z : 0.0f);
					Img.Add(X, Y, Bright * Wt * 4.0f, Bright * Wt * 4.2f, Bright * Wt * 4.6f);
				}
			}
		}
		const std::string Dd = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(Dd.c_str(), 0755);
		Img.WritePpm(Dd + "/still_warp_streaks.ppm");
		if (FILE* F = std::fopen((Dd + "/still_warp_streaks.hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-187: 600 stars ahead, flying forward: warp 0, 0.5, 1 (left to right)\",\"streaks from the camera's move only, light kept, capped at 64 px\"]}\n");
			std::fclose(F);
		}
	}
}

UNI_TEST(warp, streaks)
{
	// 1. erf against the library's.
	double WorstErf = 0.0;
	for (int I = -6000; I <= 6000; ++I) { const double X = I * 0.001; WorstErf = std::max(WorstErf, std::abs(double(H::Uni_Erf(float(X))) - std::erf(X))); }
	// 2. The streak profile: light, centroid and zero-length limit over random streaks.
	double WorstLight = 0.0, WorstAlong = 0.0, WorstAcross = 0.0, WorstLimit = 0.0;
	for (u32 I = 0; I < 300; ++I)
	{
		const double Sg = 0.6 + 3.4 * Unit(I, 0), L = 64.0 * Unit(I, 1) * Unit(I, 1), A = 6.283185307179586 * Unit(I, 2);
		const double Ax = std::cos(A), Ay = std::sin(A), Ox = Unit(I, 3) - 0.5, Oy = Unit(I, 4) - 0.5;
		const int Rad = int(std::ceil(L + 6.0 * Sg)) + 1;
		double S = 0, Su = 0, Sv = 0;
		for (int Y = -Rad; Y <= Rad; ++Y) for (int X = -Rad; X <= Rad; ++X)
		{
			const double Px = X + Ox, Py = Y + Oy;
			const double W = H::Uni_SpriteStreakGauss(float(Px), float(Py), float(Sg), float(Ax), float(Ay), float(L));
			S += W; Su += W * (Px * Ax + Py * Ay); Sv += W * (-Px * Ay + Py * Ax);
		}
		WorstLight = std::max(WorstLight, std::abs(S - 1.0));
		WorstAlong = std::max(WorstAlong, std::abs(Su / S - 0.5 * L) / (0.05 + 0.01 * L));
		WorstAcross = std::max(WorstAcross, std::abs(Sv / S));
		// Zero length: the Gaussian.
		const double G = std::exp(-0.5 * (Ox * Ox + Oy * Oy) / (Sg * Sg)) / (6.283185307179586 * Sg * Sg);
		const double Z = H::Uni_SpriteStreakGauss(float(Ox), float(Oy), float(Sg), float(Ax), float(Ay), 0.0f);
		// Just above the switch to the midpoint Gaussian: the two forms agree (continuity).
		const double Lt = 0.0501 * Sg, Lb = 0.0499 * Sg;
		const double Above = H::Uni_SpriteStreakGauss(float(Ox), float(Oy), float(Sg), float(Ax), float(Ay), float(Lt));
		const double Below = H::Uni_SpriteStreakGauss(float(Ox), float(Oy), float(Sg), float(Ax), float(Ay), float(Lb));
		WorstLimit = std::max({ WorstLimit, std::abs(Z / G - 1.0), std::abs(Above / Below - 1.0) });
	}
	// 3. Geometry (the quad basis is the camera's here: looking along +x, Side = +y, Up = +z).
	const H::float3 Side(0, 1, 0), Up(0, 0, 1);
	const H::float4 Warp(1.0f, 1.0f, 64.0f, 6.0f);
	const float Ppr = 600.0f;
	int WrongForward = 0, WrongSide = 0, AtRest = 0, OverCap = 0, Forward = 0;
	double StretchRatio = 0.0;
	for (u32 I = 0; I < 500; ++I)
	{
		const double Ty = 0.6 * (2.0 * Unit(I, 10) - 1.0), Tz = 0.6 * (2.0 * Unit(I, 11) - 1.0);
		const double N = std::sqrt(1.0 + Ty * Ty + Tz * Tz);
		const H::float3 Dir(float(1.0 / N), float(Ty / N), float(Tz / N));
		const float Dist = float(1.0e22 * (1.0 + 10.0 * Unit(I, 12)));
		// Flying forward (+x): the tail points back towards the view centre.
		const H::float3 S = H::Uni_SpriteStreak(Dir, Dist, H::float3(2.0e20f, 0, 0), Side, Up, Ppr, Warp, 1.0f);
		if (S.z > 0.0f && std::sqrt(Ty * Ty + Tz * Tz) > 0.05) { ++Forward; if (!(S.x * Ty + S.y * Tz < 0.0)) { ++WrongForward; } }
		if (S.z > Warp.z + 1e-3f) { ++OverCap; }
		// Moving sideways (+y): the tail points +y (where the object was on screen).
		const H::float3 Sd = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e20f, 0), Side, Up, Ppr, Warp, 1.0f);
		if (Sd.z > 0.0f && !(Sd.x > 0.0f)) { ++WrongSide; }
		// At rest, or with no warp: nothing.
		const H::float3 R0 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 0, 0), Side, Up, Ppr, Warp, 1.0f);
		const H::float3 R1 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e20f, 0), Side, Up, Ppr, H::float4(0.0f, 1.0f, 64.0f, 6.0f), 1.0f);
		if (R0.z != 0.0f || R1.z != 0.0f) { ++AtRest; }
		// Arriving (arrival 0): 1 + stretch times as long (when under the cap).
		const H::float4 W2(1.0f, 1.0f, 1.0e6f, 6.0f);
		const H::float3 A0 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e19f, 0), Side, Up, Ppr, W2, 0.0f);
		const H::float3 A1 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e19f, 0), Side, Up, Ppr, W2, 1.0f);
		if (A1.z > 0.0f) { StretchRatio = std::max(StretchRatio, std::abs(double(A0.z / A1.z) - 7.0)); }
	}
	// 4. Arrival from age.
	const bool bArrival = H::Uni_SpriteArrival(0u, 8.0f) == 0.0f && H::Uni_SpriteArrival(4u, 8.0f) == 0.5f && H::Uni_SpriteArrival(8u, 8.0f) == 1.0f && H::Uni_SpriteArrival(200u, 8.0f) == 1.0f && H::Uni_SpriteArrival(0u, 0.0f) == 1.0f;
	// 5. The streamer's cell ages: 0 on the frame a cell first becomes Ready, one more each frame,
	//    255 at most; the levels' fades and suspension bits unchanged.
	int AgeWrong = 0, AgeCells = 0, AgeZero = 0;
	{
		FUniverseModel M(UniScenes::GalaxyToStar());
		FRefGenerator G(M);
		FCpuGenBackend Be(G, 2097152u);
		FUniStreamer S(G, Be);
		S.bVelocityLod = false;
		S.bAsyncIntegrals = false;
		const FCoord C = UniScenes::SolarCamera();
		std::map<std::string, u32> Last;
		for (int F = 0; F < 60; ++F)
		{
			S.Update(C);
			std::vector<FCellKey> Keys;
			const std::vector<FCellGPU> T = S.CellTable(&Keys);
			std::map<std::string, u32> Now;
			for (size_t I = 0; I < T.size(); ++I)
			{
				const std::string K = std::to_string(Keys[I].Level) + ":" + ToHex(Keys[I].C[0]) + ToHex(Keys[I].C[1]) + ToHex(Keys[I].C[2]);
				const u32 Age = (T[I].Flags >> H::UNI_CELL_AGE_SHIFT) & 255u;
				Now[K] = Age;
				auto It = Last.find(K);
				if (It == Last.end()) { if (Age != 0u) { ++AgeWrong; } else { ++AgeZero; } }
				else if (Age != std::min(It->second + 1u, 255u)) { ++AgeWrong; }
				++AgeCells;
			}
			Last.swap(Now);
		}
	}
	WarpStill();
	Uni::Check(1, "T1.warp.streaks", "D-187 warp streaks and arrival: erf within 5e-7 (float); a streak keeps the sprite's light (1%) at any length 0-64 px, its centroid at half its length along the tail and on its axis, the Gaussian at zero length (2e-3); flying forward every tail points to the view centre, moving sideways along the move's screen direction, nothing at rest or without warp, never over the cap, 1 + stretch times longer while arriving; arrival 0 -> 1 over its frames; cell ages 0 on a cell's first Ready frame and one more each frame after")
		.Metric("erf", WorstErf).Metric("light", WorstLight).Metric("along", WorstAlong).Metric("across", WorstAcross).Metric("limit", WorstLimit)
		.Metric("wrong_forward", WrongForward).Metric("wrong_side", WrongSide).Metric("at_rest", AtRest).Metric("over_cap", OverCap).Metric("age_cells", AgeCells).Metric("age_wrong", AgeWrong)
		.Pass(WorstErf < 5e-7 && WorstLight < 0.01 && WorstAlong < 1.0 && WorstAcross < 5e-3 && WorstLimit < 2e-3 && Forward > 400 && WrongForward == 0 && WrongSide == 0 && AtRest == 0 && OverCap == 0
				&& StretchRatio < 1e-3 && bArrival && AgeCells > 1000 && AgeZero > 50 && AgeWrong == 0,
			Uni::Fmt("erf within %.1e; streak light within %.2f%%, centroid along %.2f of its bar, across %.1e px, zero length %.1e; %d forward tails (%d wrong), %d sideways wrong, %d streaks at rest, %d over the cap, arrival stretch off by %.1e; arrival %s; %d cell-frames (%d new), %d ages wrong",
				WorstErf, 100.0 * WorstLight, WorstAlong, WorstAcross, WorstLimit, Forward, WrongForward, WrongSide, AtRest, OverCap, StretchRatio, bArrival ? "ok" : "wrong", AgeCells, AgeZero, AgeWrong));
}
