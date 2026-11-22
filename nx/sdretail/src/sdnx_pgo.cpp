// SDNX 16.188: profile collection of the PGO-instrumented build (SDNX_PGO=generate in nx/tools/sd_build_nro.sh).
//
// libgcov normally writes one .gcda per object at exit, at the build machine's paths: neither fits the Switch (the
// game is closed from HOME, our exit is not clean, those paths do not exist). With -fprofile-info-section=gcov_info
// each instrumented object only puts a pointer to its counters in a section, and this file serialises them all with
// __gcov_info_to_gcda (the freestanding pattern of GCC's manual, "Profiling and Test Coverage in Freestanding
// Environments") into one stream, <NRO folder>/pgo/sdnx_pgo_<N>.stream (N = this session), every 2 minutes and the
// whole session each time (the counts only grow), through a .tmp file so a dump cut short keeps the previous one.
// nx/tools/sd_pgo_merge.sh turns it into .gcda files (gcov-tool merge-stream) for the SDNX_PGO=use build. The
// workflow is the one renut_nx_ultimate_base documents (docs/building.md, section 4); this is our own code.
// In every other build this file is empty.

#if defined(SDNX_PGO_GENERATE)

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern "C" {  // GCC's gcov.h has no C++ guard of its own
#include <gcov.h>
}

// One pointer per instrumented object, in the section GNU ld brackets with these two symbols.
extern "C" const struct gcov_info* const __start_gcov_info[];
extern "C" const struct gcov_info* const __stop_gcov_info[];

namespace {

constexpr auto kPeriod = std::chrono::minutes(2);

struct Stream {
  std::FILE* file = nullptr;
  bool ok = true;
  size_t bytes = 0;
  std::vector<std::unique_ptr<unsigned char[]>> allocations;  // libgcov's scratch buffers for one dump
};

void Write(const void* data, unsigned length, void* arg) {
  auto* s = static_cast<Stream*>(arg);
  if (s->ok && length && std::fwrite(data, 1, length, s->file) != length) s->ok = false;
  s->bytes += length;
}

void Filename(const char* name, void* arg) { __gcov_filename_to_gcfn(name, Write, arg); }

void* Allocate(unsigned length, void* arg) {
  auto* s = static_cast<Stream*>(arg);
  s->allocations.push_back(std::make_unique<unsigned char[]>(length ? length : 1));
  return s->allocations.back().get();
}

std::filesystem::path PickFile() {
  const std::filesystem::path folder = rex::filesystem::GetExecutableFolder() / "pgo";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  for (int n = 1; n < 10000; ++n) {
    const std::filesystem::path candidate = folder / ("sdnx_pgo_" + std::to_string(n) + ".stream");
    std::filesystem::path temporary = candidate;
    temporary += ".tmp";
    if (!std::filesystem::exists(candidate, ec) && !std::filesystem::exists(temporary, ec)) return candidate;
  }
  return {};
}

void DumpNow(const std::filesystem::path& file, int number) {
  std::filesystem::path temporary = file;
  temporary += ".tmp";
  Stream stream;
  stream.file = std::fopen(temporary.string().c_str(), "wb");
  if (!stream.file) {
    REXLOG_ERROR("[sdnx_pgo] cannot write {}", temporary.string());
    return;
  }
  static std::vector<char> buffer(1 << 20);  // one SD write per MB instead of per record
  std::setvbuf(stream.file, buffer.data(), _IOFBF, buffer.size());
  const auto start = std::chrono::steady_clock::now();
  const gcov_info* const* info = __start_gcov_info;
  const gcov_info* const* const end = __stop_gcov_info;
  __asm__("" : "+r"(info));  // as in GCC's manual: the compiler must not reason about the linker's symbols
  size_t objects = 0;
  for (; info != end && stream.ok; ++info) {
    if (*info) {
      __gcov_info_to_gcda(*info, Filename, Write, Allocate, &stream);
      ++objects;
    }
  }
  const bool closed = std::fclose(stream.file) == 0;
  if (!stream.ok || !closed) {
    REXLOG_ERROR("[sdnx_pgo] dump {} failed (the previous one stays)", number);
    return;
  }
  std::error_code ec;
  std::filesystem::rename(temporary, file, ec);
  if (ec) {  // a file system that does not replace on rename
    std::filesystem::remove(file, ec);
    std::filesystem::rename(temporary, file, ec);
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("[sdnx_pgo] dump {} written: {} ({} objects, {:.1f} MB, {:.0f} ms)", number, file.string(), objects,
              stream.bytes / 1048576.0, ms);
}

// Started by a static constructor; the first dump (and the choice of the file) waits 2 minutes, when the SDK has set
// the executable folder and the log.
const bool kStarted = [] {
  std::thread([] {
    std::filesystem::path file;
    for (int number = 1;; ++number) {
      std::this_thread::sleep_for(kPeriod);
      if (file.empty()) file = PickFile();
      if (file.empty()) return;
      DumpNow(file, number);
    }
  }).detach();
  return true;
}();

}  // namespace

#endif  // SDNX_PGO_GENERATE
