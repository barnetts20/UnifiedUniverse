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

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <limits>
#include <thread>
#include <unordered_set>
#include "UniProfile.h"

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
		std::vector<FCellKey> CellKeys;                 // D-157: the cells that joined, in join order (CellsOf looks them up)
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
			// D-168: the cell's emitter objects (indices into Readback.Objects), each asleep (not
			// registered) until the camera comes within reach of its influence; see UpdateWake.
			struct FSleeper { FVec3d Local; double InfluenceCm = 0.0; int LMax = -1; bool bAwake = false; std::vector<u32> Ids; };   // Local: centre - cell origin (cm)
			std::vector<u32> EmitterObjects;
			std::vector<FSleeper> Sleepers;        // filled the first time the cell comes near
			double MaxInfluenceCm = 0.0;           // over its emitter objects' emitters
			int MaxChildLevel = -1;                // the coarsest level any of them emits at
			size_t AwakeCount = 0;
			// The camera may move ScanSlackCm from ScanCamera before any of the cell's objects can
			// change state (negative: scan next frame). ScanTheta: the WakeTheta it was found at.
			FCoord ScanCamera;
			double ScanSlackCm = -1.0;
			double ScanTheta = 0.0;
			size_t AngleAwake = 0;                   // D-176: objects awake for their angle only (at the last scan)
			FCellContent Readback;                   // emitter objects with indices (for contributors)
			FCellContent Anchors;                    // authored objects of this cell (anchors), CPU-known
			size_t ReadOffset = 0;                   // where this cell's read-back objects start in its batch's ReadObjects
			u32 Frames = 0;                          // frames since entering (latency stats)
			u32 DeferredFrames = 0;                  // D-173: of those, frames it was left waiting while not needed
			bool bGrowthChecked = false;             // D-138: checked against the level's current gathered set
			u64 SubmittedAt = 0;                     // D-139: frame its probe was submitted
			u64 FirstReadyAt = 0;                    // D-187: frame it first became Ready (0: never; a regeneration keeps it)
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
			for (const auto& KV : Gen.CatalogueLevels()) { Ls.insert(KV.first); for (u32 A : KV.second) { AddArch(A, AddArch, 1); } }   // D-167
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
			for (const auto& KV : Gen.CatalogueLevels())   // D-167
			{
				for (u32 A : KV.second) { ArchAt[KV.first].insert(A); Collect(A, Collect, 1); }
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
			// D-155: the largest influence radius any contributor at a level can have (from the
			// archetypes placed there and their slices' brightest magnitude, and anchors).
			for (const auto& PKV : ArchAt)
			{
				double R = 0.0;
				for (u32 PA : PKV.second)
				{
					const FArchetype& A = D.Arch(PA);
					const FSlice* S = Gen.ModelRef().SlicesOf(PA).ForLevel(PKV.first);
					const double M = S ? S->BrightM : A.AbsMag.MMin;
					for (const FUniEmitter& E : A.Emitters) { R = std::max(R, E.InfluenceMul * A.RadiusMaxCm(M)); }
				}
				LevelMaxInfluence[PKV.first] = std::max(LevelMaxInfluence[PKV.first], R);
			}
			for (const FAnchor& A : D.Anchors)
			{
				const int L = Gen.ModelRef().LevelOf(A.ArchetypeId, A.AbsMag);
				for (const FUniEmitter& E : D.Arch(A.ArchetypeId).Emitters) { LevelMaxInfluence[L] = std::max(LevelMaxInfluence[L], E.InfluenceMul * std::exp2(double(A.RadiusLog2))); }
			}
		}

		double DeltaMag = 0.0;
		bool bVelocityLod = true;
		/** D-157: this frame's phase times and work counts (reset by Update; CellTable adds). */
		mutable FStreamProfile Profile;
		int VelocityK = UniHlsl::UNI_VELOCITY_LOD_K;
		/** D-162: most forward stretch, in cells, a level's window takes for speed; a level whose
		 *  lead exceeds it is suspended (velocity LOD). */
		static constexpr int UNI_WINDOW_LEAD_MAX = 3;
		static constexpr int ShrinkAfterFrames = 30;

		/** D-140: compute normalising integrals on worker threads (runtime). Offline checks that
		 *  count frames turn it off: the same values, computed inline. */
		bool bAsyncIntegrals = true;
		/** D-168: contributors subtending at least this (rad) are awake whatever their distance, so
		 *  the volume builder finds them (it asks for those above its reject angle: the engine sets
		 *  this to half of it every frame). 1/480: half the reject angle at 960 px. */
		double WakeTheta = 1.0 / 480.0;
		/** D-176: objects to keep awake for their angle alone, about (the volume uses the largest
		 *  MaxCarriers = 128); 0: no adaptation (every object above WakeTheta, as D-168). */
		size_t WakeAngleTarget = 512;
		/** D-173: entering cells not needed within the latency plus SubmitSlackFrames are built at
		 *  most this many per level per frame (negative: no limit, as before D-173). */
		int SubmitDeferPerLevel = 48;
		double SubmitSlackFrames = 4.0;
		/** D-168: false registers every emitter object as soon as its cell is Ready (the earlier
		 *  behaviour), for A/B checks. */
		bool bLazyWake = true;
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
			Profile.BeginFrame();
			FProfileScope ScopeUpdate(Profile, EStreamPhase::Update);
			const FVec3d MoveVec = bHasCamera ? ToDouble(Sub(Coord, Camera)) : FVec3d{ 0.0, 0.0, 0.0 };
			FrameMove = MoveVec;
			Camera = Coord; bHasCamera = true;
			const double Reach = double(UniHlsl::Uni_WindowReach(float(DeltaMag)));   // D-162: 2 cells at dm 0 (5^3)
			++FrameNo;
			{ FProfileScope S(Profile, EStreamPhase::Integrals); PumpIntegrals(); }
			{ FProfileScope S(Profile, EStreamPhase::Batches); AdvanceBatches(); }
			{ FProfileScope S(Profile, EStreamPhase::Wake); UpdateWake(); }
			{ FProfileScope S(Profile, EStreamPhase::Measure); MeasureContributors(); }

			// Coarse first, so a parent's readiness is this frame's when its children look at it.
			FProfileScope ScopeLevels(Profile, EStreamPhase::Levels);
			for (auto& KV : Levels) { KV.second.bPendingValid = false; }
			for (auto It = Levels.rbegin(); It != Levels.rend(); ++It)
			{
				FLevel& Lv = It->second;
				const int L = It->first;
				Lv.bPendingValid = false;   // this level's cells change below; its list is rebuilt after
				bool bParentsReady = true;   // D-137: wait for parent levels only (D-155: for the cells that can reach this one)
				{
					FProfileScope S(Profile, EStreamPhase::ParentGate);
					for (int P : Lv.Parents) { bParentsReady = bParentsReady && ParentReadyFor(Levels.at(P), P, L); }
				}
				// D-162 (replaces D-139's symmetric widening): the lead is how far the camera moves, in
				// this level's cells, while a cell fills (plus the frame the window trails by), per
				// axis. The window stretches forward by it along the direction of travel only, so the
				// cost grows linearly with speed instead of cubically, and each shift still brings in
				// one face. Past UNI_WINDOW_LEAD_MAX cells of lead the level is suspended (velocity
				// LOD): its objects would cross the screen faster than they can be generated.
				const double W = CellWidthCm(L);
				const double MoveCells[3] = { MoveVec.X / W, MoveVec.Y / W, MoveVec.Z / W };
				double LeadPos[3] = { 0.0, 0.0, 0.0 }, LeadNeg[3] = { 0.0, 0.0, 0.0 };
				double Lead = 0.0;
				if (bVelocityLod)
				{
					// Growing a window never at the cost of failing cells: above 70% pool use the
					// stretch is dropped (suspension, as before D-139, takes over).
					const bool bRoom = Pool.UsedCount() < u32(0.7 * double(Backend.PoolCapacity()));
					for (int A = 0; A < 3; ++A)
					{
						const double La = MoveCells[A] * (LatencyEst + 1.0);
						Lead = std::max(Lead, std::abs(La));
						if (!bRoom) { continue; }
						(La > 0.0 ? LeadPos[A] : LeadNeg[A]) = std::min(std::abs(La), double(UNI_WINDOW_LEAD_MAX));
					}
				}
				// Shrink hysteresis: a side grows at once and shrinks one cell per ShrinkAfterFrames.
				int MinSide[3] = { 1, 1, 1 };
				if (Lv.Win.IsValid())
				{
					bool bWantShrink = false;
					for (int A = 0; A < 3; ++A)
					{
						const int Need = WindowSide(Reach + LeadNeg[A], Reach + LeadPos[A]);
						const int Cur = Lv.Win.Geometry().Side(A);
						MinSide[A] = Cur;
						if (Need < Cur) { bWantShrink = true; }
					}
					if (bWantShrink && ++Lv.ShrinkFrames >= ShrinkAfterFrames)
					{
						Lv.ShrinkFrames = 0;
						for (int A = 0; A < 3; ++A) { MinSide[A] = std::max(WindowSide(Reach + LeadNeg[A], Reach + LeadPos[A]), Lv.Win.Geometry().Side(A) - 1); }
					}
					if (!bWantShrink) { Lv.ShrinkFrames = 0; }
				}
				bool bSusp = bVelocityLod && Lv.Lod.UpdateLead(Lead, double(UNI_WINDOW_LEAD_MAX));
				const bool bJump = Lv.Win.IsValid() && CentreJump(Lv.Win.Geometry(), CellOf(Camera, L)) > 1;
				if (bSusp) { Lv.bHeld = true; }
				else if (Lv.bHeld && bJump && Lv.Fade > 0.0f) { bSusp = true; }
				else { Lv.bHeld = false; }
				// D-128: the fade is "the fraction of this level's sprites drawn", which the volume
				// complements, so it cannot rise while visible cells are still generating (they draw
				// nothing): a filling level shows as glow, then fades in whole. D-139 amendment: only
				// cells that can hold visible objects count, those within ceil(N_eff k) of the
				// window centre. At rest (N = 2) that is the whole window, as before; in a window
				// widened for speed, the outer shell (beyond relevance by assignment) still filling
				// no longer holds the fade at 0 for as long as the camera keeps moving.
				const float Up = (Lv.Win.IsValid() && VisibleReady(Lv)) ? 0.125f : 0.0f;   // an unbuilt window is not "ready"
				Lv.Fade = bSusp ? std::max(0.0f, Lv.Fade - 0.125f) : std::min(1.0f, Lv.Fade + Up);
				if (bSusp && Lv.Win.IsValid()) { continue; }

				// D-126: a sensitivity resize keeps every cell still in the window, Ready ones
				// included; only the new outer shell is generated. Fade is not reset.
				bool bResized = false;
				std::unordered_map<FCellKey, FCell, FCellKeyHasher> Kept;
				const FWindowGeometry Next = PlanWindow(L, Camera, Reach, LeadPos, LeadNeg, MinSide, &Lv.Win);
				if (!Lv.Win.IsValid() || !Lv.Win.Geometry().SameShape(Next))
				{
					// A new shape (sensitivity, stretch): every cell still inside is kept (D-126).
					if (Lv.Win.IsValid()) { Kept = std::move(Lv.Cells); Lv.Cells.clear(); bResized = true; }
					Lv.Win = FLevelWindow(Next);
				}
				std::vector<const FContributor*> Gathered;
				std::vector<FReachBox> Boxes;
				{ FProfileScope S(Profile, EStreamPhase::Gather); Gathered = Gather(Next, &Boxes); }
				Profile.Gathered += Gathered.size();
				MaxGather = std::max(MaxGather, Gathered.size());
				if (Gathered.empty() && !AnchorInWindow(Next))
				{
					for (auto& KV : Kept) { Release(KV.second); }
					if (Lv.Win.IsValid()) { EvictAll(Lv); Lv.Win = FLevelWindow(Next); }
					Lv.GatherIds.clear();
					continue;
				}
				// D-138: Ready cells only need re-checking for new contributors when the gathered
				// set gained some (or the cell became Ready since the last check). D-168: only
				// against the ids gained since the set the checked cells were last checked against
				// (GatherIds, updated when the check runs: a frame whose parents were not ready
				// used to update it without checking), so a frame of fast travel no longer
				// re-tests every contributor of every Ready cell.
				std::vector<u32> Ids;
				Ids.reserve(Gathered.size());
				for (const FContributor* Ct : Gathered) { Ids.push_back(Ct->Id); }
				std::sort(Ids.begin(), Ids.end());   // Gather's order follows distance, which changes as the camera moves
				std::vector<u32> NewIds;
				std::set_difference(Ids.begin(), Ids.end(), Lv.GatherIds.begin(), Lv.GatherIds.end(), std::back_inserter(NewIds));
				const bool bGatherGrew = !NewIds.empty();
				{
					FProfileScope S(Profile, EStreamPhase::Window);
					const FWindowDiff Diff = Lv.Win.Recentre(Next);
					if (((Diff.bRebuilt && !bResized) || bJump) && !bFirstFrame) { Lv.Fade = 0.0f; }
					for (const FCellKey& K : Diff.Exiting) { Evict(Lv, K); }
					for (const FWindowCell& C : Diff.Entering)
					{
						auto K = Kept.find(C.Key);
						if (K != Kept.end()) { Lv.Cells[C.Key] = std::move(K->second); Kept.erase(K); continue; }
						Lv.Cells[C.Key] = FCell(); Lv.Cells[C.Key].Key = C.Key;
					}
					for (auto& KV : Kept) { Release(KV.second); }   // shrank out of the window
				}

				if (bParentsReady)
				{
					std::vector<FCell*> ToBuild;
					std::vector<FCell*> Regenerated;
					FCellBins Bins;
					bool bBinned = false;
					auto EnsureBins = [&]() -> const FCellBins& { if (!bBinned) { Bins = BinGathered(Next, Gathered, Boxes); bBinned = true; } return Bins; };
					{
						FProfileScope S(Profile, EStreamPhase::Growth);
						for (auto& KV : Lv.Cells)
						{
							FCell& C = KV.second;
							if (C.State == ECellState::Waiting) { ToBuild.push_back(&C); continue; }
							if (C.State != ECellState::Ready || (C.bGrowthChecked && !bGatherGrew)) { continue; }
							const bool bDelta = C.bGrowthChecked;
							C.bGrowthChecked = true;
							++Profile.GrowthChecks;
							if (ContributorsGrew(C, EnsureBins(), Next, bDelta ? &NewIds : nullptr)) { ++Regenerations; Release(C); C.State = ECellState::Waiting; C.bGrowthChecked = false; Regenerated.push_back(&C); }
						}
					}
					Lv.GatherIds = std::move(Ids);   // every Ready cell is now checked against it
					DeferSubmissions(Lv, Next, ToBuild);   // D-173
					ToBuild.insert(ToBuild.end(), Regenerated.begin(), Regenerated.end());   // never deferred: they were drawn
					if (!ToBuild.empty()) { FProfileScope S(Profile, EStreamPhase::Submit); SubmitCells(ToBuild, EnsureBins(), Next); }
				}
				for (auto& KV : Lv.Cells) { ++KV.second.Frames; }
			}
			bFirstFrame = false;
		}

		/** Per-frame cell table for project-and-cull: Ready cells in windows (D-104 deltas).
		 *  OutKeys, if given, receives each entry's cell key in the same order (D-142: a system
		 *  activation read back from the GPU names its cell by index into this frame's table). */
		std::vector<FCellGPU> CellTable(std::vector<FCellKey>* OutKeys = nullptr) const
		{
			FProfileScope Scope(Profile, EStreamPhase::CellTable);
			std::vector<FCellGPU> Out;
			if (OutKeys) { OutKeys->clear(); }
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
					// D-187: bits 16..23, frames since the cell first became Ready (255: settled): the
					// sprite pass fades a late cell's objects in as warp streaks (render only).
					const u32 Age = u32(std::min<u64>(C.FirstReadyAt > 0u && FrameNo >= C.FirstReadyAt ? FrameNo - C.FirstReadyAt : 255u, 255u));
					G.Flags = (Lv.Lod.bSuspended ? 1u : 0u) | (Fade << 8) | (Age << 16);
					Out.push_back(G);
					if (OutKeys) { OutKeys->push_back(C.Key); }
				}
			}
			Profile.CellTableEntries += Out.size();
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

		// Volume inputs (UniVolume.h, FVolumeSource): resident contributors, the parent object a
		// contributor came from, and level fades.
		const std::map<u32, FContributor>& Contributors() const { return Registered; }

		/** D-161: the registered contributors whose influence radius subtends more than
		 *  MinTheta (rad) from the camera, from this frame's measured distances (within
		 *  MeasureErr), sorted by id. No map walk and no 128-bit subtraction per contributor.
		 *  Valid after Update (evicted ones are skipped). */
		std::vector<const FContributor*> NearContributors(double MinTheta) const
		{
			std::vector<const FContributor*> Out;
			for (const FMeasured& M : Measured)
			{
				const double Rm = M.Dist - MeasureErr;
				if (Rm > 0.0 && M.InfluenceCm <= MinTheta * Rm) { continue; }
				if (!M.Ptr || PendingRemove.count(M.Id)) { continue; }
				Out.push_back(M.Ptr);
			}
			std::sort(Out.begin(), Out.end(), [](const FContributor* A, const FContributor* B) { return A->Id < B->Id; });
			return Out;
		}
		/** D-174: every resident emitter object of archetype ArchId (Ready cells), asleep or awake
		 *  (D-168: Contributors() holds only the awake ones), as Fn(centre, object). */
		template <typename F>
		void ForEachEmitterObject(u32 ArchId, F&& Fn) const
		{
			for (const auto& LKV : Levels)
			{
				for (const auto& CKV : LKV.second.Cells)
				{
					const FCell& C = CKV.second;
					if (C.State != ECellState::Ready) { continue; }
					for (u32 I : C.EmitterObjects)
					{
						const FObjectGPU& O = C.Readback.Objects[I];
						if ((O.A[3] & 0xFFFFu) != ArchId) { continue; }
						const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
						Fn(ObjectCoord(C.Key, Off), O);
					}
				}
			}
		}

		/** D-177: every resident object of archetype ArchId (Ready cells), emitter or not (groups
		 *  are labels with no emitter now), as Fn(centre, object). Walks every object of the levels
		 *  the archetype is placed at: for console commands, not per frame. */
		template <typename F>
		void ForEachObjectOf(u32 ArchId, F&& Fn) const
		{
			for (const auto& LKV : Levels)
			{
				for (const auto& CKV : LKV.second.Cells)
				{
					const FCell& C = CKV.second;
					if (C.State != ECellState::Ready) { continue; }
					for (const FObjectGPU& O : C.Readback.Objects)
					{
						if ((O.A[3] & 0xFFFFu) != ArchId || ((O.A[3] >> 16) & UniHlsl::UNI_OBJ_FLAG_DELTA_HIDDEN) != 0u) { continue; }
						const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
						Fn(ObjectCoord(C.Key, Off), O);
					}
				}
			}
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
		/** D-140 for the volume builder: true if C's normalising integral is known; otherwise starts
		 *  its worker job (once) and returns false, so the carrier waits instead of hitching. */
		bool RequestIntegral(const FContributor& C) const { return IntegralReady(C); }

	private:
		struct FLevel
		{
			explicit FLevel(int L) : Win(L, UniHlsl::UNI_WINDOW_N) {}
			FLevelWindow Win;
			FVelocityLod Lod;
			float Fade = 0.0f;   // D-128: fades in once the first fill is Ready
			bool bHeld = false;
			std::unordered_map<FCellKey, FCell, FCellKeyHasher> Cells;
			std::vector<int> Parents;      // D-137: levels whose objects can be contributors here
			std::vector<u32> GatherIds;    // D-138: ids of last frame's gathered set
			int ShrinkFrames = 0;          // D-139: slow frames since the window could shrink
			// D-168: the level's cells not yet Ready (nor Failed), listed once per frame after the
			// level is processed, for every finer level's parent gate (was a walk of every cell
			// per child level per frame).
			struct FPendingCell { FVec3d Off; double Len = 0.0; };   // origin - camera, and its length
			mutable std::vector<FPendingCell> Pending;
			mutable bool bPendingValid = false;
		};

		static bool LevelReady(const FLevel& Lv)
		{
			for (const auto& KV : Lv.Cells) { if (KV.second.State != ECellState::Ready && KV.second.State != ECellState::Failed) { return false; } }
			return true;
		}

		/** D-128 with the D-139 amendment: every cell within ceil(N_eff k) of the window centre
		 *  (Chebyshev, in cells) is Ready or Failed. */
		bool VisibleReady(const FLevel& Lv) const
		{
			const FWindowGeometry& G = Lv.Win.Geometry();
			const double SensScale = double(UniHlsl::Uni_SensitivityScale(float(DeltaMag)));
			const int64_t R = int64_t(std::ceil(double(UniHlsl::UNI_WINDOW_N_EFF) * SensScale - 1.0e-5));
			for (const auto& KV : Lv.Cells)
			{
				if (KV.second.State == ECellState::Ready || KV.second.State == ECellState::Failed) { continue; }
				if (CentreJump(G, KV.first) <= R) { return false; }
			}
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
			if (Gen.CatalogueAtLevel(G.Level)) { return true; }   // D-167: a catalogue repeats every tile
			for (size_t I = 0; I < Gen.AnchorLevelList().size(); ++I)
			{
				if (Gen.AnchorLevelList()[I] != G.Level) { continue; }
				for (const FWindowCell& C : G.Cells()) { if (C.Key == Gen.AnchorCellList()[I]) { return true; } }
			}
			return false;
		}

		/** D-161: the window cells a gathered contributor can reach, as a box of offsets from the
		 *  window's centre cell (within the window's [Lo, Hi]); bWide: test it against every cell. A superset
		 *  (the exact Gen.Overlaps still decides), so binning never changes a result. */
		struct FReachBox { int Lo[3] = { 0, 0, 0 }; int Hi[3] = { 0, 0, 0 }; bool bWide = true; };

		/** D-161: gathered contributors binned by the window cells they can reach, so a cell tests
		 *  only its own candidates: before, every cell tested every gathered contributor
		 *  (2,700 cells x 1,700 contributors = 1.25M exact tests, ~170 ms, when a window shifted in
		 *  a cluster). */
		struct FCellBins
		{
			int Lo[3] = { 0, 0, 0 }, Hi[3] = { 0, 0, 0 };
			bool bActive = false;                                 // false: every contributor is wide
			const std::vector<const FContributor*>* All = nullptr;
			std::vector<const FContributor*> Wide;
			std::vector<std::vector<const FContributor*>> Bins;   // by window offset
			int Side(int A) const { return Hi[A] - Lo[A] + 1; }
			int Index(int Dx, int Dy, int Dz) const { return ((Dx - Lo[0]) * Side(1) + (Dy - Lo[1])) * Side(2) + (Dz - Lo[2]); }
		};

		FCellBins BinGathered(const FWindowGeometry& G, const std::vector<const FContributor*>& Gathered, const std::vector<FReachBox>& Boxes) const
		{
			FCellBins B;
			for (int A = 0; A < 3; ++A) { B.Lo[A] = G.Lo[A]; B.Hi[A] = G.Hi[A]; }
			B.All = &Gathered;
			B.bActive = !G.AxisCovered() && Boxes.size() == Gathered.size();
			if (!B.bActive) { return B; }
			B.Bins.resize(size_t(B.Side(0)) * B.Side(1) * B.Side(2));
			for (size_t I = 0; I < Gathered.size(); ++I)
			{
				const FReachBox& X = Boxes[I];
				const long long Vol = X.bWide ? 0 : (long long)(X.Hi[0] - X.Lo[0] + 1) * (X.Hi[1] - X.Lo[1] + 1) * (X.Hi[2] - X.Lo[2] + 1);
				if (X.bWide || Vol > 64) { B.Wide.push_back(Gathered[I]); continue; }
				for (int A = X.Lo[0]; A <= X.Hi[0]; ++A) for (int Bv = X.Lo[1]; Bv <= X.Hi[1]; ++Bv) for (int C = X.Lo[2]; C <= X.Hi[2]; ++C)
				{
					B.Bins[size_t(B.Index(A, Bv, C))].push_back(Gathered[I]);
				}
			}
			return B;
		}

		/** The candidates for one cell: the wide contributors plus the cell's bin. */
		template <typename F> void ForEachCandidate(const FCellBins& B, const FWindowGeometry& G, const FCellKey& K, F&& Fn) const
		{
			int D[3] = { 0, 0, 0 };
			bool bInside = B.bActive;
			for (int A = 0; A < 3 && bInside; ++A)
			{
				const FU128 Df = Sub(K.C[A], G.Centre.C[A]);   // wraps at the level seam like the window
				const bool bSmall = (Df.L[0] == 0u && Df.L[1] == 0u && Df.L[2] == 0u) || (Df.L[0] == ~0u && Df.L[1] == ~0u && Df.L[2] == ~0u);
				D[A] = int(int32_t(Df.L[3]));
				bInside = bSmall && D[A] >= B.Lo[A] && D[A] <= B.Hi[A];
			}
			if (!bInside)
			{
				// Not binned (covered axes) or not in this window: every gathered contributor.
				if (B.All) { for (const FContributor* Ct : *B.All) { Fn(Ct); } }
				return;
			}
			for (const FContributor* Ct : B.Wide) { Fn(Ct); }
			for (const FContributor* Ct : B.Bins[size_t(B.Index(D[0], D[1], D[2]))]) { Fn(Ct); }
		}

		std::vector<const FContributor*> Gather(const FWindowGeometry& G, std::vector<FReachBox>* OutBoxes = nullptr) const
		{
			std::vector<const FContributor*> Out;
			const int L = G.Level;
			for (const FContributor& R : Gen.RootContributors())
			{
				const FSliceTable& T = Gen.ModelRef().SlicesOf(R.Emitter->ChildArchetypeId);
				if (L >= T.LMin && L <= T.LMax) { Out.push_back(&R); if (OutBoxes) { OutBoxes->push_back(FReachBox{}); } }
			}
			const double W = CellWidthCm(L);
			// D-138: distances from the window origin through the per-frame camera deltas,
			// (contributor - camera) - (origin - camera), in double. This is a prefilter only:
			// the exact overlap test (Gen.Overlaps) decides tasks, so it is made conservative by a
			// margin far above the rounding (relative 1e-12 of the distances involved).
			const FVec3d Ow = ToDouble(Sub(CellOrigin(G.Centre), Camera));
			// D-162: the window's box per axis, from the centre cell's origin.
			double Lo[3], Hi[3], Ext = 0.0;
			for (int A = 0; A < 3; ++A) { Lo[A] = G.Lo[A] * W; Hi[A] = (G.Hi[A] + 1) * W; Ext = std::max({ Ext, std::abs(Lo[A]), std::abs(Hi[A]) }); }
			// Measured is sorted by distance from the camera: nothing beyond the window's bounding
			// sphere plus the largest influence can reach it.
			const bool bCovered = G.AxisCovered();
			const double Reach = Length(Ow) + 1.7320508075688772 * Ext + MaxInfluence;
			const double Cut = Reach * (1.0 + 1.0e-9) + 1.0 + MeasureErr;
			for (const FMeasured& M : Measured)
			{
				if (!bCovered && M.Dist > Cut) { break; }
				if (L < M.LMin || L > M.LMax) { continue; }
				FReachBox Box;
				if (!bCovered)
				{
					const double P[3] = { M.D.X - Ow.X, M.D.Y - Ow.Y, M.D.Z - Ow.Z };
					const double Margin = 1.0e-12 * (M.Dist + Length(Ow) + Ext) + 1.0 + MeasureErr;
					double Dist2 = 0.0;
					for (int A = 0; A < 3; ++A) { const double V = P[A]; const double E = V < Lo[A] ? Lo[A] - V : (V > Hi[A] ? V - Hi[A] : 0.0); Dist2 += E * E; }
					const double R = M.InfluenceCm + Margin;
					if (Dist2 > R * R) { continue; }
					// D-161: the cells its sphere's bounding box touches, one cell of slack each way.
					Box.bWide = false;
					for (int A = 0; A < 3; ++A)
					{
						const double CLo = std::floor((P[A] - R) / W) - 1.0, CHi = std::floor((P[A] + R) / W) + 1.0;
						Box.Lo[A] = int(std::clamp(CLo, double(G.Lo[A]), double(G.Hi[A])));
						Box.Hi[A] = int(std::clamp(CHi, double(G.Lo[A]), double(G.Hi[A])));
					}
				}
				// Looked up again: a coarser level may have evicted (unregistered) it this frame.
				auto It = Registered.find(M.Id);
				if (It != Registered.end()) { Out.push_back(&It->second); if (OutBoxes) { OutBoxes->push_back(Box); } }
			}
			return Out;
		}

		/** D-138: every registered contributor's offset from the camera, once per frame. */
		/** D-138, made incremental by D-155: each contributor's offset from a measure anchor near
		 *  the camera is computed exactly once, when it registers; per frame its camera offset is
		 *  one double subtraction. The anchor re-bases (one exact pass, in place: D-168) when the
		 *  camera is 300 Mpc from it (D-168: was 30, a 15-40 ms pass every 18 frames at 100 Mpc/s;
		 *  the rounding at 300 Mpc is ~1e12 cm, inside every margin). The rounding of the two-step offset, at most 4 eps (|offset| + |camera
		 *  offset|), is added to every margin that uses it (MeasureErr), so Gather stays a
		 *  superset of the exact answer and the exact overlap test still decides. */
		void PushMeasureEntry(u32 Id, const FContributor& C)
		{
			FMeasured M;
			M.Id = Id;
			M.InfluenceCm = C.InfluenceCm;
			const FSliceTable& T = Gen.ModelRef().SlicesOf(C.Emitter->ChildArchetypeId);
			M.LMin = T.LMin; M.LMax = T.LMax;
			M.Off = ToDouble(Sub(C.Centre, MeasureAnchor));
			M.Ptr = &C;   // a std::map node: stable until it is erased (then its id is in PendingRemove)
			Measured.push_back(M);
			MeasuredIds.insert(Id);
		}

		void MeasureContributors()
		{
			const double Rebase = 300.0 * 3.0856775814913673e24;   // 300 Mpc (D-168)
			const bool bRebase = !bMeasureAnchor || Length(ToDouble(Sub(Camera, MeasureAnchor))) > Rebase;
			if (!bMeasureAnchor)
			{
				MeasureAnchor = Camera;
				bMeasureAnchor = true;
				Measured.clear();
				MeasuredIds.clear();
				Measured.reserve(Registered.size());
				for (const auto& KV : Registered) { PushMeasureEntry(KV.first, KV.second); }
			}
			else
			{
				if (!PendingRemove.empty())
				{
					size_t W = 0;
					for (size_t I = 0; I < Measured.size(); ++I)
					{
						if (PendingRemove.count(Measured[I].Id)) { MeasuredIds.erase(Measured[I].Id); continue; }
						Measured[W++] = Measured[I];
					}
					Measured.resize(W);
				}
				if (bRebase)
				{
					// D-168: in place, in the list's own order (nearly sorted still), each offset
					// exact again from its contributor (Ptr is valid: removals are filtered above).
					// Was a rebuild from the map in id order, which then needed a full sort.
					MeasureAnchor = Camera;
					for (FMeasured& M : Measured) { M.Off = ToDouble(Sub(M.Ptr->Centre, MeasureAnchor)); }
				}
				for (u32 Id : PendingAdd)
				{
					if (MeasuredIds.count(Id)) { continue; }
					auto It = Registered.find(Id);
					if (It == Registered.end()) { continue; }
					PushMeasureEntry(Id, It->second);
				}
			}
			PendingAdd.clear();
			PendingRemove.clear();
			const FVec3d Cam = ToDouble(Sub(Camera, MeasureAnchor));
			MaxInfluence = 0.0;
			double MaxOff = 0.0;
			for (FMeasured& M : Measured)
			{
				M.D = FVec3d{ M.Off.X - Cam.X, M.Off.Y - Cam.Y, M.Off.Z - Cam.Z };
				M.Dist = Length(M.D);
				MaxInfluence = std::max(MaxInfluence, M.InfluenceCm);
				MaxOff = std::max(MaxOff, Length(M.Off));
			}
			MeasureErr = 4.0 * 2.220446049250313e-16 * (MaxOff + Length(Cam)) + 1.0;
			// The list stays in last frame's order, which is nearly sorted: insertion sort is
			// linear then. A budget bounds it (a jump or fast travel reorders much of it): past it, std::sort.
			auto Less = [](const FMeasured& A, const FMeasured& B) { return A.Dist != B.Dist ? A.Dist < B.Dist : A.Id < B.Id; };
			size_t Moves = 0;
			const size_t Budget = 2 * Measured.size() + 64;   // D-168: past ~2 moves each, a full sort is cheaper
			bool bSorted = true;
			for (size_t I = 1; I < Measured.size(); ++I)
			{
				if (!Less(Measured[I], Measured[I - 1])) { continue; }
				FMeasured V = Measured[I];
				size_t J = I;
				while (J > 0 && Less(V, Measured[J - 1])) { Measured[J] = Measured[J - 1]; --J; if (++Moves > Budget) { break; } }
				Measured[J] = V;
				if (Moves > Budget) { bSorted = false; break; }
			}
			if (!bSorted) { std::sort(Measured.begin(), Measured.end(), Less); }
		}

		/** D-155: is every cell of parent level Lp that could hold a contributor reaching level L's
		 *  widest possible window (N_max) Ready? A cell that cannot reach it holds nothing level L
		 *  needs, so L need not wait for it: before, every galaxy level stalled while a new slab of
		 *  200 Mpc cluster cells, 200-300 Mpc away, filled (T2.stream.responsive, 30 Mpc/s). A
		 *  contributor at Lp lies inside its cell and reaches at most LevelMaxInfluence[Lp]. */
		bool ParentReadyFor(const FLevel& Lp, int LpLevel, int L) const
		{
			if (!Lp.bPendingValid)
			{
				Lp.Pending.clear();
				for (const auto& KV : Lp.Cells)
				{
					if (KV.second.State == ECellState::Ready || KV.second.State == ECellState::Failed) { continue; }
					FLevel::FPendingCell P;
					P.Off = ToDouble(Sub(CellOrigin(KV.first), Camera));
					P.Len = Length(P.Off);
					Lp.Pending.push_back(P);
				}
				Lp.bPendingValid = true;
			}
			const FParentReach Pr = ParentReachOf(LpLevel, L);
			for (const FLevel::FPendingCell& Pc : Lp.Pending) { if (Pr.Reaches(Pc.Off, Pc.Len)) { return false; } }
			return true;
		}

		/** D-155: the box of level L's widest possible window around the camera, and how far a
		 *  contributor at level LpLevel can reach (its level's largest influence): a parent cell
		 *  whose box comes within that reach can hold something L needs. */
		struct FParentReach
		{
			double CLo[3], CHi[3], Wp, NW, LenOc, Reach;
			bool Reaches(const FVec3d& Op, double LenOp) const
			{
				const double PLo[3] = { Op.X, Op.Y, Op.Z };
				double D2 = 0.0;
				for (int A = 0; A < 3; ++A)
				{
					const double Gap = std::max({ 0.0, CLo[A] - (PLo[A] + Wp), PLo[A] - CHi[A] });
					D2 += Gap * Gap;
				}
				const double Margin = 1.0e-9 * (LenOp + LenOc + Wp + NW) + 1.0;
				return D2 <= (Reach + Margin) * (Reach + Margin);
			}
		};
		FParentReach ParentReachOf(int LpLevel, int L) const
		{
			FParentReach R;
			auto It = LevelMaxInfluence.find(LpLevel);
			R.Reach = It == LevelMaxInfluence.end() ? std::numeric_limits<double>::infinity() : It->second;
			const double W = CellWidthCm(L);
			R.Wp = CellWidthCm(LpLevel);
			const FVec3d Oc = ToDouble(Sub(CellOrigin(CellOf(Camera, L)), Camera));
			const double N = double(UniHlsl::UNI_WINDOW_N_MAX + UNI_WINDOW_LEAD_MAX);   // D-162: the widest a stretched window gets
			R.NW = N * W;
			R.CLo[0] = Oc.X - N * W; R.CLo[1] = Oc.Y - N * W; R.CLo[2] = Oc.Z - N * W;
			R.CHi[0] = Oc.X + (N + 1.0) * W; R.CHi[1] = Oc.Y + (N + 1.0) * W; R.CHi[2] = Oc.Z + (N + 1.0) * W;
			R.LenOc = Length(Oc);
			return R;
		}

		/** D-173: spread entering cells over frames. A window shift, a sensitivity resize or the
		 *  stretch growing brings in hundreds of cells per level, and several levels shift on the
		 *  same frame (their boundaries nest), so task building and, a few frames later, their
		 *  readiness came in bursts of 500-1,100 cells. A waiting cell's slack is how many frames
		 *  the camera needs, at this frame's speed, to bring it inside the visible radius (its
		 *  Chebyshev gap beyond the radius over the fastest axis's cells per frame: no faster than
		 *  that). Cells with slack within the measured latency plus SubmitSlackFrames go now;
		 *  of the rest, at most SubmitDeferPerLevel per level per frame, least slack first; the
		 *  others stay Waiting (undrawn: they were never Ready). Content is unchanged (a cell's
		 *  objects do not depend on when it is built). Regenerated cells are never deferred. */
		void DeferSubmissions(const FLevel& Lv, const FWindowGeometry& G, std::vector<FCell*>& InOut)
		{
			if (SubmitDeferPerLevel < 0 || InOut.empty()) { return; }
			const double SensScale = double(UniHlsl::Uni_SensitivityScale(float(DeltaMag)));
			const int64_t R = int64_t(std::ceil(double(UniHlsl::UNI_WINDOW_N_EFF) * SensScale - 1.0e-5));
			const double W = CellWidthCm(G.Level);
			const double Sp = std::max(std::abs(FrameMove.X), std::max(std::abs(FrameMove.Y), std::abs(FrameMove.Z))) / W;
			const double Urgent = LatencyEst + SubmitSlackFrames;
			// A cell a finer level's parent gate would wait on is needed now too (deferring it would
			// stall that level: D-137/D-155).
			std::vector<FParentReach> Children;
			for (const auto& KV : Levels)
			{
				if (KV.first == G.Level || KV.second.Lod.bSuspended) { continue; }
				if (std::find(KV.second.Parents.begin(), KV.second.Parents.end(), G.Level) != KV.second.Parents.end()) { Children.push_back(ParentReachOf(G.Level, KV.first)); }
			}
			std::vector<FCell*> Now;
			std::vector<std::pair<double, FCell*>> Later;
			for (FCell* C : InOut)
			{
				const int64_t Gap = std::max<int64_t>(CentreJump(G, C->Key) - R, 0);
				double Slack = Gap == 0 ? 0.0 : (Sp > 1.0e-12 ? double(Gap) / Sp : 1.0e30);
				if (Slack > Urgent && !Children.empty())
				{
					const FVec3d Op = ToDouble(Sub(CellOrigin(C->Key), Camera));
					const double LenOp = Length(Op);
					for (const FParentReach& Pr : Children) { if (Pr.Reaches(Op, LenOp)) { Slack = 0.0; break; } }
				}
				if (Slack <= Urgent) { Now.push_back(C); }
				else { Later.push_back({ Slack, C }); }
			}
			(void)Lv;
			if (Later.size() > size_t(SubmitDeferPerLevel))
			{
				std::stable_sort(Later.begin(), Later.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
				Profile.CellsDeferred += Later.size() - size_t(SubmitDeferPerLevel);
				for (size_t I = size_t(SubmitDeferPerLevel); I < Later.size(); ++I) { ++Later[I].second->DeferredFrames; }
				Later.resize(size_t(SubmitDeferPerLevel));
			}
			for (const auto& Lt : Later) { Now.push_back(Lt.second); }
			InOut = std::move(Now);
		}

		/** Does a gathered contributor overlap C that C was not generated with? OnlyNew (sorted):
		 *  test only those ids (D-168: the cell was checked against every other one already;
		 *  the overlap test is exact, so the answer is the same). Ids C already has are not
		 *  tested at all. */
		bool ContributorsGrew(const FCell& C, const FCellBins& Bins, const FWindowGeometry& G, const std::vector<u32>* OnlyNew = nullptr) const
		{
			bool bGrew = false;
			ForEachCandidate(Bins, G, C.Key, [&](const FContributor* Ct)
			{
				if (bGrew) { return; }
				if (OnlyNew && !std::binary_search(OnlyNew->begin(), OnlyNew->end(), Ct->Id)) { return; }
				if (std::binary_search(C.ContributorSet.begin(), C.ContributorSet.end(), Ct->Id)) { return; }
				++Profile.OverlapTests;
				if (Gen.Overlaps(*Ct, C.Key)) { bGrew = true; }
			});
			return bGrew;
		}

		/** Build tasks for the given cells and submit their probe phase as one batch. */
		void SubmitCells(const std::vector<FCell*>& Cells, const FCellBins& Bins, const FWindowGeometry& G)
		{
			auto B = std::make_shared<FGenBatch>();
			B->Ticket = ++NextTicket;
			size_t Joined = 0;
			for (FCell* C : Cells)
			{
				std::vector<const FContributor*> Mine;
				ForEachCandidate(Bins, G, C->Key, [&](const FContributor* Ct) { ++Profile.OverlapTests; if (Gen.Overlaps(*Ct, C->Key)) { Mine.push_back(Ct); } });
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
				const bool bHasAnchor = Gen.HasAuthored(C->Key);   // anchors and catalogue objects (D-167)
				if (C->Tasks.empty() && !bHasAnchor)
				{
					C->Batch.reset(); C->PoolCount = 0; C->State = ECellState::Ready; C->bGrowthChecked = false;
					if (C->FirstReadyAt == 0u) { C->FirstReadyAt = FrameNo; }   // D-187
					MaxLatency = std::max<size_t>(MaxLatency, C->Frames - std::min(C->Frames, C->DeferredFrames));   // D-173: deferral is not latency
					continue;
				}
				C->Batch = B;
				B->CellKeys.push_back(C->Key);
				C->State = ECellState::Probing;
				C->SubmittedAt = FrameNo;
				++Joined;
				++Profile.CellsSubmitted;
				Profile.TasksSubmitted += C->Tasks.size();
			}
			if (Joined == 0) { return; }
			{ FProfileScope S(Profile, EStreamPhase::Backend); Backend.SubmitProbe(B); }
			Inflight.push_back(B);
			++BatchesSubmitted;
		}

		/** D-140: true if Ct's integral is known (or will be computed inline); otherwise starts
		 *  (or keeps) a worker job for it and returns false. */
		bool IntegralReady(const FContributor& Ct) const
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
				std::vector<FCell*> Mine;
				{ FProfileScope S(Profile, EStreamPhase::BatchCells); Mine = CellsOf(B); }
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
					{ FProfileScope S(Profile, EStreamPhase::BackendPlace); Backend.SubmitPlace(B); }
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
				FProfileScope ScopeRegister(Profile, EStreamPhase::Register);
				Profile.ObjectsReadBack += B->ReadObjects.size();
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
					// D-168: nothing is registered here. The emitter objects sleep until UpdateWake finds
					// the camera within reach of their influence (bulk registration of every galaxy
					// in a coarse window, 300k at once, was the intergalactic-speed hitch).
					NoteEmitterObjects(*C);
					C->State = ECellState::Ready;
					C->bGrowthChecked = false;
					C->Batch.reset();
					if (C->FirstReadyAt == 0u) { C->FirstReadyAt = FrameNo; }   // D-187: a regeneration keeps the first
					MaxLatency = std::max<size_t>(MaxLatency, C->Frames - std::min(C->Frames, C->DeferredFrames));   // D-173: deferral is not latency
					// D-139: fast attack, slow decay, clamped so one stall cannot widen every window.
					const double Obs = std::min(double(FrameNo - C->SubmittedAt), 16.0);
					LatencyEst = Obs > LatencyEst ? Obs : std::max(1.0, 0.99 * LatencyEst + 0.01 * Obs);
				}
				It = Inflight.erase(It);
			}
		}

		/** D-168: the cell's emitter objects, and the cell-level bounds the wake pass culls with. */
		void NoteEmitterObjects(FCell& C)
		{
			const FUniverseDef& D = Gen.ModelRef().Definition();
			C.EmitterObjects.clear();
			C.Sleepers.clear();
			C.AwakeCount = 0;
			C.MaxInfluenceCm = 0.0;
			C.MaxChildLevel = -1;
			C.ScanSlackCm = -1.0;
			auto LI = LevelMaxInfluence.find(C.Key.Level);
			for (size_t I = 0; I < C.Readback.Objects.size(); ++I)
			{
				const u32 Arch = C.Readback.Objects[I].A[3] & 0xFFFFu;
				const FArchetype& A = D.Arch(Arch);
				if (A.Emitters.empty()) { continue; }
				C.EmitterObjects.push_back(u32(I));
				for (const FUniEmitter& E : A.Emitters) { C.MaxChildLevel = std::max(C.MaxChildLevel, Gen.ModelRef().SlicesOf(E.ChildArchetypeId).LMax); }
			}
			// The level's bound (its archetypes' largest radius at their brightest), refined per
			// object when the cell first comes near.
			C.MaxInfluenceCm = LI == LevelMaxInfluence.end() ? std::numeric_limits<double>::infinity() : LI->second;
			if (!(C.MaxInfluenceCm < std::numeric_limits<double>::infinity()))
			{
				// No bound known for the level: measure the objects now.
				FillSleepers(C);
			}
		}

		void FillSleepers(FCell& C)
		{
			if (!C.Sleepers.empty() || C.EmitterObjects.empty()) { return; }
			const FUniverseDef& D = Gen.ModelRef().Definition();
			C.Sleepers.resize(C.EmitterObjects.size());
			double MaxI = 0.0;
			for (size_t K = 0; K < C.EmitterObjects.size(); ++K)
			{
				const FObjectGPU& O = C.Readback.Objects[C.EmitterObjects[K]];
				const FArchetype& A = D.Arch(O.A[3] & 0xFFFFu);
				const double Radius = std::exp2(double(UniHlsl::Uni_ObjectRadiusLog2(UniCore::ToHlsl(O))));
				FCell::FSleeper& Sl = C.Sleepers[K];
				const u32 Off[3] = { O.A[0], O.A[1], O.A[2] };
				Sl.Local = ToDouble(Sub(ObjectCoord(C.Key, Off), CellOrigin(C.Key)));
				for (const FUniEmitter& E : A.Emitters)
				{
					Sl.InfluenceCm = std::max(Sl.InfluenceCm, E.InfluenceMul * Radius);
					Sl.LMax = std::max(Sl.LMax, Gen.ModelRef().SlicesOf(E.ChildArchetypeId).LMax);
				}
				MaxI = std::max(MaxI, Sl.InfluenceCm);
			}
			C.MaxInfluenceCm = MaxI;
		}

		/** D-168: the farthest any window cell at level L can be from the camera: the window is at
		 *  most N_max + lead cells from the camera's cell per axis, plus that cell. */
		static double WindowReachCm(int L)
		{
			return double(UniHlsl::UNI_WINDOW_N_MAX + UNI_WINDOW_LEAD_MAX + 1) * 1.7320508075688772 * CellWidthCm(L);
		}

		/** D-168: wake and sleep contributors by distance. An emitter object is registered (its
		 *  contributors made and measured) only while it could matter this frame:
		 *    - its influence could reach a window of the levels it emits at: distance - influence
		 *      <= WindowReachCm(its coarsest child level), so every Gather sees exactly what it saw
		 *      with everything registered (the streamer stays identical to the reference), or
		 *    - it subtends WakeTheta or more (the volume builder asks for those above its reject
		 *      angle; the engine sets WakeTheta to half of it).
		 *  It sleeps again past 1.5x both. So each object wakes inside a distance WakeD and sleeps
		 *  outside SleepD > WakeD. Whole cells are culled by their bounds first, and each scan
		 *  records how far the camera can move before any of the cell's objects (or its bound)
		 *  can cross its threshold: until then the cell costs one distance per frame. Distances
		 *  only shrink by what the camera moves, so a skipped cell would have changed nothing. */
		void UpdateWake()
		{
			// D-176: the angle threshold rises in quarter-octave steps while more than
			// 2 x WakeAngleTarget objects are awake only for their angle (the volume keeps at most
			// MaxCarriers, the largest, D-161), and falls again below half the target. Never below
			// WakeTheta. The window-reach criterion (what generation needs) is untouched.
			const double Th = std::max(WakeTheta, 1.0e-9) * std::exp2(0.25 * double(WakeStep));
			size_t AngleAwake = 0;
			for (auto& LKV : Levels)
			{
				for (auto& CKV : LKV.second.Cells)
				{
					FCell& C = CKV.second;
					if (C.State != ECellState::Ready || C.EmitterObjects.empty()) { continue; }
					if (!bLazyWake)
					{
						if (C.AwakeCount < C.EmitterObjects.size())
						{
							FillSleepers(C);
							for (size_t K = 0; K < C.Sleepers.size(); ++K) { if (!C.Sleepers[K].bAwake) { WakeUp(C, K); } }
						}
						continue;
					}
					if (C.ScanSlackCm > 0.0 && std::abs(Th - C.ScanTheta) <= 1.0e-3 * C.ScanTheta
						&& Length(ToDouble(Sub(Camera, C.ScanCamera))) < C.ScanSlackCm)
					{
						Profile.Asleep += C.EmitterObjects.size() - C.AwakeCount;
						AngleAwake += C.AngleAwake;
						continue;
					}
					ScanWake(C, Th);
					Profile.Asleep += C.EmitterObjects.size() - C.AwakeCount;
					AngleAwake += C.AngleAwake;
				}
			}
			Profile.Awake = Registered.size();
			Profile.AwakeByAngle = AngleAwake;
			Profile.WakeStep = WakeStep;
			if (bLazyWake && WakeAngleTarget > 0)
			{
				if (WakeStepHold > 0) { --WakeStepHold; }
				else if (AngleAwake > 2 * WakeAngleTarget && WakeStep < 64) { ++WakeStep; WakeStepHold = 4; }
				else if (AngleAwake < WakeAngleTarget / 2 && WakeStep > 0) { --WakeStep; WakeStepHold = 4; }
			}
		}

		/** One cell of UpdateWake: wakes and sleeps its objects, and records its slack. */
		void ScanWake(FCell& C, double Th)
		{
			C.ScanCamera = Camera;
			C.ScanTheta = Th;
			// Cell bound: the nearest point of the cell, the largest influence in it.
			const double W = CellWidthCm(C.Key.Level);
			const FVec3d O = ToDouble(Sub(CellOrigin(C.Key), Camera));
			double D2 = 0.0;
			const double Oa[3] = { O.X, O.Y, O.Z };
			for (int A = 0; A < 3; ++A) { const double E = Oa[A] > 0.0 ? Oa[A] : (Oa[A] + W < 0.0 ? -(Oa[A] + W) : 0.0); D2 += E * E; }
			const double Dc = std::sqrt(D2);
			const double R = C.MaxChildLevel >= 0 ? WindowReachCm(C.MaxChildLevel) : 0.0;
			const double MaxI = C.MaxInfluenceCm;
			// Every object is past its SleepD while the nearest point is past both bounds.
			const double NearD = std::max(MaxI + 1.5 * R + 1.0, 1.5 * MaxI / Th);
			if (Dc > NearD)
			{
				if (C.AwakeCount > 0) { for (FCell::FSleeper& Sl : C.Sleepers) { if (Sl.bAwake) { Sleep(C, Sl); } } }
				C.ScanSlackCm = 0.999 * (Dc - NearD);
				C.AngleAwake = 0;
				return;
			}
			FillSleepers(C);
			C.AngleAwake = 0;
			double Slack = std::numeric_limits<double>::infinity();
			for (size_t K = 0; K < C.Sleepers.size(); ++K)
			{
				FCell::FSleeper& Sl = C.Sleepers[K];
				const double Rl = Sl.LMax >= 0 ? WindowReachCm(Sl.LMax) : 0.0;
				// (centre - origin) + (origin - camera): two roundings of at most a cell width
				// and the distance, far inside the margin.
				const double Dist = Length(FVec3d{ Sl.Local.X + O.X, Sl.Local.Y + O.Y, Sl.Local.Z + O.Z });
				const double Margin = 1.0e-9 * (Dist + Rl + W) + 1.0;
				const double WakeD = std::max(Sl.InfluenceCm + Rl + Margin, Sl.InfluenceCm / Th);
				const double SleepD = std::max(Sl.InfluenceCm + 1.5 * Rl + Margin, 1.5 * Sl.InfluenceCm / Th);
				if (!Sl.bAwake && Dist <= WakeD) { WakeUp(C, K); }
				else if (Sl.bAwake && Dist > SleepD) { Sleep(C, Sl); }
				if (Sl.bAwake && Dist > Sl.InfluenceCm + 1.5 * Rl + Margin) { ++C.AngleAwake; }   // D-176: awake for its angle only
				Slack = std::min(Slack, Sl.bAwake ? SleepD - Dist : Dist - WakeD);
			}
			// The margin absorbs the coordinate rounding; a non-positive slack rescans next frame.
			C.ScanSlackCm = 0.999 * Slack - 1.0;
		}

		void WakeUp(FCell& C, size_t K)
		{
			FCell::FSleeper& Sl = C.Sleepers[K];
			std::vector<FContributor> Made;
			Gen.MakeContributorsFor(C.Readback, C.EmitterObjects[K], Made);
			Sl.Ids.clear();
			for (FContributor& Ct : Made)
			{
				Sl.Ids.push_back(Ct.Id);
				Registered[Ct.Id] = Ct;
				PendingAdd.push_back(Ct.Id);
				PendingRemove.erase(Ct.Id);
				++Profile.ContributorsAdded;
				++Profile.Woken;
			}
			Sl.bAwake = true;
			++C.AwakeCount;
		}

		void Sleep(FCell& C, FCell::FSleeper& Sl)
		{
			for (u32 Id : Sl.Ids) { Registered.erase(Id); PendingRemove.insert(Id); ++Profile.ContributorsRemoved; ++Profile.Slept; }
			Sl.Ids.clear();
			Sl.bAwake = false;
			if (C.AwakeCount > 0) { --C.AwakeCount; }
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

		/** The cells still in batch B, in join order. D-157: looked up by key (each a hash
		 *  lookup) instead of scanning every cell of every level once per in-flight batch per frame,
		 *  O(batches x resident cells). A key whose cell was evicted (or rejoined another batch)
		 *  is skipped, as the scan skipped it. */
		std::vector<FCell*> CellsOf(const std::shared_ptr<FGenBatch>& B)
		{
			std::vector<FCell*> Out;
			Out.reserve(B->CellKeys.size());
			for (const FCellKey& K : B->CellKeys)
			{
				++Profile.BatchCellScans;
				auto L = Levels.find(K.Level);
				if (L == Levels.end()) { continue; }
				auto It = L->second.Cells.find(K);
				if (It != L->second.Cells.end() && It->second.Batch == B) { Out.push_back(&It->second); }
			}
			return Out;
		}

		void Release(FCell& C)
		{
			Pool.Release(C.PoolStart, C.PoolCount);
			C.PoolStart = C.PoolCount = 0;
			for (FCell::FSleeper& Sl : C.Sleepers)
			{
				if (!Sl.bAwake) { continue; }
				for (u32 Id : Sl.Ids) { Registered.erase(Id); PendingRemove.insert(Id); }
				Profile.ContributorsRemoved += Sl.Ids.size();
			}
			C.Sleepers.clear();
			C.EmitterObjects.clear();
			C.AwakeCount = 0;
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
		struct FMeasured { u32 Id = 0; int LMin = 0, LMax = -1; double InfluenceCm = 0.0; FVec3d Off; FVec3d D; double Dist = 0.0; const FContributor* Ptr = nullptr; };
		std::map<int, double> LevelMaxInfluence;   // D-155
		FCoord MeasureAnchor;
		bool bMeasureAnchor = false;
		double MeasureErr = 1.0;
		std::vector<u32> PendingAdd;
		std::unordered_set<u32> PendingRemove;
		std::unordered_set<u32> MeasuredIds;
		double MaxInfluence = 0.0;
		std::vector<FMeasured> Measured;   // D-138: rebuilt at the start of every Update (by id: evictions later in the frame are skipped)
		std::deque<std::shared_ptr<FGenBatch>> Inflight;
		FCoord Camera;
		bool bHasCamera = false, bFirstFrame = true;
		u64 NextTicket = 0;
		u64 FrameNo = 0;
		FVec3d FrameMove{ 0.0, 0.0, 0.0 };   // D-173: this frame's camera move (cm)
		int WakeStep = 0, WakeStepHold = 0;   // D-176: angle threshold = WakeTheta x 2^(step / 4)
		double LatencyEst = 6.0;   // D-139: frames, submit to Ready; starts at the GPU's order
		struct FIntegralJob { std::atomic<bool> bDone{ false }; double Value = 0.0; };
		mutable std::map<u32, std::shared_ptr<FIntegralJob>> IntegralJobs;   // D-140 (mutable: the volume builder requests jobs through a const streamer)
		size_t BatchesSubmitted = 0, Regenerations = 0, MaxGather = 0, AllocFailures = 0, MaxLatency = 0;
	};
}
