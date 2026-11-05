// test_sky.cpp
// D-186: the sky light's cubemap (UniSky.h). Its face convention is Direct3D's (Unreal's), its
// texels tile the sphere, and inside the Milky Way it holds the galaxy where it is: the band
// along the disc, brightest towards the centre, and the same total light at any resolution.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../scenes/UniScenes.h"
#include "UniSky.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	double Unit(u32 S, u32 K) { return double(H::Uni_DrawUnit(S, 0x5C1Bu, K)); }

	/** Direct3D's cube face selection: face, then (u, v) in [0, 1]. */
	int D3DFace(const FVec3d& D, double& U, double& V)
	{
		const double Ax = std::abs(D.X), Ay = std::abs(D.Y), Az = std::abs(D.Z);
		int F; double Sc, Tc, Ma;
		if (Ax >= Ay && Ax >= Az) { Ma = Ax; F = D.X > 0 ? 0 : 1; Sc = D.X > 0 ? -D.Z : D.Z; Tc = -D.Y; }
		else if (Ay >= Az) { Ma = Ay; F = D.Y > 0 ? 2 : 3; Sc = D.X; Tc = D.Y > 0 ? D.Z : -D.Z; }
		else { Ma = Az; F = D.Z > 0 ? 4 : 5; Sc = D.Z > 0 ? D.X : -D.X; Tc = -D.Y; }
		U = 0.5 * (Sc / Ma + 1.0); V = 0.5 * (Tc / Ma + 1.0);
		return F;
	}
}

UNI_TEST(sky, cube)
{
	// 1. Face convention: Direct3D's selection of a direction's face and texel, back through
	//    SkyTexelDir, is the direction again.
	double WorstDir = 0.0;
	for (u32 I = 0; I < 20000; ++I)
	{
		const double Z = 2.0 * Unit(I, 0) - 1.0, P = 6.283185307179586 * Unit(I, 1), R = std::sqrt(std::max(0.0, 1.0 - Z * Z));
		const FVec3d D{ R * std::cos(P), R * std::sin(P), Z };
		double U, V;
		const int F = D3DFace(D, U, V);
		const FVec3d B = SkyTexelDir(F, U * 16.0, V * 16.0, 16);
		WorstDir = std::max(WorstDir, std::sqrt((B.X - D.X) * (B.X - D.X) + (B.Y - D.Y) * (B.Y - D.Y) + (B.Z - D.Z) * (B.Z - D.Z)));
	}
	// 2. Texels tile the sphere.
	double Sphere = 0.0;
	for (int Y = 0; Y < 16; ++Y) for (int X = 0; X < 16; ++X) { Sphere += 6.0 * SkyTexelSolidAngle(X, Y, 16); }
	// 3. Inside the Milky Way (solar camera; the anchor's disc is the xy plane, its centre 8 kpc
	//    along -x): the cube at 16 and 48 texels a face.
	FUniverseModel M(UniScenes::GalaxyToStar());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	FRefPipeline P(G);
	P.bVelocityLod = false;
	const FCoord Cam = UniScenes::SolarCamera();
	P.Update(Cam, FVec3f{});
	FCamera Cm; Cm.Coord = Cam; Cm.Width = 160; Cm.Height = 100;
	const FVolumeScene Sc = VB.Build(P, Cam, FVec3f{}, Cm.View());
	double Flux[2] = { 0, 0 }, Ms16 = 0.0;
	double BrightZ = 0.0, BrightCentre = 0.0;
	const int Ns[2] = { 16, 48 };
	for (int K = 0; K < 2; ++K)
	{
		const int N = Ns[K];
		const auto T0 = std::chrono::steady_clock::now();
		const FSkyCube C = BuildSkyCube(Sc, N, 1.0);
		if (K == 0) { Ms16 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count(); }
		std::vector<std::pair<double, FVec3d>> Lum;
		for (int F = 0; F < 6; ++F) for (int Y = 0; Y < N; ++Y) for (int X = 0; X < N; ++X)
		{
			const H::float3 B = C.Texels[(size_t(F) * N + Y) * N + X];
			const double L = H::Uni_Luminance(B);
			Flux[K] += L * SkyTexelSolidAngle(X, Y, N);
			Lum.push_back({ L, SkyTexelDir(F, X + 0.5, Y + 0.5, N) });
		}
		if (K == 0)
		{
			std::sort(Lum.begin(), Lum.end(), [](const auto& A, const auto& B) { return A.first > B.first; });
			const size_t Top = Lum.size() / 20;
			for (size_t I = 0; I < Top; ++I) { BrightZ += std::abs(Lum[I].second.Z) / double(Top); }
			BrightCentre = -Lum[0].second.X;
		}
	}
	const double FluxRatio = Flux[0] / std::max(Flux[1], 1e-300);
	Uni::Check(2, "T2.sky.cube", "D-186 sky light cube: Direct3D's face selection of 20,000 directions comes back through SkyTexelDir (1e-12); texels tile the sphere (4 pi, 1e-9); inside the Milky Way the brightest 5% of texels lie along the disc (mean |z| under 0.2), the brightest towards the centre (cos > 0.8), and 16 and 48 texels a face hold the same light (3%)")
		.Metric("worst_dir", WorstDir).Metric("sphere", Sphere).Metric("bright_z", BrightZ).Metric("bright_centre", BrightCentre).Metric("flux_16_over_48", FluxRatio).Metric("ms_16", Ms16)
		.Pass(WorstDir < 1e-12 && std::abs(Sphere - 4.0 * 3.141592653589793) < 1e-9 && BrightZ < 0.2 && BrightCentre > 0.8 && std::abs(FluxRatio - 1.0) < 0.03,
			Uni::Fmt("directions back within %.1e; texels cover %.10f sr; brightest 5%% mean |z| %.3f, brightest texel towards the centre cos %.3f; light at 16 / 48 texels a face %.4f; the 16-texel cube in %.0f ms (one thread)",
				WorstDir, Sphere, BrightZ, BrightCentre, FluxRatio, Ms16));
}
