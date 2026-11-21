#pragma once

// UniCompact.h
// D-272 (Docs/COMPACT_OBJECTS.md, phase A): the CPU face of compact objects. Everything here is
// the shader core's (Shaders/Private/Core/UniCompact.ush, through the shim): a galaxy's
// supermassive black hole is a pure function of its record, so a gameplay lookup and the project
// pass's nucleus read the same numbers. Engine-agnostic.
//
// D-273 (phase D): the near field. LensTargetOf turns a black hole the camera is near into what the
// lens pass draws (UniLens.usf: the core's Uni_LensTrace per pixel): where it is in R_s, its disc
// (axis, outer radius, peak temperature), the disc's share of the nucleus's light (the hand-off, as
// the disc is resolved) and its radiance scale: from the core's table of the disc's light by
// direction (UNI_BH_DISC_BEAM / LOGNORM, lensing and all), so the disc carries exactly the light the
// nucleus sprite shows towards the camera (the D-129 rule: a hand-off moves light, it never makes
// any). DiscNormalization is the unlensed integral (the checks' and `univerify lenstable`'s
// comparison).

#include "UniGenRef.h"
#include "UniShaderCores.h"

#include <algorithm>
#include <cmath>

namespace UniCore
{
	/** A black hole as the API reports it (double, physical units). */
	struct FBlackHoleInfo
	{
		bool bValid = false;
		double MassSolar = 0.0;
		double EddingtonRatio = 0.0;         // accretion rate over the Eddington rate
		double LuminosityErgS = 0.0;         // bolometric, of its accretion
		double SchwarzschildRadiusCm = 0.0;
		float NucleusAbsMag = 99.0f;         // the nucleus its galaxy shows, V (99: none: under 1e-6 of the host)
		float DiscPeakK = 0.0f;              // D-273: its accretion disc's peak temperature (K)
		float DiscOuterRs = 0.0f;            // D-273: and outer radius (Schwarzschild radii)
	};

	/** The nucleus settings (FUniView's: uu.Bh.ActiveFraction, NucleusCap, NucleusGain). */
	struct FNucleusConfig
	{
		float Active = UniHlsl::UNI_BH_ACTIVE_SHARE;
		float Cap = 0.5f;
		float Gain = 1.0f;
	};

	/** A black hole's description from the core's FUniBlackHole. */
	inline FBlackHoleInfo BlackHoleInfoOf(const UniHlsl::FUniBlackHole& B)
	{
		FBlackHoleInfo I;
		I.bValid = true;
		I.MassSolar = std::pow(10.0, double(B.Log10Mass));
		I.EddingtonRatio = std::pow(10.0, double(B.Log10Eddington));
		I.LuminosityErgS = std::pow(10.0, double(B.Log10LumBol));
		I.SchwarzschildRadiusCm = std::pow(10.0, double(UniHlsl::UNI_BH_LOG_RS_SUN_CM) + double(B.Log10Mass));
		I.DiscPeakK = UniHlsl::Uni_BhDiscPeakK(B);
		I.DiscOuterRs = UniHlsl::Uni_BhDiscOuterX(I.DiscPeakK);
		return I;
	}

	/** A galaxy record's supermassive black hole (the caller knows the record is a galaxy's: an
	 *  archetype with a galaxy-field emitter) and the nucleus it shows. */
	inline FBlackHoleInfo GalaxyBlackHole(const FObjectGPU& O, const FNucleusConfig& C = FNucleusConfig{})
	{
		const UniHlsl::FUniObject Ho = ToHlsl(O);
		const UniHlsl::FUniBlackHole B = UniHlsl::Uni_GalaxyBlackHole(Ho, C.Active);
		FBlackHoleInfo I = BlackHoleInfoOf(B);
		I.NucleusAbsMag = UniHlsl::Uni_NucleusAbsMag(B, UniHlsl::Uni_ObjectAbsMag(Ho), C.Cap, C.Gain);
		return I;
	}

	// ----------------------------------------------------------------------------------------
	// D-273: the near field
	// ----------------------------------------------------------------------------------------

	static constexpr double Pc10Cm = 3.0856775814913673e19;   // 10 pc: where an absolute magnitude is the apparent one

	/** The disc's light averaged over every viewing direction, per unit of radiance scale, in
	 *  R_s^2 (Uni_BhDiscLight's luminance x area x |cos i|, the shifts g of a far observer, no
	 *  lensing): N = int_0^1 dmu int dphi int x dx mu PlanckV(g T(x)) opacity(x), with
	 *  g = sqrt(1 - 1.5/x) / (1 + sqrt(0.5/x) sin(phi) sqrt(1 - mu^2)). A far observer at
	 *  distance d then sees, on average, scale x R_s^2 x N / d^2: the radiance scale making that
	 *  the nucleus's flux is 10^(-0.4 (M_n - m_exp)) (10 pc / R_s)^2 / N (exposure units per sr). */
	inline double DiscNormalization(double PeakK, double OuterX)
	{
		const int Nx = 160, Nphi = 48, Nmu = 12;
		const double X0 = double(UniHlsl::UNI_BH_ISCO_X), LnSpan = std::log(std::max(OuterX, X0 * 1.0001) / X0);
		double Sum = 0.0;
		for (int Ix = 0; Ix < Nx; ++Ix)
		{
			const double X = X0 * std::exp(LnSpan * (double(Ix) + 0.5) / double(Nx));
			const double Dx = X * LnSpan / double(Nx);
			const double A = double(UniHlsl::Uni_BhDiscOpacity(float(X), float(OuterX)));
			if (!(A > 0.0)) { continue; }
			const double Prof = double(UniHlsl::Uni_BhDiscProfile(float(X)));
			const double Grav = std::sqrt(std::max(1.0 - 1.5 / X, 0.0));
			const double Beta = std::sqrt(0.5 / X);
			double Ring = 0.0;
			for (int Im = 0; Im < Nmu; ++Im)
			{
				const double Mu = (double(Im) + 0.5) / double(Nmu);
				const double SinI = std::sqrt(std::max(1.0 - Mu * Mu, 0.0));
				for (int Ip = 0; Ip < Nphi; ++Ip)
				{
					const double Phi = 6.283185307179586 * (double(Ip) + 0.5) / double(Nphi);
					const double G = Grav / (1.0 + Beta * std::sin(Phi) * SinI);
					const double T = std::max(G * PeakK * Prof, 1.0);
					Ring += Mu / std::expm1(std::min(double(UniHlsl::UNI_BH_V_HC_K) / T, 700.0));
				}
			}
			Sum += Ring * (6.283185307179586 / double(Nphi)) / double(Nmu) * A * X * Dx;
		}
		return Sum;
	}

	/** The lens pass's settings (uu.Bh.Lens*). */
	struct FLensConfig
	{
		bool bLens = true;            // uu.Bh.Lens
		bool bDisc = true;            // uu.Bh.Disc
		float RingMinPx = 0.25f;      // the pass runs while the Einstein ring is at least this (px) or the disc is drawn
		float DiscFromPx = 2.0f;      // the disc takes the nucleus's light from this outer radius (px) ..
		float DiscToPx = 6.0f;        // .. to all of it at this
		float BendMinPx = 0.1f;       // the pass's region: where the deflection is at least this (px)
		u32 MaxSteps = UniHlsl::UNI_LENS_MAX_STEPS;
		float LensScale = 1.0f;       // D-277 (uu.Bh.LensScale; the engine's default 4, D-278): the hole lenses as if this x heavier, its disc keeping its physical size (its inner edge out at the heavier hole's ISCO): the shadow and its lensing this x larger against the disc. 1: physical.
	};

	/** What the lens pass draws: one black hole the camera is near. */
	struct FLensTarget
	{
		bool bActive = false;
		double RelCm[3] = { 0.0, 0.0, 0.0 };   // the black hole from the camera (cm)
		double DistanceCm = 0.0;
		double RsCm = 0.0;
		float Axis[3] = { 0.0f, 0.0f, 1.0f };  // its disc's axis (world)
		float OuterX = 0.0f;                    // its disc's outer radius (R_s)
		float PeakK = 0.0f;                     // its disc's peak temperature
		bool bDisc = false;                     // a disc to draw (a nucleus over 1e-6 of its host, uu.Bh.Disc)
		float DiscShare = 0.0f;                 // the share of the nucleus's light the disc draws (the sprite keeps the rest)
		double DiscLog10Scale = -99.0;          // log10 of its radiance scale without the exposure: + 0.4 m_exp (UniLens.usf)
		float RegionRad = 0.0f;                 // the pass's angular radius about it (pi: the whole view)
		float RingPx = 0.0f;                    // its Einstein ring's radius (px; a source at infinity)
		float DiscPx = 0.0f;                    // its disc's outer radius (px)
		u32 NucleusSeed = 0u;                   // its galaxy's seed (the nucleus sprite's hand-off)
		float Score = 0.0f;                     // how much of the view it changes (the larger of the two, px)
		// The nucleus as a point (the lens pass draws it while active, x (1 - DiscShare); its sprite is
		// held back: a sprite in scene colour would be lensed as if behind the black hole). Exactly the
		// sprite's: the record's half-float magnitude with the disc's light towards the camera.
		float PointAppMag = 99.0f;              // apparent V magnitude (99: none)
		float PointPhysAngRad = 0.0f;           // its true angular radius (100 R_s, as the record's)
		float PointColour[3] = { 1.0f, 1.0f, 1.0f };   // brightest channel 1 (Uni_NucleusColour)
	};

	/** The lens target of a black hole at RelCm from the camera (cm). Normalisation: its galaxy's
	 *  record's nucleus (Mn, the isotropic magnitude; 99: none: lensing alone), through the core's
	 *  table (Uni_BhDiscLog10Norm: the disc's light averaged over directions, lensing and all).
	 *  ThetaPixel: rad per pixel. Style: D-280's look (nullptr: none): its extent (the disc drawn
	 *  ExtentMul x its physical outer radius; the normalisation stays the physical disc's, exact at 1: the outer disc's V light is
	 *  small, and the engine's brightness is exposure-relative, D-276). SizeScale: D-281's
	 *  uu.Bh.StellarScale for a stellar black hole (lensing and disc drawn as if its R_s were this x;
	 *  1: physical). */
	inline FLensTarget LensTargetOf(const UniHlsl::FUniBlackHole& Bh, float Mn, const double RelCm[3], const float AxisWorld[3],
		u32 NucleusSeed, double ThetaPixel, const FLensConfig& L, const UniHlsl::FUniBhStyle* Style = nullptr, double SizeScale = 1.0)
	{
		FLensTarget T;
		if (!L.bLens) { return T; }
		// D-277: the lens's R_s is LensScale x the physical one; the disc keeps its physical outer
		// radius (in the lens's units OuterX / LensScale) and starts at the lens's ISCO.
		const double RsPhys = std::pow(10.0, double(UniHlsl::UNI_BH_LOG_RS_SUN_CM) + double(Bh.Log10Mass)) * std::max(SizeScale, 1.0);   // D-281: x SizeScale
		const double Ks = std::max(double(L.LensScale), 1.0);
		T.RsCm = RsPhys * Ks;
		for (int A = 0; A < 3; ++A) { T.RelCm[A] = RelCm[A]; T.Axis[A] = AxisWorld[A]; }
		T.DistanceCm = std::sqrt(RelCm[0] * RelCm[0] + RelCm[1] * RelCm[1] + RelCm[2] * RelCm[2]);
		T.NucleusSeed = NucleusSeed;
		const double Th = std::max(ThetaPixel, 1.0e-9);
		const double Dx = T.DistanceCm / T.RsCm;   // the lens's R_s
		T.PeakK = UniHlsl::Uni_BhDiscPeakK(Bh);
		// D-280: x its look's extent (1: the physical outer radius).
		const double ExtentMul = Style ? double(std::max(Style->ExtentMul, 0.05f)) : 1.0;
		T.OuterX = std::max(float(double(UniHlsl::Uni_BhDiscOuterX(T.PeakK)) * ExtentMul / Ks), UniHlsl::UNI_BH_ISCO_X + 1.0f);
		T.RingPx = float(std::sqrt(2.0 / std::max(Dx, 1.0)) / Th);
		T.DiscPx = float(std::atan(double(T.OuterX) / std::max(Dx, 1.0)) / Th);
		T.bDisc = L.bDisc && Mn < UniHlsl::UNI_BH_NONE_MAG;
		if (T.bDisc)
		{
			const double U = std::clamp((double(T.DiscPx) - L.DiscFromPx) / std::max(double(L.DiscToPx - L.DiscFromPx), 1.0e-6), 0.0, 1.0);
			T.DiscShare = float(U * U * (3.0 - 2.0 * U));
			T.DiscLog10Scale = -0.4 * double(Mn) + 2.0 * std::log10(Pc10Cm / T.RsCm) - double(UniHlsl::Uni_BhDiscLog10Norm(T.PeakK));   // exact at LensScale 1 (the table's disc)
		}
		T.Score = std::max(T.RingPx, T.DiscShare > 0.0f ? T.DiscPx : 0.0f);
		T.bActive = T.RingPx >= L.RingMinPx || T.DiscShare > 0.0f;
		if (!T.bActive) { return T; }
		if (Mn < UniHlsl::UNI_BH_NONE_MAG && T.DistanceCm > 0.0)
		{
			// As Uni_ProjectNucleusC: the beam at the angle to the axis, the record's half float.
			const double Mu = std::fabs(double(AxisWorld[0]) * RelCm[0] + double(AxisWorld[1]) * RelCm[1] + double(AxisWorld[2]) * RelCm[2]) / T.DistanceCm;
			const float Beam = UniHlsl::Uni_BhDiscBeam(T.PeakK, float(Mu));
			const float Md = UniHlsl::Uni_F16ToF32(UniHlsl::Uni_F32ToF16(Mn - 2.5f * std::log10(std::max(Beam, 1.0e-6f))));
			T.PointAppMag = UniHlsl::Uni_ApparentMag(Md, float(T.DistanceCm));
			T.PointPhysAngRad = float(std::exp2(double(std::log2(float(100.0 * RsPhys)))) / T.DistanceCm);   // the record's (physical) size
			const UniHlsl::float3 C = UniHlsl::Uni_UnpackColour(UniHlsl::Uni_PackColour(UniHlsl::Uni_NucleusColourP(Bh, Style ? Style->Palette : 0u, Style ? Style->PaletteMix : 0.0f)));   // as the record carries it (D-280: in its palette)
			T.PointColour[0] = C.x; T.PointColour[1] = C.y; T.PointColour[2] = C.z;
		}
		// The region: the integrated sphere (and the disc) with a margin, and where the weak-field
		// deflection 2 R_s / b is at least BendMinPx.
		const double Sphere = std::max(double(UniHlsl::UNI_LENS_SPHERE_X), 1.25 * double(T.OuterX));
		const double BBend = 2.0 / (std::max(double(L.BendMinPx), 1.0e-3) * Th);   // R_s
		const double Reach = std::max(1.2 * Sphere, BBend);
		T.RegionRad = Reach >= Dx ? 3.14159265f : float(std::asin(std::min(Reach / Dx, 1.0)));
		return T;
	}

	// ----------------------------------------------------------------------------------------
	// D-276: the disc's look (the volumetric disc's settings, its flow phases, its art brightness)
	// ----------------------------------------------------------------------------------------

	/** The look's settings (uu.Bh.Disc*; render only). */
	struct FDiscLook
	{
		bool bVolume = true;          // uu.Bh.DiscVolume: the volumetric disc (false: D-273's thin disc)
		float Thickness = 0.025f;     // uu.Bh.DiscThickness: H / rho (D-277: 0.05 -> 0.025)
		float Opacity = 2.5f;         // uu.Bh.DiscOpacity: optical depth through the plane
		float Clouds = 0.85f;         // uu.Bh.DiscClouds
		float Bands = 0.3f;           // uu.Bh.DiscBands
		float Heat = 0.35f;           // uu.Bh.DiscHeat
		float Spiral = 12.0f;         // uu.Bh.DiscSpiral (D-277: 1.5 -> 4; D-278: 12)
		float Erosion = 0.8f;         // uu.Bh.DiscErosion (D-278)
		float PeriodS = 8.0f;         // uu.Bh.DiscPeriod: seconds for an orbit at the ISCO (art; 0: still)
		float CycleOrbits = 1.0f;     // uu.Bh.DiscCycle: a flow phase lasts this many ISCO orbits
		float Falloff = 0.35f;        // uu.Bh.DiscFalloff: the light's fall with temperature (1: physical)
		u32 MaxSamples = UniHlsl::UNI_LENS_VOL_SAMPLES;
		u32 Palette = 0u;             // D-280: its palette (0: the blackbody; StyledLook)
		float PaletteMix = 0.0f;      // D-280: the palette's share of the colour
	};

	/** A unit vector in the disc's plane (the azimuth's zero). */
	inline UniHlsl::float3 DiscE1(const UniHlsl::float3& Axis)
	{
		const UniHlsl::float3 Ref = std::fabs(Axis.z) < 0.9f ? UniHlsl::float3(0.0f, 0.0f, 1.0f) : UniHlsl::float3(1.0f, 0.0f, 0.0f);
		return UniHlsl::normalize(UniHlsl::cross(Ref, Axis));
	}

	/** Fills the disc's look into D for time TimeS (seconds; double: the phases are fractions of it):
	 *  each flow phase advects the clouds for one cycle then restarts with a fresh seed (the two half
	 *  a cycle apart, each weighted 0 at its restart: w_A = 1 - |2 frac(t / cycle) - 1|). */
	inline void ApplyDiscLook(UniHlsl::FUniLensDisc& D, const FDiscLook& L, double TimeS, u32 DiscSeed)
	{
		D.E1 = DiscE1(D.Axis);
		D.Thickness = L.bVolume ? std::max(L.Thickness, 0.005f) : 0.0f;
		D.Opacity = std::max(L.Opacity, 0.0f);
		D.Clouds = std::clamp(L.Clouds, 0.0f, 1.0f);
		D.Bands = std::max(L.Bands, 0.0f);
		D.Heat = std::clamp(L.Heat, 0.0f, 0.9f);
		D.Spiral = L.Spiral;
		D.MaxSamples = L.MaxSamples;
		D.Falloff = std::clamp(L.Falloff, 0.05f, 1.0f);
		D.Erosion = std::clamp(L.Erosion, 0.0f, 1.0f);   // D-278
		D.Palette = float(std::min<u32>(L.Palette, UniHlsl::UNI_BH_PALETTES - 1u));   // D-280
		D.PaletteMix = L.PaletteMix > 0.0f && L.Palette > 0u ? std::min(L.PaletteMix, 1.0f) : 0.0f;
		const double Omega = L.PeriodS > 0.0f ? 6.283185307179586 / double(L.PeriodS) : 0.0;
		D.OmegaIsco = float(Omega);
		const double Cycle = L.PeriodS > 0.0f ? std::max(double(L.CycleOrbits), 0.05) * double(L.PeriodS) : 0.0;
		if (!(Cycle > 0.0))
		{
			D.FlowTA = 0.0f; D.FlowTB = 0.0f; D.FlowWA = 1.0f;
			D.SeedA = UniHlsl::Uni_Avalanche(DiscSeed ^ 0x5bd1e995u); D.SeedB = D.SeedA;
			return;
		}
		const double Ua = TimeS / Cycle, Ub = Ua + 0.5;
		const double Fa = Ua - std::floor(Ua), Fb = Ub - std::floor(Ub);
		D.FlowTA = float(Fa * Cycle);
		D.FlowTB = float(Fb * Cycle);
		D.FlowWA = float(1.0 - std::fabs(2.0 * Fa - 1.0));
		const u32 Ca = u32(i64(std::floor(Ua)) & 0xFFFFFFFF), Cb = u32(i64(std::floor(Ub)) & 0xFFFFFFFF);
		D.SeedA = UniHlsl::Uni_Avalanche(DiscSeed ^ (Ca * 2u) ^ 0x5bd1e995u);
		D.SeedB = UniHlsl::Uni_Avalanche(DiscSeed ^ (Cb * 2u + 1u) ^ 0x5bd1e995u);
	}

	/** D-276: the exposure-relative brightness (uu.Bh.DiscBrightness > 0): the radiance scale (exposure
	 *  units per sr per Uni_BhDiscLight unit) at which the disc's peak temperature, unshifted and
	 *  optically thick, shows Brightness exposure units in a pixel of ThetaPixel. */
	inline double DiscArtScale(float PeakK, double ThetaPixel, double Brightness)
	{
		const double L = std::max(double(UniHlsl::Uni_BhPlanckV(PeakK)), 1.0e-30);
		return Brightness / (L * ThetaPixel * ThetaPixel);
	}

	/** The disc's axis for a galaxy's black hole: its host's disc normal (world z for an anchor with a
	 *  fixed orientation) tilted by BH.SPIN. */
	inline void GalaxyDiscAxis(u32 Seed, bool bFixedOrientation, float Out[3])
	{
		const UniHlsl::float3 N = bFixedOrientation ? UniHlsl::float3(0.0f, 0.0f, 1.0f) : UniHlsl::Uni_DiscNormal(Seed);
		const UniHlsl::float3 A = UniHlsl::Uni_BhDiscAxis(Seed, N);
		Out[0] = A.x; Out[1] = A.y; Out[2] = A.z;
	}
	// ----------------------------------------------------------------------------------------
	// D-280: each black hole's look (the core's Uni_BhStyleOf about the engine's tuned look)
	// ----------------------------------------------------------------------------------------

	/** The engine's look (the centre, uu.Bh.Disc*) for one black hole of style S: every setting x
	 *  its multiplier (ApplyDiscLook clamps them), its palette. */
	inline FDiscLook StyledLook(const FDiscLook& Centre, const UniHlsl::FUniBhStyle& S)
	{
		FDiscLook L = Centre;
		L.Spiral *= S.SpiralMul;
		L.Thickness *= S.ThicknessMul;
		L.Opacity *= S.OpacityMul;
		L.Clouds *= S.CloudsMul;
		L.Bands *= S.BandsMul;
		L.Heat *= S.HeatMul;
		L.Erosion *= S.ErosionMul;
		L.PeriodS *= S.PeriodMul;
		L.Palette = S.Palette;
		L.PaletteMix = S.PaletteMix;
		return L;
	}

	/** The name of the galaxy emitters' arm winding range (D-256), the disc's spiral follows it. */
	static constexpr const char* kArmsWindingRange = "Arms.Winding";

	/** A galaxy's arms' winding over its emitter's range (0..1), as its field rolls it; -1 without
	 *  arms (an early type, or no galaxy-field emitter with that range). */
	inline float HostWinding01(const FArchetype& A, u32 Seed, bool bEarly)
	{
		if (bEarly) { return -1.0f; }
		for (const FUniEmitter& E : A.Emitters)
		{
			if (E.FieldId != UniHlsl::UNI_FIELD_GALAXY) { continue; }
			if (!(E.Exemplar.V[6] > 0.0f)) { return -1.0f; }   // no arms (a dwarf's exemplar)
			for (const FParamRange& R : E.Ranges)
			{
				if (R.Name != kArmsWindingRange) { continue; }
				const float V = UniHlsl::Uni_RollRange(Seed, ChannelIdUpper(R.Name), R.Min, R.Max, R.Bias, R.Quantize);
				const float Span = R.Max - R.Min;
				return std::fabs(Span) > 0.0f ? std::clamp((V - R.Min) / Span, 0.0f, 1.0f) : 0.5f;
			}
			return -1.0f;
		}
		return -1.0f;
	}

	/** The look settings (uu.Bh.DiscVariety, uu.Bh.DiscPalette). */
	struct FStyleConfig
	{
		float Variety = 1.0f;
		float PaletteMode = -1.0f;   // < 0 rolled; 0 the blackbody; n palette n for every black hole
	};

	/** A galaxy record's black hole's look (its host archetype for the arms' winding; nullptr: none). */
	inline UniHlsl::FUniBhStyle GalaxyBhStyle(const FObjectGPU& O, const FArchetype* Host, const FStyleConfig& C, float Active = UniHlsl::UNI_BH_ACTIVE_SHARE)
	{
		const UniHlsl::FUniObject Ho = ToHlsl(O);
		const bool bEarly = (UniHlsl::Uni_ObjectFlags(Ho) & UniHlsl::UNI_OBJ_FLAG_EARLY) != 0u;
		const UniHlsl::FUniBlackHole B = UniHlsl::Uni_GalaxyBlackHole(Ho, Active);
		const float W = Host ? HostWinding01(*Host, O.B[0], bEarly) : -1.0f;
		return UniHlsl::Uni_BhStyleOf(O.B[0], B, bEarly, W, C.Variety, C.PaletteMode);
	}
}
