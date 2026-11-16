// UniNebGridRef.h
// D-252: the nebula grid's CPU emulation: the renderer's resources (headers, table, free lists,
// counters, pool) as vectors bound to the shader cores' shim (UniHlslShim.h FNebGridShim), the
// planner's operations run through the same thread functions UniVolume.usf pass 7 runs (reset,
// release, classify + allocate, bake), in the same order. The march then reads the grid exactly
// as the GPU's (its trilinear filter emulated).
#pragma once

#include "UniNebGrid.h"

#include <atomic>
#include <thread>
#include <vector>

namespace UniVerify
{
	namespace HG = UniHlsl;

	struct FNebGridEmu
	{
		UniCore::FNebGridPlanner Planner;
		UniCore::FNebGridPlanner::FFrameOps Ops;
		std::vector<HG::float4> Headers;
		std::vector<HG::uint> Table, Free;
		std::vector<HG::float4> Pool;
		HG::uint Counters[32] = {};
		HG::FUniFieldParams Par{};
		HG::FUniGalaxyConst K{};
		float Detail = 1.0f, Dark = 0.35f;
		int Threads = 2;
		size_t Classified = 0, Empty = 0, Allocated = 0, Overflow = 0, Released = 0, Points = 0;

		/** The shape and the galaxy (its parameters, the detail and dark share the bake reads). */
		void Configure(const UniCore::FNebGridConfig& C, UniCore::u64 Key, const HG::FUniFieldParams& InPar, float InDetail, float InDark)
		{
			Par = InPar;
			K = HG::Uni_GalaxyConstOf(Par);
			Detail = InDetail;
			Dark = InDark;
			if (Planner.Configure(C, Key) || Table.empty())
			{
				const UniCore::FNebGridLayout& L = Planner.Layout();
				Table.assign(L.TableSize, HG::UNI_NEB_GRID_UNBAKED);
				Free.assign(L.Slots, 0u);
				Pool.assign(size_t(L.TexX()) * L.TexY() * L.TexZ(), HG::float4(0.0f, 0.0f, 1.0f, 1.0f));
				for (HG::uint& Cn : Counters) { Cn = 0u; }
			}
		}

		void Bind()
		{
			HG::FNebGridShim& G = HG::GNebGrid;
			const UniCore::FNebGridLayout& L = Planner.Layout();
			G.Header = Headers.data(); G.HeaderCount = HG::uint(Headers.size());
			G.Table = Table.data(); G.TableCount = HG::uint(Table.size());
			G.Free = Free.data(); G.FreeCount = HG::uint(Free.size());
			G.Pool = Pool.data(); G.PoolX = L.TexX(); G.PoolY = L.TexY(); G.PoolZ = L.TexZ();
			for (int I = 0; I < 32; ++I) { G.Counters[I] = Counters[I]; }
		}
		void Sync() { for (int I = 0; I < 32; ++I) { Counters[I] = HG::GNebGrid.Counters[I]; } }
		static void Unbind() { HG::GNebGrid = HG::FNebGridShim{}; }

		/** A frame: the plan for the camera (field units) and the layer's mid-plane, then its
		 *  operations: reset, release, classify + allocate (in order), bake. Leaves the grid bound. */
		void Frame(const double InCam[3], double InMidZ, UniCore::u32 InBudget)
		{
			Planner.Update(InCam, InMidZ, InBudget, Ops);
			Headers = Ops.Headers;
			Bind();
			const UniCore::FNebGridLayout& L = Planner.Layout();
			for (size_t Lv = 0; Lv < L.Levels.size(); ++Lv)
			{
				if (!(Ops.ResetMask & (1u << Lv))) { continue; }
				const HG::uint N = std::max(L.Levels[Lv].SlotCount, L.Levels[Lv].Entries);
				for (HG::uint I = 0; I < N; ++I) { HG::Uni_NebGridResetThread(HG::uint(Lv), I); }
			}
			for (const UniCore::FNebGridOpGPU& Op : Ops.Release) { HG::Uni_NebGridReleaseThread(ToShim(Op)); ++Released; }
			// Classify (every sample, as a pass-7 group's threads together), in parallel.
			const size_t No = Ops.Process.size();
			std::vector<std::uint8_t> Occ(No, 0u);
			std::vector<HG::uint> Slot(No, HG::UNI_NEB_GRID_UNBAKED);
			auto ParallelFor = [&](size_t N, auto&& Fn)
			{
				std::atomic<size_t> Next{ 0 };
				auto Work = [&]() { for (size_t I = Next++; I < N; I = Next++) { Fn(I); } };
				std::vector<std::thread> Ts;
				for (int T = 1; T < Threads; ++T) { Ts.emplace_back(Work); }
				Work();
				for (std::thread& T : Ts) { T.join(); }
			};
			ParallelFor(No, [&](size_t I)
			{
				const UniCore::FNebGridOpGPU& Op = Ops.Process[I];
				const UniCore::FNebGridLevelLayout& V = L.Levels[Op.LevelEntry >> 24u];
				if (V.ClassifyN == 0u) { Occ[I] = 1u; return; }
				const HG::uint N = V.ClassifyN, N3 = N * N * N;
				for (HG::uint S = 0; S < N3; ++S)
				{
					if (HG::Uni_NebGridClassifySample(float(V.Cell), Op.Wx, Op.Wy, Op.Wz, S, N, Par, K)) { Occ[I] = 1u; return; }
				}
			});
			for (size_t I = 0; I < No; ++I)
			{
				Slot[I] = HG::Uni_NebGridAllocThread(ToShim(Ops.Process[I]), Occ[I] != 0u);
				++Classified;
				if (Slot[I] == HG::UNI_NEB_GRID_EMPTY) { ++Empty; }
				else if (Slot[I] == HG::UNI_NEB_GRID_UNBAKED) { ++Overflow; }
				else { ++Allocated; }
			}
			const HG::uint Pts = HG::UNI_NEB_GRID_SLOT * HG::UNI_NEB_GRID_SLOT * HG::UNI_NEB_GRID_SLOT;
			std::vector<size_t> Work;
			for (size_t I = 0; I < No; ++I) { if (Slot[I] < HG::UNI_NEB_GRID_EMPTY) { Work.push_back(I); } }
			ParallelFor(Work.size() * Pts, [&](size_t J)
			{
				const size_t I = Work[J / Pts];
				HG::Uni_NebGridBakeThread(ToShim(Ops.Process[I]), Slot[I], HG::uint(J % Pts), Par, K, Detail, Dark);
			});
			Points += Work.size() * Pts;
			Sync();
		}

		/** The march's view fields for the grid (UniCore::FVolumeViewGPU). */
		template <typename TView>
		void ToView(TView& V, UniCore::u32 ParamIndex, float FetchMax) const
		{
			V.NebGridCount = UniCore::u32(Planner.Layout().Levels.size());
			V.NebGridParam = ParamIndex;
			V.NebGridFetchMax = FetchMax;
		}

		static HG::FUniNebGridOp ToShim(const UniCore::FNebGridOpGPU& Op) { HG::FUniNebGridOp O; O.Wx = Op.Wx; O.Wy = Op.Wy; O.Wz = Op.Wz; O.LevelEntry = Op.LevelEntry; return O; }
	};
}
