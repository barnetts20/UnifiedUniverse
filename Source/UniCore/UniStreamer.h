#pragma once

// UniStreamer.h
// The runtime streaming core: level windows, contributor gather, coarse-to-fine scheduling,
// the asynchronous two-phase generation protocol (probe -> allocate -> place, UniGenTask.ush),
// pool allocation, contributor registration from readback, velocity LOD and level fades, and
// the per-frame cell table for project-and-cull. Engine-agnostic (D-015): UUniSubsystem drives
// it with the GPU backend (UnifiedUniverseRender), and the offline harness drives it with the
// CPU backend below and checks it against FRefPipeline (T2.stream.parity).
//
// CPU DUTIES ONLY (design: "the CPU only does window bookkeeping and promotion"): no field is
// evaluated here. Objects come back to the CPU only for tasks whose children own emitters
// (galaxies), because their contributors must be registered; star records never leave the GPU.
//
// Latency: a cell is Ready two backend round trips after it enters (probe, then place). A cell
// at level L waits until every coarser level's window is Ready, so contributors are complete
// before it builds its tasks (design: Level windows, Ordering).

#include "UniGenRef.h"

#include <deque>
#include <memory>

namespace UniCore
{
	/** One submitted batch of tasks, as the backend sees it. */
	struct FGenBatch
	{
		u64 Ticket = 0;
		std::vector<FGenTaskGPU> Tasks;
		std::vector<FTaskCountGPU> Counts;             // filled by the probe phase
		std::vector<u32> Accepted;                      // filled by the place phase
		std::vector<std::pair<u32, u32>> ReadRanges;    // pool ranges to read back after placing (start, count)
		std::vector<FObjectGPU> ReadObjects;            // their records, concatenated
		std::vector<std::pair<u32, FObjectGPU>> Writes; // authored records (anchors) uploaded into the pool with the place phase
		bool bFailed = false;                           // set by a backend that timed out: cells of this batch fail closed
	};

	/** Generation backend. Submit* never block; Poll* return true once results are in. */
	class IGenBackend
	{
	public:
		virtual ~IGenBackend() = default;
		virtual void SubmitProbe(std::shared_ptr<FGenBatch> Batch) = 0;
		virtual bool PollProbe(const FGenBatch& Batch) = 0;
		virtual void SubmitPlace(std::shared_ptr<FGenBatch> Batch) = 0;
		virtual bool PollPlace(const FGenBatch& Batch) = 0;
		virtual u32 PoolCapacity() const = 0;
	};

	/** CPU backend: runs the task phases synchronously into a CPU pool, through the same cores
	 *  as the GPU. For offline verification (T2.stream.parity) and the UE Automation parity test;
	 *  never a runtime fallback (lesson #1). */
	class FCpuGenBackend : public IGenBackend
	{
	public:
		FCpuGenBackend(const FRefGenerator& InGen, u32 InCapacity) : Gen(InGen), Pool(InCapacity) {}
		void SubmitProbe(std::shared_ptr<FGenBatch> B) override
		{
			B->Counts.resize(B->Tasks.size());
			for (size_t I = 0; I < B->Tasks.size(); ++I) { B->Counts[I] = Gen.RunProbe(B->Tasks[I]); }
		}
		bool PollProbe(const FGenBatch&) override { return true; }
		void SubmitPlace(std::shared_ptr<FGenBatch> B) override
		{
			B->Accepted.assign(B->Tasks.size(), 0u);
			for (size_t I = 0; I < B->Tasks.size(); ++I)
			{
				const FGenTaskGPU& T = B->Tasks[I];
				if (T.PoolCapacity == 0u) { continue; }
				std::vector<FObjectGPU> Rec;
				FTaskStats St;
				Gen.RunPlace(T, B->Counts[I], Rec, St);
				for (u32 K = 0; K < T.PoolCapacity; ++K) { Pool[T.PoolBase + K] = Rec[K]; }
				B->Accepted[I] = St.Accepted;
			}
			for (const auto& W : B->Writes) { Pool[W.first] = W.second; }
			B->ReadObjects.clear();
			for (const auto& R : B->ReadRanges) { for (u32 K = 0; K < R.second; ++K) { B->ReadObjects.push_back(Pool[R.first + K]); } }
		}
		bool PollPlace(const FGenBatch&) override { return true; }
		u32 PoolCapacity() const override { return u32(Pool.size()); }
		const std::vector<FObjectGPU>& PoolData() const { return Pool; }
	private:
		const FRefGenerator& Gen;
		std::vector<FObjectGPU> Pool;
	};

	/** First-fit range allocator over the object pool. */
	class FPoolAllocator
	{
	public:
		explicit FPoolAllocator(u32 Capacity) { Free[0] = Capacity; }
		bool Alloc(u32 Count, u32& OutStart)
		{
			if (Count == 0u) { OutStart = 0u; return true; }
			for (auto It = Free.begin(); It != Free.end(); ++It)
			{
				if (It->second < Count) { continue; }
				OutStart = It->first;
				const u32 Rest = It->second - Count, Next = It->first + Count;
				Free.erase(It);
				if (Rest) { Free[Next] = Rest; }
				Used += Count;
				return true;
			}
			return false;
		}
		void Release(u32 Start, u32 Count)
		{
			if (Count == 0u) { return; }
			Used -= Count;
			auto It = Free.emplace(Start, Count).first;
			auto Next = std::next(It);
			if (Next != Free.end() && It->first + It->second == Next->first) { It->second += Next->second; Free.erase(Next); }
			if (It != Free.begin())
			{
				auto Prev = std::prev(It);
				if (Prev->first + Prev->second == It->first) { Prev->second += It->second; Free.erase(It); }
			}
		}
		u32 UsedCount() const { return Used; }
	private:
		std::map<u32, u32> Free;
		u32 Used = 0;
	};

	class FUniStreamer
	{
	public:
		enum class ECellState : uint8_t { Waiting, Probing, Placing, Ready, Failed };

		struct FCell
		{
			FCellKey Key;
			ECellState State = ECellState::Waiting;
			u32 PoolStart = 0, PoolCount = 0;
			std::vector<u32> ContributorSet;
			std::vector<u32> TaskContributor;       // per task
			std::vector<FGenTaskGPU> Tasks;
			std::shared_ptr<FGenBatch> Batch;
			std::vector<size_t> BatchTaskIndex;     // where this cell's tasks sit in its batch
			std::vector<u32> OwnedContributors;     // registered from this cell's objects
			FCellContent Readback;                   // emitter objects with indices (for contributors)
			FCellContent Anchors;                    // authored objects of this cell (anchors), CPU-known
			size_t ReadOffset = 0;                   // where this cell's read-back objects start in its batch's ReadObjects
			u32 Frames = 0;                          // frames since entering (latency stats)
		};

		FUniStreamer(const FRefGenerator& InGen, IGenBackend& InBackend) : Gen(InGen), Backend(InBackend), Pool(InBackend.PoolCapacity())
		{
			const FUniverseDef& D = Gen.ModelRef().Definition();
			std::set<int> Ls;
			auto AddArch = [&](u32 Arch, auto&& Self, int Depth) -> void
			{
				if (Depth > 8) { return; }
				for (const FEmitter& E : D.Arch(Arch).Emitters)
				{
					const FSliceTable& T = Gen.ModelRef().SlicesOf(E.ChildArchetypeId);
					for (int L = T.LMin; L <= T.LMax; ++L) { Ls.insert(L); }
					Self(E.ChildArchetypeId, Self, Depth + 1);
				}
			};
			for (const FEmitter& E : D.RootEmitters)
			{
				const FSliceTable& T = Gen.ModelRef().SlicesOf(E.ChildArchetypeId);
				for (int L = T.LMin; L <= T.LMax; ++L) { Ls.insert(L); }
				AddArch(E.ChildArchetypeId, AddArch, 1);
			}
			for (const FAnchor& A : D.Anchors) { Ls.insert(Gen.ModelRef().LevelOf(A.ArchetypeId, A.AbsMag)); AddArch(A.ArchetypeId, AddArch, 1); }
			for (int L : Ls) { Levels.emplace(L, FLevel(L)); }
		}

		double DeltaMag = 0.0;
		bool bVelocityLod = true;
		int VelocityK = UniHlsl::UNI_VELOCITY_LOD_K;

		/** One frame. Window bookkeeping, then the generation protocol advanced by one step. */
		void Update(const FCoord& Coord)
		{
			const double Moved = bHasCamera ? Length(ToDouble(Sub(Coord, Camera))) : 0.0;
			Camera = Coord; bHasCamera = true;
			const int N = UniHlsl::Uni_WindowHalfWidth(float(DeltaMag));
			AdvanceBatches();

			bool bCoarserReady = true;   // coarse-to-fine: finer levels wait for complete coarser windows
			for (auto It = Levels.rbegin(); It != Levels.rend(); ++It)
			{
				FLevel& Lv = It->second;
				const int L = It->first;
				bool bSusp = bVelocityLod && Lv.Lod.Update(Moved / CellWidthCm(L), VelocityK);
				const bool bJump = Lv.Win.IsValid() && CentreJump(Lv.Win.Geometry(), CellOf(Camera, L)) > 1;
				if (bSusp) { Lv.bHeld = true; }
				else if (Lv.bHeld && bJump && Lv.Fade > 0.0f) { bSusp = true; }
				else { Lv.bHeld = false; }
				// D-128: the fade is "the fraction of this level's sprites drawn", which the volume
				// complements. While any window cell is not Ready it cannot rise (cells still
				// generating draw nothing), so a filling level shows as glow, then fades in whole.
				const float Up = (Lv.Win.IsValid() && LevelReady(Lv)) ? 0.125f : 0.0f;   // an unbuilt window is not "ready"
				Lv.Fade = bSusp ? std::max(0.0f, Lv.Fade - 0.125f) : std::min(1.0f, Lv.Fade + Up);
				if (bSusp && Lv.Win.IsValid()) { bCoarserReady = bCoarserReady && LevelReady(Lv); continue; }

				// D-126: a sensitivity resize keeps every cell still in the window, Ready ones
				// included; only the new outer shell is generated. Fade is not reset.
				bool bResized = false;
				std::unordered_map<FCellKey, FCell, FCellKeyHasher> Kept;
				if (Lv.Win.Geometry().N != N) { Kept = std::move(Lv.Cells); Lv.Cells.clear(); Lv.Win = FLevelWindow(L, N); bResized = true; }
				FWindowGeometry Next = Lv.Win.Geometry();
				Next.Centre = CellOf(Camera, L);
				const std::vector<const FContributor*> Gathered = Gather(Next);
				MaxGather = std::max(MaxGather, Gathered.size());
				if (Gathered.empty() && !AnchorInWindow(Next))
				{
					for (auto& KV : Kept) { Release(KV.second); }
					if (Lv.Win.IsValid()) { EvictAll(Lv); Lv.Win = FLevelWindow(L, N); }
					continue;
				}
				const FWindowDiff Diff = Lv.Win.Recentre(Next.Centre);
				if (((Diff.bRebuilt && !bResized) || bJump) && !bFirstFrame) { Lv.Fade = 0.0f; }
				for (const FCellKey& K : Diff.Exiting) { Evict(Lv, K); }
				for (const FWindowCell& C : Diff.Entering)
				{
					auto K = Kept.find(C.Key);
					if (K != Kept.end()) { Lv.Cells[C.Key] = std::move(K->second); Kept.erase(K); continue; }
					Lv.Cells[C.Key] = FCell(); Lv.Cells[C.Key].Key = C.Key;
				}
				for (auto& KV : Kept) { Release(KV.second); }   // shrank out of the window

				if (bCoarserReady)
				{
					std::vector<FCell*> ToBuild;
					for (auto& KV : Lv.Cells)
					{
						FCell& C = KV.second;
						if (C.State == ECellState::Waiting) { ToBuild.push_back(&C); continue; }
						if (C.State == ECellState::Ready && ContributorsGrew(C, Gathered)) { ++Regenerations; Release(C); C.State = ECellState::Waiting; ToBuild.push_back(&C); }
					}
					if (!ToBuild.empty()) { SubmitCells(ToBuild, Gathered); }
				}
				bCoarserReady = bCoarserReady && LevelReady(Lv);
				for (auto& KV : Lv.Cells) { ++KV.second.Frames; }
			}
			bFirstFrame = false;
		}

		/** Per-frame cell table for project-and-cull: Ready cells in windows (D-104 deltas). */
		std::vector<FCellGPU> CellTable() const
		{
			std::vector<FCellGPU> Out;
			for (const auto& LKV : Levels)
			{
				const FLevel& Lv = LKV.second;
				const u32 Fade = u32(std::lround(std::clamp(Lv.Fade, 0.0f, 1.0f) * 255.0f));
				for (const auto& KV : Lv.Cells)
				{
					const FCell& C = KV.second;
					if (C.State != ECellState::Ready || C.PoolCount == 0u) { continue; }
					FCellGPU G{};
					const FDelta D = Sub(CellOrigin(C.Key), Camera);
					for (int A = 0; A < 3; ++A)
					{
						const FU128 T = D.TwosComplement(A);
						uint32_t* Dst = A == 0 ? G.DeltaX : (A == 1 ? G.DeltaY : G.DeltaZ);
						for (int N = 0; N < 4; ++N) { Dst[N] = T.L[N]; }
					}
					G.Level = u32(C.Key.Level);
					G.PoolStart = C.PoolStart;
					G.PoolCount = C.PoolCount;
					G.Flags = (Lv.Lod.bSuspended ? 1u : 0u) | (Fade << 8);
					Out.push_back(G);
				}
			}
			return Out;
		}

		/** AllocFailures counts cells that failed closed: pool exhausted or a backend timeout. */
		struct FStats { size_t CellsReady = 0, CellsPending = 0, PoolUsed = 0, Batches = 0, Regenerations = 0, MaxGather = 0, AllocFailures = 0, MaxLatencyFrames = 0; };
		FStats Stats() const
		{
			FStats S;
			for (const auto& LKV : Levels) for (const auto& KV : LKV.second.Cells)
			{
				if (KV.second.State == ECellState::Ready) { ++S.CellsReady; } else { ++S.CellsPending; }
			}
			S.PoolUsed = Pool.UsedCount(); S.Batches = BatchesSubmitted; S.Regenerations = Regenerations;
			S.MaxGather = MaxGather; S.AllocFailures = AllocFailures; S.MaxLatencyFrames = MaxLatency;
			return S;
		}

		/** Visit every Ready cell in a window: key, pool range. */
		template <typename F> void ForEachReady(F&& Fn) const
		{
			for (const auto& LKV : Levels) for (const auto& KV : LKV.second.Cells)
			{
				if (KV.second.State == ECellState::Ready) { Fn(KV.second.Key, KV.second.PoolStart, KV.second.PoolCount, LKV.second.Fade); }
			}
		}

		bool AllReady() const
		{
			for (const auto& LKV : Levels) { if (!LevelReady(LKV.second)) { return false; } }
			return Inflight.empty();
		}

		const FCoord& CameraCoord() const { return Camera; }

		// Volume inputs (UniVolume.h, FVolumeSource): resident contributors, the parent object a
		// contributor came from, and level fades.
		const std::map<u32, FContributor>& Contributors() const { return Registered; }
		struct FLevelState { int Level = 0; bool bReady = false; float Fade = 0.0f; };
		std::vector<FLevelState> LevelStates() const
		{
			std::vector<FLevelState> Out;
			for (const auto& KV : Levels) { Out.push_back({ KV.first, LevelReady(KV.second), KV.second.Fade }); }
			return Out;
		}
		float LevelFade(int L) const { auto It = Levels.find(L); return It == Levels.end() ? 0.0f : It->second.Fade; }
		const FObjectGPU* FindObject(const FObjectId& Id) const
		{
			auto Lt = Levels.find(Id.Cell.Level);
			if (Lt == Levels.end()) { return nullptr; }
			auto It = Lt->second.Cells.find(Id.Cell);
			if (It == Lt->second.Cells.end() || It->second.State != ECellState::Ready) { return nullptr; }
			for (const FCellContent* C : { &It->second.Readback, &It->second.Anchors })
			{
				for (size_t I = 0; I < C->Indices.size() && I < C->Objects.size(); ++I) { if (C->Indices[I] == Id.Index) { return &C->Objects[I]; } }
			}
			return nullptr;
		}

	private:
		struct FLevel
		{
			explicit FLevel(int L) : Win(L, UniHlsl::UNI_WINDOW_N) {}
			FLevelWindow Win;
			FVelocityLod Lod;
			float Fade = 0.0f;   // D-128: fades in once the first fill is Ready
			bool bHeld = false;
			std::unordered_map<FCellKey, FCell, FCellKeyHasher> Cells;
		};

		static bool LevelReady(const FLevel& Lv)
		{
			for (const auto& KV : Lv.Cells) { if (KV.second.State != ECellState::Ready && KV.second.State != ECellState::Failed) { return false; } }
			return true;
		}

		static int64_t CentreJump(const FWindowGeometry& G, const FCellKey& NewCentre)
		{
			int64_t J = 0;
			for (int A = 0; A < 3; ++A)
			{
				const FU128 D = MaskLow(Sub(NewCentre.C[A], G.Centre.C[A]), G.AxisBits());
				const FU128 Nn = MaskLow(Sub(G.Centre.C[A], NewCentre.C[A]), G.AxisBits());
				const FU128 M = FU128::Compare(D, Nn) < 0 ? D : Nn;
				J = std::max<int64_t>(J, (M.L[0] | M.L[1] | M.L[2]) != 0u || M.L[3] > (1u << 30) ? (int64_t(1) << 30) : int64_t(M.L[3]));
			}
			return J;
		}

		bool AnchorInWindow(const FWindowGeometry& G) const
		{
			for (size_t I = 0; I < Gen.AnchorLevelList().size(); ++I)
			{
				if (Gen.AnchorLevelList()[I] != G.Level) { continue; }
				for (const FWindowCell& C : G.Cells()) { if (C.Key == Gen.AnchorCellList()[I]) { return true; } }
			}
			return false;
		}

		std::vector<const FContributor*> Gather(const FWindowGeometry& G) const
		{
			std::vector<const FContributor*> Out;
			const int L = G.Level;
			for (const FContributor& R : Gen.RootContributors())
			{
				const FSliceTable& T = Gen.ModelRef().SlicesOf(R.Emitter->ChildArchetypeId);
				if (L >= T.LMin && L <= T.LMax) { Out.push_back(&R); }
			}
			const double W = CellWidthCm(L);
			const FCoord O = CellOrigin(G.Centre);
			for (const auto& KV : Registered)
			{
				const FContributor& C = KV.second;
				const FSliceTable& T = Gen.ModelRef().SlicesOf(C.Emitter->ChildArchetypeId);
				if (L < T.LMin || L > T.LMax) { continue; }
				if (!G.AxisCovered())
				{
					const FVec3d D = ToDouble(Sub(C.Centre, O));
					const double P[3] = { D.X, D.Y, D.Z };
					double Dist2 = 0.0;
					for (double V : P) { const double Lo = -G.N * W, Hi = (G.N + 1) * W; const double E = V < Lo ? Lo - V : (V > Hi ? V - Hi : 0.0); Dist2 += E * E; }
					if (Dist2 > C.InfluenceCm * C.InfluenceCm) { continue; }
				}
				Out.push_back(&C);
			}
			return Out;
		}

		bool ContributorsGrew(const FCell& C, const std::vector<const FContributor*>& Gathered) const
		{
			std::vector<u32> Ids;
			for (const FContributor* Ct : Gathered) { if (Gen.Overlaps(*Ct, C.Key)) { Ids.push_back(Ct->Id); } }
			std::sort(Ids.begin(), Ids.end());
			return !std::includes(C.ContributorSet.begin(), C.ContributorSet.end(), Ids.begin(), Ids.end());
		}

		/** Build tasks for the given cells and submit their probe phase as one batch. */
		void SubmitCells(const std::vector<FCell*>& Cells, const std::vector<const FContributor*>& Gathered)
		{
			auto B = std::make_shared<FGenBatch>();
			B->Ticket = ++NextTicket;
			size_t Joined = 0;
			for (FCell* C : Cells)
			{
				std::vector<const FContributor*> Mine;
				for (const FContributor* Ct : Gathered) { if (Gen.Overlaps(*Ct, C->Key)) { Mine.push_back(Ct); } }
				std::sort(Mine.begin(), Mine.end(), [](const FContributor* A, const FContributor* Bc) { return A->Id != Bc->Id ? A->Id < Bc->Id : A->EmitterIndex < Bc->EmitterIndex; });
				C->ContributorSet.clear(); C->Tasks.clear(); C->TaskContributor.clear(); C->BatchTaskIndex.clear();
				for (const FContributor* Ct : Mine)
				{
					C->ContributorSet.push_back(Ct->Id);
					FGenTaskGPU T;
					if (!Gen.BuildTask(*Ct, C->Key, T)) { continue; }
					C->Tasks.push_back(T);
					C->TaskContributor.push_back(Ct->Id);
					C->BatchTaskIndex.push_back(B->Tasks.size());
					B->Tasks.push_back(T);
				}
				std::sort(C->ContributorSet.begin(), C->ContributorSet.end());
				// A cell with neither tasks nor anchors is empty and Ready at once. A cell with
				// anchors but no tasks still goes through the batch, so its anchors are allocated,
				// uploaded and registered like everything else.
				bool bHasAnchor = false;
				for (const FCellKey& AK : Gen.AnchorCellList()) { bHasAnchor = bHasAnchor || AK == C->Key; }
				if (C->Tasks.empty() && !bHasAnchor)
				{
					C->Batch.reset(); C->PoolCount = 0; C->State = ECellState::Ready;
					MaxLatency = std::max<size_t>(MaxLatency, C->Frames);
					continue;
				}
				C->Batch = B;
				C->State = ECellState::Probing;
				++Joined;
			}
			if (Joined == 0) { return; }
			Backend.SubmitProbe(B);
			Inflight.push_back(B);
			++BatchesSubmitted;
		}

		/** Advance every in-flight batch whose phase completed. */
		void AdvanceBatches()
		{
			for (auto It = Inflight.begin(); It != Inflight.end();)
			{
				std::shared_ptr<FGenBatch> B = *It;
				std::vector<FCell*> Mine = CellsOf(B);
				const bool bProbing = !Mine.empty() && Mine.front()->State == ECellState::Probing;
				if (Mine.empty()) { It = Inflight.erase(It); continue; }   // all its cells were evicted
				if (bProbing)
				{
					if (!Backend.PollProbe(*B)) { ++It; continue; }
					if (B->bFailed)
					{
						for (FCell* C : Mine) { C->State = ECellState::Failed; C->Batch.reset(); ++AllocFailures; }
						It = Inflight.erase(It);
						continue;
					}
					// Allocation: per cell, IndexBase = prefix sum of n (truncated at 2^15), one
					// contiguous pool range per cell, task sub-ranges in order.
					B->ReadRanges.clear();
					for (FCell* C : Mine)
					{
						u32 Index = 0;
						for (size_t T = 0; T < C->Tasks.size(); ++T)
						{
							const FTaskCountGPU& Cnt = B->Counts[C->BatchTaskIndex[T]];
							const u32 Room = Index < Gen.MaxPerCell ? Gen.MaxPerCell - Index : 0u;
							C->Tasks[T].IndexBase = Index;
							C->Tasks[T].PoolCapacity = std::min(Cnt.Count, Room);
							Index += C->Tasks[T].PoolCapacity;
						}
						// Anchors live after the generated objects in the cell's range.
						C->Anchors = FCellContent();
						Gen.GenerateCell(C->Key, {}, C->Anchors);   // no contributors: anchors only, no field evaluated
						const u32 NumAnchors = u32(C->Anchors.Objects.size());
						if (!Pool.Alloc(Index + NumAnchors, C->PoolStart)) { ++AllocFailures; C->State = ECellState::Failed; C->PoolCount = 0; continue; }   // fail closed: not drawn
						C->PoolCount = Index + NumAnchors;
						for (u32 A = 0; A < NumAnchors; ++A) { B->Writes.push_back({ C->PoolStart + Index + A, C->Anchors.Objects[A] }); }
						// Stored per cell: the cells of a batch are found again by map iteration later,
						// and that order is not stable across inserts (W5 in the UE review).
						C->ReadOffset = 0;
						for (const auto& Rg : B->ReadRanges) { C->ReadOffset += Rg.second; }
						u32 Base = C->PoolStart;
						for (size_t T = 0; T < C->Tasks.size(); ++T)
						{
							C->Tasks[T].PoolBase = Base;
							B->Tasks[C->BatchTaskIndex[T]] = C->Tasks[T];
							const u32 Child = C->Tasks[T].ChildArchetypeId;
							if (!Gen.ModelRef().Definition().Arch(Child).Emitters.empty() && C->Tasks[T].PoolCapacity) { B->ReadRanges.push_back({ Base, C->Tasks[T].PoolCapacity }); }
							Base += C->Tasks[T].PoolCapacity;
						}
						C->State = ECellState::Placing;
					}
					// Tasks of failed cells write nothing.
					for (FCell* C : Mine) { if (C->State == ECellState::Failed) { for (size_t T = 0; T < C->Tasks.size(); ++T) { B->Tasks[C->BatchTaskIndex[T]].PoolCapacity = 0; } } }
					Backend.SubmitPlace(B);
					++It;
					continue;
				}
				if (!Backend.PollPlace(*B)) { ++It; continue; }
				if (B->bFailed)
				{
					// The backend gave up (timeout): fail closed. Nothing of these cells is drawn,
					// and their ranges are released; they retry if they re-enter (lesson #27).
					for (FCell* C : Mine) { if (C->State == ECellState::Placing) { Release(*C); C->State = ECellState::Failed; C->Batch.reset(); ++AllocFailures; } }
					It = Inflight.erase(It);
					continue;
				}
				// Ready: register contributors from the read-back emitter objects.
				for (FCell* C : Mine)
				{
					if (C->State != ECellState::Placing) { continue; }
					size_t Cursor = C->ReadOffset;
					C->Readback = FCellContent();
					C->Readback.Key = C->Key;
					for (size_t T = 0; T < C->Tasks.size(); ++T)
					{
						const FGenTaskGPU& Tk = C->Tasks[T];
						if (Gen.ModelRef().Definition().Arch(Tk.ChildArchetypeId).Emitters.empty() || !Tk.PoolCapacity) { continue; }
						for (u32 K = 0; K < Tk.PoolCapacity; ++K, ++Cursor)
						{
							const FObjectGPU& O = B->ReadObjects[Cursor];
							if ((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN) { continue; }
							C->Readback.Objects.push_back(O);
							C->Readback.Indices.push_back(Tk.IndexBase + K);
							C->Readback.ObjectContributor.push_back(C->TaskContributor[T]);
						}
					}
					AppendAnchors(*C);
					for (FContributor& Ct : Gen.MakeContributors(C->Readback)) { C->OwnedContributors.push_back(Ct.Id); Registered[Ct.Id] = Ct; }
					C->State = ECellState::Ready;
					C->Batch.reset();
					MaxLatency = std::max<size_t>(MaxLatency, C->Frames);
				}
				It = Inflight.erase(It);
			}
		}

		/** Anchors are authored, not generated: the CPU knows them and registers them directly. */
		void AppendAnchors(FCell& C)
		{
			for (size_t I = 0; I < C.Anchors.Objects.size(); ++I)
			{
				C.Readback.Objects.push_back(C.Anchors.Objects[I]);
				C.Readback.Indices.push_back(C.Anchors.Indices[I]);
				C.Readback.ObjectContributor.push_back(0u);
			}
		}

		std::vector<FCell*> CellsOf(const std::shared_ptr<FGenBatch>& B)
		{
			std::vector<FCell*> Out;
			for (auto& LKV : Levels) for (auto& KV : LKV.second.Cells) { if (KV.second.Batch == B) { Out.push_back(&KV.second); } }
			return Out;
		}

		void Release(FCell& C)
		{
			Pool.Release(C.PoolStart, C.PoolCount);
			C.PoolStart = C.PoolCount = 0;
			for (u32 Id : C.OwnedContributors) { Registered.erase(Id); }
			C.OwnedContributors.clear();
		}

		void Evict(FLevel& Lv, const FCellKey& K)
		{
			auto It = Lv.Cells.find(K);
			if (It == Lv.Cells.end()) { return; }
			Release(It->second);
			Lv.Cells.erase(It);
		}

		void EvictAll(FLevel& Lv)
		{
			for (auto& KV : Lv.Cells) { Release(KV.second); }
			Lv.Cells.clear();
		}

		const FRefGenerator& Gen;
		IGenBackend& Backend;
		FPoolAllocator Pool;
		std::map<int, FLevel> Levels;
		std::map<u32, FContributor> Registered;
		std::deque<std::shared_ptr<FGenBatch>> Inflight;
		FCoord Camera;
		bool bHasCamera = false, bFirstFrame = true;
		u64 NextTicket = 0;
		size_t BatchesSubmitted = 0, Regenerations = 0, MaxGather = 0, AllocFailures = 0, MaxLatency = 0;
	};
}
