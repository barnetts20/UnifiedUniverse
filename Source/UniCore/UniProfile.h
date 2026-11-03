#pragma once

// UniProfile.h
// Streamer instrumentation (D-157). Engine-agnostic.
//
// Every frame the streamer times its own phases with a steady clock and counts the work it did,
// into FStreamProfile. The engine reads those after the frame (uu.Perf, uu.PerfHud) and can also
// hook the phases into its own profiler (FProfileHooks: Unreal Insights' dynamic CPU events), so
// a capture shows the streamer's phases as nested scopes without UniCore including Unreal.
// Phases nest (Levels contains Gather, ParentGate, Window, Growth and Submit; Backend sits in
// Batches or Submit), so times are
// inclusive: a parent phase is not the sum of the rest.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace UniCore
{
	enum class EStreamPhase : int
	{
		Update,        // the whole of FUniStreamer::Update
		Integrals,     // collecting finished normalising-integral jobs
		Batches,       // advancing in-flight generation batches (probe -> allocate -> place -> ready)
		BatchCells,    //   finding the cells of each batch
		Register,      //   turning read-back emitter objects into contributors
		Wake,          // D-168 waking and sleeping contributors by distance
		Measure,       // D-155 contributor measurement (offsets, sort)
		Levels,        // the per-level loop
		ParentGate,    //   D-137/D-155 parent readiness
		Gather,        //   contributors reaching the window
		Window,        //   recentring, evicting, entering cells
		Growth,        //   D-113 re-checks of Ready cells against new contributors
		Submit,        //   building generation tasks for waiting cells
		Backend,       // D-168 backend submits, nested in Batches or Submit (the GPU's enqueue; the CPU reference's whole generation)
		CellTable,     // the per-frame cell table for project-and-cull
		Count
	};

	inline const char* StreamPhaseName(EStreamPhase P)
	{
		switch (P)
		{
		case EStreamPhase::Update: return "Uni.Stream.Update";
		case EStreamPhase::Integrals: return "Uni.Stream.Integrals";
		case EStreamPhase::Batches: return "Uni.Stream.Batches";
		case EStreamPhase::BatchCells: return "Uni.Stream.BatchCells";
		case EStreamPhase::Register: return "Uni.Stream.Register";
		case EStreamPhase::Wake: return "Uni.Stream.Wake";
		case EStreamPhase::Measure: return "Uni.Stream.Measure";
		case EStreamPhase::Levels: return "Uni.Stream.Levels";
		case EStreamPhase::ParentGate: return "Uni.Stream.ParentGate";
		case EStreamPhase::Gather: return "Uni.Stream.Gather";
		case EStreamPhase::Window: return "Uni.Stream.Window";
		case EStreamPhase::Growth: return "Uni.Stream.Growth";
		case EStreamPhase::Submit: return "Uni.Stream.Submit";
		case EStreamPhase::Backend: return "Uni.Stream.Backend";
		case EStreamPhase::CellTable: return "Uni.Stream.CellTable";
		default: return "Uni.Stream.?";
		}
	}

	/** Optional profiler hooks (plain function pointers: no allocation, no captures). */
	struct FProfileHooks
	{
		void (*Begin)(const char* Name) = nullptr;
		void (*End)() = nullptr;
	};

	/** One frame of streamer work. Reset at the start of every Update; CellTable (called after
	 *  Update, the same frame) adds to it. */
	struct FStreamProfile
	{
		FProfileHooks Hooks;
		double Ms[int(EStreamPhase::Count)] = {};
		uint64_t CellsSubmitted = 0;       // cells handed to a generation batch
		uint64_t TasksSubmitted = 0;       // generation tasks in those batches
		uint64_t OverlapTests = 0;         // contributor x cell overlap tests while building tasks
		uint64_t ObjectsReadBack = 0;      // emitter objects read back from the GPU
		uint64_t ContributorsAdded = 0;
		uint64_t ContributorsRemoved = 0;
		uint64_t Gathered = 0;             // contributors gathered, summed over levels
		uint64_t GrowthChecks = 0;         // Ready cells re-checked (D-113)
		uint64_t BatchCellScans = 0;       // cells visited while finding the cells of in-flight batches
		uint64_t CellTableEntries = 0;
		uint64_t Woken = 0;                // D-168: contributors registered from sleep this frame
		uint64_t Slept = 0;                // D-168: contributors put back to sleep
		uint64_t Awake = 0;                // D-168: registered contributors after the wake pass
		uint64_t Asleep = 0;               // D-168: emitter objects asleep (not registered)

		void BeginFrame()
		{
			for (double& M : Ms) { M = 0.0; }
			CellsSubmitted = TasksSubmitted = OverlapTests = ObjectsReadBack = 0;
			ContributorsAdded = ContributorsRemoved = Gathered = GrowthChecks = BatchCellScans = CellTableEntries = 0;
			Woken = Slept = Awake = Asleep = 0;
		}
	};

	/** RAII phase timer: adds its wall time to the phase and brackets it with the hooks. */
	class FProfileScope
	{
	public:
		FProfileScope(FStreamProfile& InProfile, EStreamPhase InPhase)
			: Profile(InProfile), Phase(InPhase), Start(std::chrono::steady_clock::now())
		{
			if (Profile.Hooks.Begin) { Profile.Hooks.Begin(StreamPhaseName(Phase)); }
		}
		~FProfileScope()
		{
			if (Profile.Hooks.End) { Profile.Hooks.End(); }
			Profile.Ms[int(Phase)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
		}
		FProfileScope(const FProfileScope&) = delete;
		FProfileScope& operator=(const FProfileScope&) = delete;

	private:
		FStreamProfile& Profile;
		EStreamPhase Phase;
		std::chrono::steady_clock::time_point Start;
	};
}
