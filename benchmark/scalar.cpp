/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

// Dependency-free scalar benchmark; compile from the repository root.
// clang++ -std=c++17 -O3 -DNDEBUG -Iinclude -I. benchmark/scalar.cpp -o /tmp/scalar
// /tmp/scalar --milliseconds 200 [--filter ascii]
// /tmp/scalar --iterations 10000
// RISC-V (LLVM toolchain 1.0.0.102): add -mcpu=jupiter, which does not enable RVV.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "sonic/sonic.h"

namespace {

using Clock = std::chrono::steady_clock;
using sonic_json::Document;
using sonic_json::JsonPointer;
using sonic_json::WriteBuffer;

struct Options {
  uint64_t iterations = 0;
  double milliseconds = 200;
  std::string filter;
};

struct Input {
  const char* name;
  std::string json;
};

std::string Repeat(const std::string& s, size_t count) {
  std::string result;
  result.reserve(s.size() * count);
  for (size_t i = 0; i < count; ++i) result += s;
  return result;
}

std::vector<Input> Inputs() {
  std::vector<Input> inputs = {
      {"small", R"({"id":123456789,"name":"scalar parser","active":true,)"
                R"("items":[1,2,3],"meta":{"region":"cn","ratio":1.25}})"},
      {"ascii", "{\"text\":\"" +
                    Repeat("abcdefghijklmnopqrstuvwxyz012345", 128) + "\"}"},
      {"utf8", "{\"text\":\"" +
                   Repeat("\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x8c\x8d", 410) +
                   "\"}"},
      {"escaped",
       "{\"text\":\"" +
           Repeat(R"(a\nline\twith\"quotes\"\\and\u4f60\u597d)", 128) + "\"}"},
  };
  const std::string space = Repeat(" \t\r\n", 32);
  std::string whitespace = "{" + space + "\"items\"" + space + ":[";
  std::string integers = "[";
  std::string fractions = "[";
  std::string short_fractions = "[";
  const char* short_values[] = {"0.1", "0.12", "0.123", "0.1234"};
  for (size_t i = 0; i < 256; ++i) {
    if (i != 0) {
      integers += ',';
      fractions += ',';
      short_fractions += ',';
    }
    integers += std::to_string(1234567890123456789ULL + i);
    fractions += "0." + std::to_string(1234567890123456789ULL + i);
    short_fractions += short_values[i % 4];
    if (i < 32) {
      if (i != 0) whitespace += ',';
      whitespace += space + std::to_string(i) + space;
    }
  }
  inputs.push_back({"whitespace", whitespace + "]" + space + "}"});
  inputs.push_back({"integers", integers + "]"});
  inputs.push_back({"fractions", fractions + "]"});
  inputs.push_back({"short_fractions", short_fractions + "]"});
  return inputs;
}

void CheckParse(const Document& doc) {
  if (doc.HasParseError()) {
    std::cerr << "parse failed at " << doc.GetErrorOffset() << ": "
              << sonic_json::ErrorMsg(doc.GetParseError()) << '\n';
    std::exit(1);
  }
}

uint64_t Serialize(const Document& doc, WriteBuffer& buffer) {
  buffer.Clear();
  const auto error = doc.Serialize(buffer);
  if (error != sonic_json::kErrorNone) {
    std::cerr << "serialize failed: " << sonic_json::ErrorMsg(error) << '\n';
    std::exit(1);
  }
  const auto size = buffer.Size();
  const auto* data = buffer.ToString();
  return size + static_cast<unsigned char>(data[0]) +
         static_cast<unsigned char>(data[size / 2]) +
         static_cast<unsigned char>(data[size - 1]);
}

template <typename Operation>
void Measure(const Options& options, const std::string& name, size_t bytes,
             Operation operation) {
  if (!options.filter.empty() &&
      name.find(options.filter) == std::string::npos) {
    return;
  }
  uint64_t checksum = 0;
  for (size_t i = 0; i < 256; ++i) checksum += operation();

  uint64_t iterations = options.iterations;
  if (iterations == 0) {
    uint64_t calibration = 256;
    double elapsed = 0;
    do {
      const auto start = Clock::now();
      for (uint64_t i = 0; i < calibration; ++i) checksum += operation();
      elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start)
                    .count();
      if (elapsed >= options.milliseconds / 10) break;
      calibration *= 2;
    } while (calibration < (uint64_t{1} << 32));
    iterations = static_cast<uint64_t>(calibration * options.milliseconds /
                                       (elapsed > 0 ? elapsed : 0.001));
    if (iterations == 0) iterations = 1;
  }

  const auto start = Clock::now();
  for (uint64_t i = 0; i < iterations; ++i) checksum += operation();
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  std::cout << name << ',' << bytes << ',' << iterations << ',' << std::fixed
            << std::setprecision(2) << elapsed / iterations << ',' << checksum
            << '\n';
}

bool ReadOptions(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (i + 1 == argc) return false;
    const char* value = argv[++i];
    char* end = nullptr;
    if (option == "--iterations") {
      errno = 0;
      options.iterations = std::strtoull(value, &end, 10);
      if (*value == '-' || *value == '\0' || *end != '\0' ||
          errno == ERANGE || options.iterations == 0) {
        return false;
      }
    } else if (option == "--milliseconds") {
      options.milliseconds = std::strtod(value, &end);
      if (*value == '\0' || *end != '\0' ||
          !(options.milliseconds > 0 && options.milliseconds <= 60000)) {
        return false;
      }
    } else if (option == "--filter") {
      options.filter = value;
    } else {
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ReadOptions(argc, argv, options)) {
    std::cerr << "usage: " << argv[0]
              << " [--iterations N] [--milliseconds MS] [--filter NAME]\n";
    return 1;
  }
  std::cout << "case,input_bytes,iterations,ns_per_op,checksum\n";
  for (const auto& input : Inputs()) {
    Document doc;
    WriteBuffer buffer;
    doc.Parse(input.json.data(), input.json.size());
    CheckParse(doc);
    Measure(options, std::string("parse/") + input.name, input.json.size(),
            [&] {
              doc.Parse(input.json.data(), input.json.size());
              CheckParse(doc);
              return doc.Size();
            });
    Measure(options, std::string("serialize/") + input.name, input.json.size(),
            [&] { return Serialize(doc, buffer); });
    Measure(options, std::string("roundtrip/") + input.name, input.json.size(),
            [&] {
              doc.Parse(input.json.data(), input.json.size());
              CheckParse(doc);
              return Serialize(doc, buffer);
            });
  }

  const std::string skipped = Repeat("abcdefghijklmnopqrstuvwx", 512);
  const std::vector<Input> on_demand = {
      {"string", "{\"skip\":\"" + skipped + "\",\"target\":42}"},
      {"container", "{\"skip\":[" +
                        Repeat("{\"s\":\"" + skipped.substr(0, 192) +
                                   R"(\n\"x\"","nested":[true,null,1.25]},)",
                               63) +
                        "{}],\"target\":42}"},
  };
  const JsonPointer path{"target"};
  for (const auto& input : on_demand) {
    Document doc;
    Measure(options, std::string("ondemand/") + input.name, input.json.size(),
            [&] {
              doc.ParseOnDemand(input.json.data(), input.json.size(), path);
              CheckParse(doc);
              if (!doc.IsUint64() || doc.GetUint64() != 42) std::exit(1);
              return doc.GetUint64();
            });
  }
  return 0;
}
