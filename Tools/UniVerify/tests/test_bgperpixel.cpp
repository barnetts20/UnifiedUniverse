// test_bgperpixel.cpp
// D-224 (owner: frame drops travelling through the galaxy at some angles): while travelling, the
// background was marched per pixel with the galaxy (uu.BackgroundMode 2), 7-10x the march's cost
// from inside a galaxy (explore.hitch, archived: mid-disc, 20 degrees below the plane looking at the
// core, every azimuth). Inside a local carrier the renderer now keeps it on its direction map
// (UniCore::CameraInsideLocalCarrier). Checks the rule on real scenes: inside the Milky Way (the
// solar camera and the owner's spot) it holds; at 60 kpc and in a void it does not, so fast
// intergalactic travel keeps the per-pixel background D-166 made it for. And the cost it avoids:
// the march's time with the background per pixel against without, at the owner's spot.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"

#include <chrono>
#include <cmath>
#include <string>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	double MarchMs(const FVolumeScene& Sc, const FCamera& Cam)
	{
		const auto Cb = Sc.CarrierBuf(); const auto Pb = Sc.ParamBuf(); const auto Lb = Sc.LutBuf();
		const H::FUniVolumeView V = ToHlsl(Sc.View);
		const auto T0 = std::chrono::steady_clock::now();
		for (int Y = 0; Y < Cam.Height; ++Y) for (int X = 0; X < Cam.Width; ++X)
		{
			bool bSuper = false;
			(void)MarchPixel(Cb, Pb, Lb, V, Cam, Cam.Width, Cam.Height, X, Y, 3.0e38f, bSuper);
		}
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
	}
}

UNI_TEST(volume, bg_per_pixel_inside)
{
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	FVolumeBuilder VB(M, G);
	const double Kpc = Units::Kpc, Deg = 3.14159265358979 / 180.0;
	struct FCase { const char* Name; FCoord Cam; FV3 Look; bool bInside; };
	FCoord Owner = FCoord::Origin(), Far = FCoord::Origin();
	{ FVec3f Fr{}; AddDisplacement(Owner, Fr, FVec3d{ 8.0 * Kpc * std::cos(60.0 * Deg), 8.0 * Kpc * std::sin(60.0 * Deg), -8.0 * Kpc * std::tan(20.0 * Deg) }); }
	{ FVec3f Fr{}; AddDisplacement(Far, Fr, FVec3d{ 0.0, -40.0 * Kpc, 45.0 * Kpc }); }
	const FCase Cases[] = {
		{ "solar camera", UniScenes::SolarCamera(), { -1.0, 0.05, 0.02 }, true },
		{ "mid-disc, 20 deg below, at the core", Owner, { -std::cos(60.0 * Deg), -std::sin(60.0 * Deg), std::tan(20.0 * Deg) }, true },
		{ "60 kpc out", Far, { 0.0, 40.0, -45.0 }, false },
		{ "void", UniScenes::VoidCamera(), { 1.0, 0.2, 0.1 }, false },
	};
	bool bOk = true;
	std::string Detail;
	double Ratio = 0.0;
	for (const FCase& C : Cases)
	{
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(C.Cam, FVec3f{});
		FCamera Cm; Cm.Coord = C.Cam; Cm.Width = 96; Cm.Height = 54; Cm.DeltaMag = 0.0;
		Cm.LookAlong(C.Look);
		const FVolumeSettings S;
		const FVolumeScene Sc = VB.Build(P, C.Cam, FVec3f{}, Cm.View(), S);
		const FVolumeScene Local = Sc.Subset(false);
		const bool bIn = CameraInsideLocalCarrier(Local.Carriers.data(), Local.Carriers.size());
		const bool bAll = CameraInsideLocalCarrier(Sc.Carriers.data(), Sc.Carriers.size());   // periodic carriers are skipped
		if (bIn != C.bInside || bAll != bIn) { bOk = false; }
		Detail += Uni::Fmt("%s%s: %zu local carriers, inside %s (expected %s)", Detail.empty() ? "" : "; ", C.Name, Local.Carriers.size(), bIn ? "yes" : "no", C.bInside ? "yes" : "no");
		if (&C == &Cases[1])
		{
			const double A = MarchMs(Local, Cm), B = MarchMs(Sc, Cm);
			Ratio = B / std::max(A, 1e-9);
			Detail += Uni::Fmt(" (march with the background per pixel %.1fx without)", Ratio);
		}
	}
	if (!(Ratio > 2.0)) { bOk = false; }
	Uni::Check(2, "T2.vol.bg_per_pixel_inside", "D-224 the background stays on its map while the camera is inside a local carrier (in the Milky Way: the solar camera, mid-disc below the plane) and is still marched per pixel outside one (60 kpc, a void); per pixel inside costs more than 2x the march")
		.Metric("per_pixel_cost_ratio", Ratio).Pass(bOk, Detail);
}
