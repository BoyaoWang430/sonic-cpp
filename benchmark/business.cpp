/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

// Full DOM lifecycle + sparse business queries. No external dependencies.
// --workspace 0 (default) uses the original Parse API; --workspace 1 reuses
// one explicit SAX workspace across cases. Record the mode with the command.
// clang-format off
// clang++ -std=c++17 -O3 -DNDEBUG -mcpu=jupiter -Iinclude benchmark/business.cpp -o /tmp/business
// python3 benchmark/business_fixtures.py --output /tmp/business-inputs
// /tmp/business --manifest /tmp/business-inputs/manifest.tsv --milliseconds 100 --workspace 0
// /tmp/business --manifest /tmp/business-inputs/manifest.tsv --milliseconds 100 --workspace 1
// /tmp/business --manifest /tmp/business-inputs/manifest.tsv --iterations 10
// clang-format on
// Inputs and JsonPointers are prepared before timing. Each operation allocates
// fresh Documents, fully parses both inputs if present, queries, then deletes
// both Documents before returning. String getters copy their result. Only the
// explicit edit_serialize case serializes inside the timed operation.
// Integer getters accept in-range JSON integers or complete decimal strings.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sonic/parse_workspace.h"
#include "sonic/sonic.h"

namespace {
using sonic_json::Document;
using sonic_json::JsonPointer;
using sonic_json::Node;
using sonic_json::WriteBuffer;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}

struct Digest {
  uint64_t value = 14695981039346656037ULL;
  void Bytes(const char* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      value ^= static_cast<unsigned char>(data[i]);
      value *= 1099511628211ULL;
    }
  }
  void Integer(uint64_t number) {
    for (unsigned i = 0; i < 8; ++i) {
      value ^= number & 0xff;
      value *= 1099511628211ULL;
      number >>= 8;
    }
  }
  void String(const char* data, size_t length) {
    Integer(length);
    Bytes(data, length);
  }
};

enum class Kind { Int64, Int32, Boolean, String, Double, ObjectJson };
struct Query {
  unsigned document;
  JsonPointer path;
  Kind kind;
  int64_t default_integer = 0;
};
struct Input {
  std::string name;
  std::string scenario;
  fs::path path_a, path_b;
  std::string json_a, json_b;
  bool two_documents;
  std::vector<Query> queries;
  uint64_t expected;
};
struct Options {
  fs::path manifest;
  fs::path dump_dir;
  std::string filter;
  uint64_t iterations = 0;
  double milliseconds = 100;
  bool workspace = false;
};

uint64_t ReadUnsigned(const std::string& value) {
  uint64_t number = 0;
  auto result =
      std::from_chars(value.data(), value.data() + value.size(), number);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size())
    Fail("invalid unsigned integer: " + value);
  return number;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) Fail("cannot open " + path.string());
  std::string data{std::istreambuf_iterator<char>(file), {}};
  if (file.bad()) Fail("cannot read " + path.string());
  return data;
}

std::vector<Query> Queries(const std::string& scenario) {
  using K = Kind;
  if (scenario == "image") return {{0, {"image_comment_id"}, K::Int64}};
  if (scenario == "permission") return {{0, {"comment_permission"}, K::Int32}};
  if (scenario == "audio") return {{0, {"cancel_no_audio_extract"}, K::Int64}};
  if (scenario == "user")
    return {{0, {"hide_search"}, K::Boolean},
            {0, {"commerce_user_level"}, K::Int32},
            {0, {"commerce_role_bitmap"}, K::String},
            {1, {"enable_friend_active"}, K::Int32, 1},
            {1, {"favorite_permission"}, K::Int32}};
  if (scenario == "music")
    return {{0, {"is_image_beat"}, K::Int32},
            {0, {"music_video_category", "version"}, K::Int64},
            {0, {"music_video_category", "is_music"}, K::Boolean},
            {0, {"music_video_category", "score"}, K::Double},
            {0, {"luna_music_song"}, K::String},
            {0, {"music_item_type"}, K::Int64},
            {0, {"diversion_bind_sid"}, K::Int64},
            {0, {"ecom_bar_gid_interverne_type"}, K::Int64}};
  if (scenario == "edit_many")
    return {{0, {"aweme_type"}, K::Int64},
            {0, {"is_meteor"}, K::Boolean},
            {0, {"music_edit_info", "status"}, K::Int64},
            {1, {"is_origin_text"}, K::Boolean},
            {1, {"is_story"}, K::Boolean},
            {1, {"task", "id"}, K::Int64},
            {1, {"task", "status"}, K::Int64},
            {1, {"task", "channel"}, K::String},
            {1, {"mission_platform", "type"}, K::Int64},
            {1, {"mission_platform", "status"}, K::Int64},
            {1, {"star_atlas_order_id"}, K::Int64},
            {1, {"marketing_id"}, K::Int64}};
  if (scenario == "edit_early") return {{0, {"aweme_type"}, K::Int64}};
  if (scenario == "edit_serialize")
    return {{0, {"aweme_edit_info"}, K::ObjectJson}};
  if (scenario == "parse_item" || scenario == "parse_user") return {};
  Fail("unknown scenario: " + scenario);
}

std::vector<Input> LoadInputs(const Options& options) {
  std::ifstream manifest(options.manifest);
  if (!manifest) Fail("cannot open manifest " + options.manifest.string());
  std::vector<Input> inputs;
  std::set<std::string> names;
  std::string line;
  size_t line_number = 0;
  while (std::getline(manifest, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> columns;
    std::istringstream fields(line);
    std::string field;
    while (std::getline(fields, field, '\t')) columns.push_back(field);
    if (columns.size() != 5 || line.back() == '\t')
      Fail("manifest requires five TSV columns, line " +
           std::to_string(line_number));
    if (columns[0].empty() || columns[0].find(',') != std::string::npos ||
        !names.insert(columns[0]).second)
      Fail("invalid or duplicate case name at line " +
           std::to_string(line_number));
    auto queries = Queries(columns[1]);
    const bool needs_two = columns[1] == "user" || columns[1] == "edit_many" ||
                           columns[1] == "edit_early" ||
                           columns[1] == "parse_user";
    if ((columns[3] != "-") != needs_two)
      Fail("wrong document count for " + columns[0]);
    if (!options.filter.empty() &&
        columns[0].find(options.filter) == std::string::npos)
      continue;
    Input input;
    input.name = columns[0];
    input.scenario = columns[1];
    input.path_a = options.manifest.parent_path() / columns[2];
    input.json_a = ReadFile(input.path_a);
    input.two_documents = needs_two;
    if (needs_two) {
      input.path_b = options.manifest.parent_path() / columns[3];
      input.json_b = ReadFile(input.path_b);
    }
    input.queries = std::move(queries);
    input.expected = ReadUnsigned(columns[4]);
    inputs.push_back(std::move(input));
  }
  if (manifest.bad()) Fail("manifest read error");
  if (inputs.empty()) Fail("no matching benchmark cases");
  return inputs;
}

template <bool UseWorkspace>
std::unique_ptr<Document> Parse(const std::string& json,
                                const std::string& name,
                                sonic_json::ParseWorkspace& workspace) {
  auto document = std::make_unique<Document>();
  // Keep the outer allocation observable, as with NewDoc/DeleteDoc.
#if defined(__GNUC__) || defined(__clang__)
  asm volatile("" : : "g"(document.get()) : "memory");
#endif
  if constexpr (UseWorkspace) {
    document->Parse(json.data(), json.size(), workspace);
  } else {
    document->Parse(json.data(), json.size());
  }
  if (document->HasParseError())
    Fail(name + " parse error at " +
         std::to_string(document->GetErrorOffset()) + ": " +
         sonic_json::ErrorMsg(document->GetParseError()));
  return document;
}

int64_t Integer(const Node* node, int64_t fallback) {
  if (!node) return fallback;
  if (node->IsInt64()) return node->GetInt64();
  if (!node->IsString()) return fallback;
  const auto text = node->GetStringView();
  if (text.empty()) return fallback;
  int64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size()
             ? value
             : fallback;
}

template <bool UseWorkspace>
uint64_t Run(const Input& input, sonic_json::ParseWorkspace& workspace) {
  auto a = Parse<UseWorkspace>(input.json_a, input.name, workspace);
  std::unique_ptr<Document> b;
  if (input.two_documents)
    b = Parse<UseWorkspace>(input.json_b, input.name, workspace);
  Digest result;
  for (const Query& query : input.queries) {
    const Node* value =
        (query.document ? b.get() : a.get())->AtPointer(query.path);
    switch (query.kind) {
      case Kind::Int64:
        result.Integer(
            static_cast<uint64_t>(Integer(value, query.default_integer)));
        break;
      case Kind::Int32: {
        int64_t number = Integer(value, query.default_integer);
        if (number < std::numeric_limits<int32_t>::min() ||
            number > std::numeric_limits<int32_t>::max())
          number = query.default_integer;
        result.Integer(static_cast<uint64_t>(number));
        break;
      }
      case Kind::Boolean:
        result.Integer(value && value->IsBool() ? value->GetBool() : false);
        break;
      case Kind::String: {
        const std::string text =
            value && value->IsString() ? value->GetString() : "";
        result.String(text.data(), text.size());
        break;
      }
      case Kind::Double: {
        const double number = value && (value->IsDouble() || value->IsInt64() ||
                                        value->IsUint64())
                                  ? value->GetDouble()
                                  : 0.0;
        uint64_t bits;
        std::memcpy(&bits, &number, sizeof(bits));
        result.Integer(bits);
        break;
      }
      case Kind::ObjectJson: {
        WriteBuffer output;
        if (value && value->IsObject()) {
          if (value->Serialize(output) != sonic_json::kErrorNone)
            Fail("subtree serialization failed: " + input.name);
          result.String(output.ToString(), output.Size());
        } else {
          result.String("", 0);
        }
        break;
      }
    }
  }
  return result.value;
}

template <bool UseWorkspace>
uint64_t ValidateDocument(const std::string& json, const std::string& name,
                          const fs::path& dump,
                          sonic_json::ParseWorkspace& workspace) {
  auto document = Parse<UseWorkspace>(json, name, workspace);
  WriteBuffer serialized;
  if (document->Serialize(serialized) != sonic_json::kErrorNone)
    Fail("full document serialization failed: " + name);
  Digest digest;
  digest.Bytes(serialized.ToString(), serialized.Size());
  if (!dump.empty()) {
    std::ofstream file(dump, std::ios::binary);
    if (!file) Fail("cannot create " + dump.string());
    file.write(serialized.ToString(), serialized.Size());
    file.close();
    if (!file) Fail("cannot write " + dump.string());
  }
  return digest.value;
}

template <bool UseWorkspace>
void Measure(const Options& options, const Input& input, size_t index,
             std::ofstream& dump_manifest,
             sonic_json::ParseWorkspace& workspace) {
  fs::path dump_a, dump_b;
  if (!options.dump_dir.empty()) {
    dump_a = options.dump_dir / (std::to_string(index) + ".a.json");
    if (input.two_documents)
      dump_b = options.dump_dir / (std::to_string(index) + ".b.json");
  }
  const uint64_t hash_a = ValidateDocument<UseWorkspace>(
      input.json_a, input.name, dump_a, workspace);
  const uint64_t hash_b = input.two_documents
                              ? ValidateDocument<UseWorkspace>(
                                    input.json_b, input.name, dump_b, workspace)
                              : 0;
  if (!options.dump_dir.empty()) {
    dump_manifest << input.path_a.string() << '\t' << dump_a.string() << '\n';
    if (input.two_documents)
      dump_manifest << input.path_b.string() << '\t' << dump_b.string() << '\n';
  }
  if (Run<UseWorkspace>(input, workspace) != input.expected)
    Fail("query result mismatch: " + input.name);
  uint64_t warmup = 0;
  for (unsigned i = 0; i < 16; ++i)
    warmup += Run<UseWorkspace>(input, workspace);
  if (warmup != input.expected * 16)
    Fail("warmup result mismatch: " + input.name);
  uint64_t iterations = options.iterations;
  if (!iterations) {
    uint64_t calibration = 1;
    double elapsed;
    do {
      uint64_t checksum = 0;
      const auto start = Clock::now();
      for (uint64_t i = 0; i < calibration; ++i)
        checksum += Run<UseWorkspace>(input, workspace);
      elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start)
                    .count();
      if (checksum != input.expected * calibration)
        Fail("calibration result mismatch: " + input.name);
      if (elapsed >= options.milliseconds / 10) break;
      calibration *= 2;
    } while (calibration < (uint64_t{1} << 32));
    iterations = std::max<uint64_t>(
        1, static_cast<uint64_t>(calibration * options.milliseconds /
                                 std::max(elapsed, 0.001)));
  }
  uint64_t checksum = 0;
  const auto start = Clock::now();
  for (uint64_t i = 0; i < iterations; ++i)
    checksum += Run<UseWorkspace>(input, workspace);
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  if (checksum != input.expected * iterations)
    Fail("measured result mismatch: " + input.name);
  std::cout << input.name << ',' << input.scenario << ','
            << input.json_a.size() + input.json_b.size() << ','
            << (input.two_documents ? 2 : 1) << ',' << input.queries.size()
            << ',' << iterations << ',' << std::fixed << std::setprecision(2)
            << elapsed / iterations << ',' << checksum << ',' << input.expected
            << ',' << hash_a << ',' << hash_b << '\n';
}

Options ReadOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (++i == argc) Fail("missing value for " + option);
    const std::string value = argv[i];
    if (option == "--manifest")
      options.manifest = value;
    else if (option == "--dump-dir")
      options.dump_dir = value;
    else if (option == "--filter")
      options.filter = value;
    else if (option == "--iterations") {
      options.iterations = ReadUnsigned(value);
      if (!options.iterations) Fail("iterations must be positive");
    } else if (option == "--milliseconds") {
      char* end = nullptr;
      options.milliseconds = std::strtod(value.c_str(), &end);
      if (*end ||
          !(options.milliseconds > 0 && options.milliseconds <= 60000))
        Fail("invalid milliseconds");
    } else if (option == "--workspace") {
      if (value != "0" && value != "1") Fail("--workspace expects 0 or 1");
      options.workspace = value == "1";
    } else {
      Fail("unknown option: " + option);
    }
  }
  if (options.manifest.empty()) Fail("--manifest PATH is required");
  return options;
}
template <bool UseWorkspace>
int RunSuite(const Options& options) {
  sonic_json::ParseWorkspace workspace;
  const auto inputs = LoadInputs(options);
  std::ofstream dump_manifest;
  if (!options.dump_dir.empty()) {
    fs::create_directories(options.dump_dir);
    dump_manifest.open(options.dump_dir / "documents.tsv");
    if (!dump_manifest) Fail("cannot create dump manifest");
  }
  std::cout
      << "case,scenario,input_bytes,documents,queries,iterations,ns_per_op,"
         "checksum,result_hash,document_a_hash,document_b_hash\n";
  for (size_t i = 0; i < inputs.size(); ++i)
    Measure<UseWorkspace>(options, inputs[i], i, dump_manifest, workspace);
  if (dump_manifest.is_open()) {
    dump_manifest.close();
    if (!dump_manifest) Fail("cannot write dump manifest");
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  const Options options = ReadOptions(argc, argv);
  return options.workspace ? RunSuite<true>(options) : RunSuite<false>(options);
}
