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

	/** D-199 (B-018): the warp optics' motion for a frame: the camera's step (cm) as a unit vector
	 *  and the effective speed, 0 when there is no motion or no optics. In double: the step's
	 *  length squared overflows a float past 1.8e19 cm (6 pc a frame), and UE's float Size() gave
	 *  inf there, so the motion came out zero with the optics on: every direction shrank by
	 *  sqrt(1 - b^2), and every sprite off the view's centre took a capped 64 px streak (its light
	 *  spread 64x): at intergalactic speed nearly everything vanished. */
	/** D-268: the travel shift's strength (ln of the shift factor straight ahead) from the camera's
	 *  speed, its own curve (D-230's rode the travel curve: in at ~308 kpc/s, full at ~2.2 Mpc/s,
	 *  x1.65). In log speed: 0 below Start; to RefAmount at Ref as (u / U1)^1.5 (it comes in
	 *  gently); from Ref to TopAmount at Full, its slope continuous at Ref and accelerating (a x +
	 *  (1 - a) x^2: the top end climbs fastest); TopAmount above Full. Speeds in any one unit. */
	inline double TravelShiftTarget(double InSpeed, double InStart, double InRef, double InFull, double InRefAmount, double InTopAmount)
	{
		if (!(InSpeed > InStart) || !(InStart > 0.0)) { return 0.0; }
		const double Ref = std::max(InRef, InStart * 1.001);
		const double Full = std::max(InFull, Ref * 1.001);
		const double U = std::log(InSpeed / InStart);
		const double U1 = std::log(Ref / InStart), U2 = std::log(Full / Ref);
		if (U <= U1) { return InRefAmount * std::pow(U / U1, 1.5); }
		const double X = std::min((U - U1) / U2, 1.0);
		const double Rise = InTopAmount - InRefAmount;
		if (!(Rise > 0.0)) { return InRefAmount + Rise * X; }
		const double A = std::clamp(1.5 * InRefAmount / U1 * U2 / Rise, 0.0, 2.0);   // the slope at Ref carried on
		return InRefAmount + Rise * (A * X + (1.0 - A) * X * X);
	}

	/** D-268: the travel shift's strength eased in time (as the motion web's ramp: speed jitter
	 *  cannot flicker it): towards TravelShiftTarget over UpSeconds rising, DownSeconds falling. */
	struct FTravelShiftRamp
	{
		double Value = 0.0;
		double UpSeconds = 0.3, DownSeconds = 0.6;

		double Update(double InTarget, double InDt)
		{
			const double Tau = InTarget > Value ? UpSeconds : DownSeconds;
			const double K = InDt > 0.0 ? 1.0 - std::exp(-InDt / std::max(Tau, 1e-3)) : 0.0;
			Value += (InTarget - Value) * K;
			return Value;
		}
	};

	/** D-231: the motion web's ramp, from the camera's speed itself (the travel curve is 0 below
	 *  100 kpc/s; the owner wants the web while crossing a galaxy in a second or two, ~15-50 kpc/s):
	 *  0 below Start, 1 above Full (cm/s), ease-out on log speed between (it comes in early and
	 *  reaches full slowly). Eased in time so speed jitter cannot flicker it: up over UpSeconds,
	 *  down over DownSeconds (time constants). */
	struct FMotionWebRamp
	{
		double Value = 0.0;
		double UpSeconds = 0.3, DownSeconds = 0.6;

		static double Target(double InSpeed, double InStart, double InFull)
		{
			if (!(InSpeed > InStart) || !(InFull > InStart) || !(InStart > 0.0)) { return 0.0; }
			const double X = std::clamp(std::log(InSpeed / InStart) / std::log(InFull / InStart), 0.0, 1.0);
			return 1.0 - (1.0 - X) * (1.0 - X);
		}
		double Update(double InSpeed, double InDt, double InStart, double InFull)
		{
			const double T = Target(InSpeed, InStart, InFull);
			const double Tau = T > Value ? UpSeconds : DownSeconds;
			const double K = InDt > 0.0 ? 1.0 - std::exp(-InDt / std::max(Tau, 1e-3)) : 0.0;
			Value += (T - Value) * K;
			return Value;
		}
	};

	/** D-230: the travel shift for a frame: the camera's motion (unit, from its step) and the shift
	 *  amount (0 with no motion: never a direction from a zero step). */
	inline void ShiftMotion(double InStepX, double InStepY, double InStepZ, double InAmount, float OutVelAmount[4])
	{
		OutVelAmount[0] = OutVelAmount[1] = OutVelAmount[2] = OutVelAmount[3] = 0.0f;
		const double L = std::sqrt(InStepX * InStepX + InStepY * InStepY + InStepZ * InStepZ);
		if (!(L > 0.0) || !std::isfinite(L) || InAmount == 0.0) { return; }
		OutVelAmount[0] = float(InStepX / L);
		OutVelAmount[1] = float(InStepY / L);
		OutVelAmount[2] = float(InStepZ / L);
		OutVelAmount[3] = float(InAmount);
	}

	inline void WarpMotion(double InStepX, double InStepY, double InStepZ, double InBeta, float OutVelBeta[4])
	{
		OutVelBeta[0] = OutVelBeta[1] = OutVelBeta[2] = OutVelBeta[3] = 0.0f;
		const double L = std::sqrt(InStepX * InStepX + InStepY * InStepY + InStepZ * InStepZ);
		if (!(L > 0.0) || !std::isfinite(L) || !(InBeta > 0.0)) { return; }
		OutVelBeta[0] = float(InStepX / L);
		OutVelBeta[1] = float(InStepY / L);
		OutVelBeta[2] = float(InStepZ / L);
		OutVelBeta[3] = float(std::min(InBeta, 0.9));
	}
}
