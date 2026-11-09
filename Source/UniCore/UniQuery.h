#pragma once

// UniQuery.h
// D-193: the engine-agnostic part of the runtime API (design doc, Spec: runtime API): what the
// subsystem's Pick, QueryResident, FindNearest and events build on.
//
//  - FObjectInfo: an object as gameplay sees it (identity, exact coordinate, archetype,
//    magnitude, size, distance), from its identity and record.
//  - FFrameCells / NameSlot: a GPU readback names an object by (cell index in that frame's
//    cell table, slot in the cell's pool range). The game thread keeps each frame's table keys
//    and generations (and the planets' dynamic cells after them) to turn that into an identity.
//  - FResidentEvents: OnResident / OnEvicted for the objects the CPU knows (every emitter
//    object read back: galaxies, clusters, groups; and anchors), by archetype, from the
//    streamer's cells that became Ready or left each frame. Stars and other leaves stay on the
//    GPU: Pick and the queries reach them.
//  - FVisibilityWatch: OnVisible / OnHidden for watched objects the CPU knows, through the
//    shared core's Uni_ObjectSeen (the project pass's photometry and frustum).
//  - PickSprites / QueryPool: the pick and query rules run on the CPU over a sprite list or a
//    cell table and pool, exactly as the GPU passes run them (the offline checks).

#include "UniStreamer.h"
#include "UniSystem.h"
#include "UniVolume.h"

#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace UniCore
{
	/** An object as the API reports it. Planet >= 0: a planet of the star system of the star
	 *  Id (its ordinal in the system; Coord, Record and the rest are the planet's). */
	struct FObjectInfo
	{
		FObjectId Id;
		int Planet = -1;
		FCoord Coord;
		u32 Archetype = 0;
		float AbsMag = 0.0f;
		double RadiusCm = 0.0;
		double DistanceCm = 0.0;   // from the camera given to MakeObjectInfo
		u32 Seed = 0;
		FObjectGPU Record{};
	};

	/** Object info from its identity and record, in the cell named by RecordCell (the object's
	 *  own cell; for a planet, its dynamic cell). */
	inline FObjectInfo MakeObjectInfo(const FObjectId& Id, const FCellKey& RecordCell, const FObjectGPU& O, const FCoord& Camera, int Planet = -1)
	{
		FObjectInfo I;
		I.Id = Id;
		I.Planet = Planet;
		const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
		I.Coord = ObjectCoord(RecordCell, Off);
		const UniHlsl::FUniObject H = ToHlsl(O);
		I.Archetype = O.A[3] & 0xFFFFu;
		I.AbsMag = UniHlsl::Uni_ObjectAbsMag(H);
		I.RadiusCm = std::exp2(double(UniHlsl::Uni_ObjectRadiusLog2(H)));
		I.DistanceCm = Length(ToDouble(Sub(I.Coord, Camera)));
		I.Seed = O.B[0];
		I.Record = O;
		return I;
	}

	/** One frame's cell table as the game thread pushed it. */
	struct FFrameCells
	{
		std::vector<FCellKey> Keys;
		std::vector<u32> Generations;
		std::vector<FPlanetCellRef> Bodies;   // planets' dynamic cells (D-143): cell index Keys.size() + i
	};

	/** D-196: the frame's carrier handoffs as the project pass reads them: (cell index in the
	 *  frame's table, slot, share bits, 0), sorted by (cell index, slot). A carrier whose parent
	 *  is not in the table (not Ready) is left out (its sprite, if any, is whole). */
	inline std::vector<std::array<u32, 4>> CarrierHandoffTable(const std::vector<FCarrierHandoff>& InHandoffs, const std::vector<FCellKey>& InKeys, const FUniStreamer& InStreamer)
	{
		std::vector<std::array<u32, 4>> Out;
		if (InHandoffs.empty()) { return Out; }
		std::unordered_map<FCellKey, u32, FCellKeyHasher> Index;
		Index.reserve(InKeys.size());
		for (size_t I = 0; I < InKeys.size(); ++I) { Index.emplace(InKeys[I], u32(I)); }
		for (const FCarrierHandoff& H : InHandoffs)
		{
			const auto It = Index.find(H.Parent.Cell);
			u32 Slot = 0u;
			if (It == Index.end() || !InStreamer.SlotOf(H.Parent, Slot)) { continue; }
			u32 Bits;
			std::memcpy(&Bits, &H.Hand, sizeof(Bits));
			Out.push_back({ It->second, Slot, Bits, 0u });
		}
		std::sort(Out.begin(), Out.end(), [](const std::array<u32, 4>& A, const std::array<u32, 4>& B) { return A[0] != B[0] ? A[0] < B[0] : A[1] < B[1]; });
		return Out;
	}

	/** The identity behind a readback's (cell index, slot) in frame F's table. For a streamer
	 *  cell: the streamer's IdentityAt (false if the cell has gone or been regenerated since);
	 *  for a planet's dynamic cell: its star (Planet the ordinal). OutRecordCell: the cell the
	 *  record's offsets are in. */
	inline bool NameSlot(const FFrameCells& F, const FUniStreamer& S, u32 CellIndex, u32 Slot, FObjectId& OutId, int& OutPlanet, FCellKey& OutRecordCell)
	{
		OutPlanet = -1;
		if (CellIndex < F.Keys.size())
		{
			if (CellIndex >= F.Generations.size()) { return false; }
			OutRecordCell = F.Keys[CellIndex];
			return S.IdentityAt(F.Keys[CellIndex], F.Generations[CellIndex], Slot, OutId);
		}
		const size_t B = size_t(CellIndex) - F.Keys.size();
		if (B >= F.Bodies.size() || Slot != 0u || !F.Bodies[B].bStarIdKnown) { return false; }
		OutId = F.Bodies[B].StarId;
		OutPlanet = F.Bodies[B].Planet;
		OutRecordCell = F.Bodies[B].Cell;
		return true;
	}

	/** OnResident / OnEvicted for CPU-known objects whose archetype is in ArchMask (bits over
	 *  ids 0..31; 0: events off). Each Update applies the streamer's last frame: cells that left
	 *  report their objects evicted; cells that became Ready (new or regenerated) report the
	 *  difference against what was reported for them. */
	class FResidentEvents
	{
	public:
		u32 ArchMask() const { return Mask; }

		/** Change the archetypes: objects no longer wanted are reported evicted, newly wanted
		 *  ones resident (every Ready cell is rescanned). */
		void SetArchMask(u32 InMask, const FUniStreamer& S, const FCoord& Camera, std::vector<FObjectInfo>& OutResident, std::vector<FObjectId>& OutEvicted)
		{
			if (InMask == Mask) { return; }
			Mask = InMask;
			std::vector<FCellKey> Keys;
			S.ForEachReady([&](const FCellKey& K, u32, u32, float) { Keys.push_back(K); });
			for (const auto& KV : Reported) { Keys.push_back(KV.first); }
			std::sort(Keys.begin(), Keys.end(), [](const FCellKey& A, const FCellKey& B) { return HashCell(A) < HashCell(B); });
			Keys.erase(std::unique(Keys.begin(), Keys.end()), Keys.end());
			for (const FCellKey& K : Keys) { Rescan(K, S, Camera, OutResident, OutEvicted); }
		}

		void Update(const FUniStreamer& S, const FCoord& Camera, std::vector<FObjectInfo>& OutResident, std::vector<FObjectId>& OutEvicted)
		{
			for (const FCellKey& K : S.LeftLastUpdate()) { Drop(K, OutEvicted); }
			if (Mask == 0u) { return; }
			for (const FCellKey& K : S.ReadiedLastUpdate()) { Rescan(K, S, Camera, OutResident, OutEvicted); }
		}

		/** Forget everything (a new universe); nothing is reported. */
		void Reset() { Reported.clear(); Mask = 0u; }

		size_t ReportedCount() const { size_t N = 0; for (const auto& KV : Reported) { N += KV.second.size(); } return N; }
		template <typename F> void ForEachReported(F&& Fn) const { for (const auto& KV : Reported) { for (u32 I : KV.second) { Fn(FObjectId{ KV.first, I }); } } }

	private:
		u32 Mask = 0u;
		std::unordered_map<FCellKey, std::vector<u32>, FCellKeyHasher> Reported;   // sorted indices per cell

		bool Wanted(const FObjectGPU& O) const
		{
			const u32 Arch = O.A[3] & 0xFFFFu;
			if (((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN) != 0u) { return false; }
			return Mask != 0u && Arch < 32u && (Mask & (1u << Arch)) != 0u;
		}

		void Drop(const FCellKey& K, std::vector<FObjectId>& OutEvicted)
		{
			auto It = Reported.find(K);
			if (It == Reported.end()) { return; }
			for (u32 I : It->second) { OutEvicted.push_back(FObjectId{ K, I }); }
			Reported.erase(It);
		}

		void Rescan(const FCellKey& K, const FUniStreamer& S, const FCoord& Camera, std::vector<FObjectInfo>& OutResident, std::vector<FObjectId>& OutEvicted)
		{
			const FCellContent* C = S.KnownObjects(K);
			std::vector<u32> Now;
			std::vector<size_t> At;
			if (C)
			{
				for (size_t I = 0; I < C->Objects.size() && I < C->Indices.size(); ++I)
				{
					if (Wanted(C->Objects[I])) { Now.push_back(C->Indices[I]); At.push_back(I); }
				}
			}
			std::vector<size_t> Order(Now.size());
			for (size_t I = 0; I < Order.size(); ++I) { Order[I] = I; }
			std::sort(Order.begin(), Order.end(), [&](size_t A, size_t B) { return Now[A] < Now[B]; });
			std::vector<u32> Sorted;
			for (size_t I : Order) { Sorted.push_back(Now[I]); }
			static const std::vector<u32> Empty;
			auto It = Reported.find(K);
			const std::vector<u32>& Was = It == Reported.end() ? Empty : It->second;
			for (u32 I : Was) { if (!std::binary_search(Sorted.begin(), Sorted.end(), I)) { OutEvicted.push_back(FObjectId{ K, I }); } }
			for (size_t J : Order)
			{
				if (!std::binary_search(Was.begin(), Was.end(), Now[J])) { OutResident.push_back(MakeObjectInfo(FObjectId{ K, Now[J] }, K, C->Objects[At[J]], Camera)); }
			}
			if (Sorted.empty()) { if (It != Reported.end()) { Reported.erase(It); } }
			else { Reported[K] = std::move(Sorted); }
		}
	};

	/** The exact two's-complement delta (cell origin - camera) per axis, as the cell table holds it. */
	inline void CellDeltaUint4(const FCellKey& K, const FCoord& Camera, UniHlsl::uint4 Out[3])
	{
		const FDelta D = Sub(CellOrigin(K), Camera);
		for (int A = 0; A < 3; ++A)
		{
			const FU128 T = D.TwosComplement(A);
			Out[A] = UniHlsl::uint4(T.L[0], T.L[1], T.L[2], T.L[3]);
		}
	}

	/** OnVisible / OnHidden for watched objects: each frame, every watched object the CPU
	 *  knows (resident emitter objects and anchors) is tested with Uni_ObjectSeen against the
	 *  frame's view; the changes are reported. An object the CPU does not know (a star; or one
	 *  not resident) is hidden. A new watch reports its first state only if it is visible. */
	class FVisibilityWatch
	{
	public:
		void Watch(const FObjectId& Id) { Watched.emplace(Id, false); }
		void Unwatch(const FObjectId& Id) { Watched.erase(Id); }
		bool IsWatched(const FObjectId& Id) const { return Watched.count(Id) != 0; }
		bool IsSeen(const FObjectId& Id) const { auto It = Watched.find(Id); return It != Watched.end() && It->second; }
		void Clear() { Watched.clear(); }
		size_t Count() const { return Watched.size(); }

		void Update(const FUniStreamer& S, const FCoord& Camera, const UniHlsl::FUniView& V, std::vector<FObjectInfo>& OutVisible, std::vector<FObjectId>& OutHidden)
		{
			for (auto& KV : Watched)
			{
				const FObjectId& Id = KV.first;
				const FObjectGPU* O = S.FindObject(Id);
				bool bSeen = false;
				if (O)
				{
					UniHlsl::uint4 D[3];
					CellDeltaUint4(Id.Cell, Camera, D);
					bSeen = UniHlsl::Uni_ObjectSeen(D[0], D[1], D[2], u32(Id.Cell.Level), S.LevelFade(Id.Cell.Level), ToHlsl(*O), V);
				}
				if (bSeen && !KV.second) { OutVisible.push_back(MakeObjectInfo(Id, Id.Cell, *O, Camera)); }
				if (!bSeen && KV.second) { OutHidden.push_back(Id); }
				KV.second = bSeen;
			}
		}

	private:
		std::unordered_map<FObjectId, bool, FObjectIdHasher> Watched;
	};

	/** The pick over a frame's sprites, as the GPU pick pass runs it: the highest
	 *  Uni_PickScore, ties to the lowest sprite index. -1: nothing under the cursor. */
	inline int PickSprites(const std::vector<UniHlsl::FUniSprite>& Sprites, float CursorNdcX, float CursorNdcY, float HalfW, float HalfH, float PixelsPerRadian, float MinPx, float* OutScore = nullptr)
	{
		int Best = -1;
		u32 BestBits = 0u;
		for (size_t I = 0; I < Sprites.size(); ++I)
		{
			const float Sc = UniHlsl::Uni_PickScore(Sprites[I], UniHlsl::float2(CursorNdcX, CursorNdcY), UniHlsl::float2(HalfW, HalfH), PixelsPerRadian, MinPx);
			if (!(Sc > 0.0f)) { continue; }
			const u32 Bits = UniHlsl::asuint(Sc);
			if (Bits > BestBits) { BestBits = Bits; Best = int(I); }
		}
		if (OutScore) { *OutScore = Best >= 0 ? UniHlsl::asfloat(BestBits) : 0.0f; }
		return Best;
	}

	/** One query hit: where it is in the frame's table and its distance from the centre. */
	struct FQueryHit
	{
		u32 CellIndex = 0;
		u32 Slot = 0;
		float DistanceCm = 0.0f;
		FObjectGPU Record{};
	};

	/** The query over a frame's cell table and pool, as the GPU query pass runs it: Mode
	 *  UNI_QUERY_MODE_SPHERE, every match within RadiusCm; UNI_QUERY_MODE_NEAREST, the matches at
	 *  the smallest distance (any radius). Sorted by distance, then cell index and slot (the GPU
	 *  returns them unordered; the engine sorts them the same way). QueryDelta: centre - camera. */
	inline std::vector<FQueryHit> QueryPool(const std::vector<FCellGPU>& Cells, const std::vector<FObjectGPU>& Pool, const UniHlsl::uint4 QueryDelta[3],
		float RadiusCm, u32 ArchMask, float MaxAbsMag, u32 Mode)
	{
		std::vector<FQueryHit> Hits;
		const float R = Mode == UniHlsl::UNI_QUERY_MODE_NEAREST ? -1.0f : RadiusCm;
		u32 MinBits = 0xFFFFFFFFu;
		for (int Pass = 0; Pass < (Mode == UniHlsl::UNI_QUERY_MODE_NEAREST ? 2 : 1); ++Pass)
		{
			for (size_t Ci = 0; Ci < Cells.size(); ++Ci)
			{
				const FCellGPU& C = Cells[Ci];
				const UniHlsl::uint4 Dx(C.DeltaX[0], C.DeltaX[1], C.DeltaX[2], C.DeltaX[3]), Dy(C.DeltaY[0], C.DeltaY[1], C.DeltaY[2], C.DeltaY[3]), Dz(C.DeltaZ[0], C.DeltaZ[1], C.DeltaZ[2], C.DeltaZ[3]);
				if (!UniHlsl::Uni_QueryCellMayHit(Dx, Dy, Dz, QueryDelta[0], QueryDelta[1], QueryDelta[2], C.Level, R)) { continue; }
				for (u32 I = 0; I < C.PoolCount; ++I)
				{
					const FObjectGPU& O = Pool[C.PoolStart + I];
					const UniHlsl::FUniObject H = ToHlsl(O);
					if (!UniHlsl::Uni_QueryMatch(H, ArchMask, MaxAbsMag)) { continue; }
					const float D = UniHlsl::Uni_SafeLength(UniHlsl::Uni_QueryRelative(Dx, Dy, Dz, QueryDelta[0], QueryDelta[1], QueryDelta[2], H, C.Level));
					if (Mode == UniHlsl::UNI_QUERY_MODE_NEAREST)
					{
						if (Pass == 0) { MinBits = std::min(MinBits, UniHlsl::asuint(D)); }
						else if (UniHlsl::asuint(D) <= MinBits) { Hits.push_back(FQueryHit{ u32(Ci), I, D, O }); }
					}
					else if (D <= RadiusCm) { Hits.push_back(FQueryHit{ u32(Ci), I, D, O }); }
				}
			}
		}
		std::sort(Hits.begin(), Hits.end(), [](const FQueryHit& A, const FQueryHit& B)
		{
			if (A.DistanceCm != B.DistanceCm) { return A.DistanceCm < B.DistanceCm; }
			if (A.CellIndex != B.CellIndex) { return A.CellIndex < B.CellIndex; }
			return A.Slot < B.Slot;
		});
		return Hits;
	}

	/** The query centre as the GPU takes it: centre - camera, exact. */
	inline void QueryDeltaUint4(const FCoord& Centre, const FCoord& Camera, UniHlsl::uint4 Out[3])
	{
		const FDelta D = Sub(Centre, Camera);
		for (int A = 0; A < 3; ++A)
		{
			const FU128 T = D.TwosComplement(A);
			Out[A] = UniHlsl::uint4(T.L[0], T.L[1], T.L[2], T.L[3]);
		}
	}
}
