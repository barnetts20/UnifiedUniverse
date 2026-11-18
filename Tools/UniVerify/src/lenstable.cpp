// lenstable.cpp
// `univerify lenstable [--quick]`: D-273's table of the accretion disc's light by direction, for
// UniCompact.ush (UNI_BH_DISC_BEAM, UNI_BH_DISC_LOGNORM). For each peak temperature of the grid
// (ln-even, UNI_BH_BEAM_T0 .. x UNI_BH_BEAM_NT) the disc's light seen from far (UniLensRef.h's
// DiscFarLight: the lens core's rays, lensing, shifts and beaming and all) at cos i = 0, 1/8 .. 1:
//   BEAM    that light over its mean over cos i (Simpson): the nucleus sprite's factor by direction,
//           so a far nucleus is as bright as the disc it becomes (face-on brighter, edge-on dimmer,
//           a small disc lifted by its own lensing);
//   LOGNORM log10 of the mean: the radiance scale that gives the disc the nucleus's light is
//           10^(-0.4 (M_n - m_exp)) (10 pc / R_s)^2 / 10^LOGNORM.
// Prints the HLSL constants (paste over the table in UniCompact.ush); T1.lens.disc_light checks the
// table against fresh traces between its nodes.

#include "UniLensRef.h"
#include "UniCompact.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int UniLensTableCommand(int Argc, char** Argv)
{
	namespace H = UniHlsl;
	bool bQuick = false;
	for (int I = 2; I < Argc; ++I) { if (!std::strcmp(Argv[I], "--quick")) { bQuick = true; } }
	const int Nt = int(H::UNI_BH_BEAM_NT), Nm = int(H::UNI_BH_BEAM_NM);
	const int Nr = bQuick ? 80 : 220, Nps = bQuick ? 48 : 112;
	std::vector<double> Beam(static_cast<size_t>(Nt * Nm)), LogNorm(static_cast<size_t>(Nt));
	for (int T = 0; T < Nt; ++T)
	{
		const float Peak = float(std::exp(double(H::UNI_BH_BEAM_LNT0) + double(H::UNI_BH_BEAM_DLNT) * T));
		std::vector<double> F(static_cast<size_t>(Nm));
		for (int M = 0; M < Nm; ++M)
		{
			double Rgb[3];
			UniRender::DiscFarLight(Peak, double(M) / double(Nm - 1), 1.0e5, Nr, Nps, Rgb);
			F[size_t(M)] = UniRender::LumOf(Rgb);
		}
		double Mean = 0.0;   // Simpson over cos i in [0, 1] (Nm - 1 even)
		const double Hs = 1.0 / double(Nm - 1);
		for (int M = 0; M < Nm; ++M) { Mean += F[size_t(M)] * ((M == 0 || M == Nm - 1) ? 1.0 : (M % 2 ? 4.0 : 2.0)); }
		Mean *= Hs / 3.0;
		for (int M = 0; M < Nm; ++M) { Beam[size_t(T) * Nm + M] = F[size_t(M)] / Mean; }
		LogNorm[size_t(T)] = std::log10(Mean);
		std::fprintf(stderr, "peak %9.0f K: outer %6.1f R_s, mean %.5g (unlensed %.5g)\n", double(Peak), double(H::Uni_BhDiscOuterX(Peak)), Mean,
			UniCore::DiscNormalization(Peak, H::Uni_BhDiscOuterX(Peak)));
	}
	std::printf("static const float UNI_BH_DISC_BEAM[%d] = {\n", Nt * Nm);
	for (int T = 0; T < Nt; ++T)
	{
		std::printf("\t");
		for (int M = 0; M < Nm; ++M) { std::printf("%.5ff%s", Beam[size_t(T) * Nm + M], (T == Nt - 1 && M == Nm - 1) ? "" : ", "); }
		std::printf("\n");
	}
	std::printf("};\nstatic const float UNI_BH_DISC_LOGNORM[%d] = {\n\t", Nt);
	for (int T = 0; T < Nt; ++T) { std::printf("%.5ff%s", LogNorm[size_t(T)], T == Nt - 1 ? "" : ", "); }
	std::printf("\n};\n");
	return 0;
}
