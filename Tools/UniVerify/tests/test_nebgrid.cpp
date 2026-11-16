// test_nebgrid.cpp
// D-252 (owner 2026-11-15: "we would rather resolve at a lower resolution/frequency than to resolve
// at a frequency that will cause static grain"; "sparse storage ... with the ability to swap
// between 128/256"): the nebula grid (UniCore/UniNebGrid.h, Core/UniVolume.ush THE NEBULA GRID),
// through the CPU emulation of the renderer's passes (src/UniNebGridRef.h).
//  - T2.vol.nebgrid_plan: the layouts (128 and 256 cells about the Milky Way's solar circle) fit
//    their memory and the pool's atlas; over a camera walk (small moves, jumps, a budget) every
//    entry the emulated table holds is its level's box's world brick at that entry, every brick
//    processed is in its box, all are processed once the budget allows; the hysteresis holds a
//    box still for a camera wandering across a brick boundary.
//  - T2.vol.nebgrid_fetch: filled about the solar camera, every read the sparse grid answers equals
//    the dense lattice's trilinear (the same bake at the 8 points), an empty brick's exactly (no
//    occupied brick classified empty); over a 1.5 kpc flight at the default budget no read returns
//    another brick's values; the grid after the flight reads what one filled there directly reads
//    (world anchored).
//  - T2.vol.nebgrid_walk (D-255): the cell walk's average over a segment is the mean of the level's
//    reads along it, additive under any cut (step boundaries do not matter), at most a read a cell
//    crossed, no reads through empty bricks, failing where the segment leaves the level's box.
//  - T2.vol.nebgrid_grain: over the engine's path (cloud resolve, blue noise, 4 x 4, 64 still frames)
//    at engine-like pixels (1.6 mrad), 250 pc, 1 kpc and 3 kpc from the Milky Way's brightest disc
//    nebula, the grid (128 cells, D-251's steps) at most half D-251's frame-to-frame change, the
//    light within 8% of the converged reference's (the cells spread the brightest nebula's peak past
//    the view: 1 kpc away the emission along the view's rays is x0.94, the field's own integral
//    against the grid's reads at the same points), its error at most 1.8x D-251's (softer: the cell).
//    D-253: 6 kpc away, past D-251's detail reach, the grid to 32 kpc keeps the nebula (its light at
//    the view's centre over the image's 1.5x D-251's excess, 5 points more) at most 2x its frame change.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniVolumeRef.h"
#include "../src/UniBlueNoise.h"
#include "../src/UniNebGridRef.h"
#include "../scenes/UniScenes.h"
#include "UniVolume.h"
#include "UniWebTexture.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <atomic>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	const double kPcField = 1.0 / 24000.0;   // the Milky Way's field radius is 24 kpc

	FNebGridConfig MilkyWayConfig(u32 Cells)
	{
		FNebGridConfig C;
		C.Cells = Cells;
		C.Cell0 = 4.0 * kPcField;
		C.LayerHalf = 500.0 * kPcField;
		C.KnotTexel = 125.0 * kPcField;
		C.Levels = NebGridLevelsFor(8000.0 * kPcField, Cells, C.Cell0);
		return C;
	}

	/** The emulated table's world bricks (what each entry holds), from the frame's operations. */
	struct FTableModel
	{
		std::vector<std::vector<std::array<int, 4>>> E;   // per level: (valid, wx, wy, wz)
		void Init(const FNebGridLayout& L) { E.assign(L.Levels.size(), {}); for (size_t K = 0; K < L.Levels.size(); ++K) { E[K].assign(L.Levels[K].Entries, { 0, 0, 0, 0 }); } }
		void Apply(const FNebGridPlanner::FFrameOps& Ops)
		{
			for (size_t K = 0; K < E.size(); ++K) { if (Ops.ResetMask & (1u << K)) { for (auto& V : E[K]) { V[0] = 0; } } }
			for (const FNebGridOpGPU& Op : Ops.Release) { E[Op.LevelEntry >> 24u][Op.LevelEntry & 0xFFFFFFu][0] = 0; }
			for (const FNebGridOpGPU& Op : Ops.Process) { E[Op.LevelEntry >> 24u][Op.LevelEntry & 0xFFFFFFu] = { 1, Op.Wx, Op.Wy, Op.Wz }; }
		}
	};

	double Lum(const H::float4& V) { return 0.2126 * V.x + 0.7152 * V.y + 0.0722 * V.z; }
}

UNI_TEST(volume, nebgrid_plan)
{
	// Layouts.
	bool bLayout = true;
	std::string Why;
	double Mb[2] = { 0.0, 0.0 };
	for (int I = 0; I < 2; ++I)
	{
		const FNebGridLayout L = NebGridLayout(MilkyWayConfig(I == 0 ? 128u : 256u));
		Mb[I] = double(L.PoolBytes() + L.TableBytes()) / 1048576.0;
		u32 Tb = 0, Sb = 0;
		for (const FNebGridLevelLayout& V : L.Levels)
		{
			bLayout = bLayout && V.TableBase == Tb && V.SlotBase == Sb && V.Entries == V.Bx * V.Bx * V.Bz && V.SlotCount <= V.Entries && V.SlotCount > 0u;
			Tb += V.Entries; Sb += V.SlotCount;
		}
		bLayout = bLayout && Tb == L.TableSize && Sb == L.Slots && L.SlotsX * L.SlotsY * L.SlotsZ >= L.Slots && L.TexX() <= 2048u && L.TexY() <= 2048u && L.TexZ() <= 2048u;
	}
	bLayout = bLayout && Mb[0] < 48.0 && Mb[1] < 170.0;
	if (!bLayout) { Why += " layout"; }

	// A walk: small moves, a jump every 120 frames, budget 64; the emulated table checked each frame.
	FNebGridPlanner Pl;
	const FNebGridConfig C = MilkyWayConfig(128u);
	Pl.Configure(C, 7u);
	FTableModel Tm;
	Tm.Init(Pl.Layout());
	FNebGridPlanner::FFrameOps Ops;
	double Cam[3] = { -8000.0 * kPcField, 0.0, 10.0 * kPcField };
	size_t BadHeld = 0, BadProc = 0, Frames = 0;
	auto Check = [&]()
	{
		for (size_t K = 0; K < Tm.E.size(); ++K)
		{
			std::int32_t Mn[3] = { 0, 0, 0 };
			Pl.Box(K, Mn);
			const FNebGridLevelLayout& V = Pl.Layout().Levels[K];
			for (u32 E = 0; E < V.Entries; ++E)
			{
				if (!Tm.E[K][E][0]) { continue; }
				std::int32_t W[3];
				Pl.WorldOf(K, E, W);
				if (W[0] != Tm.E[K][E][1] || W[1] != Tm.E[K][E][2] || W[2] != Tm.E[K][E][3]) { ++BadHeld; }
			}
		}
		for (const FNebGridOpGPU& Op : Ops.Process)
		{
			const size_t K = Op.LevelEntry >> 24u;
			std::int32_t Mn[3] = { 0, 0, 0 };
			Pl.Box(K, Mn);
			const FNebGridLevelLayout& V = Pl.Layout().Levels[K];
			const bool bIn = Op.Wx >= Mn[0] && Op.Wx < Mn[0] + int(V.Bx) && Op.Wy >= Mn[1] && Op.Wy < Mn[1] + int(V.Bx) && Op.Wz >= Mn[2] && Op.Wz < Mn[2] + int(V.Bz);
			const std::int32_t W[3] = { Op.Wx, Op.Wy, Op.Wz };
			if (!bIn || Pl.EntryOf(K, W) != (Op.LevelEntry & 0xFFFFFFu)) { ++BadProc; }
		}
	};
	for (int F = 0; F < 480; ++F)
	{
		const double Step = 14.0 * kPcField;   // under half a level-0 brick a frame
		Cam[0] += Step * (H::Uni_DrawUnit(0xA11Cu, 1u, u32(3 * F)) * 2.0 - 0.6);
		Cam[1] += Step * (H::Uni_DrawUnit(0xA11Cu, 1u, u32(3 * F + 1)) * 2.0 - 1.0);
		Cam[2] = std::clamp(Cam[2] + Step * (H::Uni_DrawUnit(0xA11Cu, 1u, u32(3 * F + 2)) * 2.0 - 1.0), -400.0 * kPcField, 400.0 * kPcField);
		if (F % 120 == 119) { Cam[0] += 3000.0 * kPcField; Cam[1] -= 1200.0 * kPcField; }
		Pl.Update(Cam, 0.0, 64u, Ops);
		Tm.Apply(Ops);
		Check();
		++Frames;
	}
	Pl.Update(Cam, 0.0, 0xFFFFFFFFu, Ops);
	Tm.Apply(Ops);
	Check();
	size_t Unheld = 0;
	for (const auto& Lv : Tm.E) { for (const auto& V : Lv) { Unheld += V[0] ? 0 : 1; } }
	const bool bDrained = Pl.Pending() == 0 && Unheld == 0;
	if (BadHeld || BadProc || !bDrained) { Why += " walk"; }
	// The hysteresis: wandering 0.4 of a level-0 brick either side of a brick boundary moves nothing.
	const double Bs0 = 8.0 * C.Cell0;
	const double Edge = (std::floor(Cam[0] / Bs0) + 1.0) * Bs0;
	double Wc[3] = { Edge - 0.4 * Bs0, Cam[1], Cam[2] };
	Pl.Update(Wc, 0.0, 0xFFFFFFFFu, Ops);
	const u64 Moves0 = Pl.Moves;
	for (int F = 0; F < 60; ++F)
	{
		Wc[0] = Edge + 0.4 * Bs0 * (F % 2 ? 1.0 : -1.0);
		Pl.Update(Wc, 0.0, 0xFFFFFFFFu, Ops);
	}
	const bool bHyst = Pl.Moves == Moves0;
	if (!bHyst) { Why += " hysteresis"; }
	Uni::Check(2, "T2.vol.nebgrid_plan", "D-252 the nebula grid's plan: 128 / 256-cell layouts within 48 / 170 MiB with contiguous tables and slot ranges and the pool's atlas under 2048 texels a side; over a 480-frame walk (small moves, a 3 kpc jump every 120 frames, 64 bricks a frame) every entry held is its box's world brick there and every brick processed is in its box, all drained once the budget allows; wandering 0.4 brick either side of a boundary moves no box")
		.Metric("mib_128", Mb[0]).Metric("mib_256", Mb[1])
		.Pass(Why.empty(), Uni::Fmt("128 cells %.1f MiB, 256 cells %.1f MiB; walk: %zu frames, %zu wrong entries, %zu bricks out of their box, drained %s; hysteresis %s%s",
			Mb[0], Mb[1], Frames, BadHeld, BadProc, bDrained ? "yes" : "NO", bHyst ? "held" : "MOVED", Why.empty() ? "" : (" -- failed:" + Why).c_str()));
}

namespace
{
	/** The Milky Way's galaxy with its textures bound, its field radius, a camera builder. */
	struct FNgScene
	{
		FWebTexture T[3];
		bool bTex = true;
		FUniverseDef D;
		const FFieldParams* Mw = nullptr;
		H::FUniFieldParams P{};
		H::FUniGalaxyConst K{};
		FNgScene()
		{
			for (int S = 0; S < 3; ++S) { std::string E; if (!FWebTexture::LoadDelivered("Data/Noise", GalaxyTextureName(S), T[S], &E)) { bTex = false; } }
			if (!bTex) { return; }
			for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, &T[I]); }
			D = UniScenes::Flythrough();
			UniScenes::UseGalaxyTextures(D, T[0].Size);
			UniScenes::UseGalaxyPalettes(D);
			for (const FAnchor& A : D.Anchors) { if (!A.EmitterParams.empty()) { Mw = &A.EmitterParams[0]; } }
			if (Mw) { P = Mw->ToHlsl(); K = H::Uni_GalaxyConstOf(P); }
		}
		~FNgScene() { for (int I = 0; I < 3; ++I) { BindGalaxyTexture(I, nullptr); } ApplyNebulaTune(FNebulaTune{}); UniVerify::FNebGridEmu::Unbind(); }
	};

	/** Level K's dense trilinear at P (field units): the same bake at the 8 lattice points. */
	H::float4 DenseAt(const FNebGridLayout& L, const H::FUniFieldParams& P, const H::FUniGalaxyConst& K, u32 Lv, const H::float3& Pt, float Detail, float Dark)
	{
		const float C = float(L.Levels[Lv].Cell);
		const float U[3] = { Pt.x / C, Pt.y / C, Pt.z / C };
		int I0[3]; float Fr[3];
		for (int A = 0; A < 3; ++A) { const float Fl = std::floor(U[A]); I0[A] = int(Fl); Fr[A] = U[A] - Fl; }
		H::float4 R(0.0f, 0.0f, 0.0f, 0.0f);
		for (int Q = 0; Q < 8; ++Q)
		{
			const int Ix = I0[0] + (Q & 1), Iy = I0[1] + ((Q >> 1) & 1), Iz = I0[2] + ((Q >> 2) & 1);
			const float W = ((Q & 1) ? Fr[0] : 1.0f - Fr[0]) * (((Q >> 1) & 1) ? Fr[1] : 1.0f - Fr[1]) * (((Q >> 2) & 1) ? Fr[2] : 1.0f - Fr[2]);
			if (W <= 0.0f) { continue; }
			R = R + H::Uni_NebGridBakePoint(H::float3(float(Ix), float(Iy), float(Iz)) * C, C, P, K, Detail, Dark, H::Uni_NebGridHash(Lv, Ix, Iy, Iz), L.Samples) * W;
		}
		return R;
	}

	/** A random point in level Lv's box (field units) from the bound headers. */
	H::float3 PointIn(const FNebGridLayout& L, const std::vector<H::float4>& Hd, u32 Lv, u32 Seed, u32 I)
	{
		const H::float4 Hb = Hd[H::UNI_NEB_GRID_HEADERS + 3u * Lv + 1u], Hc = Hd[H::UNI_NEB_GRID_HEADERS + 3u * Lv + 2u];
		const double Bs = 8.0 * L.Levels[Lv].Cell;
		const double X = (double(Hb.x) + double(Hb.w) * H::Uni_DrawUnit(Seed, 3u, 3u * I)) * Bs;
		const double Y = (double(Hb.y) + double(Hb.w) * H::Uni_DrawUnit(Seed, 3u, 3u * I + 1u)) * Bs;
		const double Z = (double(Hb.z) + double(Hc.x) * H::Uni_DrawUnit(Seed, 3u, 3u * I + 2u)) * Bs;
		return H::float3(float(X), float(Y), float(Z));
	}

	bool Close(const H::float4& A, const H::float4& B)
	{
		const float Va[4] = { A.x, A.y, A.z, A.w }, Vb[4] = { B.x, B.y, B.z, B.w };
		for (int I = 0; I < 4; ++I) { if (!(std::abs(Va[I] - Vb[I]) <= 2.0e-4f * (1.0f + std::abs(Vb[I])))) { return false; } }
		return true;
	}
	bool IsEmpty(const H::float4& V) { return V.x == 0.0f && V.y == 0.0f && V.z == 1.0f && V.w == 1.0f; }
}

UNI_TEST(volume, nebgrid_fetch)
{
	FNgScene S;
	if (!S.bTex || !S.Mw) { Uni::Check(2, "T2.vol.nebgrid_fetch", "D-252").Pass(false, "textures not loaded"); return; }
	ApplyNebulaTune(FNebulaTune{});
	const FNebGridConfig C = MilkyWayConfig(128u);
	const float Detail = 1.0f, Dark = 0.35f;
	UniVerify::FNebGridEmu G;
	G.Configure(C, 1u, S.P, Detail, Dark);
	double Cam[3] = { -8000.0 * kPcField, 0.0, 10.0 * kPcField };
	G.Frame(Cam, 0.0, 0xFFFFFFFFu);
	const FNebGridLayout& L = G.Planner.Layout();
	const u32 Nl = u32(L.Levels.size());
	// Filled: every read against the dense lattice.
	size_t N = 0, Bad = 0, Missed = 0, Unread = 0, EmptyReads = 0;
	{
		const int Np = 2400;
		std::vector<int> R(size_t(Np), 0);
		std::atomic<int> Next{ 0 };
		auto Work = [&]()
		{
			for (int I = Next++; I < Np; I = Next++)
			{
				const u32 Lv = u32(I) % Nl;
				const H::float3 Pt = PointIn(L, G.Headers, Lv, 0x5EEDu, u32(I));
				H::float4 V(0, 0, 1, 1);
				const bool bOk = H::Uni_NebGridFetch(Lv, Pt, V);
				if (!bOk) { R[size_t(I)] = 3; continue; }
				const H::float4 Dn = DenseAt(L, S.P, S.K, Lv, Pt, Detail, Dark);
				// An empty brick reads exactly (0, 0, 1, 1); the dense lattice's trilinear of its eight
				// empty points within rounding.
				R[size_t(I)] = Close(V, Dn) ? (IsEmpty(V) ? 4 : 0) : (IsEmpty(V) ? 2 : 1);
			}
		};
		std::thread T1(Work); Work(); T1.join();
		for (int V : R) { ++N; Bad += V == 1 ? 1 : 0; Missed += V == 2 ? 1 : 0; Unread += V == 3 ? 1 : 0; EmptyReads += V == 4 ? 1 : 0; }
	}
	const size_t Overflow0 = G.Overflow;
	// A 1.5 kpc flight at the default budget: no read another brick's values.
	size_t FlightReads = 0, FlightOk = 0, FlightBad = 0;
	for (int F = 0; F < 60; ++F)
	{
		Cam[0] += 20.0 * kPcField; Cam[1] += 15.0 * kPcField;
		G.Frame(Cam, 0.0, 128u);
		std::vector<int> R(200, 0);
		std::atomic<int> Next{ 0 };
		auto Work = [&]()
		{
			for (int I = Next++; I < 200; I = Next++)
			{
				const u32 Lv = u32(I) % Nl;
				const H::float3 Pt = PointIn(L, G.Headers, Lv, 0xF1u + u32(F), u32(I));
				H::float4 V(0, 0, 1, 1);
				if (!H::Uni_NebGridFetch(Lv, Pt, V)) { R[size_t(I)] = 0; continue; }
				R[size_t(I)] = Close(V, DenseAt(L, S.P, S.K, Lv, Pt, Detail, Dark)) ? 1 : 2;
			}
		};
		std::thread T1(Work); Work(); T1.join();
		for (int V : R) { ++FlightReads; FlightOk += V == 1 ? 1 : 0; FlightBad += V == 2 ? 1 : 0; }
	}
	G.Frame(Cam, 0.0, 0xFFFFFFFFu);
	// World anchored: a grid filled straight at the flight's end reads the same.
	UniVerify::FNebGridEmu G2;
	G2.Configure(C, 1u, S.P, Detail, Dark);
	G2.Frame(Cam, 0.0, 0xFFFFFFFFu);
	size_t Same = 0, Diff = 0;
	for (u32 I = 0; I < 600; ++I)
	{
		const u32 Lv = I % Nl;
		const H::float3 Pt = PointIn(L, G2.Headers, Lv, 0xB1Bu, I);
		H::float4 A(0, 0, 1, 1), B(0, 0, 1, 1);
		G.Bind();
		const bool bA = H::Uni_NebGridFetch(Lv, Pt, A);
		G2.Bind();
		const bool bB = H::Uni_NebGridFetch(Lv, Pt, B);
		if (bA && bB) { Close(A, B) ? ++Same : ++Diff; }   // (the boxes may differ: the walked one kept by the hysteresis; a slot's place in the pool rounds its read)
	}
	UniVerify::FNebGridEmu::Unbind();
	const bool bPass = Bad == 0 && Missed == 0 && Unread == 0 && Overflow0 == 0 && FlightBad == 0 && FlightOk > FlightReads / 2 && Diff == 0 && Same > 300;
	Uni::Check(2, "T2.vol.nebgrid_fetch", "D-252 the sparse nebula grid (128 cells, filled about the solar camera through the passes' emulation) reads what the dense lattice's trilinear gives (2e-4), an empty brick exactly (no occupied brick classified empty), every read answered (no overflow); over a 1.5 kpc flight at 128 bricks a frame no read returns another brick's values (most answered); after it the grid reads what one filled there directly reads (2e-4)")
		.Metric("reads", double(N)).Metric("empty_reads", double(EmptyReads)).Metric("flight_answered", double(FlightOk) / double(std::max<size_t>(FlightReads, 1)))
		.Pass(bPass, Uni::Fmt("%zu reads: %zu off the dense lattice, %zu occupied read empty, %zu unanswered, %zu empty; overflow %zu; flight: %zu of %zu answered, %zu wrong; world anchored: %zu same, %zu differ; pool %.1f MiB, %zu bricks allocated, %zu empty",
			N, Bad, Missed, Unread, EmptyReads, Overflow0, FlightOk, FlightReads, FlightBad, Same, Diff, double(L.PoolBytes()) / 1048576.0, G.Allocated, G.Empty));
}

UNI_TEST(volume, nebgrid_walk)
{
	FNgScene S;
	if (!S.bTex || !S.Mw) { Uni::Check(2, "T2.vol.nebgrid_walk", "D-255").Pass(false, "textures not loaded"); return; }
	ApplyNebulaTune(FNebulaTune{});
	const FNebGridConfig C = MilkyWayConfig(128u);
	UniVerify::FNebGridEmu G;
	G.Configure(C, 1u, S.P, 1.0f, 0.35f);
	const double Cam[3] = { -8000.0 * kPcField, 0.0, 10.0 * kPcField };
	G.Frame(Cam, 0.0, 0xFFFFFFFFu);
	const FNebGridLayout& L = G.Planner.Layout();
	const u32 Nl = u32(L.Levels.size());
	// Segments in each level's box: 0.3 to 38 cells, random directions, one in 7 along an axis and
	// one in 7 along the diagonal (the faces crossed at once); the reference the segment's mean of
	// the level's reads at many points (midpoints of 64 a cell).
	const int Ns = 1400;
	size_t Walked = 0, Outside = 0, OutsideBad = 0, Skipped = 0, SkipReads = 0, ReadOver = 0, Zero = 0;
	std::vector<double> Errs, Adds;
	double MaxReadsPerCell = 0.0;
	auto Metric = [](const H::float4& A, const H::float4& B, double Peak)
	{
		const double Sx = std::max(Peak, 1e-6);   // (a thousandth of a bright nebula's)
		return std::max(std::max(std::abs(double(A.x) - double(B.x)) / Sx, std::abs(double(A.y) - double(B.y)) / Sx),
			std::max(std::abs(double(A.z) - double(B.z)), std::abs(double(A.w) - double(B.w))) / 8.0);
	};
	for (int I = 0; I < Ns; ++I)
	{
		const u32 Lv = u32(I) % std::min(Nl, 6u);
		const float Cl = float(L.Levels[Lv].Cell);
		// Two in three start in a nebula (the box is mostly without).
		H::float3 P0 = PointIn(L, G.Headers, Lv, 0xA1Cu, u32(I) * 256u);
		for (u32 Tr = 1u; I % 3 != 2 && Tr < 256u; ++Tr)
		{
			H::float4 V(0, 0, 1, 1);
			if (H::Uni_NebGridFetch(Lv, P0, V) && V.x > 0.0f) { break; }
			P0 = PointIn(L, G.Headers, Lv, 0xA1Cu, u32(I) * 256u + Tr);
		}
		H::float3 Dir;
		if (I % 7 == 0) { const int A = (I / 7) % 3; Dir = H::float3(A == 0 ? 1.0f : 0.0f, A == 1 ? -1.0f : 0.0f, A == 2 ? 1.0f : 0.0f); }
		else if (I % 7 == 1) { Dir = H::float3(0.57735027f, -0.57735027f, 0.57735027f); }
		else
		{
			const double Ct = 2.0 * H::Uni_DrawUnit(0xA1Du, 1u, u32(2 * I)) - 1.0, Ph = 6.2831853 * H::Uni_DrawUnit(0xA1Du, 1u, u32(2 * I + 1));
			const double St = std::sqrt(std::max(0.0, 1.0 - Ct * Ct));
			Dir = H::float3(float(St * std::cos(Ph)), float(St * std::sin(Ph)), float(Ct));
		}
		const float Len = Cl * 0.3f * float(std::exp2(7.0 * H::Uni_DrawUnit(0xA1Eu, 1u, u32(I))));
		const H::float3 P1 = P0 + Dir * Len;
		H::float4 Dummy(0, 0, 1, 1);
		const bool bIn = H::Uni_NebGridFetch(Lv, P0, Dummy) && H::Uni_NebGridFetch(Lv, P1, Dummy);
		const unsigned long long R0 = H::GNebGridReads.load();
		bool bOk = false;
		const H::float4 W = H::Uni_NebGridWalk(Lv, P0, P1, 32.0f, bOk);
		const double Rd = double(H::GNebGridReads.load() - R0);
		if (!bIn) { ++Outside; OutsideBad += bOk ? 1 : 0; continue; }   // (an end outside the box: the walk must fail)
		if (!bOk) { ++OutsideBad; continue; }
		++Walked;
		const H::float3 Dl = (P1 - P0) * (1.0f / Cl);
		const double L1 = std::abs(double(Dl.x)) + std::abs(double(Dl.y)) + std::abs(double(Dl.z));
		const int M = std::max(64, int(std::ceil(64.0 * double(Len / Cl))));
		H::float4 Ref(0, 0, 0, 0);
		double Peak = 0.0;
		bool bAllEmpty = true;
		for (int J = 0; J < M; ++J)
		{
			const H::float3 Pt = P0 + (P1 - P0) * ((float(J) + 0.5f) / float(M));
			H::float4 V(0, 0, 1, 1);
			H::Uni_NebGridFetch(Lv, Pt, V);
			Ref = Ref + V * (1.0f / float(M));
			Peak = std::max(Peak, double(V.x));
			const H::float3 U = Pt * (1.0f / Cl);
			if (H::Uni_NebGridCellEntry(Lv, int(std::floor(U.x)), int(std::floor(U.y)), int(std::floor(U.z))) != H::UNI_NEB_GRID_EMPTY) { bAllEmpty = false; }
		}
		if (bAllEmpty) { ++Skipped; SkipReads += Rd > 0.0 ? 1 : 0; }
		if (Peak <= 0.0) { ++Zero; }
		Errs.push_back(Metric(W, Ref, Peak));
		if (std::getenv("UNI_WALK_DEBUG") && Errs.back() > 1e-2) { std::printf("    dbg %d lv %u len %.2f cells L1 %.2f peak %.3e W (%.3e %.3e %.4f %.4f) R (%.3e %.3e %.4f %.4f) reads %.0f\n", I, Lv, double(Len / Cl), L1, Peak, W.x, W.y, W.z, W.w, Ref.x, Ref.y, Ref.z, Ref.w, Rd); }
		// The walk's reads: at most two a cell crossed (its far face and its middle) and one a brick
		// entered (plus the ends).
		ReadOver += Rd > 2.0 * std::ceil(L1) + std::ceil(L1 / 8.0) + 6.0 ? 1 : 0;
		MaxReadsPerCell = std::max(MaxReadsPerCell, Rd / std::max(L1, 1.0));
		// Additive: cut anywhere, the two pieces' walks weighted by their lengths are the whole's
		// (the march's step boundaries do not matter).
		const float A = 0.1f + 0.8f * float(H::Uni_DrawUnit(0xA1Fu, 1u, u32(I)));
		const H::float3 Pm = P0 + (P1 - P0) * A;
		bool bA = false, bB = false;
		const H::float4 Wa = H::Uni_NebGridWalk(Lv, P0, Pm, 32.0f, bA);
		const H::float4 Wb = H::Uni_NebGridWalk(Lv, Pm, P1, 32.0f, bB);
		if (bA && bB) { Adds.push_back(Metric(Wa * A + Wb * (1.0f - A), W, Peak)); }
	}
	UniVerify::FNebGridEmu::Unbind();
	auto Pct = [](std::vector<double> V, double Q) { std::sort(V.begin(), V.end()); return V.empty() ? 0.0 : V[size_t(Q * double(V.size() - 1))]; };
	const double E50 = Pct(Errs, 0.5), E99 = Pct(Errs, 0.99), Emax = Pct(Errs, 1.0), A99 = Pct(Adds, 0.99), Amax = Pct(Adds, 1.0);
	const bool bPass = Walked > size_t(Ns) / 2 && OutsideBad == 0 && SkipReads == 0 && Skipped > 20 && ReadOver == 0 && E99 <= 1e-3 && Emax <= 1e-2 && Amax <= 1e-2 && A99 <= 1e-3;
	Uni::Check(2, "T2.vol.nebgrid_walk", "D-255 the cell walk (Uni_NebGridWalk; the grid filled about the solar camera): over 1400 segments of 0.3 to 38 cells in six levels' boxes (two in three starting in a nebula; along axes and the diagonal too) its average is the exact integral of the level's trilinear along the segment (Simpson's rule a cell; against the reads' mean at 64 points a cell: 99% within 1e-3 of the segment's peak emission (at least 1e-6), all within 1e-2; the ratios to 1/8 of that), a segment cut anywhere the two walks' weighted sum the whole's (the same), at most a read a cell crossed and one a brick entered, none through bricks without nebulae, and a segment leaving the box fails (the next level reads)")
		.Metric("err_p50", E50).Metric("err_p99", E99).Metric("err_max", Emax).Metric("additive_p99", A99).Metric("reads_per_cell_max", MaxReadsPerCell)
		.Pass(bPass, Uni::Fmt("%zu walked (%zu emissionless), %zu leave the box (%zu wrong); error median %.2e, 99th %.2e, max %.2e; cut anywhere 99th %.2e, max %.2e; %zu segments wholly in empty bricks (%zu read the pool); %zu over the read bound, at most %.2f reads a cell",
			Walked, Zero, Outside, OutsideBad, E50, E99, Emax, A99, Amax, Skipped, SkipReads, ReadOver, MaxReadsPerCell));
}

UNI_TEST(volume, nebgrid_grain)
{
	const UniVerify::FGalShapeOff ShapeOff;   // D-262 (B-041)
	FNgScene S;
	if (!S.bTex || !S.Mw) { Uni::Check(2, "T2.vol.nebgrid_grain", "D-252").Pass(false, "textures not loaded"); return; }
	const H::FUniFieldParams& P = S.P;
	const H::FUniGalaxyConst& K = S.K;
	// The brightest disc nebula (as T2.vol.nebula_steps).
	double Hx = 0.0, Hy = 0.0, Hb = -1.0;
	for (int I = 0; I < 60000; ++I)
	{
		const double R = 0.3 + 0.4 * H::Uni_DrawUnit(0xF374u, 2u, u32(2 * I)), Ph = 6.2831853 * H::Uni_DrawUnit(0xF374u, 2u, u32(2 * I + 1));
		const H::float3 Pt{ float(R * std::cos(Ph)), float(R * std::sin(Ph)), 0.0f };
		H::float4 Cc(0, 0, 0, 0), Nv(0, 0, 0, 0);
		const float Rho = H::UniField_GalaxyFilteredCKN(Pt, P, K, 1e-5f, Cc, Nv);
		const double Sc = double(H::Uni_GalaxyNebulaX(Nv, P)) * double(H::Uni_GalaxyNebulaDensity(Nv, P, 1e-5f)) / std::max(double(Rho), 1e-30);
		if (Sc > Hb) { Hb = Sc; Hx = Pt.x; Hy = Pt.y; }
	}
	FUniverseModel Mo(S.D);
	FRefGenerator G(Mo);
	FVolumeBuilder VB(Mo, G);
	double Radius = 0.0;
	u32 GridParam = 0u;
	double CamField[3] = { 0, 0, 0 };
	auto CamAt = [](const FVec3d& Pc) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, Pc); return C; };
	auto Build = [&](const FCamera& Cm, float NebStep)
	{
		FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cm.Coord, FVec3f{});
		FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f; Sv.NebulaFineReachCm = float(7.0 * Units::Kpc); Sv.NebulaStepRatio = NebStep;
		ApplyNebulaTune(NebulaTune(Sv));
		FVolumeScene Sc = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), Sv).Subset(false);
		for (const FCarrierGPU& Cg : Sc.Carriers)
		{
			const double Rl = std::sqrt(double(Cg.CamLocal[0]) * Cg.CamLocal[0] + double(Cg.CamLocal[1]) * Cg.CamLocal[1] + double(Cg.CamLocal[2]) * Cg.CamLocal[2]);
			if (Cg.FieldId == H::UNI_FIELD_GALAXY && Rl < 1.0) { Radius = 1.0 / double(Cg.InvRadius); GridParam = Cg.ParamIndex; }
		}
		for (int A = 0; A < 3; ++A) { CamField[A] = Sc.Home.CamField[A]; }
		return Sc;
	};
	{ FCamera Cm; Cm.Coord = CamAt({ 8.0 * Units::Kpc, 0.0, 0.0 }); Cm.Width = 4; Cm.Height = 4; Cm.LookAlong({ 0.0, 1.0, 0.0 }); Build(Cm, 0.0f); }
	const int W = 128, Ht = 72;
	const double FovX = 2.0 * std::atan(0.5 * W * 1.6e-3);
	auto Reference = [&](const FCamera& Cm)
	{
		FVolumeScene Sf = Build(Cm, 0.0f);
		Sf.View.MarchFlags |= 2u; Sf.View.MaxSteps = 16384u;
		const auto Cb = Sf.CarrierBuf(); const auto Pb = Sf.ParamBuf(); const auto Lb = Sf.LutBuf();
		const auto V = ToHlsl(Sf.View);
		std::vector<H::float4> Ref(size_t(W) * Ht);
		std::atomic<int> Row{ 0 };
		auto Work = [&]() { for (int Y = Row++; Y < Ht; Y = Row++) for (int X = 0; X < W; ++X) { bool bS = false; const H::FUniVolumeResult R = MarchPixel(Cb, Pb, Lb, V, Cm, W, Ht, X, Y, 3.0e38f, bS); Ref[size_t(Y) * W + X] = H::float4(R.Radiance.x, R.Radiance.y, R.Radiance.z, 0.0f); } };
		std::thread T1(Work); Work(); T1.join();
		return Ref;
	};
	UniVerify::FBlueNoiseBind Bn(true);
	struct FRun { std::vector<H::float4> Vol; double Flick = 0.0; };
	auto Run = [&](const FCamera& Cm, bool bGrid, double ReachKpc = 8.0)
	{
		FVolumeScene Sc = Build(Cm, 0.08f);
		Sc.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
		UniVerify::FNebGridEmu Emu;
		if (bGrid)
		{
			const auto Pb = Sc.ParamBuf();
			const H::FUniFieldParams Gp = Pb[GridParam];
			FNebGridConfig Cfg;
			Cfg.Cells = 128u;
			Cfg.Cell0 = 4.0 * Units::Pc / Radius;
			Cfg.LayerHalf = 3.0 * UNI_NEBULA_HEIGHT * Gp.A.y + 50.0 * Units::Pc / Radius;
			Cfg.KnotTexel = H::Uni_GalaxyNebulaKnotTexel(Gp);
			Cfg.Levels = NebGridLevelsFor(ReachKpc * Units::Kpc / Radius, Cfg.Cells, Cfg.Cell0);
			Emu.Configure(Cfg, 1u, Gp, Sc.View.NebulaFineGain, Sc.View.NebulaDark);
			float Zc = 0.0f, Fl = 1.0f;
			H::Uni_GalaxyLayerMid(H::float3(float(CamField[0]), float(CamField[1]), float(CamField[2])), Gp, H::Uni_GalaxyConstOf(Gp).Hp, Zc, Fl);
			Emu.Frame(CamField, double(Zc), 0xFFFFFFFFu);
			Emu.ToView(Sc.View, GridParam, 32.0f);
		}
		FTemporalVolume Tv; Tv.MaxSamples = 16.0f; Tv.bCloud = true; Tv.Interleave = 4;
		FRun R; std::vector<H::float4> Last; size_t Nf = 0;
		for (int F = 0; F < 64; ++F)
		{
			Tv.Step(Sc, Cm, FV3{ 0, 0, 0 }, true, true, 2);
			if (F >= 48)
			{
				if (!Last.empty()) { for (size_t I = 0; I < Last.size(); ++I) { R.Flick += std::abs(Lum(Tv.Vol[I]) - Lum(Last[I])); } ++Nf; }
				Last = Tv.Vol;
			}
		}
		R.Flick /= double(std::max<size_t>(Nf, 1)) * double(Last.size());
		R.Vol = Tv.Vol;
		if (bGrid) { UniVerify::FNebGridEmu::Unbind(); }
		return R;
	};
	auto Mean = [&](const std::vector<H::float4>& A) { double M = 0.0; for (const H::float4& V : A) { M += Lum(V) / double(A.size()); } return M; };
	auto Err = [&](const std::vector<H::float4>& A, const std::vector<H::float4>& Ref)
	{
		const double M = Mean(Ref);
		double E = 0.0; for (size_t I = 0; I < A.size(); ++I) { E += std::abs(Lum(A[I]) - Lum(Ref[I])) / std::max(M, 1e-300) / double(A.size()); }
		return E;
	};
	const double Nxc = Hx * Radius, Nyc = Hy * Radius;
	const double Rn = std::sqrt(Nxc * Nxc + Nyc * Nyc);
	const double Ux = -Nxc / Rn, Uy = -Nyc / Rn, Vx = -Uy, Vy = Ux;
	const double Dist[3] = { 250.0, 1000.0, 3000.0 };
	bool bPass = true;
	std::string Msg;
	double FlickX[3] = { 0, 0, 0 }, ErrX[3] = { 0, 0, 0 };
	for (int I = 0; I < 3; ++I)
	{
		const double Dd = Dist[I] * Units::Pc;
		const FVec3d Cp{ Nxc + (0.6 * Ux + 0.8 * Vx) * Dd, Nyc + (0.6 * Uy + 0.8 * Vy) * Dd, 0.02 * Dd };
		FCamera Cm; Cm.Coord = CamAt(Cp); Cm.Width = W; Cm.Height = Ht; Cm.FovX = FovX; Cm.LookAlong({ Nxc - Cp.X, Nyc - Cp.Y, -Cp.Z });
		const std::vector<H::float4> Ref = Reference(Cm);
		const double M = Mean(Ref);
		const FRun A = Run(Cm, false), B = Run(Cm, true);
		const double Ea = Err(A.Vol, Ref), Eb = Err(B.Vol, Ref);
		const double Light = Mean(B.Vol) / std::max(M, 1e-300);
		FlickX[I] = B.Flick / std::max(A.Flick, 1e-300);
		ErrX[I] = Eb / std::max(Ea, 1e-300);
		const bool bOk = FlickX[I] <= 0.5 && std::abs(Light - 1.0) <= 0.10 && ErrX[I] <= 1.8;   // D-262: light 8% -> 10% (D-256's Milky Way: x0.916 at 3 kpc)
		bPass = bPass && bOk;
		Msg += Uni::Fmt("%s%.0f pc: frame change %.3f%% -> %.3f%% (x%.2f), error %.2f%% -> %.2f%% (x%.2f), light x%.3f%s", I ? "; " : "", Dist[I],
			100.0 * A.Flick / M, 100.0 * B.Flick / M, FlickX[I], 100.0 * Ea, 100.0 * Eb, ErrX[I], Light, bOk ? "" : " FAIL");
	}
	// D-253: 6 kpc away, past D-251's detail reach (the field's knots blended to their mean there):
	// the grid to 32 kpc keeps the nebula (its light over the image's at the view's centre), its
	// frame change at most D-251's x2.
	double ContrastX = 0.0;
	{
		const double Dd = 6000.0 * Units::Pc;
		const FVec3d Cp{ Nxc + (0.6 * Ux + 0.8 * Vx) * Dd, Nyc + (0.6 * Uy + 0.8 * Vy) * Dd, 0.02 * Dd };
		FCamera Cm; Cm.Coord = CamAt(Cp); Cm.Width = W; Cm.Height = Ht; Cm.FovX = FovX; Cm.LookAlong({ Nxc - Cp.X, Nyc - Cp.Y, -Cp.Z });
		const FRun A = Run(Cm, false), B = Run(Cm, true, 32.0);
		// The nebula at the view's centre: its middle (half across, a third up) over the whole image.
		auto Excess = [&](const std::vector<H::float4>& Im)
		{
			double Sc = 0.0; size_t Nc = 0;
			for (int Y = Ht / 3; Y < 2 * Ht / 3; ++Y) for (int X = W / 4; X < 3 * W / 4; ++X) { Sc += Lum(Im[size_t(Y) * W + X]); ++Nc; }
			return Sc / double(std::max<size_t>(Nc, 1)) / std::max(Mean(Im), 1e-300) - 1.0;
		};
		const double Ea = Excess(A.Vol), Eb = Excess(B.Vol);
		ContrastX = Eb;
		const bool bOk = Eb >= 1.5 * std::max(Ea, 0.0) && Eb - Ea >= 0.05 && B.Flick <= 2.0 * A.Flick;
		bPass = bPass && bOk;
		Msg += Uni::Fmt("; 6 kpc (the grid to 32 kpc): the nebula's excess at the view's centre %.1f%% -> %.1f%% (kept), frame change %.3f%% -> %.3f%%%s", 100.0 * Ea, 100.0 * Eb, 100.0 * A.Flick / Mean(A.Vol), 100.0 * B.Flick / Mean(A.Vol), bOk ? "" : " FAIL");
	}
	Uni::Check(2, "T2.vol.nebgrid_grain", "D-252 the grain: over the engine's path (cloud resolve, blue noise, 4 x 4, 64 still frames) at 1.6 mrad pixels, 250 pc, 1 kpc and 3 kpc from the Milky Way's brightest disc nebula, the nebula grid (128 cells, D-251's steps) at most half D-251's frame-to-frame change, its light within 10% of the converged reference's (the cells spread the brightest nebula's peak past the view; 8% before D-256's Milky Way); D-262: with D-258's galaxy shaping off (B-041), its error at most 1.8x D-251's (softer: the cell); D-253: 6 kpc away with the grid to 32 kpc the nebula kept (its light at the view's centre over the image's at least 1.5x D-251's excess and 5 points more: D-251's knots have blended to their mean) at most 2x its frame change")
		.Metric("excess_6000", ContrastX).Metric("flick_x_250", FlickX[0]).Metric("flick_x_1000", FlickX[1]).Metric("flick_x_3000", FlickX[2]).Metric("err_x_1000", ErrX[1])
		.Pass(bPass, Msg);
}

UNI_TEST(volume, nebgrid_outside)
{
	// D-254: the grid from outside a galaxy (it follows the nearest spiral whose disc is within its
	// reach, 64 kpc): where it switches on its look must be the field's, else the nebulae pop in.
	// The Milky Way seen from 40 and 80 kpc from its centre (10 kpc above its plane) at 1.6 mrad
	// pixels, the grid (128 cells to 64 kpc) against the field (D-251): the light within 3%, the
	// per-pixel difference small (the coarse levels' cells and the field's distance level of detail
	// both show the nebulae's mean there).
	FNgScene S;
	if (!S.bTex || !S.Mw) { Uni::Check(2, "T2.vol.nebgrid_outside", "D-254").Pass(false, "textures not loaded"); return; }
	FUniverseModel Mo(S.D);
	FRefGenerator G(Mo);
	FVolumeBuilder VB(Mo, G);
	auto CamAt = [](const FVec3d& Pc) { FCoord C = FCoord::Origin(); FVec3f F; AddDisplacement(C, F, Pc); return C; };
	const int W = 128, Ht = 72;
	const double FovX = 2.0 * std::atan(0.5 * W * 1.6e-3);
	UniVerify::FBlueNoiseBind Bn(true);
	bool bPass = true;
	std::string Msg;
	double Worst = 0.0;
	const double Dist[2] = { 40.0, 80.0 };
	for (int I = 0; I < 2; ++I)
	{
		const FVec3d Cp{ -Dist[I] * Units::Kpc, 0.0, 10.0 * Units::Kpc };
		FCamera Cm; Cm.Coord = CamAt(Cp); Cm.Width = W; Cm.Height = Ht; Cm.FovX = FovX; Cm.LookAlong({ -Cp.X, -Cp.Y, -Cp.Z });
		FRefPipeline Pp(G); Pp.bVelocityLod = false; Pp.Update(Cm.Coord, FVec3f{});
		FVolumeSettings Sv; Sv.NebulaGain = 1.0f; Sv.NebulaReflect = 6.0f; Sv.NebulaFineReachCm = float(7.0 * Units::Kpc); Sv.NebulaStepRatio = 0.08f;
		ApplyNebulaTune(NebulaTune(Sv));
		const FVolumeScene Base = VB.Build(Pp, Cm.Coord, FVec3f{}, Cm.View(), Sv).Subset(false);
		const FCarrierGPU* Gal = nullptr;
		for (const FCarrierGPU& Cg : Base.Carriers) { if (Cg.FieldId == H::UNI_FIELD_GALAXY && (!Gal || Cg.Weight > Gal->Weight)) { Gal = &Cg; } }
		if (!Gal) { bPass = false; Msg += Uni::Fmt("%s%.0f kpc: no galaxy carrier FAIL", I ? "; " : "", Dist[I]); continue; }
		const double Radius = 1.0 / double(Gal->InvRadius);
		const H::FUniFieldParams Gp = Base.ParamBuf()[Gal->ParamIndex];
		auto Frames = [&](bool bGrid)
		{
			FVolumeScene Sc = Base;
			Sc.View.MarchFlags |= H::UNI_VOL_JITTER_FLAG;
			UniVerify::FNebGridEmu Emu;
			if (bGrid)
			{
				FNebGridConfig Cfg;
				Cfg.Cells = 128u;
				Cfg.Cell0 = 4.0 * Units::Pc / Radius;
				Cfg.LayerHalf = 3.0 * UNI_NEBULA_HEIGHT * Gp.A.y + 50.0 * Units::Pc / Radius;
				Cfg.KnotTexel = H::Uni_GalaxyNebulaKnotTexel(Gp);
				Cfg.Levels = NebGridLevelsFor(64.0 * Units::Kpc / Radius, Cfg.Cells, Cfg.Cell0);
				Emu.Configure(Cfg, 1u, Gp, Sc.View.NebulaFineGain, Sc.View.NebulaDark);
				const double Cf[3] = { double(Gal->CamLocal[0]), double(Gal->CamLocal[1]), double(Gal->CamLocal[2]) };
				float Zc = 0.0f, Fl = 1.0f;
				H::Uni_GalaxyLayerMid(H::float3(Gal->CamLocal[0], Gal->CamLocal[1], Gal->CamLocal[2]), Gp, H::Uni_GalaxyConstOf(Gp).Hp, Zc, Fl);
				Emu.Frame(Cf, double(Zc), 0xFFFFFFFFu);
				Emu.ToView(Sc.View, Gal->ParamIndex, 32.0f);
			}
			FTemporalVolume Tv; Tv.MaxSamples = 16.0f; Tv.bCloud = true; Tv.Interleave = 4;
			for (int F = 0; F < 48; ++F) { Tv.Step(Sc, Cm, FV3{ 0, 0, 0 }, true, true, 2); }
			if (bGrid) { UniVerify::FNebGridEmu::Unbind(); }
			return Tv.Vol;
		};
		const std::vector<H::float4> A = Frames(false), B = Frames(true);
		double Ma = 0.0, Mb = 0.0, Dd = 0.0;
		for (size_t K = 0; K < A.size(); ++K) { Ma += Lum(A[K]); Mb += Lum(B[K]); Dd += std::abs(Lum(A[K]) - Lum(B[K])); }
		const double Light = Mb / std::max(Ma, 1e-300), Diff = Dd / std::max(Ma, 1e-300);
		Worst = std::max(Worst, Diff);
		const bool bOk = std::abs(Light - 1.0) <= 0.03 && Diff <= 0.05;
		bPass = bPass && bOk;
		Msg += Uni::Fmt("%s%.0f kpc (the disc %.0f kpc off): light x%.3f, per pixel %.2f%%%s", I ? "; " : "", Dist[I], (std::sqrt(double(Gal->CamLocal[0]) * Gal->CamLocal[0] + double(Gal->CamLocal[1]) * Gal->CamLocal[1] + double(Gal->CamLocal[2]) * Gal->CamLocal[2]) - 1.0) * Radius / Units::Kpc, Light, 100.0 * Diff, bOk ? "" : " FAIL");
	}
	Uni::Check(2, "T2.vol.nebgrid_outside", "D-254 the grid from outside a galaxy: the Milky Way 40 and 80 kpc from its centre (10 kpc above its plane) at 1.6 mrad pixels, the grid (128 cells to 64 kpc) against the field: light within 3%, per pixel within 5% (where it switches on it looks like the field: no pop)")
		.Metric("worst_pixel_diff", Worst).Pass(bPass, Msg);
}

