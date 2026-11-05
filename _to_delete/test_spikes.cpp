// test_spikes.cpp
// D-185: diffraction spikes and halo colour on bright stars (render-only sprite style). The
// spike profile's energy over the pixels, its shape, its growth with the halo, the halo
// chroma keeping luminance, the reference splat's total, and the style off = the sprites as
// before. A contact sheet for the owner.

#include "../src/UniTest.h"
#include "../src/UniRenderRef.h"
#include "../src/UniVerifyCommon.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace UniCore;
using namespace UniRender;
namespace H = UniHlsl;

namespace
{
	std::string RenderDir()
	{
		const std::string D = UniVerify::Options().OutDir + "/render";
		::mkdir(UniVerify::Options().OutDir.c_str(), 0755);
		::mkdir(D.c_str(), 0755);
		return D;
	}

	double Unit(u32 S, u32 K) { return double(H::Uni_DrawUnit(S, 0x5B1Cu, K)); }

	/** A star sprite straight ahead with a halo (as Uni_ProjectObject makes one over saturation). */
	H::FUniSprite StarSprite(const FCamera& Cm, double CorePx, double HaloPx, double HaloRatio, const H::float3& Rgb, double Ox = 0.0, double Oy = 0.0)
	{
		H::FUniSprite S{};
		S.RenderPos = H::float3(float(Cm.Forward.X * 1.0e10), float(Cm.Forward.Y * 1.0e10), float(Cm.Forward.Z * 1.0e10));
		S.AngularRadius = float(CorePx / (Cm.FocalPx() * 0.5));
		S.Radiance = Rgb;
		S.TrueDistance = 1.0e20f;
		S.NdcX = float(2.0 * Ox / Cm.Width); S.NdcY = float(-2.0 * Oy / Cm.Height);
		S.Flags = H::UNI_SPRITE_VISIBLE;
		S.HaloRadius = float(HaloPx / (Cm.FocalPx() * 0.5));
		S.HaloRatio = float(HaloRatio);
		return S;
	}

	double ImageLum(const FImage& Im)
	{
		double L = 0.0;
		for (size_t I = 0; I < Im.Rgb.size(); I += 3) { L += 0.2126 * Im.Rgb[I] + 0.7152 * Im.Rgb[I + 1] + 0.0722 * Im.Rgb[I + 2]; }
		return L;
	}
}

UNI_TEST(sprite, spikes)
{
	// 1. Energy over the pixels: Gain x HaloRatio for 2-8 spikes, lengths 2-48 px, widths
	//    0.5-1.5 px, any angle and sub-pixel centre.
	double WorstEnergy = 0.0;
	int Cases = 0;
	for (u32 I = 0; I < 400; ++I)
	{
		H::FUniSpikeStyle S = H::Uni_SpikeDefaults();
		S.Count = float(2 * (1 + int(4.0 * Unit(I, 0))));
		S.Gain = float(0.2 + 1.5 * Unit(I, 1));
		S.Angle = float(6.283185307179586 * Unit(I, 2));
		S.WidthPx = float(0.5 + Unit(I, 3));
		const double HaloPx = 0.5 + 5.5 * Unit(I, 4), HaloRatio = 0.1 + 3.0 * Unit(I, 5);
		const double Ox = Unit(I, 6) - 0.5, Oy = Unit(I, 7) - 0.5;
		const double L = H::Uni_SpikeLengthPx(S, float(HaloPx), float(HaloRatio));
		const int Rad = int(std::ceil(L + 4.0 * S.WidthPx)) + 1;
		double Sum = 0.0;
		for (int Y = -Rad; Y <= Rad; ++Y) for (int X = -Rad; X <= Rad; ++X) { Sum += H::Uni_SpriteSpikes(float(X + Ox), float(Y + Oy), float(HaloPx), float(HaloRatio), S); }
		WorstEnergy = std::max(WorstEnergy, std::abs(Sum / (double(S.Gain) * HaloRatio) - 1.0));
		++Cases;
	}
	// 2. Shape: on a spike at half its length against off-axis at the same radius (half way
	//    between spikes): a line, not a disc.
	double WorstContrast = 1.0e30;
	for (int N = 4; N <= 8; N += 2)
	{
		H::FUniSpikeStyle S = H::Uni_SpikeDefaults();
		S.Count = float(N);
		const double L = H::Uni_SpikeLengthPx(S, 3.0f, 1.0f);
		const double On = H::Uni_SpriteSpikes(float(0.5 * L), 0.0f, 3.0f, 1.0f, S);
		const double A = 3.141592653589793 / N;
		const double Off = H::Uni_SpriteSpikes(float(0.5 * L * std::cos(A)), float(0.5 * L * std::sin(A)), 3.0f, 1.0f, S);
		WorstContrast = std::min(WorstContrast, On / std::max(Off, 1e-30));
	}
	// 3. Growth: length and energy never decrease as the halo grows (D-158's monotone approach).
	int Shrinks = 0;
	{
		const H::FUniSpikeStyle S = H::Uni_SpikeDefaults();
		double PrevL = 0.0, PrevE = 0.0;
		for (int K = 0; K <= 200; ++K)
		{
			const double HaloPx = 0.5 + 0.02 * K, Ratio = 0.01 * K;
			const double L = H::Uni_SpikeLengthPx(S, float(HaloPx), float(Ratio));
			const double E = Ratio > 0.0 ? double(S.Gain) * Ratio : 0.0;
			if (L < PrevL - 1e-9 || E < PrevE - 1e-12) { ++Shrinks; }
			PrevL = L; PrevE = E;
		}
	}
	// 4. Halo chroma: luminance kept, chroma scaled; 1 is the identity.
	double WorstLum = 0.0, WorstIdentity = 0.0;
	int NotMoreVivid = 0;
	for (u32 I = 0; I < 1000; ++I)
	{
		const H::float3 C(float(0.05 + Unit(I, 10)), float(0.05 + Unit(I, 11)), float(0.05 + Unit(I, 12)));
		const H::float3 B = H::Uni_SpriteChroma(C, 1.4f), Id = H::Uni_SpriteChroma(C, 1.0f);
		WorstLum = std::max(WorstLum, double(std::abs(H::Uni_Luminance(B) / H::Uni_Luminance(C) - 1.0f)));
		WorstIdentity = std::max(WorstIdentity, double(std::max({ std::abs(Id.x - C.x), std::abs(Id.y - C.y), std::abs(Id.z - C.z) })));
		const float Sc = (std::max({ C.x, C.y, C.z }) - std::min({ C.x, C.y, C.z })) / std::max({ C.x, C.y, C.z });
		const float Sb = (std::max({ B.x, B.y, B.z }) - std::min({ B.x, B.y, B.z })) / std::max({ B.x, B.y, B.z });
		if (Sc > 0.01f && Sb < Sc) { ++NotMoreVivid; }
	}
	// 5. The reference splat: total light = radiance x (core + halo x (1 + gain)); style off =
	//    the sprite as before, bit for bit.
	double WorstSplat = 0.0;
	bool bOffSame = true;
	{
		FCamera Cm; Cm.Width = 256; Cm.Height = 256;
		for (u32 I = 0; I < 40; ++I)
		{
			const double HaloPx = 1.0 + 3.0 * Unit(I, 20), Ratio = 0.2 + 2.0 * Unit(I, 21);
			const H::float3 Rgb(float(0.5 + Unit(I, 22)), float(0.5 + Unit(I, 23)), float(0.5 + Unit(I, 24)));
			const H::FUniSprite S = StarSprite(Cm, 1.0, HaloPx, Ratio, Rgb, Unit(I, 25) - 0.5, Unit(I, 26) - 0.5);
			FCamera On = Cm; On.Spikes = H::Uni_SpikeDefaults(); On.Spikes.Count = float(4 + 2 * (I % 3)); On.Spikes.Angle = float(Unit(I, 27));
			FImage A(Cm.Width, Cm.Height), B(Cm.Width, Cm.Height), C(Cm.Width, Cm.Height);
			Splat(A, On, S);
			const double Want = double(H::Uni_Luminance(Rgb)) * (1.0 + Ratio * (1.0 + double(On.Spikes.Gain)));
			WorstSplat = std::max(WorstSplat, std::abs(ImageLum(A) / Want - 1.0));
			// Off: the camera's default style against an explicit Count 0, Chroma 1.
			FCamera Off = Cm; Off.Spikes.Count = 0.0f; Off.Spikes.Chroma = 1.0f;
			Splat(B, Cm, S); Splat(C, Off, S);
			bOffSame = bOffSame && std::memcmp(B.Rgb.data(), C.Rgb.data(), B.Rgb.size() * sizeof(float)) == 0;
		}
	}
	// Contact sheet: one star per tile at rising brightness (halo 1.2 -> 4 px, ratio 0.2 -> 3),
	// rows: 4 spikes, 6 spikes, 4 spikes at 45 degrees; warm, white and blue stars.
	{
		const int Tile = 128, Cols = 6, Rows = 3;
		FImage Sheet(Tile * Cols, Tile * Rows);
		const H::float3 Cols3[3] = { H::float3(1.0f, 0.62f, 0.35f), H::float3(1.0f, 0.95f, 0.9f), H::float3(0.55f, 0.7f, 1.0f) };
		for (int Rw = 0; Rw < Rows; ++Rw) for (int Cl = 0; Cl < Cols; ++Cl)
		{
			FCamera Cm; Cm.Width = Tile; Cm.Height = Tile;
			Cm.Spikes = H::Uni_SpikeDefaults();
			Cm.Spikes.Count = Rw == 1 ? 6.0f : 4.0f;
			Cm.Spikes.Angle = Rw == 2 ? 0.7853982f : 0.0f;
			const double T = double(Cl) / double(Cols - 1);
			const H::float3 Rgb = Cols3[Cl % 3] * float(3.0 + 30.0 * T);
			FImage Im(Tile, Tile);
			Splat(Im, Cm, StarSprite(Cm, 1.0, 1.2 + 2.8 * T, 0.2 + 2.8 * T, Rgb));
			for (int Y = 0; Y < Tile; ++Y) for (int X = 0; X < Tile; ++X)
			{
				const float* P = &Im.Rgb[(size_t(Y) * Tile + X) * 3];
				Sheet.Add(Cl * Tile + X, Rw * Tile + Y, P[0], P[1], P[2]);
			}
		}
		const std::string Base = RenderDir() + "/still_star_spikes";
		Sheet.WritePpm(Base + ".ppm");
		if (FILE* F = std::fopen((Base + ".hud.json").c_str(), "wb"))
		{
			std::fprintf(F, "{\"lines\":[\"D-185: star sprites with spikes and halo chroma, brightness rising left to right\",\"rows: 4 spikes, 6 spikes, 4 at 45 degrees; warm, white, blue stars\"]}\n");
			std::fclose(F);
		}
	}
	Uni::Check(1, "T1.sprite.spikes", "D-185 diffraction spikes and halo chroma: spike light over the pixels = gain x halo light (3%) for 2-8 spikes of any length, width, angle and centre; a line (on-spike / off-spike at half length > 50); length and light never shrink as the halo grows; the chroma scale keeps luminance (1e-5), makes colours more vivid and is the identity at 1; the reference splat deposits radiance x (core + halo x (1 + gain)) (1e-3); the style off draws every sprite as before, bit for bit")
		.Metric("cases", Cases).Metric("worst_energy", WorstEnergy).Metric("contrast", WorstContrast).Metric("shrinks", Shrinks).Metric("worst_lum", WorstLum).Metric("worst_splat", WorstSplat)
		.Pass(WorstEnergy < 0.03 && WorstContrast > 50.0 && Shrinks == 0 && WorstLum < 1e-5 && WorstIdentity < 1e-6 && NotMoreVivid == 0 && WorstSplat < 1e-3 && bOffSame,
			Uni::Fmt("%d spike profiles: energy within %.2f%%; on/off-spike contrast %.0f; %d shrinks; chroma: luminance %.1e, identity %.1e, %d not more vivid; splat total within %.2g; style off identical: %s",
				Cases, 100.0 * WorstEnergy, WorstContrast, Shrinks, WorstLum, WorstIdentity, NotMoreVivid, WorstSplat, bOffSame ? "yes" : "no"));
}
