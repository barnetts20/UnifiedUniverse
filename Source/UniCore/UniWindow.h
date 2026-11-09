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

#include <algorithm>
#include <cmath>
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

	/** Window geometry for one level. Pure functions of (centre, box); no streaming state.
	 *  D-162: the window is a box of cell offsets [Lo[a], Hi[a]] from the centre cell on each axis
	 *  (any size, even or odd, asymmetric), not only the symmetric (2N+1)^3: an even 4^3 window
	 *  placed by the camera's position in its cell, stretched forward along the direction of
	 *  travel. N stays the bounding half-width (max |offset|) for code that only needs a bound.
	 *  The slot rules generalise per axis: Side = Hi - Lo + 1; an axis is covered when it has at
	 *  most 2 (Side + N) cells; in seam mode cells within Side - 1 + N of zero read as
	 *  c + 2^(128-L) (a no-op when Side divides 2^(128-L), e.g. 4). */
	struct FWindowGeometry
	{
		int Level = 0;
		int N = 2;
		FCellKey Centre;
		int Lo[3] = { -2, -2, -2 };
		int Hi[3] = { 2, 2, 2 };

		static FWindowGeometry Symmetric(int InLevel, int InN)
		{
			FWindowGeometry G;
			G.Level = InLevel;
			G.N = InN;
			for (int A = 0; A < 3; ++A) { G.Lo[A] = -InN; G.Hi[A] = InN; }
			return G;
		}

		void UpdateBound() { N = 0; for (int A = 0; A < 3; ++A) { N = std::max(N, std::max(-Lo[A], Hi[A])); } }
		/** Same slot table: the same Side and the same covered state on every axis. */
		bool SameShape(const FWindowGeometry& O) const { for (int A = 0; A < 3; ++A) { if (Side(A) != O.Side(A) || AxisCovered(A) != O.AxisCovered(A)) { return false; } } return true; }
		bool SameBox(const FWindowGeometry& O) const { for (int A = 0; A < 3; ++A) { if (Lo[A] != O.Lo[A] || Hi[A] != O.Hi[A]) { return false; } } return Centre == O.Centre; }

		int Side(int I) const { return Hi[I] - Lo[I] + 1; }
		int Side() const { return Side(0); }
		int AxisBits() const { return 128 - Level; }

		/** The axis has at most 2 (Side + N) cells, so the window covers all of it (case 1). The
		 *  margin keeps the seam rule's two ends apart for any box, even one not containing the
		 *  centre cell. */
		bool AxisCovered(int I) const { return AxisBits() < 63 && (int64_t(1) << AxisBits()) <= int64_t(2 * (Side(I) + N)); }
		bool AxisCovered() const { return AxisCovered(0) || AxisCovered(1) || AxisCovered(2); }

		/** Slots per axis: Side, or the whole axis when covered. */
		int Span(int I) const { return AxisCovered(I) ? int(int64_t(1) << AxisBits()) : Side(I); }
		int Span() const { return Span(0); }
		int NumSlots() const { return Span(0) * Span(1) * Span(2); }

		/** The window's run of cells crosses the level seam on axis I. */
		bool StraddlesSeam(int I) const
		{
			if (AxisCovered(I)) { return false; }
			const FU128& C = Centre.C[I];
			// c + Lo < 0, or c + Hi > 2^k - 1.
			if (Lo[I] < 0 && FU128::Compare(C, FU128::FromU64(u64(-Lo[I]))) < 0) { return true; }
			if (Hi[I] > 0)
			{
				const FU128 Top = Sub(MaskLow(FU128::Make(~0u, ~0u, ~0u, ~0u), AxisBits()), FU128::FromU64(u64(Hi[I])));   // 2^k - 1 - Hi
				return FU128::Compare(C, Top) > 0;
			}
			return false;
		}

		bool SeamMode() const { return SeamMask() != 0; }

		/** Per-axis seam state. Residues depend on it per axis, so ANY change in the mask, not
		 *  just entering or leaving seam mode overall, forces a rebuild. */
		int SeamMask() const { return (StraddlesSeam(0) ? 1 : 0) | (StraddlesSeam(1) ? 2 : 0) | (StraddlesSeam(2) ? 4 : 0); }

		/** Residue of a window cell's coordinate on axis I, in [0, Span). */
		int Residue(const FU128& Cell, int I) const
		{
			if (AxisCovered(I)) { return int(Cell.L[3]); }
			const u32 S = u32(Side(I));
			u32 R = ModSmall(Cell, S);
			if (StraddlesSeam(I) && FU128::Compare(Cell, FU128::FromU64(u64(Side(I) - 1 + N))) <= 0)
			{
				const u32 TwoK = AxisBits() >= 128 ? u32((u64(ModSmall(FU128::Make(~0u, ~0u, ~0u, ~0u), S)) + 1u) % S)
				                                   : ModSmall(FU128::Pow2(AxisBits()), S);
				R = (R + TwoK) % S;
			}
			return int(R);
		}

		int SlotOf(const FCellKey& K) const
		{
			return (Residue(K.C[0], 0) * Span(1) + Residue(K.C[1], 1)) * Span(2) + Residue(K.C[2], 2);
		}

		/** Every distinct cell in the window, with its slot: Span(0) Span(1) Span(2) cells. */
		std::vector<FWindowCell> Cells() const
		{
			std::vector<FWindowCell> Out;
			Out.reserve(size_t(NumSlots()));
			std::vector<FU128> Axis[3];
			for (int A = 0; A < 3; ++A)
			{
				if (AxisCovered(A))
				{
					const int64_t Len = int64_t(1) << AxisBits();
					for (int64_t D = 0; D < Len; ++D) { Axis[A].push_back(FU128::FromU64(u64(D))); }
				}
				else
				{
					for (int D = Lo[A]; D <= Hi[A]; ++D) { Axis[A].push_back(MaskLow(Add(Centre.C[A], FU128::FromI64(D)), AxisBits())); }
				}
			}
			for (const FU128& X : Axis[0])
			{
				for (const FU128& Y : Axis[1])
				{
					for (const FU128& Z : Axis[2])
					{
						FWindowCell C;
						C.Key.Level = Level;
						C.Key.C[0] = X; C.Key.C[1] = Y; C.Key.C[2] = Z;
						C.Slot = SlotOf(C.Key);
						Out.push_back(C);
					}
				}
			}
			return Out;
		}
	};

	/** D-162: placement of a window box on one axis. The window must cover the camera's
	 *  surroundings from Back cells behind to Fwd cells ahead of its position Fr in [0, 1) within
	 *  its cell: Side = ceil(Back + Fwd) + 1 cells, the first at offset Lo with
	 *  Lo <= floor(Fr - Back) and Lo + Side - 1 >= floor(Fr + Fwd). PrevLo (the previous window's
	 *  first offset, re-expressed from the new centre cell) is kept when it is still valid, and
	 *  otherwise moved the least, so a window only shifts when it must. */
	inline int WindowSide(double Back, double Fwd) { return int(std::ceil(Back + Fwd - 1.0e-9)) + 1; }

	inline int PlaceWindowAxis(double Fr, double Back, double Fwd, int Side, bool bHasPrev, int64_t PrevLo)
	{
		const int Max = int(std::floor(Fr - Back));             // Lo may not exceed this
		const int Min = int(std::floor(Fr + Fwd)) - Side + 1;   // nor be below this
		if (Min > Max) { return Max; }                          // Side too small (cannot happen with WindowSide)
		if (!bHasPrev) { return Max; }
		if (PrevLo > int64_t(Max)) { return Max; }
		if (PrevLo < int64_t(Min)) { return Min; }
		return int(PrevLo);
	}

	/** D-203: one level's per-cell fades, for the volume march. A cell still loading (not Ready)
	 *  or fading in draws only part of its sprites; the march draws the rest of its children's
	 *  expected light as glow, from the same field (Uni_CellGlow), so pop-in becomes glow
	 *  condensing into points. OriginCm: the corner of the window's first cell (offsets Lo)
	 *  relative to the camera; Fades: Side[0] x Side[1] x Side[2], x fastest, each the fraction of
	 *  the cell's sprites drawn (0: not Ready; ramping to 1 once it is). */
	struct FCellGlowLevel
	{
		int Level = 0;
		double OriginCm[3] = { 0.0, 0.0, 0.0 };
		double CellCm = 0.0;
		int Side[3] = { 0, 0, 0 };
		std::vector<float> Fades;
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
		FLevelWindow(int InLevel, int InN) { Geo = FWindowGeometry::Symmetric(InLevel, InN); Slots.resize(size_t(Geo.NumSlots())); }
		/** D-162: a window of this shape (Lo/Hi per axis; the centre is set by Recentre). */
		explicit FLevelWindow(const FWindowGeometry& Shape) { Geo = Shape; Geo.UpdateBound(); Slots.resize(size_t(Geo.NumSlots())); }

		/** Number of distinct cells the window holds. */
		size_t NumCells() const { return size_t(Geo.NumSlots()); }

		const FWindowGeometry& Geometry() const { return Geo; }
		const std::vector<FSlot>& SlotTable() const { return Slots; }
		std::vector<FSlot>& SlotTable() { return Slots; }
		bool IsValid() const { return bValid; }

		/** Re-centre on the camera's cell at this level. Returns entering/exiting cells. */
		FWindowDiff Recentre(const FCellKey& NewCentre)
		{
			FWindowGeometry NewGeo = Geo;
			NewGeo.Centre = NewCentre;
			return Recentre(NewGeo);
		}

		/** D-162: move to a new box of the same shape (same Side per axis; a shape change is a
		 *  new window). Resident cells keep their slots: a contiguous run of Side cells has
		 *  distinct residues wherever it starts. */
		FWindowDiff Recentre(FWindowGeometry NewGeo)
		{
			FWindowDiff D;
			NewGeo.Level = Geo.Level;
			NewGeo.UpdateBound();
			if (!NewGeo.SameShape(Geo))
			{
				// A new shape is a new slot table: everything resident exits, everything enters.
				std::vector<FCellKey> Old;
				for (const FSlot& Sl : Slots) { if (Sl.bOccupied) { Old.push_back(Sl.Key); } }
				Geo = NewGeo;
				Slots.assign(size_t(Geo.NumSlots()), FSlot());
				bValid = false;
				FWindowDiff R = Recentre(NewGeo);
				R.Exiting = Old;
				return R;
			}
			const bool bSeamChange = bValid && (NewGeo.SeamMask() != Geo.SeamMask());
			if (bValid && !bSeamChange && NewGeo.SameBox(Geo)) { return D; }

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

		/** Latency-aware form (D-139, the runtime streamer): LeadCells is how far the camera moves,
		 *  in this level's cells, while an entering cell is generated (speed x latency). Suspend
		 *  when that exceeds the slack between the widest window motion may use and the visible
		 *  radius, resume below 0.8 of it. */
		bool UpdateLead(double LeadCells, double SlackCells)
		{
			if (!bSuspended && LeadCells > SlackCells) { bSuspended = true; }
			else if (bSuspended && LeadCells < 0.8 * SlackCells) { bSuspended = false; }
			return bSuspended;
		}
	};

	/** D-162: the window a level should have this frame. Reach (cells) on every side, plus a
	 *  forward stretch along the direction of travel: LeadPos[a] cells more ahead on axes the
	 *  camera moves along +a, LeadNeg[a] on -a. Side per axis is at least MinSide (the caller's
	 *  shrink hysteresis). Placement keeps the previous window's position whenever it still covers
	 *  the reach (same shape only), so the window shifts only when it must. */
	inline FWindowGeometry PlanWindow(int L, const FCoord& Cam, double Reach, const double LeadPos[3], const double LeadNeg[3], const int MinSide[3], const FLevelWindow* Prev)
	{
		FWindowGeometry G;
		G.Level = L;
		G.Centre = CellOf(Cam, L);
		const double W = std::ldexp(1.0, L);
		const FVec3d InCell = ToDouble(Sub(Cam, CellOrigin(G.Centre)));
		const double Fr[3] = { std::clamp(InCell.X / W, 0.0, 1.0 - 1.0e-12), std::clamp(InCell.Y / W, 0.0, 1.0 - 1.0e-12), std::clamp(InCell.Z / W, 0.0, 1.0 - 1.0e-12) };
		int Side[3];
		for (int A = 0; A < 3; ++A) { Side[A] = std::max(WindowSide(Reach + LeadNeg[A], Reach + LeadPos[A]), MinSide[A]); }
		bool bPrev = Prev && Prev->IsValid();
		int64_t PrevLo[3] = { 0, 0, 0 };
		if (bPrev)
		{
			const FWindowGeometry& P = Prev->Geometry();
			for (int A = 0; A < 3; ++A)
			{
				if (P.Side(A) != Side[A]) { bPrev = false; break; }
				// The previous first cell, as an offset from the new centre (small, or no previous).
				const FU128 D = MaskLow(Sub(Add(P.Centre.C[A], FU128::FromI64(P.Lo[A])), G.Centre.C[A]), G.AxisBits());
				const FU128 Nd = MaskLow(Sub(G.Centre.C[A], Add(P.Centre.C[A], FU128::FromI64(P.Lo[A]))), G.AxisBits());
				const bool bNeg = FU128::Compare(Nd, D) < 0;
				const FU128& M = bNeg ? Nd : D;
				if ((M.L[0] | M.L[1] | M.L[2]) != 0u || M.L[3] > 1024u) { bPrev = false; break; }
				PrevLo[A] = bNeg ? -int64_t(M.L[3]) : int64_t(M.L[3]);
			}
		}
		for (int A = 0; A < 3; ++A)
		{
			G.Lo[A] = PlaceWindowAxis(Fr[A], Reach + LeadNeg[A], Reach + LeadPos[A], Side[A], bPrev, PrevLo[A]);
			G.Hi[A] = G.Lo[A] + Side[A] - 1;
		}
		G.UpdateBound();
		return G;
	}
}

