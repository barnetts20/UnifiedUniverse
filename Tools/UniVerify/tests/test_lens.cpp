// test_lens.cpp
// D-273 (Docs/COMPACT_OBJECTS.md, phase D): lensing and the accretion disc (UniCompact.ush's
// Uni_LensTrace; the lens pass's CPU reference UniLensRef.h).
//   deflection     the traced ray's direction at infinity against the exact photon orbit
//                  (u'' + u = 1.5 u^2 integrated in double, by angle), cameras from 6 R_s to 1e6;
//                  the reference itself against the weak-field series far out.
//   shadow         the capture edge at the impact parameter 3 sqrt(3) / 2 (far camera) and at
//                  Synge's angle (near ones).
//   redshift       the disc's shift: face-on exactly sqrt(1 - 1.5 / x) (no Doppler along the
//                  line of sight); inclined, far out, special relativity's Doppler with the
//                  gravitational shift.
//   brightness     surface brightness conserved: a uniform scene stays uniform through the lens
//                  (1 to 1e-6, every pixel not in the shadow), the shadow's solid angle Synge's.
//   disc_light     the disc's light averaged over directions (the lens pass's render, lensing and
//                  all) against the nucleus it takes over from; its colour against the nucleus's.
//   cost           steps per ray over views from 10 R_s to 1e4.
//   handoff        a real galaxy's nucleus approached from far (sprite) through the pass's start
//                  (its point) and the disc's hand-off (point + disc): the light it shows the
//                  camera continuous (the sprite's whole light at every distance, 3%); the pass's
//                  point the sprite's style exactly (core, glare, sigmas).
//   foreground     D-275/D-279: something in front of the hole (the player) is never lensed: no copy
//                  of it anywhere else, its own pixels untouched, no contours beside it.
//   disc_knee      D-274: the disc's light softened above uu.Bh.DiscKnee: unchanged below, continuous,
//                  monotone, colour kept, 10x -> 2x, 1e8x -> 9x; 0 physical; still render/still_bh_knee.
//   disc_volume    D-276: the volumetric disc: its thin, opaque limit the thin disc's light; no jump
//                  at a flow phase's restart; the clouds turn the right way at the right speed; a
//                  camera inside it; the samples a ray takes; still render/still_bh_disc.
//   lens_scale     D-277: uu.Bh.LensScale: the lens's R_s k x, the disc's physical size kept (its outer
//                  radius / k in the lens's units, the same pixels), the ring sqrt(k) x, the shadow k x.
//   stills         lensing alone on a star field; a disc nearly edge-on; a quiet and an active
//                  black hole at the centre of a generated galaxy (volume and stars, lensed).

#include "../src/UniTest.h"
#include "../src/UniVerifyCommon.h"
#include "../src/UniRenderRef.h"
#include "../src/UniLensRef.h"
#include "../src/UniVolumeRef.h"
#include "../scenes/UniScenes.h"
#include "UniCompact.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	constexpr double Pi = 3.14159265358979323846;
	const double BCrit = 1.5 * std::sqrt(3.0);

	/** The exact photon orbit from a static camera at Rc looking Th from the black hole's direction
	 *  (the camera on +x, the ray turning towards +y): u'' = 1.5 u^2 - u by RK4 in phi. Escape:
	 *  the angle at infinity (its direction there, in the plane); captured: u reached 1. */
	struct FExact { bool bCaptured = false; double PhiInf = 0.0; };
	FExact ExactOrbit(double Rc, double Th)
	{
		FExact E;
		const double S = std::sqrt(1.0 - 1.0 / Rc);
		const double B = Rc * std::sin(Th) / S;
		double U = 1.0 / Rc;
		double W = std::sqrt(std::max(1.0 / (B * B) - U * U + U * U * U, 0.0)) * (std::cos(Th) > 0.0 ? 1.0 : -1.0);
		double Phi = 0.0;
		auto F = [](double u) { return 1.5 * u * u - u; };
		while (Phi < 60.0)
		{
			const double Hs = std::min(2.0e-4, 0.02 * std::max(U, 1.0e-3) / std::max(std::fabs(W), 1.0e-9));
			const double H0 = std::max(Hs, 1.0e-7);
			const double K1u = W, K1w = F(U);
			const double K2u = W + 0.5 * H0 * K1w, K2w = F(U + 0.5 * H0 * K1u);
			const double K3u = W + 0.5 * H0 * K2w, K3w = F(U + 0.5 * H0 * K2u);
			const double K4u = W + H0 * K3w, K4w = F(U + H0 * K3u);
			const double U1 = U + H0 / 6.0 * (K1u + 2 * K2u + 2 * K3u + K4u);
			const double W1 = W + H0 / 6.0 * (K1w + 2 * K2w + 2 * K3w + K4w);
			if (U1 <= 0.0) { E.PhiInf = Phi + H0 * U / (U - U1); return E; }
			if (U1 >= 1.0) { E.bCaptured = true; return E; }
			U = U1; W = W1; Phi += H0;
		}
		E.bCaptured = true;
		return E;
	}

	double WrapPi(double A) { while (A > Pi) { A -= 2.0 * Pi; } while (A < -Pi) { A += 2.0 * Pi; } return A; }

	H::FUniLensDisc NoDisc() { H::FUniLensDisc D{}; D.Axis = H::float3(0.0f, 0.0f, 1.0f); D.OuterX = 0.0f; D.PeakK = 0.0f; D.bDisc = 0.0f; return D; }
	H::FUniLensDisc DiscOf(float PeakK)
	{
		H::FUniLensDisc D{}; D.Axis = H::float3(0.0f, 0.0f, 1.0f); D.PeakK = PeakK; D.OuterX = H::Uni_BhDiscOuterX(PeakK); D.bDisc = 1.0f;
		return D;
	}

	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	/** A camera at distance Dx (R_s) on the direction (sin i, 0, cos i) from the black hole, looking at it. */
	FCamera CamAt(double Dx, double CosI, int Px, double Fov)
	{
		FCamera C; C.Width = Px; C.Height = Px; C.FovX = Fov;
		const double S = std::sqrt(std::max(1.0 - CosI * CosI, 0.0));
		C.LookAlong(FV3{ -S, 0.0, -CosI }, FV3{ 0.0, 0.0, 1.0 });
		(void)Dx;
		return C;
	}
}

UNI_TEST(lens, deflection)
{
	// The traced ray (float core) against the exact orbit (double, by angle) over impact parameters
	// from just outside the shadow to 3000 R_s, cameras 6 R_s .. 1e6 R_s, looking in and out.
	const double Cams[] = { 6.0, 15.0, 40.0, 300.0, 1.0e4, 1.0e6 };
	const double Bs[] = { 2.62, 2.65, 2.7, 2.8, 3.0, 3.5, 4.0, 5.0, 6.5, 8.0, 10.0, 15.0, 20.0, 30.0, 50.0, 100.0, 300.0, 1000.0, 3000.0 };
	double WorstNear = 0.0, WorstFar = 0.0, WorstRel = 0.0;
	size_t Cases = 0, CapMismatch = 0;
	std::string Table;
	for (double Rc : Cams)
	{
		const double S = std::sqrt(1.0 - 1.0 / Rc);
		for (double B : Bs)
		{
			const double SinTh = B * S / Rc;
			if (SinTh >= 0.999) { continue; }
			for (int Out = 0; Out < 2; ++Out)
			{
				if (Out == 1 && Rc > 50.0) { continue; }   // looking away from a far black hole: nothing to bend
				const double Th = Out ? Pi - std::asin(SinTh) : std::asin(SinTh);
				const FExact E = ExactOrbit(Rc, Th);
				const H::FUniLensResult R = H::Uni_LensTrace(H::float3(float(Rc), 0.0f, 0.0f), H::float3(float(-std::cos(Th)), float(std::sin(Th)), 0.0f), NoDisc(), H::UNI_LENS_MAX_STEPS);
				++Cases;
				if (E.bCaptured != (R.Captured > 0.5f)) { ++CapMismatch; continue; }
				if (E.bCaptured) { continue; }
				const double Got = std::atan2(double(R.Dir.y), double(R.Dir.x));
				const double Err = std::fabs(WrapPi(Got - E.PhiInf));
				const double Defl = std::fabs(WrapPi(E.PhiInf - (Pi - Th)));   // from the straight line
				if (B < 3.0) { WorstNear = std::max(WorstNear, Err); } else { WorstFar = std::max(WorstFar, Err); }
				if (Defl > 1e-6) { WorstRel = std::max(WorstRel, Err / Defl); }
				if (Rc == 1.0e4 && Out == 0 && (B == 2.65 || B == 4.0 || B == 30.0 || B == 1000.0)) { Table += Uni::Fmt("%sb %.4g: %.6f vs %.6f", Table.empty() ? "" : "; ", B, Defl, std::fabs(WrapPi(Got - (Pi - Th)))); }
			}
		}
	}
	// The reference against the weak-field series (2/b + 15 pi / 16 / b^2), a camera at 1e7.
	double SeriesErr = 0.0;
	for (double B : { 100.0, 300.0, 1000.0 })
	{
		const double Rc = 1.0e7, Th = std::asin(B * std::sqrt(1.0 - 1.0 / Rc) / Rc);
		const FExact E = ExactOrbit(Rc, Th);
		const double Defl = WrapPi(E.PhiInf - (Pi - Th));
		const double Want = 2.0 / B + 15.0 * Pi / 16.0 / (B * B);
		SeriesErr = std::max(SeriesErr, std::fabs(Defl / Want - 1.0));
	}
	const bool bOk = CapMismatch == 0 && WorstFar < 2.0e-4 && WorstNear < 3.0e-3 && SeriesErr < 2.0e-3;
	Uni::Check(1, "T1.lens.deflection", "D-273 the traced ray's direction at infinity against the exact photon orbit (u'' + u = 1.5 u^2, double): captured the same; within 2e-4 rad from b = 3 R_s out, 3e-3 just outside the shadow (b 2.62-3), cameras 6 R_s to 1e6; the reference within 0.2% of the weak-field series (2/b + 15 pi / 16 b^2)")
		.Metric("worst_far_rad", WorstFar).Metric("worst_near_rad", WorstNear).Metric("worst_rel", WorstRel).Metric("series_err", SeriesErr).Metric("cases", double(Cases))
		.Pass(bOk, Uni::Fmt("%zu rays: %zu captured differently; worst %.2g rad (b >= 3), %.2g rad (b < 3), %.2g of the deflection; reference vs series %.2g; deflections at 1e4 R_s (exact vs traced): %s",
			Cases, CapMismatch, WorstFar, WorstNear, WorstRel, SeriesErr, Table.c_str()));
}

UNI_TEST(lens, shadow)
{
	// The capture edge by bisection on the camera's angle: at 1e4 R_s the impact parameter
	// 3 sqrt(3)/2; near, Synge's angle sin theta = b_c sqrt(1 - 1/r) / r.
	std::string Txt;
	double Worst = 0.0;
	for (double Rc : { 1.0e4, 100.0, 10.0, 4.0 })
	{
		double Lo = 0.0, Hi = Rc < 10.0 ? 1.5 : std::asin(std::min(4.0 / Rc, 0.99));
		for (int I = 0; I < 50; ++I)
		{
			const double M = 0.5 * (Lo + Hi);
			const H::FUniLensResult R = H::Uni_LensTrace(H::float3(float(Rc), 0.0f, 0.0f), H::float3(float(-std::cos(M)), float(std::sin(M)), 0.0f), NoDisc(), H::UNI_LENS_MAX_STEPS);
			if (R.Captured > 0.5f) { Lo = M; } else { Hi = M; }
		}
		const double Th = 0.5 * (Lo + Hi);
		const double Synge = std::asin(std::min(BCrit * std::sqrt(1.0 - 1.0 / Rc) / Rc, 1.0));
		const double BEdge = Rc * std::sin(Th) / std::sqrt(1.0 - 1.0 / Rc);
		const double Err = std::fabs(BEdge / BCrit - 1.0);
		Worst = std::max(Worst, Err);
		Txt += Uni::Fmt("%sr %.4g: b %.5f (angle %.6g vs Synge %.6g)", Txt.empty() ? "" : "; ", Rc, BEdge, Th, Synge);
	}
	Uni::Check(1, "T1.lens.shadow", "D-273 the shadow: the capture edge's impact parameter 3 sqrt(3)/2 R_s within 0.2% (Synge's angle) for cameras at 1e4, 100, 10 and 4 R_s")
		.Metric("worst_rel", Worst)
		.Pass(Worst < 2.0e-3, Uni::Fmt("worst %.2g; %s", Worst, Txt.c_str()));
}

UNI_TEST(lens, redshift)
{
	// Face-on (camera on the axis): every crossing's g is sqrt(1 - 1.5 / x) (the photon's angular
	// momentum about the axis is 0). Inclined 60 degrees, far out (x 100-400, camera 1e5): special
	// relativity's Doppler at the orbit's speed measured by a static observer, with the
	// gravitational shift: g = sqrt(1 - 1/x) / (gamma (1 - beta . n)).
	H::FUniLensDisc D = NoDisc();
	D.bDisc = 1.0f; D.OuterX = 400.0f; D.PeakK = 1.0e5f;
	double WorstFace = 0.0, WorstIncl = 0.0;
	size_t NFace = 0, NIncl = 0;
	{
		const float Rc = 1.0e4f;
		for (int I = 0; I < 400; ++I)
		{
			const double Ang = (3.2 + 395.0 * I / 399.0) / 1.0e4;   // impact ~ the crossing's radius
			const double Az = 2.399963 * I;
			const H::float3 Dir(float(std::sin(Ang) * std::cos(Az)), float(std::sin(Ang) * std::sin(Az)), float(-std::cos(Ang)));
			const H::FUniLensResult R = H::Uni_LensTrace(H::float3(0.0f, 0.0f, Rc), Dir, D, H::UNI_LENS_MAX_STEPS);
			if (!(R.FirstX > 0.0f)) { continue; }
			++NFace;
			WorstFace = std::max(WorstFace, std::fabs(double(R.FirstG) / std::sqrt(1.0 - 1.5 / double(R.FirstX)) - 1.0));
		}
	}
	{
		const double Rc = 1.0e5, Ci = 0.5, Si = std::sqrt(0.75);
		const FV3 O{ Si, 0.0, Ci };   // to the camera
		const H::float3 Cam(float(Rc * Si), 0.0f, float(Rc * Ci));
		for (int I = 0; I < 600; ++I)
		{
			// Aim at disc points x 100..400, every azimuth: from the camera to that point.
			const double X = 100.0 + 300.0 * ((I * 0.618034) - std::floor(I * 0.618034));
			const double Az = 2.0 * Pi * I / 600.0;
			const FV3 P{ X * std::cos(Az), X * std::sin(Az), 0.0 };
			const FV3 Dr = Norm(FV3{ P.X - Rc * Si, P.Y, P.Z - Rc * Ci });
			const H::FUniLensResult R = H::Uni_LensTrace(Cam, H::float3(float(Dr.X), float(Dr.Y), float(Dr.Z)), D, H::UNI_LENS_MAX_STEPS);
			if (!(R.FirstX > 90.0f)) { continue; }
			const double Xr = R.FirstX;
			const FV3 Pc{ R.FirstPos.x, R.FirstPos.y, R.FirstPos.z };
			const FV3 Vhat = Norm(FV3{ -Pc.Y, Pc.X, 0.0 });          // counter-clockwise about z
			const double Beta = std::sqrt(0.5 / (Xr - 1.0)), Gam = 1.0 / std::sqrt(1.0 - Beta * Beta);
			const FV3 N = Norm(FV3{ O.X * Rc - Pc.X, O.Y * Rc - Pc.Y, O.Z * Rc - Pc.Z });   // towards the camera (straight, far out)
			const double Want = std::sqrt(1.0 - 1.0 / Xr) / (Gam * (1.0 - Beta * Dot(Vhat, N)));
			++NIncl;
			WorstIncl = std::max(WorstIncl, std::fabs(double(R.FirstG) / Want - 1.0));
		}
	}
	const bool bOk = NFace > 300 && NIncl > 400 && WorstFace < 1.0e-4 && WorstIncl < 3.0e-3;
	Uni::Check(1, "T1.lens.redshift", "D-273 the disc's shift g: face-on sqrt(1 - 1.5/x) within 1e-4 (no Doppler along the line of sight); inclined 60 degrees, x 100-400, within 3e-3 of special relativity's Doppler with the gravitational shift (the bending there ~2/x)")
		.Metric("worst_face", WorstFace).Metric("worst_inclined", WorstIncl)
		.Pass(bOk, Uni::Fmt("face-on %zu crossings worst %.2g; inclined %zu worst %.2g", NFace, WorstFace, NIncl, WorstIncl));
}

UNI_TEST(lens, brightness)
{
	// A uniform scene (1 everywhere) through the lens pass (no disc), a camera at 30 R_s looking at
	// the black hole: every pixel 1 to 1e-6 or in the shadow (0); the shadow's solid angle Synge's.
	const int Px = 384;
	FCamera C = CamAt(30.0, 0.3, Px, 0.9);
	FImage In(Px, Px);
	std::fill(In.Rgb.begin(), In.Rgb.end(), 1.0f);
	FLensView L;
	L.CamRs = H::float3(float(30.0 * std::sqrt(1.0 - 0.09)), 0.0f, float(30.0 * 0.3));
	L.Disc = NoDisc();
	FImage Out(Px, Px);
	FLensStats St;
	ApplyLens(In, C, L, Out, &St);
	size_t Odd = 0;
	double ShadowSr = 0.0;
	for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
	{
		const float V = Out.Rgb[(size_t(Y) * Px + X) * 3];
		if (V == 0.0f)
		{
			double CosF = 1.0; PixelRay(C, X + 0.5, Y + 0.5, CosF);
			ShadowSr += CosF * CosF * CosF / (C.FocalPx() * C.FocalPx());
		}
		else if (std::fabs(V - 1.0f) > 1.0e-6f) { ++Odd; }
	}
	const double Synge = std::asin(BCrit * std::sqrt(1.0 - 1.0 / 30.0) / 30.0);
	const double Want = 2.0 * Pi * (1.0 - std::cos(Synge));
	const double Err = std::fabs(ShadowSr / Want - 1.0);
	Uni::Check(1, "T1.lens.brightness", "D-273 surface brightness conserved: a uniform scene through the lens pass (camera 30 R_s) every pixel 1 to 1e-6 or in the shadow; the shadow's solid angle Synge's within 2% (384^2, pixel edges)")
		.Metric("odd_pixels", double(Odd)).Metric("shadow_err", Err).Metric("mean_steps", St.Traced ? St.StepSum / double(St.Traced) : 0.0)
		.Pass(Odd == 0 && Err < 0.02, Uni::Fmt("%zu pixels traced, %zu neither 1 nor shadow; shadow %.5g sr vs Synge %.5g (%.2g); %zu captured", St.Traced, Odd, ShadowSr, Want, Err, St.Captured));
}

UNI_TEST(lens, disc_light)
{
	// The disc carries the nucleus's light, through the table (UNI_BH_DISC_BEAM / LOGNORM,
	// `univerify lenstable`): fresh far-field traces (lensing, shifts and all) at peaks between the
	// table's rows and at cos i between its columns against the table's prediction
	// 10^LOGNORM x BEAM; the mean over directions (Simpson, 9 inclinations) against 10^LOGNORM;
	// the beam's mean over directions 1 at every row; the disc's colour averaged over directions
	// against the nucleus's (Uni_NucleusColour's 12 taps, unshifted).
	std::string Txt;
	double WorstPoint = 0.0, WorstMean = 0.0, WorstChroma = 0.0, WorstBeamMean = 0.0;
	for (H::uint T = 0; T < H::UNI_BH_BEAM_NT; ++T)
	{
		double M = 0.0;
		for (H::uint I = 0; I < H::UNI_BH_BEAM_NM; ++I) { M += double(H::UNI_BH_DISC_BEAM[T * H::UNI_BH_BEAM_NM + I]) * ((I == 0 || I == H::UNI_BH_BEAM_NM - 1) ? 1.0 : (I % 2 ? 4.0 : 2.0)); }
		WorstBeamMean = std::max(WorstBeamMean, std::fabs(M / (3.0 * double(H::UNI_BH_BEAM_NM - 1)) - 1.0));
	}
	for (float Peak : { 5000.0f, 8000.0f, 20000.0f, 50000.0f, 3.0e5f, 3.0e6f })
	{
		double Mean = 0.0, Rgb[3] = { 0.0, 0.0, 0.0 }, WorstHere = 0.0;
		const int Nm = 9;
		for (int Im = 0; Im < Nm; ++Im)
		{
			for (int Half = 0; Half < 2; ++Half)
			{
				if (Half == 1 && Im == Nm - 1) { continue; }
				const double Mu = (Im + 0.5 * Half) / double(Nm - 1);
				double F[3];
				DiscFarLight(Peak, Mu, 1.0e5, 160, 96, F);
				const double L = LumOf(F);
				const double Want = std::pow(10.0, double(H::Uni_BhDiscLog10Norm(Peak))) * double(H::Uni_BhDiscBeam(Peak, float(Mu)));
				const double Err = std::fabs(L / Want - 1.0);
				WorstHere = std::max(WorstHere, Err);
				if (Half == 0)
				{
					const double W = (Im == 0 || Im == Nm - 1) ? 1.0 : (Im % 2 ? 4.0 : 2.0);
					Mean += L * W;
					for (int K = 0; K < 3; ++K) { Rgb[K] += F[K] * W; }
				}
			}
		}
		Mean /= 3.0 * double(Nm - 1);
		const double MeanErr = std::fabs(Mean / std::pow(10.0, double(H::Uni_BhDiscLog10Norm(Peak))) - 1.0);
		// The nucleus's colour at this peak (Uni_NucleusColour's taps, read at the peak directly).
		H::float3 Sum(0.0f, 0.0f, 0.0f);
		const float Outer = H::Uni_BhDiscOuterX(Peak);
		for (H::uint I = 0; I < H::UNI_BH_COLOUR_TAPS; ++I)
		{
			const float X = H::UNI_BH_ISCO_X * std::exp(std::log(Outer / H::UNI_BH_ISCO_X) * ((float(I) + 0.5f) / float(H::UNI_BH_COLOUR_TAPS)));
			Sum = Sum + H::Uni_BhDiscLight(Peak * H::Uni_BhDiscProfile(X)) * (H::Uni_BhDiscOpacity(X, Outer) * X * X);
		}
		const double Ts = double(Sum.x) + Sum.y + Sum.z, Tr = Rgb[0] + Rgb[1] + Rgb[2];
		const double Sc[3] = { Sum.x / Ts, Sum.y / Ts, Sum.z / Ts };
		double Chroma = 0.0;
		for (int K = 0; K < 3; ++K) { Chroma = std::max(Chroma, std::fabs(Rgb[K] / Tr - Sc[K])); }
		WorstPoint = std::max(WorstPoint, WorstHere);
		WorstMean = std::max(WorstMean, MeanErr);
		WorstChroma = std::max(WorstChroma, Chroma);
		Txt += Uni::Fmt("%s%.0f K: worst %.3f, mean %.4f, chroma %.4f", Txt.empty() ? "" : "; ", double(Peak), WorstHere, MeanErr, Chroma);
	}
	const bool bOk = WorstPoint < 0.03 && WorstMean < 0.01 && WorstBeamMean < 1.0e-4 && WorstChroma < 0.012;
	Uni::Check(1, "T1.lens.disc_light", "D-273 the disc carries the nucleus's light (the table, UNI_BH_DISC_BEAM / LOGNORM): fresh far-field traces between the table's rows and columns within 3% of it; their mean over directions within 1% of 10^LOGNORM; each row's beam averaging 1; the disc's colour over directions within 0.012 chromaticity of the nucleus's")
		.Metric("worst_point", WorstPoint).Metric("worst_mean", WorstMean).Metric("worst_beam_mean", WorstBeamMean).Metric("worst_chroma", WorstChroma)
		.Pass(bOk, Uni::Fmt("beam rows' means within %.2g of 1; %s", WorstBeamMean, Txt.c_str()));
}

UNI_TEST(lens, cost)
{
	// Steps per traced ray over 256^2 views of a disc (peak 25000 K) from 10, 30, 300 and 1e4 R_s
	// (the pass's region at each): the mean, the most, the share stopped at the cap.
	const H::FUniLensDisc D = DiscOf(25000.0f);
	std::string Txt;
	double WorstMean = 0.0, WorstCapShare = 0.0;
	for (double Dx : { 10.0, 30.0, 300.0, 1.0e4 })
	{
		FCamera C = CamAt(Dx, 0.2, 256, Dx < 50.0 ? 1.6 : 2.0 * std::atan(1.3 * D.OuterX / Dx));
		FLensView L;
		L.CamRs = H::float3(float(Dx * std::sqrt(0.96)), 0.0f, float(Dx * 0.2));
		L.Disc = D;
		L.DiscScale = 1.0e-6;
		FImage In(256, 256), Out(256, 256);
		FLensStats St;
		ApplyLens(In, C, L, Out, &St);
		const double Mean = St.Traced ? St.StepSum / double(St.Traced) : 0.0;
		const double CapShare = St.Traced ? double(St.Capped) / double(St.Traced) : 0.0;
		WorstMean = std::max(WorstMean, Mean);
		WorstCapShare = std::max(WorstCapShare, CapShare);
		Txt += Uni::Fmt("%s%.4g R_s: mean %.1f, max %.0f, capped %.2g%%", Txt.empty() ? "" : "; ", Dx, Mean, St.StepMax, 100.0 * CapShare);
	}
	Uni::Check(1, "T1.lens.cost", "D-273 the lens pass's cost: RK4 steps per traced ray at most 120 on average over views from 10 R_s to 1e4 R_s; under 0.5% of rays stopped at the step cap (those wind at the photon sphere)")
		.Metric("worst_mean_steps", WorstMean).Metric("worst_capped_share", WorstCapShare)
		.Pass(WorstMean < 120.0 && WorstCapShare < 0.005, Txt);
}

namespace
{
	/** The web scene's resident galaxies (void camera) with their nuclei: the galaxy with the brightest
	 *  nucleus relative to its host, and one whose black hole is quiet (no nucleus drawn). */
	struct FGalaxyPick { FCellKey K{}; FObjectGPU O{}; bool bFound = false; float Ratio = 0.0f; };
	struct FBhScene
	{
		FUniverseModel M{ UniScenes::WebToGalaxy() };
		FRefGenerator G{ M };
		u32 GalMask = 0;
		FGalaxyPick Active, Quiet;
		FBhScene()
		{
			FRefPipeline P(G);
			P.bVelocityLod = false;
			P.Update(UniScenes::VoidCamera(), FVec3f{});
			for (const FArchetype& A : M.Definition().Archetypes)
			{
				for (const FUniEmitter& E : A.Emitters) { if (E.FieldId == H::UNI_FIELD_GALAXY && A.Id < 32u) { GalMask |= 1u << A.Id; } }
			}
			P.ForEachResident([&](const FCellKey& K, const FObjectGPU& O, float, u64)
			{
				const u32 Arch = O.A[3] & 0xFFFFu;
				if (Arch >= 32u || !((GalMask >> Arch) & 1u) || !((O.A[3] >> 16) & H::UNI_OBJ_FLAG_HAS_CONTRIBUTOR)) { return; }
				const H::FUniObject Ho = ToHlsl(O);
				const float Host = H::Uni_ObjectAbsMag(Ho);
				const H::FUniBlackHole Bh = H::Uni_GalaxyBlackHole(Ho, H::UNI_BH_ACTIVE_SHARE);
				const float Mn = H::Uni_NucleusAbsMag(Bh, Host, 0.5f, 1.0f);
				const float Ratio = Mn < H::UNI_BH_NONE_MAG ? float(std::pow(10.0, -0.4 * double(Mn - Host))) : 0.0f;
				if (Ratio > Active.Ratio) { Active = FGalaxyPick{ K, O, true, Ratio }; }
				if (!Quiet.bFound && Bh.Log10Eddington < -6.0f && Host < -20.0f) { Quiet = FGalaxyPick{ K, O, true, Ratio }; }
			});
		}
	};

	/** A camera at distance Dcm from a galaxy's black hole, looking at it along Dir (unit). */
	FCoord CameraNear(const FGalaxyPick& Gp, const FV3& Dir, double Dcm, FVec3f& Frac)
	{
		FCoord C = ObjectCoord(Gp.K, Gp.O.A);
		Frac = FVec3f{};
		AddDisplacement(C, Frac, FVec3d{ -Dir.X * Dcm, -Dir.Y * Dcm, -Dir.Z * Dcm });
		return C;
	}

	FV3 ViewDirAt(const float Axis[3], double CosI)
	{
		const FV3 A{ Axis[0], Axis[1], Axis[2] };
		const FV3 P = Norm(Cross(A, std::fabs(A.Z) < 0.9 ? FV3{ 0.0, 0.0, 1.0 } : FV3{ 1.0, 0.0, 0.0 }));
		const double S = std::sqrt(1.0 - CosI * CosI);
		return Norm(FV3{ -(A.X * CosI + P.X * S), -(A.Y * CosI + P.Y * S), -(A.Z * CosI + P.Z * S) });   // from the camera towards the hole
	}

	/** The disc's light the pass draws (exposure units): a log-polar grid of rays about the hole. */
	double DiscFluxNear(const FLensTarget& T, double ExposureMag)
	{
		if (!T.bDisc || !(T.DiscShare > 0.0f)) { return 0.0; }
		const double Dx = T.DistanceCm / T.RsCm;
		const H::float3 Cam(float(-T.RelCm[0] / T.RsCm), float(-T.RelCm[1] / T.RsCm), float(-T.RelCm[2] / T.RsCm));
		const FV3 Fw{ T.RelCm[0] / T.DistanceCm, T.RelCm[1] / T.DistanceCm, T.RelCm[2] / T.DistanceCm };
		const FV3 E1 = Norm(Cross(Fw, std::fabs(Fw.Z) < 0.9 ? FV3{ 0.0, 0.0, 1.0 } : FV3{ 1.0, 0.0, 0.0 })), E2 = Cross(E1, Fw);
		H::FUniLensDisc D{};
		D.Axis = H::float3(T.Axis[0], T.Axis[1], T.Axis[2]); D.OuterX = T.OuterX; D.PeakK = T.PeakK; D.bDisc = 1.0f;
		const double Scale = std::pow(10.0, T.DiscLog10Scale + 0.4 * ExposureMag) * double(T.DiscShare);
		const int Nr = 160, Nps = 96;
		const double ThLo = 0.4 / Dx, ThHi = 1.3 * double(T.OuterX) / Dx, LnSpan = std::log(ThHi / ThLo);
		double Sum = 0.0;
		for (int Ir = 0; Ir < Nr; ++Ir)
		{
			const double Th = ThLo * std::exp(LnSpan * (Ir + 0.5) / Nr), Dth = Th * LnSpan / Nr, Ct = std::cos(Th), St = std::sin(Th);
			for (int Ip = 0; Ip < Nps; ++Ip)
			{
				const double Ps = 6.283185307179586 * (Ip + 0.5) / Nps, Cp = std::cos(Ps), Sp = std::sin(Ps);
				const FV3 Dd{ Fw.X * Ct + (E1.X * Cp + E2.X * Sp) * St, Fw.Y * Ct + (E1.Y * Cp + E2.Y * Sp) * St, Fw.Z * Ct + (E1.Z * Cp + E2.Z * Sp) * St };
				const H::FUniLensResult R = H::Uni_LensTrace(Cam, H::float3(float(Dd.X), float(Dd.Y), float(Dd.Z)), D, H::UNI_LENS_MAX_STEPS);
				Sum += H::Uni_Luminance(R.Disc) * St * Dth * (6.283185307179586 / Nps);
			}
		}
		return Sum * Scale;
	}

	/** The 95th percentile of the disc's pixels (luminance, the pass over a black scene). */
	double DiscP95(const FCamera& C, const FLensView& L)
	{
		FImage Black(C.Width, C.Height), Probe(C.Width, C.Height);
		FLensView Lp = L;
		for (int K = 0; K < 3; ++K) { Lp.PointCore[K] = 0.0; Lp.PointHalo[K] = 0.0; }
		Lp.DiscScale = 1.0;   // at unit scale (the pass clamps scene colour at 6e4), scaled back below
		ApplyLens(Black, C, Lp, Probe);
		std::vector<double> V;
		for (size_t P = 0; P < Probe.Rgb.size(); P += 3)
		{
			const double Y = 0.2126 * Probe.Rgb[P] + 0.7152 * Probe.Rgb[P + 1] + 0.0722 * Probe.Rgb[P + 2];
			if (Y > 0.0) { V.push_back(Y); }
		}
		if (V.empty()) { return 1.0; }
		std::sort(V.begin(), V.end());
		return std::max(V[V.size() * 95 / 100] * L.DiscScale, 1e-300);
	}

	/** The pass's point (FLensView fields) for a target and view, as the renderer builds it. */
	void PointOf(const FLensTarget& T, const H::FUniView& V, FLensView& L)
	{
		if (!(T.PointAppMag < H::UNI_BH_NONE_MAG)) { return; }
		const float Keep = 1.0f - std::clamp(T.DiscShare, 0.0f, 1.0f);
		const float I = float(std::pow(10.0, -0.4 * (double(T.PointAppMag) - double(V.ExposureMag)))) * H::Uni_MagFade(T.PointAppMag, V.LimitingMag) * H::Uni_HorizonDimming(float(T.DistanceCm)) * Keep;
		if (!(I > 0.0f)) { return; }
		const H::float4 St = H::Uni_PointStyle(I, T.PointPhysAngRad, V);
		const H::float3 Col(T.PointColour[0], T.PointColour[1], T.PointColour[2]);
		const H::float3 Core = Col * (St.x / std::max(H::Uni_Luminance(Col), 1.0e-3f));
		L.PointCore[0] = Core.x; L.PointCore[1] = Core.y; L.PointCore[2] = Core.z;
		L.PointCoreSigmaPx = St.z / V.ThetaPixel;
		if (St.y > 0.0f && St.x > 0.0f)
		{
			const H::float3 Halo = H::Uni_SpriteChroma(Core, H::UNI_SPRITE_HALO_CHROMA) * (St.y / St.x);
			L.PointHalo[0] = Halo.x; L.PointHalo[1] = Halo.y; L.PointHalo[2] = Halo.z;
			L.PointHaloSigmaPx = St.w / V.ThetaPixel;
		}
	}
}

UNI_TEST(lens, handoff)
{
	// The web scene's galaxy with the brightest nucleus (relative to its host), approached along a
	// line 60 degrees from its disc's axis: from the sprite alone, through the lens pass's start
	// (Einstein ring 0.25 px: the pass draws the nucleus as its point, the sprite held back) and the
	// disc's hand-off (2-6 px: point x (1 - share) + disc x share), to the disc alone. At each
	// distance the light shown (physical style: all in the core) against the sprite's whole light
	// there (its magnitude with the disc's light towards the camera): within 3%. At the pass's start
	// the point's style (default: saturation and glare) the sprite's (core light, glare ratio, sigmas;
	// 1e-5).
	FBhScene Sc;
	if (!Sc.Active.bFound)
	{
		Uni::Check(2, "T2.lens.handoff", "D-273 the nucleus's light continuous from sprite to point to disc").Pass(false, "no galaxy with a nucleus in the web scene");
		return;
	}
	const H::FUniObject Ho = ToHlsl(Sc.Active.O);
	const H::FUniBlackHole Bh = H::Uni_GalaxyBlackHole(Ho, H::UNI_BH_ACTIVE_SHARE);
	const float Mn = H::Uni_NucleusAbsMag(Bh, H::Uni_ObjectAbsMag(Ho), 0.5f, 1.0f);
	float Axis[3];
	GalaxyDiscAxis(Ho.B.x, false, Axis);
	const FV3 Dir = ViewDirAt(Axis, 0.5);
	const double Rs = std::pow(10.0, double(H::UNI_BH_LOG_RS_SUN_CM) + double(Bh.Log10Mass));
	const float OuterX = H::Uni_BhDiscOuterX(H::Uni_BhDiscPeakK(Bh));
	FCamera Cm; Cm.Width = 640; Cm.Height = 360;
	const double ThetaPx = 1.0 / Cm.FocalPx();
	FLensConfig Lc;
	// Distances: the ring at 0.1 .. 0.4 px (the pass starts at 0.25), the disc at 0.5 .. 10 px.
	std::vector<double> Ds;
	for (double Rp : { 0.1, 0.2, 0.24, 0.26, 0.3, 0.4 }) { Ds.push_back(Rs * 2.0 / (Rp * ThetaPx) / (Rp * ThetaPx)); }
	for (double Dp : { 0.5, 1.0, 1.9, 2.1, 3.0, 4.0, 5.0, 5.9, 6.1, 8.0, 10.0 }) { Ds.push_back(Rs * double(OuterX) / (Dp * ThetaPx)); }
	std::sort(Ds.begin(), Ds.end(), std::greater<double>());
	double Worst = 0.0, StyleWorst = 0.0;
	size_t Inactive = 0, Active = 0, SpriteLeak = 0;
	std::string Txt;
	for (double D : Ds)
	{
		FVec3f Fr{};
		const FCoord CamG = CameraNear(Sc.Active, Dir, D, Fr);
		H::uint4 Dd[3];
		CellDelta(Sc.Active.K, CamG, Dd);
		FCamera Cv = Cm; Cv.Coord = CamG; Cv.LookAlong(Dir);
		Cv.NucleusArchetypeMask = Sc.GalMask; Cv.DiscArchetypeMask = Sc.GalMask;
		H::FUniView V = Cv.View(); V.LimitingMag = 30.0f; V.ExposureMag = 0.0f; V.CameraFraction = H::float3(Fr.X, Fr.Y, Fr.Z);
		H::FUniView Vp = V; Vp.CoreSat = 0.0f;   // physical: all the light in the core
		const H::FUniSprite Whole = H::Uni_ProjectNucleusH(Dd[0], Dd[1], Dd[2], u32(Sc.Active.K.Level), 1.0f, Ho, Vp);
		const double Want = H::Uni_Luminance(Whole.Radiance);
		const FVec3d Rel = ToDouble(Sub(ObjectCoord(Sc.Active.K, Sc.Active.O.A), CamG));
		const double RelCm[3] = { Rel.X - Fr.X, Rel.Y - Fr.Y, Rel.Z - Fr.Z };
		const FLensTarget T = LensTargetOf(Bh, Mn, RelCm, Axis, Ho.B.x, ThetaPx, Lc);
		double Got = 0.0;
		if (!T.bActive) { Got = Want; ++Inactive; }
		else
		{
			++Active;
			H::FUniView Vh = Vp; Vh.NucleusHandSeed = Ho.B.x; Vh.NucleusHandShare = 1.0f;
			const H::FUniSprite Held = H::Uni_ProjectNucleusH(Dd[0], Dd[1], Dd[2], u32(Sc.Active.K.Level), 1.0f, Ho, Vh);
			if (Held.Flags & H::UNI_SPRITE_VISIBLE) { ++SpriteLeak; }
			FLensView L;
			PointOf(T, Vp, L);
			Got = 0.2126 * L.PointCore[0] + 0.7152 * L.PointCore[1] + 0.0722 * L.PointCore[2] + DiscFluxNear(T, double(Vp.ExposureMag));
			// The point's style against the sprite's (default style), with the disc's share 0.
			FLensTarget T0 = T; T0.DiscShare = 0.0f;
			FLensView L0; PointOf(T0, V, L0);
			const H::FUniSprite S = H::Uni_ProjectNucleusH(Dd[0], Dd[1], Dd[2], u32(Sc.Active.K.Level), 1.0f, Ho, V);
			const double Ce = 0.2126 * L0.PointCore[0] + 0.7152 * L0.PointCore[1] + 0.0722 * L0.PointCore[2];
			const double He = 0.2126 * L0.PointHalo[0] + 0.7152 * L0.PointHalo[1] + 0.0722 * L0.PointHalo[2];
			StyleWorst = std::max({ StyleWorst, std::fabs(Ce / std::max(double(H::Uni_Luminance(S.Radiance)), 1e-30) - 1.0),
				std::fabs(He / std::max(Ce, 1e-30) - double(S.HaloRatio)), std::fabs(L0.PointCoreSigmaPx * ThetaPx / double(S.AngularRadius) - 1.0),
				S.HaloRatio > 0.0f ? std::fabs(L0.PointHaloSigmaPx * ThetaPx / double(S.HaloRadius) - 1.0) : 0.0 });
		}
		const double Err = std::fabs(Got / std::max(Want, 1e-300) - 1.0);
		Worst = std::max(Worst, Err);
		Txt += Uni::Fmt("%s%.3g R_s ring %.2f disc %.2f px share %.2f: %.4f", Txt.empty() ? "" : "; ", D / Rs, double(T.RingPx), double(T.DiscPx), double(T.DiscShare), Got / std::max(Want, 1e-300));
	}
	const bool bOk = Inactive > 0 && Active > 0 && SpriteLeak == 0 && Worst < 0.03 && StyleWorst < 1.0e-5;
	Uni::Check(2, "T2.lens.handoff", "D-273 a galaxy's nucleus approached 60 degrees from its disc's axis: the light shown (sprite; then the pass's point, the sprite held back; then point x (1 - share) + disc x share) within 3% of the sprite's whole light at every distance; the point's style the sprite's (1e-5)")
		.Metric("worst", Worst).Metric("style_worst", StyleWorst).Metric("sprite_leak", double(SpriteLeak))
		.Pass(bOk, Uni::Fmt("galaxy nucleus/host %.3f, M_bh 10^%.2f, lambda 10^%.2f, disc peak %.0f K, outer %.0f R_s; %zu distances sprite, %zu the pass's; sprite visible while held %zu; worst %.4f; style %.2g; %s",
			double(Sc.Active.Ratio), double(Bh.Log10Mass), double(Bh.Log10Eddington), double(H::Uni_BhDiscPeakK(Bh)), double(OuterX), Inactive, Active, SpriteLeak, Worst, StyleWorst, Txt.c_str()));
}

UNI_TEST(lens, stills)
{
	// Four stills (tonemapped 1 - exp(-I)), each 480 x 300:
	//   a. lensing alone: a camera 15 R_s from a quiet black hole over a star field;
	//   b. a disc (peak 12000 K) seen 83 degrees from its axis from 60 R_s over the same field;
	//   c. the web scene's quiet black hole at the centre of its galaxy (stars and volume), 40 R_s;
	//   d. its brightest nucleus's disc from 60 R_s, 80 degrees from its axis, exposed for the disc
	//      (its 95th-percentile pixel at 1; the galaxy behind outshone). b likewise.
	const int W = 480, Hh = 300;
	FImage Sheet(2 * W, 2 * Hh);
	auto Blit = [&](const FImage& I, int Ox, int Oy) { for (int Y = 0; Y < Hh; ++Y) for (int X = 0; X < W; ++X) { const float* P = &I.Rgb[(size_t(Y) * W + X) * 3]; Sheet.Add(Ox + X, Oy + Y, P[0], P[1], P[2]); } };
	auto StarField = [&](FImage& I)
	{
		for (int K = 0; K < 1600; ++K)
		{
			const double X = W * H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K)), Y = Hh * H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K + 1));
			const double B = 0.05 + 2.0 * std::pow(double(H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K + 2))), 6.0);
			const H::float3 C = H::Uni_TemperatureColour(3500.0f + 9000.0f * H::Uni_DrawUnit(0x5EEDu, 0x52u, u32(K)));
			for (int Dy = -2; Dy <= 2; ++Dy) for (int Dx = -2; Dx <= 2; ++Dx)
			{
				const double G = B * std::exp(-0.5 * ((std::floor(X) + Dx + 0.5 - X) * (std::floor(X) + Dx + 0.5 - X) + (std::floor(Y) + Dy + 0.5 - Y) * (std::floor(Y) + Dy + 0.5 - Y)) / 0.36) / (6.283 * 0.36);
				I.Add(int(X) + Dx, int(Y) + Dy, float(G * C.x), float(G * C.y), float(G * C.z));
			}
		}
		for (size_t P = 0; P < I.Rgb.size(); P += 3) { I.Rgb[P] += 0.012f; I.Rgb[P + 1] += 0.010f; I.Rgb[P + 2] += 0.016f; }   // a faint glow, so the shadow reads
	};
	std::string Txt;
	size_t Captured[4] = { 0, 0, 0, 0 };
	{
		FCamera C = CamAt(15.0, 0.3, W, 1.3); C.Height = Hh;
		FImage In(W, Hh), Out(W, Hh); StarField(In);
		FLensView L; L.CamRs = H::float3(float(15.0 * std::sqrt(0.91)), 0.0f, float(15.0 * 0.3)); L.Disc = NoDisc();
		FLensStats St; ApplyLens(In, C, L, Out, &St); Captured[0] = St.Captured;
		Blit(Out, 0, 0);
	}
	{
		const double Ci = std::cos(83.0 * Pi / 180.0);
		FCamera C = CamAt(60.0, Ci, W, 1.15); C.Height = Hh;
		FImage In(W, Hh), Out(W, Hh); StarField(In);
		FLensView L; L.CamRs = H::float3(float(60.0 * std::sqrt(1.0 - Ci * Ci)), 0.0f, float(60.0 * Ci)); L.Disc = DiscOf(12000.0f);
		L.DiscScale = 1.0;
		L.DiscScale = 1.0 / DiscP95(C, L);   // exposed as a camera would: the disc's 95th-percentile pixel at 1
		FLensStats St; ApplyLens(In, C, L, Out, &St); Captured[1] = St.Captured;
		Blit(Out, W, 0);
	}
	FBhScene Sc;
	FVolumeBuilder VB(Sc.M, Sc.G);
	for (int Pass = 0; Pass < 2; ++Pass)
	{
		const FGalaxyPick& Gp = Pass == 0 ? Sc.Quiet : Sc.Active;
		if (!Gp.bFound) { Txt += Pass == 0 ? "no quiet galaxy; " : "no active galaxy; "; continue; }
		const H::FUniObject Ho = ToHlsl(Gp.O);
		const H::FUniBlackHole Bh = H::Uni_GalaxyBlackHole(Ho, H::UNI_BH_ACTIVE_SHARE);
		const float Mn = H::Uni_NucleusAbsMag(Bh, H::Uni_ObjectAbsMag(Ho), 0.5f, 1.0f);
		float Axis[3]; GalaxyDiscAxis(Ho.B.x, false, Axis);
		const FV3 Dir = ViewDirAt(Axis, Pass == 0 ? 0.6 : 0.17);
		const double Rs = std::pow(10.0, double(H::UNI_BH_LOG_RS_SUN_CM) + double(Bh.Log10Mass));
		FCamera Cg; Cg.Width = W; Cg.Height = Hh; Cg.FovX = 1.2;
		const double ThetaPx = 1.0 / Cg.FocalPx();
		const double D = Pass == 0 ? 40.0 * Rs : 60.0 * Rs;
		FVec3f Fr{};
		const FCoord CamG = CameraNear(Gp, Dir, D, Fr);
		FRefPipeline Pg(Sc.G);
		Pg.bVelocityLod = false;
		Pg.Update(CamG, Fr);
		Cg.Coord = CamG; Cg.LookAlong(Dir); Cg.DiscArchetypeMask = Sc.GalMask; Cg.NucleusArchetypeMask = Sc.GalMask;
		const FVec3d Rel = ToDouble(Sub(ObjectCoord(Gp.K, Gp.O.A), CamG));
		const double RelCm[3] = { Rel.X - Fr.X, Rel.Y - Fr.Y, Rel.Z - Fr.Z };
		const FLensTarget T = LensTargetOf(Bh, Mn, RelCm, Axis, Ho.B.x, ThetaPx, FLensConfig{});
		auto LensOf = [&](const FCamera& Cc)
		{
			FLensView Lv;
			Lv.CamRs = H::float3(float(-RelCm[0] / T.RsCm), float(-RelCm[1] / T.RsCm), float(-RelCm[2] / T.RsCm));
			Lv.Disc.Axis = H::float3(Axis[0], Axis[1], Axis[2]); Lv.Disc.OuterX = T.OuterX; Lv.Disc.PeakK = T.PeakK; Lv.Disc.bDisc = T.bDisc ? 1.0f : 0.0f;
			Lv.DiscScale = T.bDisc ? std::pow(10.0, T.DiscLog10Scale + 0.4 * double(Cc.View().ExposureMag)) * double(T.DiscShare) : 0.0;
			Lv.RegionRad = T.RegionRad;
			Lv.BhDistanceCm = T.DistanceCm;
			PointOf(T, Cc.View(), Lv);
			return Lv;
		};
		if (Pass == 1 && T.bDisc) { Cg.ExposureBias = -2.5 * std::log10(DiscP95(Cg, LensOf(Cg))); }   // exposed for the disc, as a camera would
		Cg.NucleusHandSeed = Ho.B.x; Cg.NucleusHandShare = T.PointAppMag < H::UNI_BH_NONE_MAG ? 1.0f : 0.0f;
		const FVolumeScene Vs = VB.Build(Pg, CamG, Fr, Cg.View());
		FImage In(W, Hh), Out(W, Hh);
		RenderFrameWithVolume(Pg, Vs, Cg, In);
		const FLensView L = LensOf(Cg);
		FLensStats St; ApplyLens(In, Cg, L, Out, &St); Captured[2 + Pass] = St.Captured;
		Blit(Out, 0 + Pass * W, Hh);
		Txt += Uni::Fmt("%s: M 10^%.2f, lambda 10^%.2f, %.0f R_s (%.3g pc), ring %.1f px, disc %.1f px (share %.2f, peak %.0f K), traced %zu, shadow %zu px; ",
			Pass == 0 ? "quiet" : "active", double(Bh.Log10Mass), double(Bh.Log10Eddington), D / Rs, D / 3.0857e18, double(T.RingPx), double(T.DiscPx), double(T.DiscShare), double(T.PeakK), St.Traced, St.Captured);
	}
	const std::string Base = RenderDir() + "/still_bh_lens";
	Sheet.WritePpm(Base + ".ppm");
	if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
	{
		std::fprintf(F, "{\"lines\":[\"D-273: top: lensing alone (15 R_s, star field); a disc 83 deg from its axis (60 R_s)\",\"bottom: the web scene's quiet black hole at its galaxy's centre (40 R_s); its brightest nucleus's disc from 60 R_s, 80 deg from its axis (exposed for the disc)\"]}\n");
		std::fclose(F);
	}
	const bool bOk = Captured[0] > 100 && Captured[1] > 20 && Captured[2] > 100;
	Uni::Check(2, "T2.lens.stills", "D-273 stills: lensing alone over a star field; a nearly edge-on disc; a quiet and an active black hole at the centre of a generated galaxy (its stars and volume lensed, the nucleus point and disc the pass's); every shadow drawn")
		.Pass(bOk, Txt + Uni::Fmt("shadow px: field %zu, disc %zu", Captured[0], Captured[1]));
}

UNI_TEST(lens, disc_knee)
{
	// D-274: Uni_BhDiscSoften. Below the knee bit for bit; above it Knee (1 + log10(L / Knee)):
	// continuous at the knee (value and slope), monotone over 1e-3..1e12 x the knee, the colour's
	// chromaticity kept, 10x -> 2x, 1e8x -> 9x (1e-5); knee 0 bit for bit. Still: the web scene's
	// brightest nucleus's disc from 60 R_s at the scene's own exposure, physical, knee 2 (the
	// default) and knee 0.5.
	const H::float3 Col = H::Uni_TemperatureColour(9000.0f);
	size_t Bad = 0;
	double Prev = -1.0, WorstChroma = 0.0;
	for (int I = 0; I <= 300; ++I)
	{
		const double X = std::pow(10.0, -3.0 + 15.0 * I / 300.0);   // x the knee
		const H::float3 In = Col * float(2.0 * X / double(H::Uni_Luminance(Col)));
		const H::float3 Out = H::Uni_BhDiscSoften(In, 2.0f);
		const double Lo = H::Uni_Luminance(Out);
		if (X <= 1.0 && (Out.x != In.x || Out.y != In.y || Out.z != In.z)) { ++Bad; }
		if (!(Lo >= Prev)) { ++Bad; }
		Prev = Lo;
		const double Si = double(In.x) + In.y + In.z, So = double(Out.x) + Out.y + Out.z;
		WorstChroma = std::max({ WorstChroma, std::fabs(In.x / Si - Out.x / So), std::fabs(In.y / Si - Out.y / So), std::fabs(In.z / Si - Out.z / So) });
	}
	auto At = [&](double X) { return double(H::Uni_Luminance(H::Uni_BhDiscSoften(Col * float(2.0 * X / double(H::Uni_Luminance(Col))), 2.0f)) / 2.0); };
	const double R10 = At(10.0), R1e8 = At(1.0e8);
	const double Slope = (At(1.0001) - At(1.0)) / 0.0001;   // d(out)/d(in) just above the knee, in knee units: 1 / ln 10 ... the log's
	const H::float3 Big = Col * 1.0e9f;
	const H::float3 Phys = H::Uni_BhDiscSoften(Big, 0.0f);
	if (Phys.x != Big.x || Phys.y != Big.y || Phys.z != Big.z) { ++Bad; }
	// The still.
	std::string Txt;
	{
		FBhScene Sc;
		if (Sc.Active.bFound)
		{
			const int W = 320, Hh = 200;
			FImage Sheet(3 * W, Hh);
			const H::FUniObject Ho = ToHlsl(Sc.Active.O);
			const H::FUniBlackHole Bh = H::Uni_GalaxyBlackHole(Ho, H::UNI_BH_ACTIVE_SHARE);
			const float Mn = H::Uni_NucleusAbsMag(Bh, H::Uni_ObjectAbsMag(Ho), 0.5f, 1.0f);
			float Axis[3]; GalaxyDiscAxis(Ho.B.x, false, Axis);
			const FV3 Dir = ViewDirAt(Axis, 0.17);
			const double Rs = std::pow(10.0, double(H::UNI_BH_LOG_RS_SUN_CM) + double(Bh.Log10Mass));
			FVec3f Fr{};
			const FCoord CamG = CameraNear(Sc.Active, Dir, 60.0 * Rs, Fr);
			FRefPipeline Pg(Sc.G); Pg.bVelocityLod = false; Pg.Update(CamG, Fr);
			FVolumeBuilder VB(Sc.M, Sc.G);
			FCamera Cg; Cg.Width = W; Cg.Height = Hh; Cg.FovX = 1.2; Cg.Coord = CamG; Cg.LookAlong(Dir);
			Cg.DiscArchetypeMask = Sc.GalMask; Cg.NucleusArchetypeMask = Sc.GalMask; Cg.NucleusHandSeed = Ho.B.x; Cg.NucleusHandShare = 1.0f;
			const FVec3d Rel = ToDouble(Sub(ObjectCoord(Sc.Active.K, Sc.Active.O.A), CamG));
			const double RelCm[3] = { Rel.X - Fr.X, Rel.Y - Fr.Y, Rel.Z - Fr.Z };
			const FLensTarget T = LensTargetOf(Bh, Mn, RelCm, Axis, Ho.B.x, 1.0 / Cg.FocalPx(), FLensConfig{});
			const FVolumeScene Vs = VB.Build(Pg, CamG, Fr, Cg.View());
			FImage In(W, Hh);
			RenderFrameWithVolume(Pg, Vs, Cg, In);
			const float Knees[3] = { 0.0f, H::UNI_BH_DISC_KNEE, 0.5f };
			for (int P = 0; P < 3; ++P)
			{
				FLensView L;
				L.CamRs = H::float3(float(-RelCm[0] / T.RsCm), float(-RelCm[1] / T.RsCm), float(-RelCm[2] / T.RsCm));
				L.Disc.Axis = H::float3(Axis[0], Axis[1], Axis[2]); L.Disc.OuterX = T.OuterX; L.Disc.PeakK = T.PeakK; L.Disc.bDisc = T.bDisc ? 1.0f : 0.0f;
				L.DiscScale = T.bDisc ? std::pow(10.0, T.DiscLog10Scale + 0.4 * double(Cg.View().ExposureMag)) * double(T.DiscShare) : 0.0;
				L.DiscKnee = Knees[P];
				L.RegionRad = T.RegionRad; L.BhDistanceCm = T.DistanceCm;
				PointOf(T, Cg.View(), L);
				FImage Out(W, Hh); FLensStats St;
				ApplyLens(In, Cg, L, Out, &St);
				double Mx = 0.0;
				for (size_t Q = 0; Q < Out.Rgb.size(); Q += 3) { Mx = std::max(Mx, 0.2126 * Out.Rgb[Q] + 0.7152 * Out.Rgb[Q + 1] + 0.0722 * Out.Rgb[Q + 2]); }
				for (int Y = 0; Y < Hh; ++Y) for (int X = 0; X < W; ++X) { const float* Px = &Out.Rgb[(size_t(Y) * W + X) * 3]; Sheet.Add(P * W + X, Y, Px[0], Px[1], Px[2]); }
				Txt += Uni::Fmt("%sknee %.1f: brightest pixel %.3g", P ? ", " : "", double(Knees[P]), Mx);
			}
			const std::string Base = RenderDir() + "/still_bh_knee";
			Sheet.WritePpm(Base + ".ppm");
			if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
			{
				std::fprintf(F, "{\"lines\":[\"D-274: the web scene's brightest nucleus's disc from 60 R_s at the scene's own exposure\",\"left: physical (scene colour clamped at 6e4); middle: uu.Bh.DiscKnee 2 (the default); right: 0.5\"]}\n");
				std::fclose(F);
			}
		}
	}
	const bool bOk = Bad == 0 && WorstChroma < 1.0e-6 && std::fabs(R10 - 2.0) < 1.0e-5 && std::fabs(R1e8 - 9.0) < 1.0e-4 && std::fabs(Slope * std::log(10.0) - 1.0) < 1.0e-3;
	Uni::Check(1, "T1.lens.disc_knee", "D-274 the disc's light softened above uu.Bh.DiscKnee: bit for bit below it and at knee 0; monotone over 1e-3..1e12 x the knee; chromaticity kept (1e-6); 10x reads 2x, 1e8x 9x; the slope just above the knee 1 / ln 10 of the knee's per knee (continuous value); still render/still_bh_knee")
		.Metric("r10", R10).Metric("r1e8", R1e8).Metric("worst_chroma", WorstChroma)
		.Pass(bOk, Uni::Fmt("%zu problems; 10x -> %.6f x, 1e8x -> %.5f x; chroma %.2g; slope x ln10 %.5f; %s", Bad, R10, R1e8, WorstChroma, Slope * std::log(10.0), Txt.c_str()));
}

UNI_TEST(lens, foreground)
{
	// D-275 (owner 2026-11-17 10:25, engine: the player in front of the hole showed a lensed copy of
	// itself): a uniform grey sky through the lens (camera 30 R_s, looking at the hole) with a green
	// block in front of it (scene distance 100 cm, a figure off to the side of the hole). Every pixel
	// outside the block: no green (G not above R and B by more than 1e-6 of the block's); every
	// pixel of the block untouched.
	const int Px = 320;
	FCamera C = CamAt(30.0, 0.3, Px, 1.2);
	FImage In(Px, Px);
	for (size_t P = 0; P < In.Rgb.size(); P += 3) { In.Rgb[P] = 0.2f; In.Rgb[P + 1] = 0.2f; In.Rgb[P + 2] = 0.2f; }
	// D-279: the figure's colour reaches 2 px past its depth (the engine's antialiased, TSR-upscaled
	// edge over depth at the internal resolution): that fringe must not be copied either.
	size_t Block = 0;
	for (int Y = 148; Y < 232; ++Y) for (int X = 173; X < 217; ++X)
	{
		const size_t I = size_t(Y) * Px + X;
		In.Rgb[I * 3] = 0.0f; In.Rgb[I * 3 + 1] = 5.0f; In.Rgb[I * 3 + 2] = 0.0f;
		if (Y >= 150 && Y < 230 && X >= 175 && X < 215) { In.SceneDist[I] = 100.0; ++Block; }
	}
	auto InFig = [&](int X, int Y) { return Y >= 148 && Y < 232 && X >= 173 && X < 217; };
	FLensView L;
	L.CamRs = H::float3(float(30.0 * std::sqrt(0.91)), 0.0f, float(30.0 * 0.3));
	L.Disc = NoDisc();
	L.BhDistanceCm = 30.0 * 3.0e12;
	FImage Out(Px, Px);
	FLensStats St;
	ApplyLens(In, C, L, Out, &St);
	size_t Ghost = 0, Touched = 0;
	for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
	{
		const size_t I = size_t(Y) * Px + X;
		const float* P = &Out.Rgb[I * 3];
		if (In.SceneDist[I] < 1.0e3) { if (P[0] != In.Rgb[I * 3] || P[1] != In.Rgb[I * 3 + 1] || P[2] != In.Rgb[I * 3 + 2]) { ++Touched; } }
		else if (!InFig(X, Y) && P[1] - 0.5f * (P[0] + P[2]) > 5.0e-6f) { ++Ghost; }   // the fringe itself is the pass's to repaint
	}
	// D-279 (owner 2026-11-17 12:50, engine: "a repeating series of silhouettes of refraction shells"
	// beside the player: D-275's quarter steps landing on the colour fringe): a smooth sky (a gradient
	// in x and y) behind the same figure: outside it, the largest step between neighbouring pixels at
	// most 1.5 x (+ 1e-3) the lensed gradient's without the figure, and the count of steps over 3 x
	// that sky's typical step no more than without it + two outlines (measured: the mirror fill +493,
	// D-275's quarter steps +2246 with 1908 pixels of the figure's colour copied: the shells).
	FImage G0(Px, Px), G1(Px, Px);
	for (int Y = 0; Y < Px; ++Y) for (int X = 0; X < Px; ++X)
	{
		const size_t I = size_t(Y) * Px + X;
		const float V = 0.2f + 0.6f * float(X) / Px + 0.3f * float(Y) / Px;
		for (FImage* Im : { &G0, &G1 }) { Im->Rgb[I * 3] = V; Im->Rgb[I * 3 + 1] = V * 0.8f; Im->Rgb[I * 3 + 2] = V * 0.6f; }
		if (InFig(X, Y)) { G1.Rgb[I * 3] = 0.0f; G1.Rgb[I * 3 + 1] = 5.0f; G1.Rgb[I * 3 + 2] = 0.0f; }
		if (In.SceneDist[I] < 1.0e3) { G1.SceneDist[I] = 100.0; }
	}
	FImage O0(Px, Px), O1(Px, Px);
	ApplyLens(G0, C, L, O0); ApplyLens(G1, C, L, O1);
	auto Steps = [&](const FImage& O, double& MaxStep, std::vector<double>& All)
	{
		MaxStep = 0.0;
		for (int Y = 1; Y < Px - 1; ++Y) for (int X = 1; X < Px - 1; ++X)
		{
			const size_t I = size_t(Y) * Px + X;
			bool bNear = false;
			for (int Dy = -1; Dy <= 1; ++Dy) for (int Dx = -1; Dx <= 1; ++Dx) { bNear |= InFig(X + Dx, Y + Dy); }
			if (bNear || O0.Rgb[I * 3] == 0.0f) { continue; }   // the figure, its edge, the shadow
			const double Sx = std::fabs(double(O.Rgb[I * 3]) - O.Rgb[(I + 1) * 3]), Sy = std::fabs(double(O.Rgb[I * 3]) - O.Rgb[(I + Px) * 3]);
			if (O0.Rgb[(I + 1) * 3] == 0.0f || O0.Rgb[(I + Px) * 3] == 0.0f) { continue; }
			MaxStep = std::max({ MaxStep, Sx, Sy });
			All.push_back(std::max(Sx, Sy));
		}
	};
	double M0 = 0.0, M1 = 0.0;
	std::vector<double> A0, A1;
	Steps(O0, M0, A0); Steps(O1, M1, A1);
	std::vector<double> Sorted = A0; std::sort(Sorted.begin(), Sorted.end());
	const double Typical = Sorted.empty() ? 0.0 : Sorted[Sorted.size() / 2];
	size_t Big0 = 0, Big1 = 0;
	for (double V : A0) { Big0 += V > 3.0 * Typical + 1e-4 ? 1 : 0; }
	for (double V : A1) { Big1 += V > 3.0 * Typical + 1e-4 ? 1 : 0; }
	const size_t Perimeter = 2 * (44 + 84);
	const bool bSmooth = M1 <= 1.5 * M0 + 1.0e-3 && Big1 <= Big0 + 2 * Perimeter;   // at most two outlines' worth (the mirror's fold and its fallback edge), not a train of them (D-275's stepping: ~9)
	Uni::Check(1, "T1.lens.foreground", "D-275/D-279 something in front of the black hole is not lensed: a green figure before the hole (camera 30 R_s): no green anywhere else (its lensed copy), its own pixels untouched; its colour fringe (2 px past its depth) not copied either; over a smooth sky no contours beside it (the largest neighbour step within 1.5x the figure-less image's, steps over 3x typical no more than it + two outlines; D-275's quarter steps: ~9 and 1908 copied pixels)")
		.Metric("ghost_pixels", double(Ghost)).Metric("touched", double(Touched)).Metric("max_step", M1).Metric("max_step_clear", M0).Metric("big_steps", double(Big1)).Metric("big_steps_clear", double(Big0))
		.Pass(Ghost == 0 && Touched == 0 && St.Traced > 10000 && bSmooth, Uni::Fmt("%zu block pixels; %zu pixels elsewhere with its green, %zu of its own changed; %zu traced, %zu shadow; smooth sky: largest step %.4g (without the figure %.4g), steps over 3x typical %zu (%zu)",
			Block, Ghost, Touched, St.Traced, St.Captured, M1, M0, Big1, Big0));
}

namespace
{
	/** A synthetic star field over a faint glow (the stills' background). */
	void StarFieldOf(FImage& I)
	{
		for (int K = 0; K < 1500; ++K)
		{
			const double X = I.W * H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K)), Y = I.H * H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K + 1));
			const double B = 0.05 + 2.0 * std::pow(double(H::Uni_DrawUnit(0x5EEDu, 0x51u, u32(3 * K + 2))), 6.0);
			I.Add(int(X), int(Y), float(B), float(B), float(B));
		}
		for (size_t P = 0; P < I.Rgb.size(); P += 3) { I.Rgb[P] += 0.01f; I.Rgb[P + 1] += 0.01f; I.Rgb[P + 2] += 0.013f; }
	}

	/** The lens pass over the star field with a volumetric (or thin) disc at the exposure-relative scale. */
	FImage DiscView(double Dx, double CosI, float Peak, const FDiscLook& Lk, double TimeS, int W, int Hh, FLensStats* St = nullptr, double Scale = -1.0, float LensScale = 1.0f)
	{
		FCamera C; C.Width = W; C.Height = Hh; C.FovX = 1.2;
		const double S = std::sqrt(std::max(1.0 - CosI * CosI, 0.0));
		C.LookAlong(FV3{ -S, 0.0, -CosI }, FV3{ 0.0, 0.0, 1.0 });
		FImage In(W, Hh); StarFieldOf(In);
		FLensView L;
		L.CamRs = H::float3(float(Dx * S), 0.0f, float(Dx * CosI));
		L.Disc.Axis = H::float3(0.0f, 0.0f, 1.0f); L.Disc.PeakK = Peak; L.Disc.OuterX = std::max(H::Uni_BhDiscOuterX(Peak) / LensScale, H::UNI_BH_ISCO_X + 1.0f); L.Disc.bDisc = 1.0f;   // D-277: Dx in the lens's R_s
		ApplyDiscLook(L.Disc, Lk, TimeS, 12345u);
		L.DiscScale = Scale >= 0.0 ? Scale : DiscArtScale(Peak, 1.0 / C.FocalPx(), 1.0);
		L.DiscKnee = H::UNI_BH_DISC_KNEE;
		FImage Out(W, Hh);
		ApplyLens(In, C, L, Out, St);
		return Out;
	}

	double MeanAbsDiff(const FImage& A, const FImage& B)
	{
		double D = 0.0, M = 0.0;
		for (size_t I = 0; I < A.Rgb.size(); ++I) { D += std::fabs(double(A.Rgb[I]) - double(B.Rgb[I])); M += std::fabs(double(A.Rgb[I])); }
		return D / std::max(M, 1e-30);
	}
}

UNI_TEST(lens, disc_volume)
{
	// D-276 (owner 2026-11-17 10:23: clouds, rotation, bands and colours; a volume to fly through).
	// - Thin limit: the volume with no clouds, bands or heat, physical falloff, thickness 0.01 and
	//   optical depth 50 shows the thin disc's light (summed over the view, 60 R_s, 60 degrees; 5%).
	// - The flow's restart: the image 0.02 s either side of a phase's restart no more different than
	//   either side of mid-cycle x2 (+0.002): no pop.
	// - Rotation: along the ring at 10 R_s in its plane the clouds' opacity at t + dt is the pattern
	//   at t turned counter-clockwise by Omega(10) dt (the best of 720 shifts within 2 of it).
	// - A camera inside the disc (8 R_s, in the plane): every value finite; looking along the plane
	//   the disc hides at least half the sky.
	// - Cost: volume samples a ray (mean, p99, the share past the budget) over four views.
	std::string Txt;
	bool bOk = true;
	{
		FDiscLook Thin; Thin.bVolume = false;
		FDiscLook Lim; Lim.Clouds = 0.0f; Lim.Bands = 0.0f; Lim.Heat = 0.0f; Lim.Falloff = 1.0f; Lim.Thickness = 0.01f; Lim.Opacity = 50.0f; Lim.PeriodS = 0.0f; Lim.Erosion = 0.0f;
		FLensStats A, B;
		DiscView(60.0, 0.5, 20000.0f, Thin, 0.0, 240, 150, &A, 1.0);
		DiscView(60.0, 0.5, 20000.0f, Lim, 0.0, 240, 150, &B, 1.0);
		const double La = 0.2126 * A.DiscFlux[0] + 0.7152 * A.DiscFlux[1] + 0.0722 * A.DiscFlux[2];
		const double Lb = 0.2126 * B.DiscFlux[0] + 0.7152 * B.DiscFlux[1] + 0.0722 * B.DiscFlux[2];
		const double E = std::fabs(Lb / std::max(La, 1e-300) - 1.0);
		bOk = bOk && E < 0.05;
		Txt += Uni::Fmt("thin limit %.4f of the thin disc's light; ", Lb / std::max(La, 1e-300));
	}
	{
		FDiscLook Lk;   // defaults: period 8 s, a cycle of one orbit
		const double Cyc = double(Lk.PeriodS) * double(Lk.CycleOrbits);
		const FImage R0 = DiscView(60.0, 0.4, 20000.0f, Lk, 3.0 * Cyc - 0.01, 160, 100), R1 = DiscView(60.0, 0.4, 20000.0f, Lk, 3.0 * Cyc + 0.01, 160, 100);
		const FImage M0 = DiscView(60.0, 0.4, 20000.0f, Lk, 3.25 * Cyc - 0.01, 160, 100), M1 = DiscView(60.0, 0.4, 20000.0f, Lk, 3.25 * Cyc + 0.01, 160, 100);
		const double Dr = MeanAbsDiff(R0, R1), Dm = MeanAbsDiff(M0, M1);
		bOk = bOk && Dr <= 2.0 * Dm + 0.002;
		Txt += Uni::Fmt("0.02 s across a restart %.5f, mid-cycle %.5f; ", Dr, Dm);
	}
	{
		FDiscLook Lk; Lk.CycleOrbits = 100.0f;   // one long phase: pure advection
		H::FUniLensDisc D{}; D.Axis = H::float3(0.0f, 0.0f, 1.0f); D.PeakK = 20000.0f; D.OuterX = H::Uni_BhDiscOuterX(20000.0f); D.bDisc = 1.0f;
		const double T0 = 37.0, Dt = 0.25, Rho = 10.0;
		const int N = 720;
		std::vector<double> A0(N), A1(N);
		for (int Pass = 0; Pass < 2; ++Pass)
		{
			ApplyDiscLook(D, Lk, T0 + Pass * Dt, 777u);
			for (int I = 0; I < N; ++I)
			{
				const double Ph = 2.0 * Pi * I / N;
				float Al; H::float3 Src;
				H::Uni_BhDiscSample(H::float3(float(Rho * std::cos(Ph)), float(Rho * std::sin(Ph)), 0.0f), D, 0.0f, Al, Src);
				(Pass ? A1 : A0)[size_t(I)] = Al;
			}
		}
		int Best = 0; double BestC = -1e300;
		for (int K = -N / 2; K < N / 2; ++K)
		{
			double C = 0.0;
			for (int I = 0; I < N; ++I) { C += A1[size_t((I + K + N) % N)] * A0[size_t(I)]; }
			if (C > BestC) { BestC = C; Best = K; }
		}
		const double Want = 2.0 * Pi / double(Lk.PeriodS) * std::pow(Rho / 3.0, -1.5) * Dt / (2.0 * Pi) * N;   // shift in samples
		bOk = bOk && std::fabs(double(Best) - Want) <= 2.0;
		Txt += Uni::Fmt("clouds at 10 R_s turned %d / 720 in %.2f s (Keplerian %.1f); ", Best, Dt, Want);
	}
	{
		FDiscLook Lk;
		H::FUniLensDisc D{}; D.Axis = H::float3(0.0f, 0.0f, 1.0f); D.PeakK = 20000.0f; D.OuterX = H::Uni_BhDiscOuterX(20000.0f); D.bDisc = 1.0f;
		ApplyDiscLook(D, Lk, 5.0, 3u);
		size_t Bad = 0, Hidden = 0, Rays = 0;
		for (int I = 0; I < 360; ++I)
		{
			const double A = 2.0 * Pi * I / 360.0;
			for (double El : { -0.02, 0.0, 0.02 })
			{
				const H::float3 Dir(float(std::cos(A) * std::cos(El)), float(std::sin(A) * std::cos(El)), float(std::sin(El)));
				const H::FUniLensResult R = H::Uni_LensTrace(H::float3(8.0f, 0.0f, 0.0f), Dir, D, H::UNI_LENS_MAX_STEPS);
				++Rays;
				if (!std::isfinite(R.Disc.x) || !std::isfinite(R.Disc.y) || !std::isfinite(R.Disc.z) || !std::isfinite(R.Transmit)) { ++Bad; }
				if (R.Transmit < 0.5f || R.Captured > 0.5f) { ++Hidden; }
			}
		}
		bOk = bOk && Bad == 0 && Hidden * 2 >= Rays;
		Txt += Uni::Fmt("inside (8 R_s, in the plane): %zu of %zu rays not finite, %zu hidden; ", Bad, Rays, Hidden);
	}
	{
		FDiscLook Lk;
		std::vector<float> Used;
		size_t Past = 0;
		const double Views[4][3] = { { 60.0, 0.17, 20000.0 }, { 60.0, 0.5, 20000.0 }, { 20.0, 0.12, 20000.0 }, { 200.0, 0.1, 1.0e5 } };
		for (const auto& V : Views)
		{
			FCamera C; C.Width = 160; C.Height = 100; C.FovX = 1.2;
			const double S = std::sqrt(1.0 - V[1] * V[1]);
			C.LookAlong(FV3{ -S, 0.0, -V[1] }, FV3{ 0.0, 0.0, 1.0 });
			H::FUniLensDisc D{}; D.Axis = H::float3(0.0f, 0.0f, 1.0f); D.PeakK = float(V[2]); D.OuterX = H::Uni_BhDiscOuterX(float(V[2])); D.bDisc = 1.0f;
			ApplyDiscLook(D, Lk, 2.0, 9u);
			for (int Y = 0; Y < C.Height; ++Y) for (int X = 0; X < C.Width; ++X)
			{
				double Cf; const H::float3 Dr = PixelRay(C, X + 0.5, Y + 0.5, Cf);
				const H::FUniLensResult R = H::Uni_LensTrace(H::float3(float(V[0] * S), 0.0f, float(V[0] * V[1])), Dr, D, H::UNI_LENS_MAX_STEPS);
				if (R.Pad0 > 0.0f) { Used.push_back(R.Pad0); if (R.Pad0 > float(H::UNI_LENS_VOL_SAMPLES)) { ++Past; } }
			}
		}
		std::sort(Used.begin(), Used.end());
		double Mean = 0.0; for (float U : Used) { Mean += U; }
		Mean /= std::max<size_t>(Used.size(), 1);
		const double P99 = Used.empty() ? 0.0 : Used[Used.size() * 99 / 100];
		const double PastShare = double(Past) / std::max<size_t>(Used.size(), 1);
		bOk = bOk && Mean < 80.0 && PastShare < 0.02;
		Txt += Uni::Fmt("samples a ray through it: mean %.1f, p99 %.0f, past the budget %.2f%%; ", Mean, P99, 100.0 * PastShare);
	}
	{
		// Still: the four views of the volumetric disc at the defaults.
		const int W = 480, Hh = 300;
		FImage Sheet(2 * W, 2 * Hh);
		const double Views[4][3] = { { 60.0, 0.17, 20000.0 }, { 60.0, 0.5, 20000.0 }, { 40.0, 0.05, 12000.0 }, { 20.0, 0.12, 20000.0 } };
		for (int Vi = 0; Vi < 4; ++Vi)
		{
			const FImage O = DiscView(Views[Vi][0], Views[Vi][1], float(Views[Vi][2]), FDiscLook{}, 2.0, W, Hh, nullptr, -1.0, 4.0f);   // the engine's defaults (D-278: lens scale 4)
			for (int Y = 0; Y < Hh; ++Y) for (int X = 0; X < W; ++X) { const float* P = &O.Rgb[(size_t(Y) * W + X) * 3]; Sheet.Add((Vi % 2) * W + X, (Vi / 2) * Hh + Y, P[0], P[1], P[2]); }
		}
		const std::string Base = RenderDir() + "/still_bh_disc";
		Sheet.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-276/D-277: the volumetric disc at the engine's defaults (brightness 1 x the exposure, thickness 0.025, clouds 0.85, bands 0.3, heat 0.35, spiral 12, erosion 0.8, lens scale 4)\",\"60 / 60 / 40 / 20 lens R_s; 80 / 60 / 87 / 83 deg\"]}\n");
			std::fclose(F);
		}
	}
	Uni::Check(1, "T1.lens.disc_volume", "D-276 the volumetric disc: its thin opaque limit within 5% of the thin disc's light; no jump at a flow phase's restart (x2 mid-cycle's change + 0.002); the clouds turn counter-clockwise at the Keplerian rate (within 2 / 720); a camera inside it finite and mostly hidden along the plane; under 80 samples a ray on average, under 2% past the budget; still render/still_bh_disc")
		.Pass(bOk, Txt);
}

UNI_TEST(lens, lens_scale)
{
	// D-277: LensTargetOf with LensScale 2 against 1, a black hole of 1e7 M_sun seen from 2e4 R_s:
	// the lens's R_s 2x; the disc's outer radius in its units half (the same physical size: the same
	// pixels); the Einstein ring sqrt(2)x; the nucleus point's size the record's; the shadow traced
	// from that distance 2x the angle (1%).
	H::FUniBlackHole Bh = H::Uni_BlackHoleOf(7.0f, -1.0f);
	const double Rs = std::pow(10.0, double(H::UNI_BH_LOG_RS_SUN_CM) + 7.0);
	const double Rel[3] = { 2.0e4 * Rs, 0.0, 0.0 };
	const float Axis[3] = { 0.0f, 0.0f, 1.0f };
	FLensConfig L1, L2; L2.LensScale = 2.0f;
	const FLensTarget A = LensTargetOf(Bh, -18.0f, Rel, Axis, 1u, 1.0e-4, L1), B = LensTargetOf(Bh, -18.0f, Rel, Axis, 1u, 1.0e-4, L2);
	auto ShadowAngle = [](double Dx)
	{
		double Lo = 0.0, Hi = std::asin(std::min(6.0 / Dx, 0.99));
		for (int I = 0; I < 40; ++I)
		{
			const double M = 0.5 * (Lo + Hi);
			const H::FUniLensResult R = H::Uni_LensTrace(H::float3(float(Dx), 0.0f, 0.0f), H::float3(float(-std::cos(M)), float(std::sin(M)), 0.0f), NoDisc(), H::UNI_LENS_MAX_STEPS);
			if (R.Captured > 0.5f) { Lo = M; } else { Hi = M; }
		}
		return 0.5 * (Lo + Hi);
	};
	const double ShA = ShadowAngle(A.DistanceCm / A.RsCm), ShB = ShadowAngle(B.DistanceCm / B.RsCm);
	const bool bOk = std::fabs(B.RsCm / A.RsCm - 2.0) < 1e-9 && std::fabs(double(B.OuterX) / double(A.OuterX) - 0.5) < 1e-5 && std::fabs(double(B.DiscPx) / double(A.DiscPx) - 1.0) < 1e-4
		&& std::fabs(double(B.RingPx) / double(A.RingPx) - std::sqrt(2.0)) < 1e-4 && B.PointPhysAngRad == A.PointPhysAngRad && std::fabs(ShB / ShA - 2.0) < 0.01;
	Uni::Check(1, "T1.lens.lens_scale", "D-277 uu.Bh.LensScale 2: the lens's R_s 2x, the disc's outer radius in its units half (the same pixels), the ring sqrt 2 x, the nucleus point's size unchanged, the traced shadow 2x the angle (1%)")
		.Metric("shadow_ratio", ShB / ShA)
		.Pass(bOk, Uni::Fmt("R_s x%.6f, outer %.1f -> %.1f, disc px %.4g -> %.4g, ring px %.4g -> %.4g, shadow %.4g -> %.4g rad (x%.4f)", B.RsCm / A.RsCm, double(A.OuterX), double(B.OuterX), double(A.DiscPx), double(B.DiscPx), double(A.RingPx), double(B.RingPx), ShA, ShB, ShB / ShA));
}

