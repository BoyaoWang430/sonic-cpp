/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

// Small-document public Parse benchmark; compile from the repository root.
// clang-format off
// clang++ -std=c++17 -O3 -DNDEBUG -Iinclude -I. benchmark/small.cpp -o /tmp/small
// /tmp/small --milliseconds 150 --filter fraction
// /tmp/small --iterations 100
// clang-format on
// RISC-V (LLVM toolchain 1.0.0.102): add -mcpu=jupiter.
// Serialization and full-output hashing happen outside the measured Parse loop.

#include <algorithm>
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
using sonic_json::WriteBuffer;
struct Options {
  uint64_t iterations = 0;
  double milliseconds = 150;
  std::string filter;
};
struct Input {
  std::string name;
  std::string json;
};
[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}
std::vector<Input> Inputs() {
  std::vector<Input> inputs = {
      {"empty_object", "{}"},
      {"empty_array", "[]"},
      {"no_numbers",
       R"({"ok":true,"off":false,"missing":null,"name":"alice"})"},
      {"integer_1", R"({"n":7})"},
      {"integer_4", R"({"n":1234})"},
      {"integer_8", R"({"n":12345678})"},
      {"integer_19", R"({"n":1234567890123456789})"},
      {"integer_20", R"({"n":18446744073709551615})"},
      {"integer_array",
       "[0,1,-1,7,42,-99,1234,32767,65536,12345678,4294967295,"
       "-2147483648,1234567890123456789,18446744073709551615,"
       "-9223372036854775808,0]"},
      {"fraction_1", R"({"n":0.1})"},
      {"fraction_4", R"({"n":-12.3456})"},
      {"fraction_8", R"({"n":0.12345678})"},
      {"fraction_16", R"({"n":0.1234567890123456})"},
  };
  std::string keys = "{";
  std::string strings = "{";
  for (size_t length : {1, 7, 8, 15, 16, 31, 32}) {
    if (keys.size() > 1) keys += ',';
    keys += "\"" + std::string(length, 'k') + "\":true";
    if (strings.size() > 1) strings += ',';
    strings += "\"s" + std::to_string(length) + "\":\"" +
               std::string(length, 'a') + "\"";
  }
  inputs.push_back({"key_lengths", keys + '}'});
  inputs.push_back({"string_lengths", strings + R"(,"empty":""})"});
  inputs.push_back(
      {"escaped_utf8", R"({"line":"a\nline\tend","quote":"\"x\"",)"
                       R"("path":"a\\b","text":"\u4f60\u597d )" +
                           std::string("\xe4\xb8\x96\xe7\x95\x8c") + R"("})"});
  inputs.push_back({"nested_integers",
                    R"({"meta":{"active":true,"ids":[1,2,3]},)"
                    R"("rows":[{"id":10},{"id":11}],"more":{"a":{"b":42}}})"});
  inputs.push_back({"nested_one_fraction",
                    R"({"outer":[{"name":"x","value":{"ratio":0.125}}],)"
                    R"("empty":[],"ready":true})"});
  inputs.push_back({"whitespace_one_fraction",
                    "{\n  \"name\": \"sample\",\n  \"value\": 0.25,\n"
                    "  \"flags\": [ true, false, null ]\n}"});
  inputs.push_back({"short_log_record",
                    R"({"ts":1699999999999,"level":"info","service":"api",)"
                    R"("request":"a1b2c3","status":200,"elapsed":0.75,)"
                    R"("cached":false,"tags":["edge","v2"]})"});
  return inputs;
}
void CheckParse(const Document& document, const std::string& name) {
  if (document.HasParseError())
    Fail(name + " parse failure at " +
         std::to_string(document.GetErrorOffset()) + ": " +
         sonic_json::ErrorMsg(document.GetParseError()));
}
uint64_t OutputHash(const Document& document) {
  WriteBuffer buffer;
  if (document.Serialize(buffer) != sonic_json::kErrorNone)
    Fail("serialization failure while validating parsed result");
  uint64_t hash = 14695981039346656037ULL;
  const char* data = buffer.ToString();
  for (size_t i = 0; i < buffer.Size(); ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}
void Measure(const Options& options, const Input& input) {
  Document document;
  document.Parse(input.json.data(), input.json.size());
  CheckParse(document, input.name);
  if (!document.IsContainer())
    Fail("benchmark fixture must be an object or array");
  const uint64_t output_hash = OutputHash(document);
  uint64_t checksum = 0;
  auto parse = [&] {
    document.Parse(input.json.data(), input.json.size());
    CheckParse(document, input.name);
    return document.Size() + 1;
  };
  for (size_t i = 0; i < 64; ++i) checksum += parse();
  uint64_t iterations = options.iterations;
  if (!iterations) {
    uint64_t calibration = 1;
    double elapsed;
    do {
      const auto start = Clock::now();
      for (uint64_t i = 0; i < calibration; ++i) checksum += parse();
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
  for (uint64_t i = 0; i < iterations; ++i) checksum += parse();
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  if (OutputHash(document) != output_hash)
    Fail(input.name + " result changed between parses");
  std::cout << "parse/" << input.name << ',' << input.json.size() << ','
            << iterations << ',' << std::fixed << std::setprecision(2)
            << elapsed / iterations << ',' << checksum << ',' << output_hash
            << '\n';
}
Options ReadOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (++i == argc) Fail("missing value for " + option);
    const char* value = argv[i];
    char* end = nullptr;
    if (option == "--iterations") {
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
    } else if (option == "--filter")
      options.filter = value;
    else
      Fail("unknown option " + option);
  }
  return options;
}
}  // namespace
int main(int argc, char** argv) {
  const Options options = ReadOptions(argc, argv);
  std::cout << "case,input_bytes,iterations,ns_per_op,checksum,output_hash\n";
  for (const Input& input : Inputs()) {
    if (!options.filter.empty() &&
        ("parse/" + input.name).find(options.filter) == std::string::npos)
      continue;
    Measure(options, input);
  }
}
