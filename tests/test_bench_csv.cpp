// The C++ benchmark driver must append exactly `reps` well-formed CSV lines per invocation.
#include <catch2/catch_test_macros.hpp>
#include <fastzonal/fastzonal.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::vector<std::string> lines_of(const fs::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

// cmd.exe strips the outer quotes of a command line that starts with one; wrapping the whole
// line in another pair keeps the quoted executable and argument paths intact.
int run(const std::string& cmd) {
#ifdef _WIN32
    return std::system(("\"" + cmd + "\"").c_str());
#else
    return std::system(cmd.c_str());
#endif
}

int fields(const std::string& line) {
    int n = 1;
    for (char c : line) n += (c == ',');
    return n;
}

}  // namespace

TEST_CASE("fz_bench appends reps lines with the shared columns") {
    const fs::path fixture = fs::path(FZ_FIXTURE_DIR) / "synthetic_small";
    const fs::path out = fs::temp_directory_path() / "fz_bench_test_results.csv";
    fs::remove(out);
    const bool omp = fz_has_openmp() != 0;
    const std::string backend = omp ? "openmp" : "serial";
    const std::string cmd = std::string("\"") + FZ_BENCH_EXE + "\" \"" + fixture.string() + "\" mean rect synth --backend " +
                            backend + " --threads 2 --reps 3 --out \"" + out.string() + "\"";
    REQUIRE(run(cmd) == 0);
    auto l = lines_of(out);
    REQUIRE(l.size() == 4);  // header + 3 reps
    REQUIRE(l[0] == "fixture,op,rung,threads,rep,seconds,checksum,zones,raster,den,n_zones,n_cells,threads_eff,band_rows,load_seconds");
    for (size_t i = 1; i < l.size(); ++i) {
        REQUIRE(fields(l[i]) == 15);
        REQUIRE(l[i].rfind(omp ? "synthetic_small,mean,fastzonal-openmp,2," : "synthetic_small,mean,fastzonal-serial,1,", 0) == 0);
        // the synthetic raster has 200 rows: 13 bands of 16, so two requested threads are two effective ones
        REQUIRE(l[i].find(omp ? ",2,16," : ",1,16,") != std::string::npos);
    }
    // a second call appends without repeating the header
    REQUIRE(run(cmd) == 0);
    REQUIRE(lines_of(out).size() == 7);
    // every op is accepted and the serial backend is named
    for (const char* op : {"rasterize", "majority", "ratio"}) {
        const std::string c2 = std::string("\"") + FZ_BENCH_EXE + "\" \"" + fixture.string() + "\" " + op +
                               " rect synth --den synth --backend serial --reps 1 --out \"" + out.string() + "\"";
        REQUIRE(run(c2) == 0);
    }
    auto all = lines_of(out);
    REQUIRE(all.size() == 10);
    REQUIRE(all.back().rfind("synthetic_small,ratio,fastzonal-serial,1,", 0) == 0);
    fs::remove(out);
}
