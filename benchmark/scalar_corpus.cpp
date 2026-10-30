/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

// Dependency-free corpus benchmark; compile and run from the repository root.
// clang-format off
// clang++ -std=c++17 -O3 -DNDEBUG -Iinclude -I. benchmark/scalar_corpus.cpp \
//   -o /tmp/scalar_corpus
// /tmp/scalar_corpus --milliseconds 150
// clang-format on
// RISC-V (LLVM toolchain 1.0.0.102): add -mcpu=jupiter, which does not enable RVV.
// --data-dir DIR selects fixtures; repeat --file PATH to replace the defaults.
// --iterations N fixes the work count; --filter NAME selects matching cases.
// Output hashes validate complete serialized results outside the timed loops.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "sonic/sonic.h"

namespace {
using Clock = std::chrono::steady_clock;
using sonic_json::Document;
using sonic_json::JsonPointer;
using sonic_json::Node;
using sonic_json::WriteBuffer;
struct Options {
  std::string data_dir = "testdata";
  std::vector<std::string> files;
  std::string filter;
  uint64_t iterations = 0;
  double milliseconds = 150;
};
[[noreturn]] void Fail(const std::string& reason) {
  std::cerr << reason << '\n';
  std::exit(1);
}
void CheckParse(const Document& doc) {
  if (doc.HasParseError())
    Fail("parse failure at " + std::to_string(doc.GetErrorOffset()) + ": " +
         sonic_json::ErrorMsg(doc.GetParseError()));
}
uint64_t Hash(const char* data, size_t length) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < length; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}
uint64_t Fingerprint(const Node& node) {
  if (node.IsContainer() || node.IsString()) return node.Size() + 11;
  if (node.IsUint64()) return node.GetUint64();
  if (node.IsInt64()) return static_cast<uint64_t>(node.GetInt64());
  if (node.IsDouble()) {
    const double value = node.GetDouble();
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  }
  return node.IsBool() ? node.GetBool() + 3 : 7;
}
uint64_t Serialize(const Node& node, WriteBuffer& buffer) {
  buffer.Clear();
  const auto error = node.Serialize(buffer);
  if (error != sonic_json::kErrorNone)
    Fail(std::string("serialize failure: ") + sonic_json::ErrorMsg(error));
  const size_t size = buffer.Size();
  const char* data = buffer.ToString();
  return size + static_cast<unsigned char>(data[0]) +
         static_cast<unsigned char>(data[size / 2]) +
         static_cast<unsigned char>(data[size - 1]);
}
JsonPointer LastLeafPath(const Node& root, std::string& expected) {
  JsonPointer path;
  const Node* node = &root;
  for (size_t depth = 0; depth < 64 && node->IsContainer() && node->Size();
       ++depth) {
    if (node->IsObject()) {
      auto last = node->MemberEnd() - 1;
      path.emplace_back(last->name.GetString());
      node = &last->value;
    } else {
      path.emplace_back(static_cast<int>(node->Size() - 1));
      node = node->End() - 1;
    }
  }
  WriteBuffer buffer;
  Serialize(*node, buffer);
  expected.assign(buffer.ToString(), buffer.Size());
  return path;
}

template <typename Operation>
void Measure(const Options& options, const std::string& name, size_t bytes,
             uint64_t output_hash, Operation operation) {
  if (!options.filter.empty() && name.find(options.filter) == std::string::npos)
    return;
  uint64_t checksum = 0;
  const size_t warmup =
      std::max<size_t>(2, std::min<size_t>(32, 65536 / bytes));
  for (size_t i = 0; i < warmup; ++i) checksum += operation();
  uint64_t iterations = options.iterations;
  if (!iterations) {
    uint64_t calibration = 1;
    double elapsed;
    do {
      const auto start = Clock::now();
      for (uint64_t i = 0; i < calibration; ++i) checksum += operation();
      elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start)
                    .count();
      if (elapsed >= options.milliseconds / 10) break;
      calibration *= 2;
    } while (calibration < (uint64_t{1} << 32));
    iterations = std::max<uint64_t>(
        1, static_cast<uint64_t>(calibration * options.milliseconds /
                                 std::max(elapsed, 0.001)));
  }
  const auto start = Clock::now();
  for (uint64_t i = 0; i < iterations; ++i) checksum += operation();
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  std::cout << name << ',' << bytes << ',' << iterations << ',' << std::fixed
            << std::setprecision(2) << elapsed / iterations << ',' << checksum
            << ',' << output_hash << '\n';
}
Options ReadOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (++i == argc) Fail("missing value for " + option);
    const char* value = argv[i];
    char* end = nullptr;
    if (option == "--data-dir")
      options.data_dir = value;
    else if (option == "--file")
      options.files.emplace_back(value);
    else if (option == "--filter")
      options.filter = value;
    else if (option == "--iterations") {
      errno = 0;
      options.iterations = std::strtoull(value, &end, 10);
      if (*value == '-' || !*value || *end || errno == ERANGE ||
          !options.iterations)
        Fail("invalid iterations");
    } else if (option == "--milliseconds") {
      options.milliseconds = std::strtod(value, &end);
      if (!*value || *end ||
          !(options.milliseconds > 0 && options.milliseconds <= 60000))
        Fail("invalid milliseconds");
    } else
      Fail("unknown option " + option);
  }
  if (options.files.empty()) {
    for (const char* name :
         {"book", "github_events", "lottie", "twitter", "twitterescaped",
          "citm_catalog", "canada", "gsoc-2018", "poet"})
      options.files.push_back(options.data_dir + "/" + name + ".json");
  }
  return options;
}
}  // namespace

int main(int argc, char** argv) {
  const Options options = ReadOptions(argc, argv);
  std::cout << "case,input_bytes,iterations,ns_per_op,checksum,output_hash\n";
  for (const auto& filename : options.files) {
    std::ifstream stream(filename, std::ios::binary);
    if (!stream) Fail("cannot open " + filename);
    const std::string input((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
    if (input.empty()) Fail("empty input " + filename);
    const std::string name = filename.substr(filename.find_last_of('/') + 1);
    Document document;
    WriteBuffer buffer;
    document.Parse(input.data(), input.size());
    CheckParse(document);
    Serialize(document, buffer);
    const uint64_t output_hash = Hash(buffer.ToString(), buffer.Size());
    std::string selected;
    const JsonPointer path = LastLeafPath(document, selected);
    Document demand;
    demand.ParseOnDemand(input.data(), input.size(), path);
    CheckParse(demand);
    Serialize(demand, buffer);
    if (selected != std::string(buffer.ToString(), buffer.Size()))
      Fail("on-demand result differs for " + filename);
    std::cerr << name << ": " << input.size() << " bytes, last-leaf path";
    for (const auto& part : path) {
      if (part.IsNum())
        std::cerr << "/[" << part.GetNum() << ']';
      else
        std::cerr << '/' << part.GetStr();
    }
    std::cerr << ", selected " << selected.size() << " serialized bytes\n";
    Measure(options, "parse/" + name, input.size(), output_hash, [&] {
      document.Parse(input.data(), input.size());
      CheckParse(document);
      return Fingerprint(document);
    });
    Measure(options, "serialize/" + name, input.size(), output_hash,
            [&] { return Serialize(document, buffer); });
    Measure(options, "roundtrip/" + name, input.size(), output_hash, [&] {
      document.Parse(input.data(), input.size());
      CheckParse(document);
      return Serialize(document, buffer);
    });
    Measure(options, "ondemand/" + name, input.size(),
            Hash(selected.data(), selected.size()), [&] {
              demand.ParseOnDemand(input.data(), input.size(), path);
              CheckParse(demand);
              return Fingerprint(demand);
            });
  }
}
