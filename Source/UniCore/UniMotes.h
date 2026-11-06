#pragma once

// UniMotes.h
// D-192: warp ambience (WARP_PLAN E4). In a void the sky barely moves even at 30 Mpc/s; motes
// say "fast". They are decoration: points fixed in the world on two cubic lattice levels around
// the camera, whose cell size follows the speed (the camera's move over MoteFrames frames), so
// the nearest stream past in about that time (about 20 in a 90 degree view); the sprite pass
// draws them (Uni_MoteSprite, 16 a cell, 2 x 64 cells) with
// the stars' streaks (E1) and optics (E3), faded by the warp amount. Nothing is generated and
// nothing depends on them. Engine-agnostic: the engine keeps an FMoteScale and fills the
// sprite pass's mote parameters from MoteLattice every frame.

#include "UniCoord.h"

#include <algorithm>
#include <cmath>

namespace UniCore
{
	/** One lattice level as the shader takes it. */
	struct FMoteSlot
	{
		u32 Cell[3] = { 0, 0, 0 };   // the camera's cell at this level, low 32 bits per axis
		u32 Level = 0;               // log2 of the cell size (cm)
		float Frac[3] = { 0, 0, 0 }; // the camera's position in its cell, [0, 1]
		float Weight = 0.0f;         // warp amount x the cross-fade (0: none)
		double CellCm = 0.0;
	};

	/** Slot (Level & 1) holds each level, so a level keeps its slot while it is visible: the
	 *  slots change hands only at zero weight. */
	struct FMoteLattice
	{
		FMoteSlot Slot[2];
	};

	/** The lattice scale: log2 of the cell size (cm), following |camera step| x Frames,
	 *  smoothed in log space; held while the camera stands still (the warp amount fades the
	 *  motes then) and reset on a jump. */
	struct FMoteScale
	{
		double Log2Cm = -1.0;   // < 0: not set

		void Update(double StepCm, double Frames, double Dt, double TimeConstS = 0.3)
		{
			if (!(StepCm > 0.0) || !(Frames > 0.0)) { return; }
			const double Target = std::log2(std::max(StepCm * Frames, 100.0));   // at least a metre
			if (Log2Cm < 0.0 || !(Dt > 0.0)) { Log2Cm = Target; return; }
			Log2Cm += (Target - Log2Cm) * (1.0 - std::exp(-Dt / std::max(TimeConstS, 1e-3)));
		}
		void Reset() { Log2Cm = -1.0; }
	};

	/** The two levels around the camera at scale Log2Cm (level n weighs Amount x (1 - |x - n|)),
	 *  the camera's cell and fraction in each. */
	inline FMoteLattice MoteLattice(const FCoord& Cam, const FVec3f& CamFrac, double Log2Cm, float Amount)
	{
		FMoteLattice L;
		if (!(Amount > 0.0f) || !(Log2Cm >= 0.0)) { return L; }
		const int N0 = int(std::floor(Log2Cm));
		const double F = Log2Cm - double(N0);
		for (int K = 0; K < 2; ++K)
		{
			const int N = N0 + K;
			if (N < 0 || N > 120) { continue; }
			FMoteSlot& S = L.Slot[N & 1];
			S.Level = u32(N);
			S.Weight = Amount * float(K == 0 ? 1.0 - F : F);
			S.CellCm = std::ldexp(1.0, N);
			const float Cf[3] = { CamFrac.X, CamFrac.Y, CamFrac.Z };
			for (int A = 0; A < 3; ++A)
			{
				S.Cell[A] = Shr(Cam.Axis(A), N).L[3];
				const double Fr = (U128ToDouble(MaskLow(Cam.Axis(A), N)) + double(Cf[A])) / S.CellCm;
				S.Frac[A] = float(std::clamp(Fr, 0.0, 1.0));
			}
		}
		return L;
	}
}
