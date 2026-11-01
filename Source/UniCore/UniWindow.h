#pragma once

// UniWindow.h
// Per-level streaming windows: the (2N+1)^3 cells around the camera at one level, held in
// toroidal slots. UltraLargeScale's FTierStreamingSystem window, generalised to every level
// and to 128-bit cell coordinates.
//
// SLOT ADDRESSING. SlotOf(cell) = PosMod per axis over Side = 2N + 1, flattened, unchanged
// from FTierStreamingSystem::SlotOf. A resident cell keeps its slot while the window slides,
// so entering cells land exactly in the slots exiting cells vacate.
//
// TWO CASES FTierStreamingSystem never met, because its coordinates were int32 in one frame:
//
//   1. Coarse levels have few cells per axis. When an axis has at most 4N + 2 cells (only
//      the top few levels), the window covers the WHOLE axis: the cell coordinate itself is
//      the residue, and the slot table is 2^(128-L) wide on that level instead of Side. Level
//      128 has one cell, level 127 two, and so on.
//
//   2. The level seam. Cell coordinates wrap over 2^(128-L), which is never a multiple of an
//      odd Side, so a window straddling coordinate 0 / 2^(128-L) would alias two cells onto
//      one slot. In seam mode, cells within 2N of zero read as c + 2^(128-L), which turns the
//      window into one contiguous run, so its slots are still a bijection. The rule needs the
//      axis to have more than 4N + 1 cells, which case 1 guarantees. Entering or leaving seam
//      mode changes residues, so it forces a full rebuild of that level (D-100). The seam
//      sits at the antipode of the nominal origin, 2^127 cm (about 4 billion observable
//      universes) away, so this costs nothing in practice, but every operation still has a
//      defined result.

#include "UniCoord.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace UniCore
{
	inline int PosMod(int64_t A, int64_t M) { return int(((A % M) + M) % M); }

	struct FWindowCell
	{
		FCellKey Key;
		int Slot = -1;
	};

	/** Window geometry for one level. Pure functions of (centre, N); no streaming state. */
	struct FWindowGeometry
	{
		int Level = 0;
		int N = 2;
		FCellKey Centre;

		int Side() const { return 2 * N + 1; }
		int AxisBits() const { return 128 - Level; }

		/** The axis has at most 4N + 2 cells, so the window covers all of it (case 1). */
		bool AxisCovered() const { return AxisBits() < 63 && (int64_t(1) << AxisBits()) <= int64_t(4 * N + 2); }

		/** Slots per axis: Side, or the whole axis when covered. */
		int Span() const { return AxisCovered() ? int(int64_t(1) << AxisBits()) : Side(); }
		int NumSlots() const { return Span() * Span() * Span(); }

		/** The window's run of cells crosses the level seam on axis I. */
		bool StraddlesSeam(int I) const
		{
			if (AxisCovered()) { return false; }
			const FU128& C = Centre.C[I];
			// c < N, or c > 2^k - 1 - N.
			const FU128 NB = FU128::FromU64(u64(N));
			if (FU128::Compare(C, NB) < 0) { return true; }
			const FU128 Top = Sub(MaskLow(FU128::Make(~0u, ~0u, ~0u, ~0u), AxisBits()), NB); // 2^k - 1 - N
			return FU128::Compare(C, Top) > 0;
		}

		bool SeamMode() const { return SeamMask() != 0; }

		/** Per-axis seam state. Residues depend on it per axis, so ANY change in the mask, not
		 *  just entering or leaving seam mode overall, forces a rebuild. */
		int SeamMask() const { return (StraddlesSeam(0) ? 1 : 0) | (StraddlesSeam(1) ? 2 : 0) | (StraddlesSeam(2) ? 4 : 0); }

		/** Residue of a window cell's coordinate on axis I, in [0, Span). */
		int Residue(const FU128& Cell, int I) const
		{
			if (AxisCovered()) { return int(Cell.L[3]); }
			const u32 S = u32(Side());
			u32 R = ModSmall(Cell, S);
			if (StraddlesSeam(I) && FU128::Compare(Cell, FU128::FromU64(u64(2 * N))) <= 0)
			{
				// Just past the seam: read as c + 2^k, with 2^k mod S computed exactly.
				const u32 TwoK = AxisBits() >= 128 ? u32((u64(ModSmall(FU128::Make(~0u, ~0u, ~0u, ~0u), S)) + 1u) % S)
				                                   : ModSmall(FU128::Pow2(AxisBits()), S);
				R = (R + TwoK) % S;
			}
			return int(R);
		}

		int SlotOf(const FCellKey& K) const
		{
			const int S = Span();
			return (Residue(K.C[0], 0) * S + Residue(K.C[1], 1)) * S + Residue(K.C[2], 2);
		}

		/** Every distinct cell in the window, with its slot: Span^3 cells. */
		std::vector<FWindowCell> Cells() const
		{
			std::vector<FWindowCell> Out;
			Out.reserve(size_t(NumSlots()));
			const int Lo = AxisCovered() ? 0 : -N;
			const int Hi = AxisCovered() ? int((int64_t(1) << AxisBits()) - 1) : N;
			for (int Dx = Lo; Dx <= Hi; ++Dx)
			{
				for (int Dy = Lo; Dy <= Hi; ++Dy)
				{
					for (int Dz = Lo; Dz <= Hi; ++Dz)
					{
						FWindowCell C;
						if (AxisCovered())
						{
							C.Key.Level = Level;
							C.Key.C[0] = FU128::FromU64(u64(Dx)); C.Key.C[1] = FU128::FromU64(u64(Dy)); C.Key.C[2] = FU128::FromU64(u64(Dz));
						}
						else
						{
							C.Key = Neighbour(Centre, Dx, Dy, Dz);
						}
						C.Slot = SlotOf(C.Key);
						Out.push_back(C);
					}
				}
			}
			return Out;
		}
	};

	enum class ESlotStatus : uint8_t { Empty, Pending, Generated, Cached };

	struct FSlot
	{
		FCellKey Key;
		ESlotStatus Status = ESlotStatus::Empty;
		bool bOccupied = false;
	};

	/** Result of moving a window. */
	struct FWindowDiff
	{
		std::vector<FWindowCell> Entering;  // need generation (or cache restore)
		std::vector<FCellKey> Exiting;      // leave the window; content goes to the cell cache
		bool bRebuilt = false;              // first fill, teleport-sized jump, or seam transition
	};

	/** Streaming state of one level: geometry plus slot table. */
	class FLevelWindow
	{
	public:
		FLevelWindow() = default;
		FLevelWindow(int InLevel, int InN) { Geo.Level = InLevel; Geo.N = InN; Slots.resize(size_t(Geo.NumSlots())); }

		/** Number of distinct cells the window holds. */
		size_t NumCells() const { return size_t(Geo.NumSlots()); }

		const FWindowGeometry& Geometry() const { return Geo; }
		const std::vector<FSlot>& SlotTable() const { return Slots; }
		std::vector<FSlot>& SlotTable() { return Slots; }
		bool IsValid() const { return bValid; }

		/** Re-centre on the camera's cell at this level. Returns entering/exiting cells. */
		FWindowDiff Recentre(const FCellKey& NewCentre)
		{
			FWindowDiff D;
			FWindowGeometry NewGeo = Geo;
			NewGeo.Centre = NewCentre;
			const bool bSeamChange = bValid && (NewGeo.SeamMask() != Geo.SeamMask());
			if (bValid && !bSeamChange && NewCentre == Geo.Centre) { return D; }

			const std::vector<FWindowCell> Cells = NewGeo.Cells();
			std::vector<char> Used(Slots.size(), 0);
			D.bRebuilt = !bValid || bSeamChange;

			for (const FWindowCell& C : Cells)
			{
				FSlot& S = Slots[size_t(C.Slot)];
				Used[size_t(C.Slot)] = 1;
				if (!D.bRebuilt && S.bOccupied && S.Key == C.Key) { continue; }   // resident, keeps its slot
				if (S.bOccupied) { D.Exiting.push_back(S.Key); }
				S.Key = C.Key;
				S.bOccupied = true;
				S.Status = ESlotStatus::Pending;
				D.Entering.push_back(C);
			}
			// Every slot is used (the window is Span^3 cells); this loop only matters if a future
			// change makes the window sparser than its slot table.
			for (size_t I = 0; I < Slots.size(); ++I)
			{
				if (!Used[I] && Slots[I].bOccupied) { D.Exiting.push_back(Slots[I].Key); Slots[I].bOccupied = false; Slots[I].Status = ESlotStatus::Empty; }
			}
			Geo = NewGeo;
			bValid = true;
			return D;
		}

		/** Window half-width change (sensitivity). Rebuilds: the slot modulus changes. */
		FWindowDiff Resize(int NewN)
		{
			const FCellKey C = Geo.Centre;
			std::vector<FCellKey> Old;
			for (const FSlot& S : Slots) { if (S.bOccupied) { Old.push_back(S.Key); } }
			Geo.N = NewN;
			Slots.assign(size_t(Geo.NumSlots()), FSlot());
			bValid = false;
			FWindowDiff D = Recentre(C);
			D.Exiting = Old;
			return D;
		}

		bool AllGenerated() const
		{
			for (const FSlot& S : Slots) { if (S.bOccupied && S.Status != ESlotStatus::Generated && S.Status != ESlotStatus::Cached) { return false; } }
			return bValid;
		}

	private:
		FWindowGeometry Geo;
		std::vector<FSlot> Slots;
		bool bValid = false;
	};

	/** Velocity LOD (D-010): suspend a level whose cells the camera crosses faster than once
	 *  every K frames. Resume below 0.8 of that, so a level at the threshold does not flicker. */
	struct FVelocityLod
	{
		bool bSuspended = false;

		/** CellsPerFrame: camera displacement this frame divided by the level's cell width. */
		bool Update(double CellsPerFrame, int K)
		{
			const double Limit = 1.0 / double(K);
			if (!bSuspended && CellsPerFrame > Limit) { bSuspended = true; }
			else if (bSuspended && CellsPerFrame < 0.8 * Limit) { bSuspended = false; }
			return bSuspended;
		}
	};
}
