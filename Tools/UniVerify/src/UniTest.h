#pragma once

// UniTest.h
// Dependency-free test registry and check reporting for univerify (D-016).
//
// A TEST is a function registered with UNI_TEST. A CHECK is one row of the verification
// report: a named criterion from VERIFICATION_TOOLING.md with a pass/fail verdict and the
// measured numbers. Tests emit checks through Uni::Report; verify_all reads the JSON lines
// and writes Verification/REPORT.md. Nothing in the report is typed by hand.

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Uni
{
	struct FCheck
	{
		int Tier = 1;
		std::string Id;          // stable, e.g. "T1.coord.fuzz"
		std::string Title;       // the criterion, as the tooling spec words it
		bool bPass = false;
		std::string Measured;    // human readable numbers
		std::vector<std::pair<std::string, double>> Metrics;
	};

	class Report
	{
	public:
		static Report& Get() { static Report R; return R; }

		void Add(const FCheck& C)
		{
			Checks.push_back(C);
			std::printf("  [%s] %-28s %s\n", C.bPass ? "PASS" : "FAIL", C.Id.c_str(), C.Measured.c_str());
			std::fflush(stdout);
		}

		bool WriteJsonl(const std::string& Path) const
		{
			FILE* F = std::fopen(Path.c_str(), "ab");
			if (!F) { return false; }
			for (const FCheck& C : Checks)
			{
				std::fprintf(F, "{\"tier\":%d,\"id\":\"%s\",\"title\":\"%s\",\"pass\":%s,\"measured\":\"%s\",\"metrics\":{",
					C.Tier, Esc(C.Id).c_str(), Esc(C.Title).c_str(), C.bPass ? "true" : "false", Esc(C.Measured).c_str());
				for (size_t I = 0; I < C.Metrics.size(); ++I)
				{
					std::fprintf(F, "%s\"%s\":%.17g", I ? "," : "", Esc(C.Metrics[I].first).c_str(), C.Metrics[I].second);
				}
				std::fprintf(F, "}}\n");
			}
			std::fclose(F);
			return true;
		}

		int NumFailed() const
		{
			int N = 0;
			for (const FCheck& C : Checks) { N += C.bPass ? 0 : 1; }
			return N;
		}

		size_t Num() const { return Checks.size(); }

	private:
		static std::string Esc(const std::string& S)
		{
			std::string O;
			for (char C : S)
			{
				if (C == '"' || C == '\\') { O += '\\'; O += C; }
				else if (C == '\n') { O += "\\n"; }
				else { O += C; }
			}
			return O;
		}
		std::vector<FCheck> Checks;
	};

	/** Builder: Check(1, "T1.x", "title").Metric("n", 5).Pass(ok, "measured text"). */
	class CheckBuilder
	{
	public:
		CheckBuilder(int Tier, const std::string& Id, const std::string& Title) { C.Tier = Tier; C.Id = Id; C.Title = Title; }
		CheckBuilder& Metric(const std::string& K, double V) { C.Metrics.emplace_back(K, V); return *this; }
		void Pass(bool bOk, const std::string& Measured) { C.bPass = bOk; C.Measured = Measured; Report::Get().Add(C); }
	private:
		FCheck C;
	};

	inline CheckBuilder Check(int Tier, const std::string& Id, const std::string& Title) { return CheckBuilder(Tier, Id, Title); }

	// --------------------------------------------------------------------------------------
	// Test registry
	// --------------------------------------------------------------------------------------

	struct FTestEntry { std::string Group; std::string Name; std::function<void()> Fn; };

	inline std::vector<FTestEntry>& Registry() { static std::vector<FTestEntry> R; return R; }

	struct FRegistrar
	{
		FRegistrar(const char* Group, const char* Name, std::function<void()> Fn) { Registry().push_back({ Group, Name, std::move(Fn) }); }
	};

	/** Hard assertion counter for plain unit expectations inside a test. A test whose
	 *  expectations fail emits a failing "unit" check for its group. */
	struct FExpect
	{
		static FExpect& Get() { static FExpect E; return E; }
		int Total = 0;
		int Failed = 0;
		std::vector<std::string> FirstFailures;
		void Record(bool bOk, const char* Expr, const char* File, int Line)
		{
			++Total;
			if (!bOk)
			{
				++Failed;
				if (FirstFailures.size() < 20)
				{
					std::ostringstream S; S << File << ":" << Line << ": " << Expr;
					FirstFailures.push_back(S.str());
					std::printf("    EXPECT FAILED %s\n", S.str().c_str());
				}
			}
		}
	};

	/** splitmix64: the harness's own deterministic generator for test inputs. */
	struct FTestRng
	{
		unsigned long long S;
		explicit FTestRng(unsigned long long Seed) : S(Seed) {}
		unsigned long long Next()
		{
			unsigned long long Z = (S += 0x9E3779B97F4A7C15ull);
			Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
			Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
			return Z ^ (Z >> 31);
		}
		unsigned U32() { return unsigned(Next() >> 32); }
		double Unit() { return double(Next() >> 11) * (1.0 / 9007199254740992.0); }
		/** Limb biased toward carry/borrow edge cases. */
		unsigned EdgeLimb()
		{
			static const unsigned E[] = { 0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu };
			const unsigned R = U32();
			return (R & 3u) == 0u ? E[(R >> 2) & 7u] : U32();
		}
	};

	class Timer
	{
	public:
		Timer() : T0(std::chrono::steady_clock::now()) {}
		double Seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count(); }
	private:
		std::chrono::steady_clock::time_point T0;
	};

	inline std::string Fmt(const char* F, ...) __attribute__((format(printf, 1, 2)));
	inline std::string Fmt(const char* F, ...)
	{
		char Buf[1024];
		va_list A; va_start(A, F); std::vsnprintf(Buf, sizeof(Buf), F, A); va_end(A);
		return Buf;
	}
}

#define UNI_TEST_CAT2(A, B) A##B
#define UNI_TEST_CAT(A, B) UNI_TEST_CAT2(A, B)
#define UNI_TEST(Group, Name) \
	static void UNI_TEST_CAT(UniTest_, Name)(); \
	static Uni::FRegistrar UNI_TEST_CAT(UniTestReg_, Name)(#Group, #Name, &UNI_TEST_CAT(UniTest_, Name)); \
	static void UNI_TEST_CAT(UniTest_, Name)()

#define UNI_EXPECT(Expr) Uni::FExpect::Get().Record(static_cast<bool>(Expr), #Expr, __FILE__, __LINE__)
