#pragma once

// UniTravel.h
// Travel sensitivity (D-154). Engine-agnostic.
//
// Physically correct brightness makes deep space nearly empty to the naked eye: a bright galaxy
// fades out beyond about 3 Mpc and a rich cluster's mean surface brightness (~25.4 mag/arcsec^2)
// sits just past the naked-eye limit. While travelling between galaxies the structure you steer
// by should show. So while the camera moves fast, sensitivity rises by itself: a magnitude offset
// added to the owner's own sensitivity (uu.Sensitivity), driven by speed, in log speed between a
// start and a full speed, eased, and rate-limited so the windows widen gradually (the D-139 pool
// guard still applies). It goes through the existing sensitivity path (windows widen with it, so
// nothing pops: T3.sensitivity), so it changes what is drawn, never what is generated.

#include <algorithm>
#include <cmath>

namespace UniCore
{
	struct FTravelSensitivity
	{
		double StartCmPerS = 3.0856775814913673e23;   // 100 kpc/s: no boost below
		double FullCmPerS = 3.0856775814913673e25;    // 10 Mpc/s: the full boost above
		double MaxDeltaMag = 2.5;                     // = D-152's cover: parents stay complete
		double RiseMagPerS = 2.0;                     // rate limits: the verified sensitivity sweep
		double FallMagPerS = 1.0;                     //   ran at ~2.5 mag/s with 0 pops
		double Current = 0.0;

		/** The boost a steady speed asks for: 0 below Start, MaxDeltaMag above Full, smoothstep
		 *  in log speed between. */
		double Target(double SpeedCmPerS) const
		{
			if (!(MaxDeltaMag > 0.0) || !(SpeedCmPerS > StartCmPerS) || !(FullCmPerS > StartCmPerS)) { return 0.0; }
			const double T = std::clamp(std::log(SpeedCmPerS / StartCmPerS) / std::log(FullCmPerS / StartCmPerS), 0.0, 1.0);
			return MaxDeltaMag * T * T * (3.0 - 2.0 * T);
		}

		/** Advance by Dt seconds at this speed; returns the boost now in effect. */
		double Update(double SpeedCmPerS, double DtSeconds)
		{
			const double Want = Target(SpeedCmPerS);
			const double Dt = std::max(DtSeconds, 0.0);
			if (Want > Current) { Current = std::min(Want, Current + RiseMagPerS * Dt); }
			else { Current = std::max(Want, Current - FallMagPerS * Dt); }
			return Current;
		}
	};
}
