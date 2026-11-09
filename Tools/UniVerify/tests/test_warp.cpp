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
#include "UniTravel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
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
				const H::float3 S = H::Uni_SpriteStreak(Dir, Dist, H::float3(2.0e20f, 0, 0), Side, Up, float(Ppr), Warp, 1.0f, H::float4(0, 0, 0, 0));
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
		const H::float3 S = H::Uni_SpriteStreak(Dir, Dist, H::float3(2.0e20f, 0, 0), Side, Up, Ppr, Warp, 1.0f, H::float4(0, 0, 0, 0));
		if (S.z > 0.0f && std::sqrt(Ty * Ty + Tz * Tz) > 0.05) { ++Forward; if (!(S.x * Ty + S.y * Tz < 0.0)) { ++WrongForward; } }
		if (S.z > Warp.z + 1e-3f) { ++OverCap; }
		// Moving sideways (+y): the tail points +y (where the object was on screen).
		const H::float3 Sd = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e20f, 0), Side, Up, Ppr, Warp, 1.0f, H::float4(0, 0, 0, 0));
		if (Sd.z > 0.0f && !(Sd.x > 0.0f)) { ++WrongSide; }
		// At rest, or with no warp: nothing.
		const H::float3 R0 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 0, 0), Side, Up, Ppr, Warp, 1.0f, H::float4(0, 0, 0, 0));
		const H::float3 R1 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e20f, 0), Side, Up, Ppr, H::float4(0.0f, 1.0f, 64.0f, 6.0f), 1.0f, H::float4(0, 0, 0, 0));
		if (R0.z != 0.0f || R1.z != 0.0f) { ++AtRest; }
		// Arriving (arrival 0): 1 + stretch times as long (when under the cap).
		const H::float4 W2(1.0f, 1.0f, 1.0e6f, 6.0f);
		const H::float3 A0 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e19f, 0), Side, Up, Ppr, W2, 0.0f, H::float4(0, 0, 0, 0));
		const H::float3 A1 = H::Uni_SpriteStreak(Dir, Dist, H::float3(0, 1.0e19f, 0), Side, Up, Ppr, W2, 1.0f, H::float4(0, 0, 0, 0));
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

#include "UniVolume.h"

UNI_TEST(warp, optics)
{
	// D-190 (WARP_PLAN E3): stylised aberration and Doppler. The remap is the relativistic one
	// and its inverse; the motion's direction is fixed (steering stays where you look when
	// flying forward); the forward cone tightens by sqrt((1 + b) / (1 - b)); the Doppler factor
	// and tint (luminance kept); the projection puts a sprite at the aberrated direction; streaks
	// and the temporal reprojection follow the remap; b = 0 changes nothing.
	const double Pi = 3.141592653589793;
	double WorstInverse = 0.0, WorstFormula = 0.0;
	int NotForward = 0;
	for (u32 I = 0; I < 5000; ++I)
	{
		const double Z = 2.0 * Unit(I, 40) - 1.0, Ph = 2.0 * Pi * Unit(I, 41), R = std::sqrt(std::max(0.0, 1.0 - Z * Z));
		const H::float3 D(float(R * std::cos(Ph)), float(R * std::sin(Ph)), float(Z));
		const double Zv = 2.0 * Unit(I, 42) - 1.0, Pv = 2.0 * Pi * Unit(I, 43), Rv = std::sqrt(std::max(0.0, 1.0 - Zv * Zv));
		const H::float3 V(float(Rv * std::cos(Pv)), float(Rv * std::sin(Pv)), float(Zv));
		const float B = float(0.05 + 0.85 * Unit(I, 44));
		const H::float3 A = H::Uni_Aberrate(D, V, B);
		const H::float3 Back = H::Uni_Aberrate(A, V, -B);
		WorstInverse = std::max(WorstInverse, double(std::sqrt(H::dot(Back - D, Back - D))));
		const double C = H::dot(D, V), Ca = H::dot(A, V);
		WorstFormula = std::max(WorstFormula, std::abs(Ca - (C + B) / (1.0 + B * C)));
		if (Ca < C - 1e-6) { ++NotForward; }
	}
	const H::float3 V0(1, 0, 0);
	const bool bFixed = H::Uni_Aberrate(V0, V0, 0.5f).x == 1.0f && H::Uni_Aberrate(H::float3(-1, 0, 0), V0, 0.5f).x == -1.0f;
	// The forward cone: tan(t / 2) / tan(t' / 2).
	const double T = 0.5, Tp = std::acos(double(H::dot(H::Uni_Aberrate(H::float3(float(std::cos(T)), float(std::sin(T)), 0.0f), V0, 0.5f), V0)));
	const double Cone = std::tan(0.5 * T) / std::tan(0.5 * Tp);
	// Doppler.
	const double Dahead = H::Uni_DopplerFactor(V0, V0, 0.5f), Dbehind = H::Uni_DopplerFactor(H::float3(-1, 0, 0), V0, 0.5f);
	const H::float3 Warm(1.0f, 0.8f, 0.6f);
	const H::float3 Ta = H::Uni_DopplerTint(Warm, float(Dahead), 1.0f, 0.0f), Tb = H::Uni_DopplerTint(Warm, float(Dbehind), 1.0f, 0.0f), Ti = H::Uni_DopplerTint(Warm, 1.0f, 1.0f, 0.0f);
	const double LumKept = std::max(std::abs(H::Uni_Luminance(Ta) / H::Uni_Luminance(Warm) - 1.0f), std::abs(H::Uni_Luminance(Tb) / H::Uni_Luminance(Warm) - 1.0f));
	const bool bTint = Ta.z / Ta.x > Warm.z / Warm.x && Tb.z / Tb.x < Warm.z / Warm.x && Ti.x == Warm.x && Ti.y == Warm.y && Ti.z == Warm.z;
	// Streaks: b = 0 is the old path exactly; with b > 0, flying forward still points every tail
	// at the view centre (the motion), and nothing at rest.
	const H::float3 Side(0, 1, 0), Up(0, 0, 1);
	const H::float4 Warp(1.0f, 1.0f, 64.0f, 6.0f);
	int StreakDiff = 0, StreakWrong = 0, StreakRest = 0;
	for (u32 I = 0; I < 300; ++I)
	{
		const double Ty = 0.6 * (2.0 * Unit(I, 50) - 1.0), Tz = 0.6 * (2.0 * Unit(I, 51) - 1.0);
		const double N = std::sqrt(1.0 + Ty * Ty + Tz * Tz);
		const H::float3 Dir(float(1.0 / N), float(Ty / N), float(Tz / N));
		const H::float3 S0 = H::Uni_SpriteStreak(Dir, 1.0e22f, H::float3(2.0e20f, 0, 0), Side, Up, 600.0f, Warp, 1.0f, H::float4(0, 0, 0, 0));
		const H::float3 S1 = H::Uni_SpriteStreak(Dir, 1.0e22f, H::float3(2.0e20f, 0, 0), Side, Up, 600.0f, Warp, 1.0f, H::float4(1, 0, 0, 0));   // w = 0: off
		if (std::memcmp(&S0, &S1, sizeof(S0)) != 0) { ++StreakDiff; }
		const H::float3 S2 = H::Uni_SpriteStreak(Dir, 1.0e22f, H::float3(2.0e20f, 0, 0), Side, Up, 600.0f, Warp, 1.0f, H::float4(1, 0, 0, 0.5f));
		if (S2.z > 0.0f && std::sqrt(Ty * Ty + Tz * Tz) > 0.05 && !(S2.x * Ty + S2.y * Tz < 0.0)) { ++StreakWrong; }
		const H::float3 S3 = H::Uni_SpriteStreak(Dir, 1.0e22f, H::float3(0, 0, 0), Side, Up, 600.0f, Warp, 1.0f, H::float4(1, 0, 0, 0.5f));
		if (S3.z != 0.0f) { ++StreakRest; }
	}
	// Temporal: at rest with the optics on (same view, same b), every pixel's true direction
	// reprojects onto the pixel itself.
	double WorstRepro = 0.0;
	{
		H::FUniTemporalView TV;
		TV.PrevForward = H::float3(1, 0, 0); TV.PrevRight = H::float3(0, 1, 0); TV.PrevUp = H::float3(0, 0, 1);
		TV.PrevTanX = 1.0f; TV.PrevTanY = 0.5625f; TV.Phase = 0u; TV.CamStep = H::float3(0, 0, 0); TV.bHistory = 1u;
		TV.PrevVel = H::float3(0.8f, 0.6f, 0.0f); TV.PrevBeta = 0.4f;
		for (int Y = 0; Y < 90; Y += 7) for (int X = 0; X < 160; X += 7)
		{
			const double Nx = ((X + 0.5) / 160.0 - 0.5) * 2.0, Ny = (0.5 - (Y + 0.5) / 90.0) * 2.0 * 0.5625;
			const double L = std::sqrt(1.0 + Nx * Nx + Ny * Ny);
			const H::float3 Ap(float(1.0 / L), float(Nx / L), float(Ny / L));
			const H::float3 Tr = H::Uni_Aberrate(Ap, TV.PrevVel, -TV.PrevBeta);
			H::float2 Pp(0, 0);
			H::Uni_TemporalPrevPixel(Tr, H::UNI_VOL_LN_DEPTH_NONE, TV, H::float2(160.0f, 90.0f), Pp);
			WorstRepro = std::max(WorstRepro, std::sqrt((Pp.x - (X + 0.5)) * (Pp.x - (X + 0.5)) + (Pp.y - (Y + 0.5)) * (Pp.y - (Y + 0.5))));
		}
	}
	// Still: 3,000 stars all around, looking along the motion (90 degrees across), at b 0, 0.25 and
	// 0.5 (full warp at the default), Doppler colour at strength 1.
	{
		const int W = 320, Hh = 200;
		UniRender::FImage Img(W * 3, Hh);
		const double Ppr = W * 0.5;
		for (int Panel = 0; Panel < 3; ++Panel)
		{
			const float B = 0.25f * float(Panel);
			for (u32 I = 0; I < 3000; ++I)
			{
				const double Z = 2.0 * Unit(I, 60) - 1.0, Ph = 2.0 * Pi * Unit(I, 61), R = std::sqrt(std::max(0.0, 1.0 - Z * Z));
				const H::float3 D(float(Z), float(R * std::cos(Ph)), float(R * std::sin(Ph)));   // x: the motion
				const H::float3 A = H::Uni_Aberrate(D, V0, B);
				if (!(A.x > 0.05f)) { continue; }
				const double Sx = A.y / A.x, Sy = A.z / A.x;
				const H::float3 C = H::Uni_DopplerTint(H::float3(1.0f, 0.93f, 0.85f), H::Uni_DopplerFactor(D, V0, B), 1.0f, 0.0f) * float(0.6 + 2.0 * Unit(I, 62) * Unit(I, 62));
				const double Cx = Panel * W + W * 0.5 + Sx * Ppr, Cy = Hh * 0.5 - Sy * Ppr;
				for (int Y = int(Cy) - 3; Y <= int(Cy) + 3; ++Y) for (int X = int(Cx) - 3; X <= int(Cx) + 3; ++X)
				{
					if (X < Panel * W || X >= (Panel + 1) * W || Y < 0 || Y >= Hh) { continue; }
					const double Dx = X + 0.5 - Cx, Dy = Y + 0.5 - Cy;
					const float G = float(std::exp(-0.5 * (Dx * Dx + Dy * Dy) / 0.64) * 1.2);
					Img.Add(X, Y, C.x * G, C.y * G, C.z * G);
				}
			}
		}
		const std::string Dd = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(Dd.c_str(), 0755);
		Img.WritePpm(Dd + "/still_warp_optics.ppm");
		if (FILE* F = std::fopen((Dd + "/still_warp_optics.hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-190: stars all around, looking along the motion: effective speed 0, 0.25, 0.5 (left to right)\",\"aberration crowds them forward; Doppler colour blue ahead\"]}\n");
			std::fclose(F);
		}
	}
	Uni::Check(1, "T1.warp.optics", "D-190 warp optics: the remap is the relativistic aberration (1e-6) and its own inverse at -b (1e-5); nothing moves away from the motion; the motion's direction is fixed; the forward cone tightens by sqrt((1 + b) / (1 - b)) (1.732 at 0.5); the Doppler factor is g (1 + b cos t); its tint keeps luminance (1e-5), blue ahead, red behind, identity at 1; streaks unchanged without warp and towards the vanishing point with the optics on, none at rest; the temporal reprojection puts every pixel back on itself at rest under the optics (1e-3 px)")
		.Metric("inverse", WorstInverse).Metric("formula", WorstFormula).Metric("cone", Cone).Metric("d_ahead", Dahead).Metric("d_behind", Dbehind).Metric("tint_lum", LumKept).Metric("repro_px", WorstRepro)
		.Pass(WorstInverse < 1e-5 && WorstFormula < 1e-6 && NotForward == 0 && bFixed && std::abs(Cone - std::sqrt(3.0)) < 1e-4 && std::abs(Dahead - std::sqrt(3.0)) < 1e-5 && std::abs(Dbehind - 1.0 / std::sqrt(3.0)) < 1e-5
				&& LumKept < 1e-5 && bTint && StreakDiff == 0 && StreakWrong == 0 && StreakRest == 0 && WorstRepro < 1e-3,
			Uni::Fmt("inverse %.1e, formula %.1e, %d moved backwards, fixed %s; cone x%.4f at b 0.5; Doppler %.4f ahead, %.4f behind; tint luminance %.1e, colours %s; streaks: %d changed off, %d wrong, %d at rest; reprojection %.1e px",
				WorstInverse, WorstFormula, NotForward, bFixed ? "yes" : "no", Cone, Dahead, Dbehind, LumKept, bTint ? "ok" : "wrong", StreakDiff, StreakWrong, StreakRest, WorstRepro));
}

#include "UniMotes.h"

UNI_TEST(warp, motes)
{
	// D-192: warp ambience motes (WARP_PLAN E4). A scripted flight from the void camera: 1 s at
	// 1 Mpc/s, 3 s accelerating to 30 Mpc/s, 1 s at 30 Mpc/s, 60 frames a second, looking along the
	// motion (90 degrees across, 1920 px), at full warp. Every frame the lattice (UniMotes.h) and
	// every mote (Uni_MoteSprite, as the sprite pass builds them):
	//  - motes are fixed in the world: each visible one is found again the next frame at the same
	//    place (its level's 1e-5 of a cell) unless it fades out, and no mote appears or vanishes
	//    in view at more than 2% of the brightest's light (no pops: the block edge, the camera's
	//    half-cell shift and the levels' hand-over all happen at zero light);
	//  - the scale follows the speed (the cell about MoteFrames frames of motion once settled);
	//  - there are enough of them in view, and they streak (E1's streaks: median length);
	//  - none at rest (amount 0), none behind the camera.
	const double Mpc = Units::Mpc, Dt = 1.0 / 60.0, Ppr = 960.0, Frames = 30.0;
	const FVec3d Fwd = [] { const double L = std::sqrt(1.0 + 0.09 + 0.04); return FVec3d{ 1.0 / L, 0.3 / L, 0.2 / L }; }();
	const H::float3 F3(float(Fwd.X), float(Fwd.Y), float(Fwd.Z));
	const H::float3 Side = H::normalize(H::cross(F3, H::float3(0, 0, 1)));
	const H::float3 Up = H::cross(Side, F3);
	FCoord Cam = UniScenes::VoidCamera();
	FVec3f Frac{};
	FVec3d World{ 0, 0, 0 };   // the camera's displacement from the start (cm)
	FMoteScale Sc;
	struct FMote { FVec3d Pos; float I; int Slot; double CellCm; bool bView; float Streak; };
	std::vector<FMote> Prev;
	double WorstPop = 0.0, WorstStep = 0.0, ScaleErr = 0.0;
	size_t Matched = 0, Lost = 0, Behind = 0, Frames2 = 0;
	double InView = 0.0;
	std::vector<float> Streaks;
	const int N = 300;
	for (int Fr = 0; Fr < N; ++Fr)
	{
		const double Speed = Fr < 60 ? 1.0 * Mpc : (Fr < 240 ? Mpc * std::pow(30.0, double(Fr - 60) / 180.0) : 30.0 * Mpc);
		const FVec3d Step{ Fwd.X * Speed * Dt, Fwd.Y * Speed * Dt, Fwd.Z * Speed * Dt };
		AddDisplacement(Cam, Frac, Step);
		World = FVec3d{ World.X + Step.X, World.Y + Step.Y, World.Z + Step.Z };
		Sc.Update(Speed * Dt, Frames, Dt);
		if (Fr >= 270) { ScaleErr = std::max(ScaleErr, std::abs(Sc.Log2Cm - std::log2(Speed * Dt * Frames))); }
		const FMoteLattice L = MoteLattice(Cam, Frac, Sc.Log2Cm, 1.0f);
		const H::uint4 C0(L.Slot[0].Cell[0], L.Slot[0].Cell[1], L.Slot[0].Cell[2], L.Slot[0].Level), C1(L.Slot[1].Cell[0], L.Slot[1].Cell[1], L.Slot[1].Cell[2], L.Slot[1].Level);
		const H::float4 F0(L.Slot[0].Frac[0], L.Slot[0].Frac[1], L.Slot[0].Frac[2], L.Slot[0].Weight), F1(L.Slot[1].Frac[0], L.Slot[1].Frac[1], L.Slot[1].Frac[2], L.Slot[1].Weight);
		const H::float4 Scale(float(L.Slot[0].CellCm), float(L.Slot[1].CellCm), 1.0f, float(1.0 / Ppr));
		const H::float3 CamStep(float(Step.X), float(Step.Y), float(Step.Z));
		std::vector<FMote> Cur;
		size_t ViewNow = 0;
		for (u32 I = 0; I < H::UNI_MOTE_COUNT; ++I)
		{
			const H::FUniSprite S = H::Uni_MoteSprite(I, C0, F0, C1, F1, Scale, F3, H::float4(0, 0, 0, 0), H::float2(0, 0));
			if (!(S.Flags & H::UNI_SPRITE_VISIBLE)) { continue; }
			const double R = S.TrueDistance;
			const H::float3 D = H::normalize(S.RenderPos);
			Behind += H::dot(D, F3) <= 0.0f ? 1 : 0;
			FMote M;
			M.Pos = FVec3d{ World.X + D.x * R, World.Y + D.y * R, World.Z + D.z * R };
			M.I = S.Radiance.x;
			M.Slot = I >= H::UNI_MOTE_COUNT / 2u ? 1 : 0;
			M.CellCm = L.Slot[M.Slot].CellCm;
			M.bView = H::dot(D, F3) > 0.7071f && std::abs(H::dot(D, Side)) < 0.7071f * H::dot(D, F3) && std::abs(H::dot(D, Up)) < 0.5625f * H::dot(D, F3);   // 16:9, 90 degrees across
			const H::float3 St = H::Uni_SpriteStreak(D, float(R), CamStep, Side, Up, float(Ppr), H::float4(1.0f, 1.0f, 64.0f, 0.0f), 1.0f, H::float4(0, 0, 0, 0));
			M.Streak = St.z;
			if (M.bView && M.I > 0.05f) { ++ViewNow; if (Fr >= 240) { Streaks.push_back(St.z); } }
			Cur.push_back(M);
		}
		if (Fr >= 60) { InView += double(ViewNow); ++Frames2; }
		// Match against the previous frame: the same slot, the same place.
		if (!Prev.empty())
		{
			std::vector<char> Used(Prev.size(), 0);
			for (const FMote& M : Cur)
			{
				int Hit = -1;
				for (size_t J = 0; J < Prev.size(); ++J)
				{
					if (Used[J] || Prev[J].Slot != M.Slot) { continue; }
					const double Dx = M.Pos.X - Prev[J].Pos.X, Dy = M.Pos.Y - Prev[J].Pos.Y, Dz = M.Pos.Z - Prev[J].Pos.Z;
					if (std::sqrt(Dx * Dx + Dy * Dy + Dz * Dz) < 1e-5 * M.CellCm) { Hit = int(J); break; }
				}
				if (Hit >= 0) { Used[size_t(Hit)] = 1; ++Matched; WorstStep = std::max(WorstStep, double(std::abs(M.I - Prev[size_t(Hit)].I))); }
				else if (M.bView) { WorstPop = std::max(WorstPop, double(M.I)); }
			}
			for (size_t J = 0; J < Prev.size(); ++J) { if (!Used[J]) { ++Lost; if (Prev[J].bView) { WorstPop = std::max(WorstPop, double(Prev[J].I)); } } }
		}
		Prev = Cur;
	}
	std::sort(Streaks.begin(), Streaks.end());
	const double Median = Streaks.empty() ? 0.0 : Streaks[Streaks.size() / 2];
	const double MeanView = Frames2 ? InView / double(Frames2) : 0.0;
	// At rest: none.
	size_t AtRest = 0;
	{
		const FMoteLattice L = MoteLattice(Cam, Frac, Sc.Log2Cm, 0.0f);
		const H::uint4 C0(L.Slot[0].Cell[0], L.Slot[0].Cell[1], L.Slot[0].Cell[2], L.Slot[0].Level), C1(L.Slot[1].Cell[0], L.Slot[1].Cell[1], L.Slot[1].Cell[2], L.Slot[1].Level);
		const H::float4 F0(L.Slot[0].Frac[0], L.Slot[0].Frac[1], L.Slot[0].Frac[2], L.Slot[0].Weight), F1(L.Slot[1].Frac[0], L.Slot[1].Frac[1], L.Slot[1].Frac[2], L.Slot[1].Weight);
		for (u32 I = 0; I < H::UNI_MOTE_COUNT; ++I)
		{
			const H::FUniSprite S = H::Uni_MoteSprite(I, C0, F0, C1, F1, H::float4(float(L.Slot[0].CellCm), float(L.Slot[1].CellCm), 1.0f, 1e-3f), F3, H::float4(0, 0, 0, 0), H::float2(0, 0));
			AtRest += (S.Flags & H::UNI_SPRITE_VISIBLE) ? 1 : 0;
		}
	}
	// Still: the last frame's motes in view, drawn with the pixel shader's streak profile.
	{
		const int W = 480, Hh = 270;
		const double P = W * 0.5;
		UniRender::FImage Img(W, Hh);
		for (const FMote& M : Prev)
		{
			if (!M.bView) { continue; }
			const FVec3d Rl{ M.Pos.X - World.X, M.Pos.Y - World.Y, M.Pos.Z - World.Z };
			const double Rn = std::sqrt(Rl.X * Rl.X + Rl.Y * Rl.Y + Rl.Z * Rl.Z);
			const H::float3 D(float(Rl.X / Rn), float(Rl.Y / Rn), float(Rl.Z / Rn));
			const double Fz = H::dot(D, F3), Sx = H::dot(D, Side) / Fz, Sy = H::dot(D, Up) / Fz;
			const double Cx = W * 0.5 + Sx * P, Cy = Hh * 0.5 - Sy * P;
			const float Ex = float(P / Ppr);   // the still is a quarter of the 1920 px view
			const H::float3 Lf = H::cross(F3, H::float3(0, 0, 1));
			(void)Lf;
			const FVec3d Stp{ Fwd.X * 30.0 * Mpc * Dt, Fwd.Y * 30.0 * Mpc * Dt, Fwd.Z * 30.0 * Mpc * Dt };
			const H::float3 St = H::Uni_SpriteStreak(D, float(Rn), H::float3(float(Stp.X), float(Stp.Y), float(Stp.Z)), Side, Up, float(P), H::float4(1.0f, 1.0f, 64.0f * Ex, 0.0f), 1.0f, H::float4(0, 0, 0, 0));
			const int Rr = int(St.z) + 4;
			for (int Y = int(Cy) - Rr; Y <= int(Cy) + Rr; ++Y) for (int X = int(Cx) - Rr; X <= int(Cx) + Rr; ++X)
			{
				if (X < 0 || X >= W || Y < 0 || Y >= Hh) { continue; }
				const float Px = float((X + 0.5) - Cx), Py = float(-((Y + 0.5) - Cy));
				const bool bS = St.z > 0.0f;
				const float Wt = H::Uni_SpriteProfileStreak(Px, Py, 1.0f, 0.0f, 0.0f, 1.0f, bS ? St.x : 1.0f, bS ? St.y : 0.0f, bS ? St.z : 0.0f);
				Img.Add(X, Y, M.I * Wt * 6.0f, M.I * Wt * 6.2f, M.I * Wt * 6.8f);
			}
		}
		const std::string Dd = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(Dd.c_str(), 0755);
		Img.WritePpm(Dd + "/still_warp_motes.ppm");
		if (FILE* Fh = std::fopen((Dd + "/still_warp_motes.hud.json").c_str(), "wb"))
		{
			std::fprintf(Fh, "{\"lines\":[\"D-192: warp motes at 30 Mpc/s in a void, full warp, looking along the motion (90 degrees)\",\"decoration fixed in space; the nearest streak past\"]}\n");
			std::fclose(Fh);
		}
	}
	Uni::Check(1, "T1.warp.motes", "D-192 warp motes over a scripted flight (1 to 30 Mpc/s, 300 frames, full warp): fixed in the world (found again the next frame within 1e-5 of a cell unless fading out), nothing appears or vanishes in view above 2% of the brightest's light, the scale settles to the speed (0.1 octave), at least 15 in a 90 degree view on average, median streak at 30 Mpc/s at least 2 px, none behind the camera, none at rest")
		.Metric("matched", double(Matched)).Metric("lost", double(Lost)).Metric("worst_pop", WorstPop).Metric("worst_step", WorstStep).Metric("scale_err_octaves", ScaleErr).Metric("in_view", MeanView).Metric("median_streak_px", Median)
		.Pass(Matched > 1000 && WorstPop < 0.02 && ScaleErr < 0.1 && MeanView >= 15.0 && Median >= 2.0 && Behind == 0 && AtRest == 0,
			Uni::Fmt("%zu mote-frames found again, %zu faded out; worst appearance or loss in view %.4f of the brightest, worst frame-to-frame change %.3f; scale %.3f octaves off once settled; %.1f in view on average; median streak %.1f px at 30 Mpc/s; %zu behind the camera, %zu at rest",
				Matched, Lost, WorstPop, WorstStep, ScaleErr, MeanView, Median, Behind, AtRest));
}

UNI_TEST(warp, speed)
{
	// D-199 (B-018, owner: at 425 kpc/s inside a cluster nearly every galaxy vanished with warp
	// on). The warp optics' motion was the camera's step normalised in float: its length squared
	// overflows past 1.8e19 cm (6 pc a frame), so at any warp speed the motion came out zero with
	// the optics on. A zero motion shortened every direction by sqrt(1 - b^2): every sprite took a
	// tail of that shortening whatever its distance (at full warp ~50 px off centre, its light
	// spread that far), and the volume marched shortened rays. Now in double
	// (UniCore::WarpMotion), and the shader's aberration and Doppler leave a direction alone when
	// there is no motion.
	const double Kpc = 3.0856775814913673e21;
	const double Step[3] = { 7.0 * Kpc, 0.0, 0.0 };   // 425 kpc/s at 60 fps, strafing (+x is the view's right here)
	float Vb[4];
	UniCore::WarpMotion(Step[0], Step[1], Step[2], 0.5 * 0.25, Vb);
	const double Ul = std::sqrt(double(Vb[0]) * Vb[0] + double(Vb[1]) * Vb[1] + double(Vb[2]) * Vb[2]);
	// What UE's float Size() made of it.
	const float Fs = float(Step[0]);
	const float FloatLen = std::sqrt(Fs * Fs);
	// The shader with a zero motion: nothing changes.
	const H::float3 Dir(0.6f, 0.0f, 0.8f);
	const H::float3 Ab = H::Uni_Aberrate(Dir, H::float3(0, 0, 0), 0.5f);
	const float AbErr = std::abs(Ab.x - Dir.x) + std::abs(Ab.y - Dir.y) + std::abs(Ab.z - Dir.z);
	const float Dz = H::Uni_DopplerFactor(Dir, H::float3(0, 0, 0), 0.5f);
	// Streaks of galaxies 20 degrees off the view's centre, the view looking along +y, the step
	// along +x: at 1 Mpc and 50 Mpc, with the fixed motion and with the old zero one.
	const float Ppr = 1197.0f;
	const H::float3 Side(1, 0, 0), Up(0, 0, 1), Look(0.3420201f, 0.9396926f, 0.0f);
	const H::float4 Warp(0.25f, 1.0f, 64.0f, 6.0f);
	const H::float3 Cs(float(Step[0]), 0.0f, 0.0f);
	auto Streak = [&](double DistMpc, const H::float4& VelBeta)
	{
		return H::Uni_SpriteStreak(Look, float(DistMpc * 1000.0 * Kpc), Cs, Side, Up, Ppr, Warp, 1.0f, VelBeta).z;
	};
	const H::float4 Fixed(Vb[0], Vb[1], Vb[2], Vb[3]);
	const float S1 = Streak(1.0, Fixed), S50 = Streak(50.0, Fixed);
	// The camera's parallax x the warp amount (the streak's exposure), slightly squeezed by the optics.
	const float Expected1 = float(7.0 / 1000.0 * std::cos(20.0 * 3.141592653589793 / 180.0) * Ppr) * Warp.x;
	// The old streak with a zero motion, emulated (the aberration before D-199): the tail is the
	// directions' shortening by sqrt(1 - b^2) projected on the view, whatever the distance. At full
	// warp (amount 1, b 0.5) and at the owner's 425 kpc/s (amount ~0.23, b ~0.12).
	auto OldTail = [&](float Amount, float Beta, float OffDeg)
	{
		const float Short = 1.0f - std::sqrt(1.0f - Beta * Beta);
		return std::min(Short * std::sin(OffDeg * 3.14159265f / 180.0f) * Ppr * Amount, 64.0f);
	};
	const float OldFull20 = OldTail(1.0f, 0.5f, 20.0f), OldFull40 = OldTail(1.0f, 0.5f, 40.0f), Old425 = OldTail(0.23f, 0.115f, 40.0f);
	Uni::Check(1, "T1.warp.speed", "D-199 (B-018) warp at intergalactic speed: the optics' motion for a 7 kpc step is a unit vector along it (1e-6; UE's float length was inf); a zero motion leaves directions and colours alone; a galaxy 20 degrees off centre at 1 Mpc streaks the camera's parallax x the warp amount (10%), and at 50 Mpc under 1 px")
		.Metric("unit_err", std::abs(Ul - 1.0)).Metric("streak_1mpc_px", S1).Metric("streak_50mpc_px", S50).Metric("old_full_warp_20deg_px", OldFull20)
		.Pass(std::abs(Ul - 1.0) < 1e-6 && Vb[0] > 0.999f && AbErr < 1e-6f && std::abs(Dz - 1.0f) < 1e-6f
				&& std::abs(S1 / Expected1 - 1.0f) < 0.10f && S50 < 1.0f,
			Uni::Fmt("motion %.7f %.7f %.7f (|v| - 1 = %.1e), beta %.3f; float length of the step %g; zero motion: direction change %.1e, Doppler %.6f; streaks at 1 Mpc %.2f px (parallax x warp %.2f), at 50 Mpc %.3f px. Before (zero motion): every sprite's tail %.0f px at 20 degrees off centre and %.0f px at 40 at full warp, at any distance (cap 64); %.1f px at 40 degrees at 425 kpc/s",
				Vb[0], Vb[1], Vb[2], Ul - 1.0, Vb[3], double(FloatLen), AbErr, Dz, S1, Expected1, S50, OldFull20, OldFull40, Old425));
}
