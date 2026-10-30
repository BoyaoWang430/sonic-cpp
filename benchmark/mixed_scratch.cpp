/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

// Synthetic fresh-Document parse/query/destroy sequences: 1/4/16 KiB objects,
// dense arrays, malformed/recovery pairs, and occasional >=1 MiB inputs.
// --workspace 1 reuses one explicit SAX workspace across cases; the default
// --workspace 0 uses the original Parse API.
// Build from the repository root; LLVM toolchain 1.0.0.102 supports this scalar
// RISC-V target without enabling RVV.
// clang-format off
// mixed_run=$(mktemp -d /tmp/sonic-mixed.XXXXXX)
// clang++ -std=c++17 -O3 -DNDEBUG -mcpu=jupiter -Iinclude -I. benchmark/mixed_scratch.cpp -o "$mixed_run/mixed_scratch"
// "$mixed_run/mixed_scratch" --validate-only --dump-dir "$mixed_run/dump"
// "$mixed_run/mixed_scratch" --milliseconds 200 --workspace 0 > "$mixed_run/timing.csv"
// "$mixed_run/mixed_scratch" --milliseconds 200 --workspace 1 > "$mixed_run/workspace.csv"
// python3 benchmark/verify_mixed.py --dump-dir "$mixed_run/dump" --csv "$mixed_run/timing.csv" --csv "$mixed_run/workspace.csv"
// "$mixed_run/mixed_scratch" --iterations 10000 --filter mixed_dense
// clang-format on
// --iterations counts individual Documents per sequence and overrides duration;
// duration mode rounds to complete cycles. All inputs are validated before
// filtering and warmup, so measurements describe a warmed process.
// --validate-only skips timing. The Python oracle checks full typed DOM dumps
// and CSV hashes/counts independently, without using timing results.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "sonic/parse_workspace.h"
#include "sonic/sonic.h"

namespace {
using sonic_json::Document;
using sonic_json::JsonPointer;
using sonic_json::Node;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
[[noreturn]] void Fail(const std::string& text) {
  std::cerr << text << '\n';
  std::exit(1);
}
struct Digest {
  uint64_t value = 14695981039346656037ULL;
  void Bytes(const char* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      value ^= static_cast<unsigned char>(p[i]);
      value *= 1099511628211ULL;
    }
  }
  void Integer(uint64_t value_) {
    for (unsigned i = 0; i < 8; ++i) {
      value ^= value_ & 255;
      value *= 1099511628211ULL;
      value_ >>= 8;
    }
  }
  void String(const std::string& value_) {
    Integer(value_.size());
    Bytes(value_.data(), value_.size());
  }
};
uint64_t BytesHash(const std::string& value) {
  Digest d;
  d.Bytes(value.data(), value.size());
  return d.value;
}
std::string Quote(const std::string& text) {
  static const char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += char(c);
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += char(c);
  }
  out += '"';
  return out;
}
enum class Kind { Object, Dense, Malformed };
const char* KindName(Kind kind) {
  return kind == Kind::Object  ? "object"
         : kind == Kind::Dense ? "dense"
                               : "malformed";
}
struct Input {
  std::string name, json;
  Kind kind;
  uint64_t expected;
};
struct Paths {
  JsonPointer id{"meta", "id"};
  JsonPointer active{"meta", "active"};
  JsonPointer label{"meta", "label"};
  JsonPointer rows{"rows"};
};
Input Object(const std::string& name, size_t target, uint64_t id) {
  const bool active = id % 2 != 0;
  std::string label =
      "sample-" + std::to_string(id) + "/\xe4\xb8\xad\xe6\x96\x87\n";
  label.push_back('\0');
  label += "tail";
  std::string json = "{\"meta\":{\"id\":" + std::to_string(id) +
                     ",\"active\":" + (active ? "true" : "false") +
                     ",\"label\":" + Quote(label) + "},\"rows\":[";
  json.reserve(target + 512);
  size_t rows = 0;
  do {
    if (rows) json += ',';
    json += "{\"seq\":" + std::to_string(rows) + ",\"name\":" +
            Quote("row-" + std::to_string(rows) + "/\xe4\xb8\x96\xe7\x95\x8c") +
            ",\"active\":true,\"numbers\":[-7,0,42,0.125],\"extra\":{\"text\":"
            "\"line\\nquote\\\"\",\"flag\":false,\"none\":null}}";
    ++rows;
  } while (json.size() + 2 < target);
  json += "]}";
  Digest expected;
  expected.Integer(1);
  expected.Integer(1);
  expected.Integer(id);
  expected.Integer(active);
  expected.String(label);
  expected.Integer(rows);
  expected.Integer(rows - 1);
  return {name, std::move(json), Kind::Object, expected.value};
}
Input Dense(size_t count) {
  std::string json = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i) json += ',';
    json += char('0' + i % 10);
  }
  json += ']';
  Digest expected;
  expected.Integer(1);
  expected.Integer(2);
  expected.Integer(count);
  expected.Integer(0);
  expected.Integer((count / 2) % 10);
  expected.Integer((count - 1) % 10);
  return {"dense_4096", std::move(json), Kind::Dense, expected.value};
}
Input Truncated(const Input& source) {
  Input result{source.name + "_truncated", source.json, Kind::Malformed, 0};
  result.json.pop_back();
  Digest expected;
  expected.Integer(0);
  expected.Integer(result.json.size());
  result.expected = expected.value;
  return result;
}
std::vector<Input> Inputs() {
  std::vector<Input> inputs;
  inputs.push_back(Object("object_1KiB", 1024, 101));
  inputs.push_back(Object("object_4KiB", 4096, 202));
  inputs.push_back(Object("object_16KiB", 16384, 303));
  inputs.push_back(Dense(4096));
  inputs.push_back(Object("object_over_1MiB", 1024 * 1024 + 1, 404));
  for (size_t i = 0; i < 5; ++i) inputs.push_back(Truncated(inputs[i]));
  return inputs;
}
struct Sequence {
  std::string name;
  std::vector<size_t> order;
};
std::vector<Sequence> Sequences() {
  std::vector<Sequence> sequences = {
      {"fresh/mixed_sizes", {0, 1, 2}},
      {"fresh/mixed_dense", {0, 3, 1, 2}},
      {"fresh/malformed_recovery", {5, 0, 7, 1, 8, 3, 6, 2}}};
  Sequence occasional{"fresh/occasional_large_recovery", {}};
  const size_t regular[] = {0, 3, 1, 2};
  for (size_t i = 0; i < 64; ++i) occasional.order.push_back(regular[i % 4]);
  occasional.order[0] =
      4;  // Valid >=1 MiB, immediately followed by fresh small DOM.
  occasional.order[1] = 0;
  occasional.order[32] = 9;  // Malformed >=1 MiB, then fresh small recovery.
  occasional.order[33] = 0;
  sequences.push_back(std::move(occasional));
  return sequences;
}
template <bool UseWorkspace>
std::unique_ptr<Document> ParseFresh(const Input& input,
                                     sonic_json::ParseWorkspace& workspace) {
  auto document = std::make_unique<Document>();
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(document.get()) : "memory");
#endif
  if constexpr (UseWorkspace) {
    document->Parse(input.json.data(), input.json.size(), workspace);
  } else {
    document->Parse(input.json.data(), input.json.size());
  }
  return document;
}
struct Operation {
  uint64_t hash;
  bool failed;
};
template <bool UseWorkspace>
Operation Run(const Input& input, const Paths& paths,
              sonic_json::ParseWorkspace& workspace) {
  auto document = ParseFresh<UseWorkspace>(input, workspace);
  Digest result;
  if (input.kind == Kind::Malformed) {
    if (!document->HasParseError() ||
        document->GetParseError() == sonic_json::kErrorNoMem)
      Fail("expected a syntax failure: " + input.name);
    result.Integer(0);
    result.Integer(input.json.size());
    return {result.value, true};
  }
  if (document->HasParseError())
    Fail("unexpected parse failure: " + input.name);
  result.Integer(1);
  if (input.kind == Kind::Object) {
    const Node* id = document->AtPointer(paths.id);
    const Node* active = document->AtPointer(paths.active);
    const Node* label = document->AtPointer(paths.label);
    const Node* rows = document->AtPointer(paths.rows);
    if (!document->IsObject() || !id || !id->IsUint64() || !active ||
        !active->IsBool() || !label || !label->IsString() || !rows ||
        !rows->IsArray() || rows->Empty())
      Fail("object query shape mismatch: " + input.name);
    const Node& last = (*rows)[rows->Size() - 1];
    if (!last.IsObject()) Fail("last row is not an object: " + input.name);
    auto member = last.FindMember("seq");
    if (member == last.MemberEnd() || !member->value.IsUint64())
      Fail("last row mismatch: " + input.name);
    const std::string owned_label = label->GetString();
    result.Integer(1);
    result.Integer(id->GetUint64());
    result.Integer(active->GetBool());
    result.String(owned_label);
    result.Integer(rows->Size());
    result.Integer(member->value.GetUint64());
  } else {
    if (!document->IsArray() || document->Empty()) Fail("dense shape mismatch");
    const size_t count = document->Size();
    const Node& first = (*document)[size_t(0)];
    const Node& middle = (*document)[count / 2];
    const Node& last = (*document)[count - 1];
    if (!first.IsUint64() || !middle.IsUint64() || !last.IsUint64())
      Fail("dense query types mismatch");
    result.Integer(2);
    result.Integer(count);
    result.Integer(first.GetUint64());
    result.Integer(middle.GetUint64());
    result.Integer(last.GetUint64());
  }
  return {result.value, false};
}
struct Totals {
  uint64_t checksum = 0, bytes = 0, failures = 0;
};
Totals Expected(const Sequence& sequence, const std::vector<Input>& inputs,
                uint64_t count) {
  Totals cycle, prefix;
  for (size_t i = 0; i < sequence.order.size(); ++i) {
    const Input& input = inputs[sequence.order[i]];
    cycle.checksum += input.expected;
    cycle.bytes += input.json.size();
    cycle.failures += input.kind == Kind::Malformed;
    if (i < count % sequence.order.size()) {
      prefix.checksum += input.expected;
      prefix.bytes += input.json.size();
      prefix.failures += input.kind == Kind::Malformed;
    }
  }
  const uint64_t cycles = count / sequence.order.size();
  if (cycles >
      (std::numeric_limits<uint64_t>::max() - prefix.bytes) / cycle.bytes)
    Fail("requested processed-byte count would overflow");
  return {cycle.checksum * cycles + prefix.checksum,
          cycle.bytes * cycles + prefix.bytes,
          cycle.failures * cycles + prefix.failures};
}
template <bool UseWorkspace>
Totals RunBatch(const Sequence& sequence, const std::vector<Input>& inputs,
                const Paths& paths, uint64_t count,
                sonic_json::ParseWorkspace& workspace) {
  Totals result;
  size_t phase = 0;
  for (uint64_t i = 0; i < count; ++i) {
    const Input& input = inputs[sequence.order[phase]];
    const Operation operation = Run<UseWorkspace>(input, paths, workspace);
    result.checksum += operation.hash;
    result.bytes += input.json.size();
    result.failures += operation.failed;
    if (++phase == sequence.order.size()) phase = 0;
  }
  return result;
}
void CheckTotals(const Totals& got, const Totals& expected,
                 const std::string& name) {
  if (got.checksum != expected.checksum || got.bytes != expected.bytes ||
      got.failures != expected.failures)
    Fail("sequence result mismatch: " + name);
}
void Write(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) Fail("cannot write " + path.string());
  out.write(text.data(), text.size());
  out.close();
  if (!out) Fail("write failed " + path.string());
}
template <bool UseWorkspace>
void Validate(const std::vector<Input>& inputs,
              const std::vector<Sequence>& sequences, const Paths& paths,
              const fs::path& dump, sonic_json::ParseWorkspace& workspace) {
  std::ofstream manifest, order;
  if (!dump.empty()) {
    fs::create_directories(dump);
    manifest.open(dump / "fixtures.tsv");
    order.open(dump / "sequences.tsv");
    if (!manifest || !order) Fail("cannot create dump manifests");
    manifest << "name\tkind\tinput_json\tcanonical_json\texpected_result_"
                "hash\tinput_hash\tcanonical_hash\n";
    order << "case\tposition\tfixture\n";
  }
  for (const Input& input : inputs) {
    if (Run<UseWorkspace>(input, paths, workspace).hash != input.expected)
      Fail("fixture query oracle mismatch: " + input.name);
    uint64_t canonical_hash = 0;
    std::string canonical;
    if (input.kind != Kind::Malformed) {
      auto doc = ParseFresh<UseWorkspace>(input, workspace);
      sonic_json::WriteBuffer buffer;
      if (doc->HasParseError() ||
          doc->Serialize(buffer) != sonic_json::kErrorNone)
        Fail("fixture serialization failed: " + input.name);
      canonical.assign(buffer.ToString(), buffer.Size());
      canonical_hash = BytesHash(canonical);
    }
    if (!dump.empty()) {
      const std::string source_file = input.name + ".json";
      const std::string canonical_file =
          input.kind == Kind::Malformed ? "-" : input.name + ".canonical.json";
      Write(dump / source_file, input.json);
      if (canonical_file != "-") Write(dump / canonical_file, canonical);
      manifest << input.name << '\t' << KindName(input.kind) << '\t'
               << source_file << '\t' << canonical_file << '\t'
               << input.expected << '\t' << BytesHash(input.json) << '\t'
               << canonical_hash << '\n';
    }
  }
  for (const Sequence& sequence : sequences) {
    CheckTotals(RunBatch<UseWorkspace>(sequence, inputs, paths,
                                       sequence.order.size(), workspace),
                Expected(sequence, inputs, sequence.order.size()),
                sequence.name);
    if (!dump.empty())
      for (size_t i = 0; i < sequence.order.size(); ++i)
        order << sequence.name << '\t' << i << '\t'
              << inputs[sequence.order[i]].name << '\n';
  }
  if (!dump.empty()) {
    manifest.close();
    order.close();
    if (!manifest || !order) Fail("manifest close failed");
    Write(dump / "contract.txt",
          "Synthetic inputs; no production distribution claim. Every op uses a "
          "fresh Document and default allocator.\n"
          "Hash: FNV1a64 offset 14695981039346656037, prime 1099511628211; "
          "integers as 8 little-endian bytes; strings length then full UTF-8 "
          "bytes.\n"
          "object result: 1,1,meta.id,meta.active,meta.label,rows.length,last "
          "rows.seq.\n"
          "dense result: 1,2,length,first,middle,last. malformed result: "
          "0,input byte length.\n"
          "CSV checksum sums actual op hashes modulo 2^64, excluding "
          "validation/warmup/calibration; result_hash hashes cycle length then "
          "expected op hashes in sequence.\n"
          "iterations counts individual fresh Documents, not cycles. "
          "Timed-duration mode rounds up to whole cycles; fixed iterations "
          "uses the exact requested prefix.\n");
  }
}
struct Options {
  uint64_t iterations = 0;
  double milliseconds = 100;
  bool validate_only = false;
  bool workspace = false;
  std::string filter;
  fs::path dump;
};
Options ReadOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--validate-only") {
      options.validate_only = true;
      continue;
    }
    if (++i == argc) Fail("missing value: " + flag);
    const std::string value = argv[i];
    if (flag == "--iterations") {
      const auto parsed = std::from_chars(
          value.data(), value.data() + value.size(), options.iterations);
      if (parsed.ec != std::errc() ||
          parsed.ptr != value.data() + value.size() || !options.iterations)
        Fail("invalid iterations");
    } else if (flag == "--milliseconds") {
      char* end = nullptr;
      options.milliseconds = std::strtod(value.c_str(), &end);
      if (*end ||
          !(options.milliseconds > 0 && options.milliseconds <= 60000))
        Fail("invalid milliseconds");
    } else if (flag == "--dump-dir")
      options.dump = value;
    else if (flag == "--filter")
      options.filter = value;
    else if (flag == "--workspace") {
      if (value != "0" && value != "1") Fail("--workspace expects 0 or 1");
      options.workspace = value == "1";
    } else
      Fail("unknown option: " + flag);
  }
  return options;
}
template <bool UseWorkspace>
void Measure(const Options& options, const Sequence& sequence,
             const std::vector<Input>& inputs, const Paths& paths,
             sonic_json::ParseWorkspace& workspace) {
  const uint64_t period = sequence.order.size();
  const Totals cycle = Expected(sequence, inputs, period);
  CheckTotals(
      RunBatch<UseWorkspace>(sequence, inputs, paths, period, workspace), cycle,
      sequence.name);  // One interleaved warmup cycle.
  uint64_t iterations = options.iterations;
  if (!iterations) {
    uint64_t calibration = period;
    double elapsed;
    do {
      const auto start = Clock::now();
      const Totals got = RunBatch<UseWorkspace>(sequence, inputs, paths,
                                                calibration, workspace);
      elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start)
                    .count();
      CheckTotals(got, Expected(sequence, inputs, calibration), sequence.name);
      if (elapsed >= options.milliseconds / 10 ||
          calibration >= (uint64_t{1} << 31))
        break;
      calibration *= 2;
    } while (true);
    const double estimate =
        calibration * options.milliseconds / std::max(elapsed, 0.001);
    iterations = std::max<uint64_t>(
        period,
        static_cast<uint64_t>(std::min(estimate, double(uint64_t{1} << 32))));
    iterations = ((iterations + period - 1) / period) * period;
  }
  const Totals expected = Expected(sequence, inputs, iterations);
  const auto start = Clock::now();
  const Totals got =
      RunBatch<UseWorkspace>(sequence, inputs, paths, iterations, workspace);
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  CheckTotals(got, expected, sequence.name);
  Digest sequence_hash;
  sequence_hash.Integer(period);
  for (size_t i : sequence.order) sequence_hash.Integer(inputs[i].expected);
  std::cout << sequence.name << ',' << period << ',' << cycle.bytes << ','
            << cycle.failures << ',' << iterations << ',' << std::fixed
            << std::setprecision(2) << elapsed / iterations << ','
            << got.checksum << ',' << sequence_hash.value << ',' << got.bytes
            << ',' << got.failures << '\n';
}
template <bool UseWorkspace>
int RunSuite(const Options& options) {
  sonic_json::ParseWorkspace workspace;
  const auto inputs = Inputs();
  const auto sequences = Sequences();
  const Paths paths;
  Validate<UseWorkspace>(inputs, sequences, paths, options.dump, workspace);
  if (options.validate_only) {
    std::cout << "validated " << inputs.size() << " fixtures and "
              << sequences.size() << " mixed sequences\n";
    return 0;
  }
  std::cout
      << "case,cycle_documents,cycle_input_bytes,cycle_failures,iterations,ns_"
         "per_op,checksum,result_hash,processed_bytes,parse_failures\n";
  size_t selected = 0;
  for (const Sequence& sequence : sequences) {
    if (!options.filter.empty() &&
        sequence.name.find(options.filter) == std::string::npos)
      continue;
    Measure<UseWorkspace>(options, sequence, inputs, paths, workspace);
    ++selected;
  }
  if (!selected) Fail("no matching sequence");
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  const Options options = ReadOptions(argc, argv);
  return options.workspace ? RunSuite<true>(options) : RunSuite<false>(options);
}
