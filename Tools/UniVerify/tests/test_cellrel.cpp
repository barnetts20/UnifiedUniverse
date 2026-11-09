// test_cellrel.cpp
// D-204 (owner: objects in a cell need not redo the digits their cell shares). The project pass
// placed every object by adding its in-cell offset into its cell's exact 128-bit camera delta
// and converting to float (D-104), ~100 integer operations per object per frame. Now each cell
// is converted once (hi + lo floats) and, when the camera is a cell width or more from it on
// some axis, its objects are float adds (Uni_CellRel, Uni_ObjectRelativeC); the cells around the
// camera keep the exact path.
//
//   T2.parallax.cell_rel   every resident object of the flythrough scene at four cameras (the
//                          solar camera, the anchor's centre, the edge of a cell, intergalactic
//                          space with the web): direction and distance against the exact
//                          double-precision vector within 3e-7 (the exact path's own bound is
//                          ~1e-7), and the far path taking most objects

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../scenes/UniScenes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

UNI_TEST(parallax, cell_rel)
{
	FUniverseModel M(UniScenes::Flythrough());
	FRefGenerator G(M);
	struct FCam { const char* Name; FCoord C; FVec3f Frac; };
	std::vector<FCam> Cams;
	Cams.push_back({ "solar", UniScenes::SolarCamera(), FVec3f{ 0.37f, 0.81f, 0.05f } });
	{
		FCoord C = FCoord::Origin(); FVec3f F{};
		AddDisplacement(C, F, FVec3d{ 0.31 * Units::Pc, -0.2 * Units::Pc, 0.05 * Units::Pc });
		Cams.push_back({ "centre", C, FVec3f{ 0.5f, 0.5f, 0.5f } });
	}
	{
		// Just inside the low corner of a level-66 cell (star cells ~24 pc): the far test's edge.
		FCoord C = UniScenes::SolarCamera();
		FCellKey K = CellOf(C, 66);
		C = CellOrigin(K);
		FVec3f F{};
		AddDisplacement(C, F, FVec3d{ 3.0, 7.0, 1.0 });
		Cams.push_back({ "cell edge", C, FVec3f{ 0.9f, 0.1f, 0.6f } });
	}
	Cams.push_back({ "intergalactic", UniScenes::VoidCamera(), FVec3f{ 0.2f, 0.4f, 0.7f } });
	double WorstDir = 0.0, WorstDist = 0.0, WorstExactDir = 0.0;
	size_t Objects = 0, Far = 0;
	double NsExact = 0.0, NsFar = 0.0;
	std::string Detail;
	for (const FCam& Cm : Cams)
	{
		FRefPipeline P(G);
		P.bVelocityLod = false;
		P.Update(Cm.C, FVec3f{});
		size_t N = 0, Nf = 0;
		double Wd = 0.0;
		// Per cell: the context once, then every object (as the project pass does).
		std::vector<std::pair<FCellKey, std::vector<FObjectGPU>>> Cells;
		P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float, u64)
		{
			if (Cells.empty() || !(Cells.back().first == K)) { Cells.push_back({ K, {} }); }
			Cells.back().second.push_back(O);
		});
		const H::float3 Fr(Cm.Frac.X, Cm.Frac.Y, Cm.Frac.Z);
		for (const auto& Cl : Cells)
		{
			H::uint4 D[3];
			CellDelta(Cl.first, Cm.C, D);
			const H::FUniCellRel R = H::Uni_CellRel(D[0], D[1], D[2], u32(Cl.first.Level));
			for (const FObjectGPU& O : Cl.second)
			{
				const H::uint3 Off(O.A[0], O.A[1], O.A[2]);
				const u32 Offs[3] = { O.A[0], O.A[1], O.A[2] };
				const FVec3d E = ToDouble(Sub(ObjectCoord(Cl.first, Offs), Cm.C));
				const double Ex = E.X - Cm.Frac.X, Ey = E.Y - Cm.Frac.Y, Ez = E.Z - Cm.Frac.Z;
				const double El = std::sqrt(Ex * Ex + Ey * Ey + Ez * Ez);
				if (!(El > 0.0)) { continue; }
				const H::float3 V = H::Uni_ObjectRelativeC(R, Off, Fr);
				const H::float3 X = H::Uni_ObjectRelative(D[0], D[1], D[2], Off, u32(Cl.first.Level), Fr);
				auto Err = [&](const H::float3& A, double* OutDist)
				{
					const double Ax = A.x, Ay = A.y, Az = A.z;
					const double Al = std::sqrt(Ax * Ax + Ay * Ay + Az * Az);
					const double Cx = Ay * Ez - Az * Ey, Cy = Az * Ex - Ax * Ez, Cz = Ax * Ey - Ay * Ex;
					if (OutDist) { *OutDist = std::fabs(Al / El - 1.0); }
					return std::sqrt(Cx * Cx + Cy * Cy + Cz * Cz) / (Al * El);
				};
				double Dd = 0.0;
				const double De = Err(V, &Dd);
				WorstDir = std::max(WorstDir, De);
				WorstDist = std::max(WorstDist, Dd);
				Wd = std::max(Wd, De);
				WorstExactDir = std::max(WorstExactDir, Err(X, nullptr));
				++N;
				Nf += R.bFar ? 1u : 0u;
			}
		}
		Objects += N;
		Far += Nf;
		Detail += Uni::Fmt("%s%s %zu objects, %.0f%% far, worst %.2g rad", Detail.empty() ? "" : "; ", Cm.Name, N, N ? 100.0 * double(Nf) / double(N) : 0.0, Wd);
		// Time on the CPU shim (a proxy: the GPU does the 128-bit math natively in 32-bit lanes).
		if (std::string(Cm.Name) == "solar")
		{
			using Clock = std::chrono::steady_clock;
			volatile float Sink = 0.0f;
			size_t Count = 0;
			const auto T0 = Clock::now();
			for (const auto& Cl : Cells)
			{
				H::uint4 D[3];
				CellDelta(Cl.first, Cm.C, D);
				for (const FObjectGPU& O : Cl.second) { Sink = Sink + H::Uni_ObjectRelative(D[0], D[1], D[2], H::uint3(O.A[0], O.A[1], O.A[2]), u32(Cl.first.Level), Fr).x; ++Count; }
			}
			const auto T1 = Clock::now();
			for (const auto& Cl : Cells)
			{
				H::uint4 D[3];
				CellDelta(Cl.first, Cm.C, D);
				const H::FUniCellRel R = H::Uni_CellRel(D[0], D[1], D[2], u32(Cl.first.Level));
				for (const FObjectGPU& O : Cl.second) { Sink = Sink + H::Uni_ObjectRelativeC(R, H::uint3(O.A[0], O.A[1], O.A[2]), Fr).x; }
			}
			const auto T2 = Clock::now();
			NsExact = std::chrono::duration<double, std::nano>(T1 - T0).count() / double(std::max<size_t>(Count, 1));
			NsFar = std::chrono::duration<double, std::nano>(T2 - T1).count() / double(std::max<size_t>(Count, 1));
		}
	}
	Uni::Check(2, "T2.parallax.cell_rel", "D-204 objects placed from their cell's hi + lo float origin when the camera is a cell width or more away (the exact integer path otherwise): direction and distance within 3e-7 of exact double precision at four cameras; most objects take the float path")
		.Metric("worst_dir", WorstDir).Metric("worst_dist", WorstDist).Metric("worst_dir_exact_path", WorstExactDir).Metric("objects", double(Objects)).Metric("far_share", Objects ? double(Far) / double(Objects) : 0.0)
		.Metric("ns_exact", NsExact).Metric("ns_cell", NsFar)
		.Pass(WorstDir < 3.0e-7 && WorstDist < 3.0e-7 && Objects > 10000 && double(Far) > 0.5 * double(Objects),
			Uni::Fmt("%s. Over all: direction within %.2g rad, distance %.2g (the exact path alone: %.2g rad); %.0f%% of %zu objects on the float path; CPU shim %.0f -> %.0f ns an object",
				Detail.c_str(), WorstDir, WorstDist, WorstExactDir, Objects ? 100.0 * double(Far) / double(Objects) : 0.0, Objects, NsExact, NsFar));
}
