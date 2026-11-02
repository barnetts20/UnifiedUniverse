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
// at level L waits until the windows of its PARENT levels are Ready, so contributors are complete
// before it builds its tasks (design: Level windows, Ordering). A parent level is one that holds
// an archetype whose emitters place objects at L (D-137). Levels fed only by root emitters wait
// for nothing, and levels never wait for unrelated levels, so latency does not cascade down the
// ladder: before D-137 every level waited for every coarser one, and the galaxy scene's finest
// star level took 108 frames to fill (18 levels x 6 frames).
//
// Motion (D-139): a GPU fill takes ~6 frames (two phases of ~3), not the one frame D-118
// assumed, so at speed an entering cell would become Ready up to speed x latency closer than it
// entered, inside the visible radius N_eff: pop-in. With velocity LOD on, each level's window is
// widened so that N >= N_eff + speed x (latency + 1), up to UNI_WINDOW_N_MOVE_MAX; a level is
// suspended only when even that cannot cover the lead. Latency is measured (submit to Ready,
// fast attack, slow decay). Level assignment still uses N_eff, so nothing rerolls; widening goes
// through the D-126 resize path (cells kept), and the window shrinks back after 30 slow frames.
//
// Normalising integrals (D-140): a contributor's first task needs its field integral, ~2.4M
// field evaluations (~0.2 s, D-123). With bAsyncIntegrals (the runtime default) it is computed
// on a worker thread; cells that need it stay Waiting until it arrives, and nothing else waits.
// The worker comes from RunJob: the UE module hands in its thread pool (AsyncTask); without one,
// a plain std::thread is used (offline). No std::future: UE builds without exceptions.
// The value is identical either way (same pure function), so content does not change.
//
// Cost (D-138): contributor distances are measured once per frame from the camera, not once per
// level, and a level re-checks its Ready cells for new contributors only when its gathered set
// changed. Before D-138 the web scene spent ~10 ms of game thread per frame in Gather.

#include "UniGenRef.h"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <thread>

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
			bool bGrowthChecked = false;             // D-138: checked against the level's current gathered set
			u64 SubmittedAt = 0;                     // D-139: frame its probe was submitted
		};

		FUniStreamer(const FRefGenerator& InGen, IGenBackend& InBackend) : Gen(InGen), Backend(InBackend), Pool(InBackend.PoolCapacity())
		{
			const FUniverseDef& D = Gen.ModelRef().Definition();
			std::set<int> Ls;
			auto AddArch = [&](u32 Arch, auto&& Self, int Depth) -> void
			{
				if (Depth > 8) { return; }
				for (const FUniEmitter& E : D.Arch(Arch).Emitters)
				{
					const FSliceTable& T = Gen.ModelRef().SlicesOf(E.ChildArchetypeId);
					for (int L = T.LMin; L <= T.LMax; ++L) { Ls.insert(L); }
					Self(E.ChildArchetypeId, Self, Depth + 1);
				}
			};
			for (const FUniEmitter& E : D.RootEmitters)
			{
				const FSliceTable& T = Gen.ModelRef().SlicesOf(E.ChildArchetypeId);
				for (int L = T.LMin; L <= T.LMax; ++L) { Ls.insert(L); }
				AddArch(E.ChildArchetypeId, AddArch, 1);
			}
			for (const FAnchor& A : D.Anchors) { Ls.insert(Gen.ModelRef().LevelOf(A.ArchetypeId, A.AbsMag)); AddArch(A.ArchetypeId, AddArch, 1); }
			for (int L : Ls) { Levels.emplace(L, FLevel(L)); }

			// D-137: parent levels. ArchAt[L] = archetypes that can have objects at level L (from
			// the slice tables of every emitter child, and the anchors' levels). P is a parent of L
			// if an archetype at P has an emitter whose child archetype can be at L.
			std::map<int, std::set<u32>> ArchAt;
			std::set<u32> Children;
			auto Collect = [&](u32 Arch, auto&& Self, int Depth) -> void
			{
				if (Depth > 8) { return; }
				for (const FUniEmitter& E : D.Arch(Arch).Emitters)
				{
					const FSliceTable& T = Gen.ModelRef().SlicesOf(E.ChildArchetypeId);
					for (int L = T.LMin; L <= T.LMax; ++L) { ArchAt[L].insert(E.ChildArchetypeId); }
					if (Children.insert(E.ChildArchetypeId).second) { Self(E.ChildArchetypeId, Self, Depth + 1); }
				}
			};
			for (const FUniEmitter& E : D.RootEmitters)
			{
				const FSliceTable& T = Gen.ModelRef().SlicesOf(E.ChildArchetypeId);
				for (int L = T.LMin; L <= T.LMax; ++L) { ArchAt[L].insert(E.ChildArchetypeId); }
				if (Children.insert(E.ChildArchetypeId).second) { Collect(E.ChildArchetypeId, Collect, 1); }
			}
			for (const FAnchor& A : D.Anchors)
			{
				ArchAt[Gen.ModelRef().LevelOf(A.ArchetypeId, A.AbsMag)].insert(A.ArchetypeId);
				Collect(A.ArchetypeId, Collect, 1);
			}
			for (auto& LKV : Levels)
			{
				const std::set<u32>& Here = ArchAt[LKV.first];
				for (const auto& PKV : ArchAt)
				{
					if (PKV.first == LKV.first || !Levels.count(PKV.first)) { continue; }
					bool bParent = false;
					for (u32 PA : PKV.second)
					{
						for (const FUniEmitter& E : D.Arch(PA).Emitters) { bParent = bParent || Here.count(E.ChildArchetypeId) != 0; }
					}
					if (bParent) { LKV.second.Parents.push_back(PKV.first); }
				}
			}
		}

		double DeltaMag = 0.0;
		bool bVelocityLod = true;
		int VelocityK = UniHlsl::UNI_VELOCITY_LOD_K;
		static constexpr int UNI_WINDOW_N_MOVE_MAX = 4;   // D-139: widest window motion may use (9^3 cells)
		static constexpr int ShrinkAfterFrames = 30;

		/** D-139: window half-width for a level whose camera lead is LeadCells (speed x latency,
		 *  in its cells). Equals Uni_WindowHalfWidth(dm) at rest. */
		static int MovingHalfWidth(double InDeltaMag, double LeadCells)
		{
			const int Rest = UniHlsl::Uni_WindowHalfWidth(float(InDeltaMag));
			const double SensScale = double(UniHlsl::Uni_SensitivityScale(float(InDeltaMag)));
			const double Need = double(UniHlsl::UNI_WINDOW_N_EFF) * SensScale + LeadCells;
			const int Moving = std::min(int(std::ceil(Need - 1.0e-5)), std::max(Rest, UNI_WINDOW_N_MOVE_MAX));
			return std::max(Rest, std::min(Moving, UniHlsl::UNI_WINDOW_N_MAX));
		}

		/** D-140: compute normalising integrals on worker threads (runtime). Offline checks that
		 *  count frames turn it off: the same values, computed inline. */
		bool bAsyncIntegrals = true;
		static constexpr size_t MaxIntegralJobs = 4;
		/** Runs a job on some worker thread (D-140). Empty: a detached std::thread. */
		std::function<void(std::function<void()>)> RunJob;

		~FUniStreamer()
		{
			// Jobs read the model through their contributor copy: wait for them (each < 1 s).
			for (auto& KV : IntegralJobs) { while (!KV.second->bDone.load(std::memory_order_acquire)) { std::this_thread::yield(); } }
		}
		FUniStreamer(const FUniStreamer&) = delete;
		FUniStreamer& operator=(const FUniStreamer&) = delete;

		/** Integral jobs still running (D-140). */
		size_t PendingIntegrals() const { return IntegralJobs.size(); }

		/** Measured backend latency, frames from submit to Ready (D-139). */
		double LatencyFrames() const { return LatencyEst; }

		/** One frame. Window bookkeeping, then the generation protocol advanced by one step. */
		void Update(const FCoord& Coord)
		{
			const double Moved = bHasCamera ? Length(ToDouble(Sub(Coord, Camera))) : 0.0;
			Camera = Coord; bHasCamera = true;
			const int NRest = UniHlsl::Uni_WindowHalfWidth(float(DeltaMag));
			++FrameNo;
			PumpIntegrals();
			AdvanceBatches();
			MeasureContributors();

			// Coarse first, so a parent's readiness is this frame's when its children look at it.
			for (auto It = Levels.rbegin(); It != Levels.rend(); ++It)
			{
				FLevel& Lv = It->second;
				const int L = It->first;
				bool bParentsReady = true;   // D-137: wait for parent levels only
				for (int P : Lv.Parents) { bParentsReady = bParentsReady && LevelReady(Levels.at(P)); }
				// D-139: lead = how far the camera moves (in this level's cells) while a cell fills,
				// plus the frame the window itself trails by.
				const double Lead = (Moved / CellWidthCm(L)) * (LatencyEst + 1.0);
				int N = NRest;
				if (bVelocityLod)
				{
					const int Want = MovingHalfWidth(DeltaMag, Lead);
					const int Cur = Lv.Win.IsValid() ? Lv.Win.Geometry().N : NRest;
					// Widening multiplies a level's cells by up to (9/5)^3; never at the cost of
					// failing cells: above 70% pool use a window keeps its width (and suspension,
					// as before D-139, takes over).
					const bool bRoom = Pool.UsedCount() < u32(0.7 * double(Backend.PoolCapacity()));
					if (Want > Cur && !bRoom) { N = Cur; Lv.ShrinkFrames = 0; }
					else if (Want >= Cur) { N = Want; Lv.ShrinkFrames = 0; }
					else { N = (++Lv.ShrinkFrames >= ShrinkAfterFrames) ? std::max(Want, Cur - 1) : Cur; if (N != Cur) { Lv.ShrinkFrames = 0; } }
					N = std::max(N, NRest);
				}
				const double SensScale = double(UniHlsl::Uni_SensitivityScale(float(DeltaMag)));
				const double Slack = double(std::max(NRest, UNI_WINDOW_N_MOVE_MAX)) - double(UniHlsl::UNI_WINDOW_N_EFF) * SensScale;
				bool bSusp = bVelocityLod && Lv.Lod.UpdateLead(Lead, Slack);
				const bool bJump = Lv.Win.IsValid() && CentreJump(Lv.Win.Geometry(), CellOf(Camera, L)) > 1;
				if (bSusp) { Lv.bHeld = true; }
				else if (Lv.bHeld && bJump && Lv.Fade > 0.0f) { bSusp = true; }
				else { Lv.bHeld = false; }
				Lv.Fade = bSusp ? std::max(0.0f, Lv.Fade - 0.125f) : std::min(1.0f, Lv.Fade + 0.125f);
				if (bSusp && Lv.Win.IsValid()) { continue; }

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
					Lv.GatherIds.clear();
					continue;
				}
				// D-138: Ready cells only need re-checking for new contributors when the gathered
				// set changed (or the cell became Ready since the last check).
				std::vector<u32> Ids;
				Ids.reserve(Gathered.size());
				for (const FContributor* Ct : Gathered) { Ids.push_back(Ct->Id); }
				std::sort(Ids.begin(), Ids.end());   // Gather's order follows distance, which changes as the camera moves
				const bool bGatherChanged = Ids != Lv.GatherIds;
				if (bGatherChanged) { Lv.GatherIds = std::move(Ids); }
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

				if (bParentsReady)
				{
					std::vector<FCell*> ToBuild;
					for (auto& KV : Lv.Cells)
					{
						FCell& C = KV.second;
						if (C.State == ECellState::Waiting) { ToBuild.push_back(&C); continue; }
						if (C.State != ECellState::Ready || (C.bGrowthChecked && !bGatherChanged)) { continue; }
						C.bGrowthChecked = true;
						if (ContributorsGrew(C, Gathered)) { ++Regenerations; Release(C); C.State = ECellState::Waiting; C.bGrowthChecked = false; ToBuild.push_back(&C); }
					}
					if (!ToBuild.empty()) { SubmitCells(ToBuild, Gathered); }
				}
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
			return Inflight.empty() && IntegralJobs.empty();
		}

		const FCoord& CameraCoord() const { return Camera; }

		/** Per-level state, for uu.Stats and the offline checks. */
		struct FLevelState { int Level = 0; bool bReady = false; float Fade = 0.0f; size_t ReadyCells = 0, Cells = 0; bool bSuspended = false; size_t Parents = 0; int HalfWidth = 0; };
		std::vector<FLevelState> LevelStates() const
		{
			std::vector<FLevelState> Out;
			for (const auto& KV : Levels)
			{
				FLevelState S;
				S.Level = KV.first; S.bReady = LevelReady(KV.second); S.Fade = KV.second.Fade;
				S.Cells = KV.second.Cells.size(); S.bSuspended = KV.second.Lod.bSuspended; S.Parents = KV.second.Parents.size();
				S.HalfWidth = KV.second.Win.IsValid() ? KV.second.Win.Geometry().N : 0;
				for (const auto& C : KV.second.Cells) { S.ReadyCells += C.second.State == ECellState::Ready ? 1u : 0u; }
				Out.push_back(S);
			}
			return Out;
		}

	private:
		struct FLevel
		{
			explicit FLevel(int L) : Win(L, UniHlsl::UNI_WINDOW_N) {}
			FLevelWindow Win;
			FVelocityLod Lod;
			float Fade = 1.0f;
			bool bHeld = false;
			std::unordered_map<FCellKey, FCell, FCellKeyHasher> Cells;
			std::vector<int> Parents;      // D-137: levels whose objects can be contributors here
			std::vector<u32> GatherIds;    // D-138: ids of last frame's gathered set
			int ShrinkFrames = 0;          // D-139: slow frames since the window could shrink
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
			// D-138: distances from the window origin through the per-frame camera deltas,
			// (contributor - camera) - (origin - camera), in double. This is a prefilter only:
			// the exact overlap test (Gen.Overlaps) decides tasks, so it is made conservative by a
			// margin far above the rounding (relative 1e-12 of the distances involved).
			const FVec3d Ow = ToDouble(Sub(CellOrigin(G.Centre), Camera));
			const double Lo = -G.N * W, Hi = (G.N + 1) * W;
			// Measured is sorted by distance from the camera: nothing beyond the window's bounding
			// sphere plus the largest influence can reach it.
			const bool bCovered = G.AxisCovered();
			const double Reach = Length(Ow) + 1.7320508075688772 * std::max(std::abs(Lo), std::abs(Hi)) + MaxInfluence;
			const double Cut = Reach * (1.0 + 1.0e-9) + 1.0;
			for (const FMeasured& M : Measured)
			{
				if (!bCovered && M.Dist > Cut) { break; }
				if (L < M.LMin || L > M.LMax) { continue; }
				if (!bCovered)
				{
					const double P[3] = { M.D.X - Ow.X, M.D.Y - Ow.Y, M.D.Z - Ow.Z };
					const double Margin = 1.0e-12 * (M.Dist + Length(Ow) + std::abs(Hi)) + 1.0;
					double Dist2 = 0.0;
					for (double V : P) { const double E = V < Lo ? Lo - V : (V > Hi ? V - Hi : 0.0); Dist2 += E * E; }
					const double R = M.InfluenceCm + Margin;
					if (Dist2 > R * R) { continue; }
				}
				// Looked up again: a coarser level may have evicted (unregistered) it this frame.
				auto It = Registered.find(M.Id);
				if (It != Registered.end()) { Out.push_back(&It->second); }
			}
			return Out;
		}

		/** D-138: every registered contributor's offset from the camera, once per frame. */
		void MeasureContributors()
		{
			Measured.clear();
			Measured.reserve(Registered.size());
			MaxInfluence = 0.0;
			for (const auto& KV : Registered)
			{
				FMeasured M;
				M.Id = KV.first;
				M.InfluenceCm = KV.second.InfluenceCm;
				const FSliceTable& T = Gen.ModelRef().SlicesOf(KV.second.Emitter->ChildArchetypeId);
				M.LMin = T.LMin; M.LMax = T.LMax;
				M.D = ToDouble(Sub(KV.second.Centre, Camera));
				M.Dist = Length(M.D);
				Measured.push_back(M);
				MaxInfluence = std::max(MaxInfluence, M.InfluenceCm);
			}
			std::sort(Measured.begin(), Measured.end(), [](const FMeasured& A, const FMeasured& B) { return A.Dist != B.Dist ? A.Dist < B.Dist : A.Id < B.Id; });
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
				// D-140: wait (stay Waiting) for any normalising integral still being computed.
				bool bIntegrals = true;
				for (const FContributor* Ct : Mine) { bIntegrals = IntegralReady(*Ct) && bIntegrals; }
				if (!bIntegrals) { continue; }
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
					C->Batch.reset(); C->PoolCount = 0; C->State = ECellState::Ready; C->bGrowthChecked = false;
					MaxLatency = std::max<size_t>(MaxLatency, C->Frames);
					continue;
				}
				C->Batch = B;
				C->State = ECellState::Probing;
				C->SubmittedAt = FrameNo;
				++Joined;
			}
			if (Joined == 0) { return; }
			Backend.SubmitProbe(B);
			Inflight.push_back(B);
			++BatchesSubmitted;
		}

		/** D-140: true if Ct's integral is known (or will be computed inline); otherwise starts
		 *  (or keeps) a worker job for it and returns false. */
		bool IntegralReady(const FContributor& Ct)
		{
			if (!bAsyncIntegrals || Gen.HasIntegral(Ct)) { return true; }
			if (!IntegralJobs.count(Ct.Id) && IntegralJobs.size() < MaxIntegralJobs)
			{
				// The job owns a copy of the contributor (its Emitter points into the model, which
				// outlives the streamer) and a shared result slot.
				std::shared_ptr<FIntegralJob> Job = std::make_shared<FIntegralJob>();
				IntegralJobs.emplace(Ct.Id, Job);
				std::function<void()> Work = [Job, Copy = Ct]()
				{
					Job->Value = FRefGenerator::ComputeIntegral(Copy);
					Job->bDone.store(true, std::memory_order_release);
				};
				if (RunJob) { RunJob(std::move(Work)); }
				else { std::thread(std::move(Work)).detach(); }
			}
			return false;
		}

		void PumpIntegrals()
		{
			for (auto It = IntegralJobs.begin(); It != IntegralJobs.end();)
			{
				if (!It->second->bDone.load(std::memory_order_acquire)) { ++It; continue; }
				Gen.StoreIntegral(It->first, It->second->Value);
				It = IntegralJobs.erase(It);
			}
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
					C->bGrowthChecked = false;
					C->Batch.reset();
					MaxLatency = std::max<size_t>(MaxLatency, C->Frames);
					// D-139: fast attack, slow decay, clamped so one stall cannot widen every window.
					const double Obs = std::min(double(FrameNo - C->SubmittedAt), 16.0);
					LatencyEst = Obs > LatencyEst ? Obs : std::max(1.0, 0.99 * LatencyEst + 0.01 * Obs);
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
		struct FMeasured { u32 Id = 0; int LMin = 0, LMax = -1; double InfluenceCm = 0.0; FVec3d D; double Dist = 0.0; };
		double MaxInfluence = 0.0;
		std::vector<FMeasured> Measured;   // D-138: rebuilt at the start of every Update (by id: evictions later in the frame are skipped)
		std::deque<std::shared_ptr<FGenBatch>> Inflight;
		FCoord Camera;
		bool bHasCamera = false, bFirstFrame = true;
		u64 NextTicket = 0;
		u64 FrameNo = 0;
		double LatencyEst = 6.0;   // D-139: frames, submit to Ready; starts at the GPU's order
		struct FIntegralJob { std::atomic<bool> bDone{ false }; double Value = 0.0; };
		std::map<u32, std::shared_ptr<FIntegralJob>> IntegralJobs;   // D-140
		size_t BatchesSubmitted = 0, Regenerations = 0, MaxGather = 0, AllocFailures = 0, MaxLatency = 0;
	};
}
