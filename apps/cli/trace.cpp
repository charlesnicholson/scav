// `scav trace` and `dump --trace`'s streaming: a run's encoded trace goes through a queue
// to one writer thread, which appends it to a file or decodes it to JSON on stdout.

#include "cli.h"

#include "scav/scav_core.h"
#include "scav/scav_layout.h"
#include "scav_chunk_queue.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace cli {

namespace {

constexpr size_t TRACE_READ{ size_t{ 256 } << 10U };  // bytes per read of a trace file
constexpr uint32_t TRACE_QUEUE_DEPTH{ 4 };            // chunks the queue holds

bool write_stdout(void * /*ctx*/, char const *text, size_t n) {
  return std::fwrite(text, 1, n, stdout) == n;
}

// Renames `from` onto `to`, replacing `to` if it exists.
bool replace_file(char const *from, char const *to) {
#ifdef _WIN32
  return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return std::rename(from, to) == 0;
#endif
}

// A file written as `<path>.tmp` and renamed onto `path` once whole.
struct AtomicFile {
  std::FILE *file{ nullptr };
  std::string path;
  std::string temp;
};

bool atomic_file_open(AtomicFile &f, char const *path) {
  f.path = path;
  f.temp = f.path + ".tmp";
  f.file = std::fopen(f.temp.c_str(), "wb");
  return f.file != nullptr;
}

// A `ChunkWrite` appending to the temp file.
bool atomic_file_write(void *ctx, uint8_t const *data, size_t n) {
  auto &f{ *static_cast<AtomicFile *>(ctx) };
  return std::fwrite(data, 1, n, f.file) == n;
}

// Closes the temp file and renames it onto the path when `whole`; otherwise, or when
// either fails, removes it.
bool atomic_file_close(AtomicFile &f, bool whole) {
  bool const closed{ std::fclose(f.file) == 0 };
  f.file = nullptr;
  bool const moved{ whole && closed && replace_file(f.temp.c_str(), f.path.c_str()) };
  if (!moved) { std::ignore = std::remove(f.temp.c_str()); }
  return moved;
}

// A `ChunkWrite` decoding to JSON on stdout.
bool json_write(void *ctx, uint8_t const *data, size_t n) {
  return trace_decode(*static_cast<TraceDecoder *>(ctx), data, n);
}

// A `TraceChunk` handing the chunk to the writer thread.
bool queue_chunk(void *ctx, uint8_t const *data, size_t n) {
  return static_cast<ChunkQueue *>(ctx)->push(data, n);
}

// Feeds the rest of `f` to `d` a read at a time; false when `f` cannot be read or `d`
// refuses it.
bool feed_file(std::FILE *f, TraceDecoder &d) {
  std::vector<uint8_t> buffer(TRACE_READ);
  for (;;) {
    size_t const got{ std::fread(buffer.data(), 1, buffer.size(), f) };
    if (!trace_decode(d, buffer.data(), got) || (std::ferror(f) != 0)) { return false; }
    if (got < buffer.size()) { return std::feof(f) != 0; }
  }
}

}  // namespace

bool trace_layout(Chart &c,
                  scav_spaces const &s,
                  scav_layout_opts const &o,
                  std::vector<scav_placed> &placed,
                  std::vector<Diagnostic> &diags,
                  DumpTrace const &trace,
                  LayoutArgs const &args,
                  bool &laid) {
  laid = false;
  AtomicFile file;
  TraceDecoder json;
  json.write = write_stdout;
  bool const to_file{ trace.file != nullptr };
  if (to_file && !atomic_file_open(file, trace.file)) { return false; }
  ChunkQueue queue{ to_file ? atomic_file_write : json_write,
                    to_file ? static_cast<void *>(&file) : static_cast<void *>(&json),
                    TRACE_QUEUE_DEPTH };
  bool streamed{ false };
  laid = layout_trace(c,
                      s,
                      o,
                      placed,
                      diags,
                      queue_chunk,
                      &queue,
                      streamed,
                      args.row,
                      trace.scope,
                      &args.pins);
  bool const written{ queue.close() && streamed };
  if (to_file) { return atomic_file_close(file, written); }
  return written && trace_decode_end(json);
}

// Checks the whole file, then prints it; a file that is not one whole trace prints
// nothing.
int run_trace(char const *path) {
  std::FILE *const f{ std::fopen(path, "rb") };
  TraceDecoder check;
  if ((f == nullptr) || !feed_file(f, check) || !trace_decode_end(check)) {
    if (f != nullptr) { std::ignore = std::fclose(f); }
    write_error(check.foreign ? "trace written by a different scav build"
                              : "not a whole trace file",
                path);
    return EXIT_UNUSABLE;
  }
  TraceDecoder json;
  json.write = write_stdout;
  bool const printed{ (std::fseek(f, 0, SEEK_SET) == 0) && feed_file(f, json) &&
                      trace_decode_end(json) };
  std::ignore = std::fclose(f);
  if (!printed) {
    write_error("cannot write the trace", "-");
    return EXIT_UNUSABLE;
  }
  return EXIT_CLEAN;
}

}  // namespace cli
