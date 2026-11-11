// UniGalaxyMapCache.h
// D-214: which galaxies have a map, and where. The galaxy maps (D-212, UniGalaxyMap.h) live in the
// slots of one GPU texture array; this cache decides which galaxy each slot holds, asks the host
// to bake new ones (epochs), and answers "is this galaxy's map there?" for the two readers:
//   - generation (UniGenRef / UniStreamer): a galaxy's stars are placed from its map, so a cell
//     with a galaxy contributor waits until that map's light channels are baked (AcquireLight);
//   - the volume: the camera's galaxy marched from its map once the whole map, dust included, is
//     baked (AcquireFull).
// A map is a pure function of the galaxy's parameters (the key is their hash), so a slot evicted
// and baked again later holds the same texels: content never depends on when it was baked.
//
// Eviction: a slot is reused only when no reader has asked for it for EvictTicks ticks, longer
// than any generation batch stays in flight, so no batch reads a slot that is being re-baked.
// Engine-agnostic (no UE includes).

#pragma once

#include "UniGpuTypes.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace UniCore
{
	/** What a galaxy's map is baked from: its light parameters and, when it has dust, the dust's. */
	struct FGalaxyMapRequest
	{
		FFieldParamsGPU Par{};
		FFieldParamsGPU Dust{};
		bool bDust = false;
	};

	/** The map's identity: FNV-1a over the light parameters' bytes (the map's R, B and A channels
	 *  are a function of them alone; G, the dust's, is filled when a reader asks with dust). */
	inline uint64_t GalaxyMapKey(const FFieldParamsGPU& Par)
	{
		uint64_t H = 1469598103934665603ull;
		const unsigned char* B = reinterpret_cast<const unsigned char*>(&Par);
		for (size_t I = 0; I < sizeof(Par); ++I) { H ^= uint64_t(B[I]); H *= 1099511628211ull; }
		return H == 0ull ? 1ull : H;
	}

	class FGalaxyMapCache
	{
	public:
		struct FEntry
		{
			uint64_t Key = 0;            // 0: empty slot
			FGalaxyMapRequest Req;
			uint32_t Epoch = 0;          // the bake this slot's contents belong to
			bool bLight = false;         // R, B, A baked (placement may read it)
			bool bReady = false;         // every channel of this epoch baked (the volume may read it)
			uint64_t LastUsed = 0;       // tick of the last Acquire
		};

		/** Offline: bake a slot synchronously when it is requested (the entry is then ready at
		 *  once). Unset in the engine: the host bakes PendingBakes() and calls MarkBaked. */
		std::function<void(uint32_t Slot, const FEntry&)> SyncBake;
		uint64_t EvictTicks = 64;

		/** Slots and texels a side. A change empties every slot (new epochs for all). */
		void Configure(uint32_t InSlots, uint32_t InSize)
		{
			if (InSlots == uint32_t(Entries.size()) && InSize == Size) { return; }
			Entries.assign(InSlots, FEntry{});
			Size = InSize;
			++Generation;
		}
		/** Empties every slot (new epochs for all). */
		void Reset()
		{
			Entries.assign(Entries.size(), FEntry{});
			++Generation;
		}
		uint32_t NumSlots() const { return uint32_t(Entries.size()); }
		uint32_t MapSize() const { return Size; }
		/** Bumped by Configure: a host holding textures rebuilds them. */
		uint32_t ConfigGeneration() const { return Generation; }

		/** One tick per frame, by whoever owns the cache (the subsystem; offline, the pipeline). */
		void Tick() { ++Now; }
		uint64_t Ticks() const { return Now; }

		/** Placement: the slot holding R's map with its light channels baked, or -1 (requested:
		 *  the caller waits). */
		int AcquireLight(const FGalaxyMapRequest& R) { return Acquire(R, false); }
		/** The volume: the slot holding R's whole map (dust too if R has it), or -1. */
		int AcquireFull(const FGalaxyMapRequest& R) { return Acquire(R, true); }

		/** Slots whose current epoch is not baked yet (the host bakes them, then MarkBaked). */
		std::vector<uint32_t> PendingBakes() const
		{
			std::vector<uint32_t> Out;
			for (uint32_t S = 0; S < Entries.size(); ++S) { if (Entries[S].Key != 0ull && !Entries[S].bReady) { Out.push_back(S); } }
			return Out;
		}
		const FEntry& Entry(uint32_t Slot) const { return Entries[Slot]; }

		/** The host finished baking Slot for Epoch (a stale epoch is ignored). */
		void MarkBaked(uint32_t Slot, uint32_t Epoch)
		{
			if (Slot >= Entries.size()) { return; }
			FEntry& E = Entries[Slot];
			if (E.Key == 0ull || E.Epoch != Epoch) { return; }
			E.bLight = true;
			E.bReady = true;
		}

		/** The slot holding Key, or -1. */
		int SlotOf(uint64_t Key) const
		{
			for (uint32_t S = 0; S < Entries.size(); ++S) { if (Entries[S].Key == Key) { return int(S); } }
			return -1;
		}

		uint64_t Requests = 0, Evictions = 0, Waits = 0;   // stats

	private:
		int Acquire(const FGalaxyMapRequest& R, bool bFull)
		{
			if (Entries.empty() || Size == 0u) { ++Waits; return -1; }
			const uint64_t Key = GalaxyMapKey(R.Par);
			int Slot = SlotOf(Key);
			if (Slot >= 0)
			{
				FEntry& E = Entries[uint32_t(Slot)];
				E.LastUsed = Now;
				if (R.bDust && !E.Req.bDust)
				{
					// The dust channel was not baked: bake the slot again with it. The light channels
					// come out the same, so placement keeps reading the slot meanwhile.
					E.Req.Dust = R.Dust;
					E.Req.bDust = true;
					E.Epoch = ++NextEpoch;
					E.bReady = false;
					Bake(uint32_t(Slot));
				}
				const bool bOk = bFull ? E.bReady : E.bLight;
				if (!bOk) { ++Waits; }
				return bOk ? Slot : -1;
			}
			// A new map: an empty slot, else the least recently used one nobody asked for lately.
			int Free = -1;
			uint64_t Oldest = ~0ull;
			for (uint32_t S = 0; S < Entries.size(); ++S)
			{
				const FEntry& E = Entries[S];
				if (E.Key == 0ull) { Free = int(S); break; }
				if (E.LastUsed + EvictTicks <= Now && E.LastUsed < Oldest) { Oldest = E.LastUsed; Free = int(S); }
			}
			if (Free < 0) { ++Waits; return -1; }
			FEntry& E = Entries[uint32_t(Free)];
			if (E.Key != 0ull) { ++Evictions; }
			E = FEntry{};
			E.Key = Key;
			E.Req = R;
			E.Epoch = ++NextEpoch;
			E.LastUsed = Now;
			++Requests;
			Bake(uint32_t(Free));
			const bool bOk = bFull ? E.bReady : E.bLight;
			if (!bOk) { ++Waits; }
			return bOk ? Free : -1;
		}

		void Bake(uint32_t Slot)
		{
			if (!SyncBake) { return; }
			FEntry& E = Entries[Slot];
			SyncBake(Slot, E);
			E.bLight = true;
			E.bReady = true;
		}

		std::vector<FEntry> Entries;
		uint32_t Size = 0;
		uint32_t Generation = 0;
		uint32_t NextEpoch = 0;
		uint64_t Now = 1;
	};
}
