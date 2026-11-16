// UniNebGrid.h
// D-252: the nebula grid's layout and its per-frame plan (engine-agnostic). The grid itself (its
// lattices, bricks, table and pool, the reads and the bake) is THE NEBULA GRID in
// Shaders/Private/Core/UniVolume.ush; the renderer runs this planner on the render thread and
// executes its operations as passes (UniVolume.usf pass 7), the checks run the same operations on
// the CPU (Tools/UniVerify/src/UniNebGridRef.h).
//
// LAYOUT. Level K's cell is Cell0 x 2^K (field units of the camera's spiral). A level is a box of
// bricks (UNI_NEB_GRID_BRICK cells a side): Cells / 8 across (x, y) and as many up as cover the
// gas layer (LayerHalf either side of its mid-plane) or the box across, whichever is fewer. Its
// table holds one entry a brick, toroidally (a world brick's entry is its index modulo the box),
// after the levels before it; its pool slots are a range of the pool's, sized from the occupancy
// expected at its brick size (NebGridOccupancy) x PoolScale (a level whose bricks are too coarse
// to classify keeps every brick: a slot each).
//
// PLAN. Each frame, per level: the box's first brick from the camera (x, y: the camera's brick
// less half the box; z: the layer's mid-plane where the box covers the layer, else the camera
// clamped into the layer), moved only when the camera is more than NebGridHysteresis bricks off
// its centre (no churn at a brick boundary). A move by the box or more resets the level; a smaller
// one releases every entry whose world brick changed (the march then reads the next level there)
// and queues it. Up to the budget of queued entries are processed (classified, allocated, baked),
// the coarsest level first (the finer fall back to them). The headers carry the levels' boxes to
// the march (UNI_NEB_GRID_HEADER).
#pragma once

#include "UniShaderCores.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace UniCore
{
	namespace HN = UniHlsl;

	/** D-252: one brick to update (FUniNebGridOp): its world brick and level << 24 | its entry. */
	// GPU-MIRROR
	struct FNebGridOpGPU
	{
		std::int32_t Wx;
		std::int32_t Wy;
		std::int32_t Wz;
		u32 LevelEntry;
	};
	static_assert(sizeof(FNebGridOpGPU) == 16, "FNebGridOpGPU must match FUniNebGridOp (UniVolume.ush)");
	static_assert(sizeof(HN::FUniNebGridOp) == 16, "shim FUniNebGridOp layout");

	/** D-252: a level's bricks up at most (128 cells: the layer is thin; the next level covers it). */
	inline constexpr u32 NebGridMaxBricksUp = 16u;

	/** D-252: the camera may drift this many bricks off a level's centre before its box moves. */
	inline constexpr int NebGridHysteresis = 1;

	/** D-252: the grid's shape. */
	struct FNebGridConfig
	{
		u32 Cells = 128;          // lattice cells across a level (x, y); a multiple of the brick (128 or 256)
		u32 Levels = 6;           // at most UNI_NEB_GRID_MAX_LEVELS
		double Cell0 = 0.0;       // field units: level 0's cell
		double LayerHalf = 0.0;   // field units: the gas layer's half thickness the levels cover
		double KnotTexel = 0.0;   // field units: the knot read's texel (classify's spacing, the occupancy)
		double PoolScale = 1.25;  // slots over the occupancy expected (past it: the next level reads)
		u32 Samples = 1u;         // the bake's samples a lattice point (1, or 8: 2 x 2 x 2 strata)

		bool operator==(const FNebGridConfig& O) const
		{
			return Cells == O.Cells && Levels == O.Levels && Cell0 == O.Cell0 && LayerHalf == O.LayerHalf && KnotTexel == O.KnotTexel && PoolScale == O.PoolScale && Samples == O.Samples;
		}
		bool operator!=(const FNebGridConfig& O) const { return !(*this == O); }
	};

	/** D-252: the share of bricks classify keeps (UNI_NEB_GRID_SLACK), by the brick's size over the
	 *  knot texel: measured over the Milky Way's gas layer about the solar circle at the default
	 *  prevalence (experiments/nebula_grid_d252). Interpolated in log2; 1 past the table. */
	inline double NebGridOccupancy(double InBrickOverKnot)
	{
		static const double X[6] = { 0.256, 0.512, 1.024, 2.048, 4.096, 8.192 };
		static const double Y[6] = { 0.32, 0.28, 0.49, 0.76, 0.95, 1.0 };
		if (!(InBrickOverKnot > X[0])) { return Y[0]; }
		for (int I = 1; I < 6; ++I)
		{
			if (InBrickOverKnot <= X[I])
			{
				const double F = std::log2(InBrickOverKnot / X[I - 1]) / std::log2(X[I] / X[I - 1]);
				return Y[I - 1] + (Y[I] - Y[I - 1]) * F;
			}
		}
		return 1.0;
	}

	struct FNebGridLevelLayout
	{
		double Cell = 0.0;     // field units
		u32 Bx = 0, Bz = 0;    // bricks across (x, y) and up
		u32 TableBase = 0, Entries = 0;
		u32 SlotBase = 0, SlotCount = 0;
		u32 ClassifyN = 0;     // classify's samples a side (0: every brick kept, unread)
	};

	struct FNebGridLayout
	{
		std::vector<FNebGridLevelLayout> Levels;
		u32 TableSize = 0;
		u32 Slots = 0;
		u32 SlotsX = 0, SlotsY = 0, SlotsZ = 0;
		u32 Samples = 1u;

		u32 TexX() const { return SlotsX * HN::UNI_NEB_GRID_SLOT; }
		u32 TexY() const { return SlotsY * HN::UNI_NEB_GRID_SLOT; }
		u32 TexZ() const { return SlotsZ * HN::UNI_NEB_GRID_SLOT; }
		/** The pool's bytes (RGBA16F). */
		u64 PoolBytes() const { return u64(TexX()) * u64(TexY()) * u64(TexZ()) * 8ull; }
		u64 TableBytes() const { return u64(TableSize) * 4ull; }
		bool operator==(const FNebGridLayout& O) const
		{
			if (Levels.size() != O.Levels.size() || TableSize != O.TableSize || Slots != O.Slots || SlotsX != O.SlotsX || SlotsY != O.SlotsY || SlotsZ != O.SlotsZ) { return false; }
			for (size_t K = 0; K < Levels.size(); ++K)
			{
				const FNebGridLevelLayout& A = Levels[K];
				const FNebGridLevelLayout& B = O.Levels[K];
				if (A.Cell != B.Cell || A.Bx != B.Bx || A.Bz != B.Bz || A.TableBase != B.TableBase || A.SlotBase != B.SlotBase || A.SlotCount != B.SlotCount || A.ClassifyN != B.ClassifyN) { return false; }
			}
			return true;
		}
	};

	/** D-252: the levels' bricks, tables and pool slots for a shape (see the file comment). */
	inline FNebGridLayout NebGridLayout(const FNebGridConfig& C)
	{
		FNebGridLayout L;
		L.Samples = C.Samples == 8u ? 8u : 1u;
		const u32 Br = HN::UNI_NEB_GRID_BRICK;
		const u32 Bx = std::max(C.Cells / Br, 2u);
		const u32 Nl = std::min(std::max(C.Levels, 1u), HN::UNI_NEB_GRID_MAX_LEVELS);
		u32 Table = 0, Slot = 0;
		for (u32 K = 0; K < Nl; ++K)
		{
			FNebGridLevelLayout V;
			V.Cell = C.Cell0 * std::exp2(double(K));
			V.Bx = Bx;
			const double Up = 2.0 * C.LayerHalf / std::max(V.Cell, 1.0e-30) + 2.0;   // cells up the layer, a cell's margin either side
			V.Bz = std::min(std::min(Bx, NebGridMaxBricksUp), std::max(u32(std::ceil(Up / double(Br))), 1u));
			V.TableBase = Table;
			V.Entries = V.Bx * V.Bx * V.Bz;
			Table += V.Entries;
			const double Span = 10.0 * V.Cell;   // classify's region: a brick's points' bakes, a cell either side
			const u32 N = u32(std::ceil(Span / std::max(0.5 * C.KnotTexel, 1.0e-30))) + 1u;
			V.ClassifyN = (C.KnotTexel > 0.0 && N <= HN::UNI_NEB_GRID_CLASSIFY_MAX) ? std::max(N, 2u) : 0u;
			const double Occ = V.ClassifyN > 0u ? NebGridOccupancy(double(Br) * V.Cell / std::max(C.KnotTexel, 1.0e-30)) : 1.0;
			V.SlotBase = Slot;
			V.SlotCount = V.ClassifyN > 0u ? std::min(V.Entries, std::max(64u, u32(std::ceil(double(V.Entries) * Occ * std::max(C.PoolScale, 0.1))))) : V.Entries;
			Slot += V.SlotCount;
			L.Levels.push_back(V);
		}
		L.TableSize = Table;
		L.Slots = Slot;
		L.SlotsX = std::max(u32(std::ceil(std::cbrt(double(Slot)))), 1u);
		L.SlotsY = L.SlotsX;
		L.SlotsZ = std::max((Slot + L.SlotsX * L.SlotsY - 1u) / (L.SlotsX * L.SlotsY), 1u);
		return L;
	}

	/** D-252: levels to cover InCoverField (field units) from the camera with InCells cells across
	 *  from InCell0 (each level's half box doubling), at most UNI_NEB_GRID_MAX_LEVELS. */
	inline u32 NebGridLevelsFor(double InCoverField, u32 InCells, double InCell0)
	{
		const double Half0 = 0.5 * double(InCells) * InCell0;
		if (!(Half0 > 0.0)) { return 1u; }
		const double N = std::ceil(std::log2(std::max(InCoverField / Half0, 1.0))) + 1.0;
		return u32(std::min(std::max(N, 1.0), double(HN::UNI_NEB_GRID_MAX_LEVELS)));
	}

	/** D-252: the per-frame plan (see the file comment). */
	class FNebGridPlanner
	{
	public:
		struct FFrameOps
		{
			u32 ResetMask = 0;                     // levels to reset first (every entry unbaked, every slot free)
			std::vector<FNebGridOpGPU> Release;    // then: entries whose world brick changed (slot freed, unbaked)
			std::vector<FNebGridOpGPU> Process;    // then: entries to classify, allocate and bake
			std::vector<HN::float4> Headers;       // the march's (UNI_NEB_GRID_HEADER)
		};

		/** A shape and a key (the galaxy and everything its bake reads: a change rebakes). Returns
		 *  whether the layout changed (the resources must be made again); either change resets. */
		bool Configure(const FNebGridConfig& InConfig, u64 InKey)
		{
			const FNebGridLayout L = NebGridLayout(InConfig);
			const bool bLayout = !bConfigured || !(L == Lay);
			if (bLayout || InKey != Key || InConfig != Cfg)
			{
				Cfg = InConfig;
				Lay = L;
				Key = InKey;
				bConfigured = true;
				States.assign(Lay.Levels.size(), FLevelState{});
				for (size_t K = 0; K < Lay.Levels.size(); ++K) { States[K].bPending.assign(Lay.Levels[K].Entries, 0u); }
				++Resets;
			}
			return bLayout;
		}

		bool IsConfigured() const { return bConfigured; }
		const FNebGridLayout& Layout() const { return Lay; }
		const FNebGridConfig& Config() const { return Cfg; }
		u64 KeyOf() const { return Key; }

		/** The camera at InCam (field units) with the gas layer's mid-plane at height InMidZ there:
		 *  the frame's operations, InBudget entries processed at most. */
		void Update(const double InCam[3], double InMidZ, u32 InBudget, FFrameOps& Out)
		{
			Out.ResetMask = 0u;
			Out.Release.clear();
			Out.Process.clear();
			Out.Headers.clear();
			if (!bConfigured) { return; }
			const int Nl = int(Lay.Levels.size());
			for (int K = 0; K < Nl; ++K) { PlaceLevel(K, InCam, InMidZ, Out); }
			u32 Left = InBudget;
			for (int K = Nl - 1; K >= 0 && Left > 0u; --K)
			{
				FLevelState& S = States[size_t(K)];
				const FNebGridLevelLayout& V = Lay.Levels[size_t(K)];
				while (Left > 0u && !S.Queue.empty())
				{
					const u32 E = S.Queue.front();
					S.Queue.pop_front();
					if (!S.bPending[E]) { continue; }
					S.bPending[E] = 0u;
					Out.Process.push_back(OpOf(u32(K), E, S, V));
					--Left;
				}
			}
			Headers(Out.Headers);
			++Frames;
		}

		/** The headers for the levels' current boxes (UNI_NEB_GRID_HEADER). */
		void Headers(std::vector<HN::float4>& Out) const
		{
			Out.clear();
			Out.push_back(HN::float4(float(Lay.SlotsX), float(Lay.SlotsY), float(Lay.SlotsZ), float(Lay.Levels.size())));
			Out.push_back(HN::float4(1.0f / float(std::max(Lay.TexX(), 1u)), 1.0f / float(std::max(Lay.TexY(), 1u)), 1.0f / float(std::max(Lay.TexZ(), 1u)), float(Lay.Samples)));
			for (size_t K = 0; K < Lay.Levels.size(); ++K)
			{
				const FNebGridLevelLayout& V = Lay.Levels[K];
				const FLevelState& S = States[K];
				Out.push_back(HN::float4(float(V.Cell), float(V.TableBase), float(V.SlotBase), float(V.SlotCount)));
				Out.push_back(HN::float4(float(S.Min[0]), float(S.Min[1]), float(S.Min[2]), float(V.Bx)));
				Out.push_back(HN::float4(float(V.Bz), float(V.ClassifyN), 0.0f, 0.0f));
			}
		}

		/** Entries waiting to be processed (all levels; per level). */
		size_t Pending() const { size_t N = 0; for (const FLevelState& S : States) { N += PendingOf(S); } return N; }
		size_t PendingAt(size_t InK) const { return InK < States.size() ? PendingOf(States[InK]) : 0; }
		/** A level's box: its first world brick (x, y, z); false before it is placed. */
		bool Box(size_t InK, std::int32_t OutMin[3]) const
		{
			if (InK >= States.size() || !States[InK].bPlaced) { return false; }
			for (int A = 0; A < 3; ++A) { OutMin[A] = States[InK].Min[A]; }
			return true;
		}
		/** The world brick level InK's entry InE stands for in its current box. */
		void WorldOf(size_t InK, u32 InE, std::int32_t OutW[3]) const
		{
			const FNebGridOpGPU Op = OpOf(u32(InK), InE, States[InK], Lay.Levels[InK]);
			OutW[0] = Op.Wx; OutW[1] = Op.Wy; OutW[2] = Op.Wz;
		}
		/** A level's entry for a world brick. */
		u32 EntryOf(size_t InK, const std::int32_t InW[3]) const
		{
			const FNebGridLevelLayout& V = Lay.Levels[InK];
			const u32 Tx = u32(PMod(InW[0], int(V.Bx))), Ty = u32(PMod(InW[1], int(V.Bx))), Tz = u32(PMod(InW[2], int(V.Bz)));
			return Tx + V.Bx * (Ty + V.Bx * Tz);
		}

		u64 Resets = 0, LevelResets = 0, Moves = 0, Frames = 0;

	private:
		struct FLevelState
		{
			bool bPlaced = false;
			std::int32_t Min[3] = { 0, 0, 0 };
			std::vector<std::uint8_t> bPending;
			std::deque<u32> Queue;
		};

		static int PMod(int A, int B) { const int M = A % B; return M < 0 ? M + B : M; }
		static size_t PendingOf(const FLevelState& S) { size_t N = 0; for (std::uint8_t P : S.bPending) { N += P; } return N; }

		static FNebGridOpGPU OpOf(u32 InK, u32 InE, const FLevelState& S, const FNebGridLevelLayout& V)
		{
			const int Tx = int(InE % V.Bx), Ty = int((InE / V.Bx) % V.Bx), Tz = int(InE / (V.Bx * V.Bx));
			FNebGridOpGPU Op;
			Op.Wx = S.Min[0] + PMod(Tx - S.Min[0], int(V.Bx));
			Op.Wy = S.Min[1] + PMod(Ty - S.Min[1], int(V.Bx));
			Op.Wz = S.Min[2] + PMod(Tz - S.Min[2], int(V.Bz));
			Op.LevelEntry = (InK << 24u) | InE;
			return Op;
		}

		/** Level InK's box for the camera; resets or releases what changed. */
		void PlaceLevel(int InK, const double InCam[3], double InMidZ, FFrameOps& Out)
		{
			FLevelState& S = States[size_t(InK)];
			const FNebGridLevelLayout& V = Lay.Levels[size_t(InK)];
			const double Bs = double(HN::UNI_NEB_GRID_BRICK) * V.Cell;   // a brick, field units
			const int Dim[3] = { int(V.Bx), int(V.Bx), int(V.Bz) };
			// The box's centre wanted: the camera across; up, the layer's mid-plane where the box
			// covers the layer, else the camera clamped into it.
			const double HalfZ = 0.5 * double(V.Bz) * Bs;
			double Tz = InMidZ;
			if (HalfZ < Cfg.LayerHalf) { Tz = std::min(std::max(InCam[2], InMidZ - Cfg.LayerHalf + HalfZ), InMidZ + Cfg.LayerHalf - HalfZ); }
			const double Want[3] = { InCam[0], InCam[1], Tz };
			std::int32_t Min[3];
			bool bMove = !S.bPlaced;
			for (int A = 0; A < 3; ++A)
			{
				const std::int32_t Cb = std::int32_t(std::floor(Want[A] / Bs));
				const std::int32_t Fresh = Cb - Dim[A] / 2;
				Min[A] = S.Min[A];
				if (!S.bPlaced || std::abs(Cb - (S.Min[A] + Dim[A] / 2)) > NebGridHysteresis) { Min[A] = Fresh; bMove = true; }
			}
			if (!bMove) { return; }
			bool bReset = !S.bPlaced;
			for (int A = 0; A < 3; ++A) { bReset = bReset || std::abs(Min[A] - S.Min[A]) >= Dim[A]; }
			const std::int32_t Old[3] = { S.Min[0], S.Min[1], S.Min[2] };
			for (int A = 0; A < 3; ++A) { S.Min[A] = Min[A]; }
			S.bPlaced = true;
			++Moves;
			if (bReset)
			{
				// Every entry unbaked; queued from the box's centre out (the camera's surroundings first).
				Out.ResetMask |= 1u << u32(InK);
				++LevelResets;
				S.Queue.clear();
				std::vector<std::pair<int, u32>> Order;
				Order.reserve(V.Entries);
				for (u32 E = 0; E < V.Entries; ++E)
				{
					const FNebGridOpGPU Op = OpOf(u32(InK), E, S, V);
					const int Dx = 2 * (Op.Wx - S.Min[0]) - Dim[0] + 1, Dy = 2 * (Op.Wy - S.Min[1]) - Dim[1] + 1, Dz = 2 * (Op.Wz - S.Min[2]) - Dim[2] + 1;
					Order.push_back({ std::max(std::abs(Dx), std::max(std::abs(Dy), std::abs(Dz))), E });
				}
				std::stable_sort(Order.begin(), Order.end(), [](const std::pair<int, u32>& A, const std::pair<int, u32>& B) { return A.first < B.first; });
				for (const std::pair<int, u32>& O : Order) { S.bPending[O.second] = 1u; S.Queue.push_back(O.second); }
				return;
			}
			// The bricks in the new box and not the old: their entries change world brick.
			for (std::int32_t Z = S.Min[2]; Z < S.Min[2] + Dim[2]; ++Z)
			{
				const bool bZ = Z >= Old[2] && Z < Old[2] + Dim[2];
				for (std::int32_t Y = S.Min[1]; Y < S.Min[1] + Dim[1]; ++Y)
				{
					const bool bY = Y >= Old[1] && Y < Old[1] + Dim[1];
					for (std::int32_t X = S.Min[0]; X < S.Min[0] + Dim[0]; ++X)
					{
						if (bZ && bY && X >= Old[0] && X < Old[0] + Dim[0]) { continue; }
						const std::int32_t W[3] = { X, Y, Z };
						const u32 E = EntryOf(size_t(InK), W);
						FNebGridOpGPU Op;
						Op.Wx = X; Op.Wy = Y; Op.Wz = Z;
						Op.LevelEntry = (u32(InK) << 24u) | E;
						Out.Release.push_back(Op);
						if (!S.bPending[E]) { S.bPending[E] = 1u; S.Queue.push_back(E); }
					}
				}
			}
		}

		FNebGridConfig Cfg;
		FNebGridLayout Lay;
		u64 Key = 0;
		bool bConfigured = false;
		std::vector<FLevelState> States;
	};
}
