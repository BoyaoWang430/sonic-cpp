/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

// Dependency-free numeric benchmark; compile and run from the repository root.
// clang-format off
// clang++ -std=c++17 -O3 -DNDEBUG -Iinclude -I. benchmark/numbers.cpp \
//   -o /tmp/numbers
// /tmp/numbers --milliseconds 150 --filter fraction
// /tmp/numbers --iterations 1000
// clang-format on
// RISC-V (toolchain 1.0.0.102): add -mcpu=jupiter, which does not enable RVV.
// Full serialized-output hashes are computed outside the timed loops.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "sonic/sonic.h"

namespace {
using Clock = std::chrono::steady_clock;
using sonic_json::Document;
using sonic_json::Node;
using sonic_json::WriteBuffer;

struct Options {
  uint64_t iterations = 0;
  double milliseconds = 150;
  std::string filter;
};
enum class Mode { Parse, Raw, Overflow, Serialize };
struct Input {
  std::string name;
  std::string json;
  Mode mode = Mode::Parse;
};

[[noreturn]] void Fail(const std::string& reason) {
  std::cerr << reason << '\n';
  std::exit(1);
}
std::string Digits(size_t count) {
  std::string result;
  for (size_t i = 0; i < count; ++i) result += "1234567890"[i % 10];
  return result;
}
std::string Array(const std::vector<std::string>& values) {
  std::string result = "[";
  for (size_t i = 0; i < 64; ++i) {
    if (i) result += ',';
    result += values[i % values.size()];
  }
  return result + ']';
}
std::vector<Input> Inputs() {
  std::vector<Input> inputs;
  for (size_t length : {1, 4, 8, 19, 20, 21, 64, 256}) {
    std::vector<std::string> values;
    for (size_t i = 0; i < 8; ++i) {
      std::string value =
          length == 20 ? std::to_string(UINT64_MAX - i) : Digits(length);
      if (length != 20) value.back() = static_cast<char>('1' + i);
      values.push_back(value);
    }
    inputs.push_back(
        {"parse/integer_" + std::to_string(length), Array(values)});
  }
  for (size_t length : {1, 4, 8, 16, 32}) {
    const auto digits = Digits(length);
    inputs.push_back({"parse/fraction_" + std::to_string(length),
                      Array({"0." + digits, "-0." + digits})});
  }
  inputs.push_back({"parse/leading_zeros_256",
                    Array({"0." + std::string(256, '0') + "12345678"})});
  inputs.push_back(
      {"parse/discarded_tail_1024",
       Array({"0.12345678901234567" + std::string(1024, '0') + "1"})});
  inputs.push_back({"parse_raw/integer_256",
                    Array({Digits(256), "-" + Digits(256)}), Mode::Raw});
  inputs.push_back(
      {"parse_overflow/mixed",
       Array({"18446744073709551616", "-9223372036854775809", Digits(256),
              "1e309", "-1e309", "1.25", "42", "-42"}),
       Mode::Overflow});
  inputs.push_back({"serialize/ordinary",
                    Array({"0.0", "-0.0", "0.1", "-1.25", "3.141592653589793",
                           "1234.5678", "0.00012345", "999999.5"}),
                    Mode::Serialize});
  inputs.push_back({"serialize/scientific",
                    Array({"1e-100", "-1.23456789e-50", "1e-9", "1.25e21",
                           "1e100", "-1.2345678901234567e200"}),
                    Mode::Serialize});
  inputs.push_back(
      {"serialize/extremes",
       Array({"1.7976931348623157e308", "-1.7976931348623157e308",
              "2.2250738585072014e-308", "2.225073858507201e-308",
              "4.9406564584124654e-324", "-4.9406564584124654e-324",
              "2.220446049250313e-16", "1.0000000000000002"}),
       Mode::Serialize});
  return inputs;
}
void CheckParse(const Document& doc) {
  if (doc.HasParseError()) {
    Fail("parse failure at " + std::to_string(doc.GetErrorOffset()) + ": " +
         sonic_json::ErrorMsg(doc.GetParseError()));
  }
}
uint64_t Hash(const char* data, size_t size) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < size; ++i) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}
uint64_t Fingerprint(const Node& node) {
  if (node.IsRaw() || node.IsStringNumber()) return node.Size();
  if (node.IsUint64()) return node.GetUint64();
  if (node.IsInt64()) return static_cast<uint64_t>(node.GetInt64());
  const double value = node.GetDouble();
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}
uint64_t Serialize(const Document& doc, WriteBuffer& buffer) {
  buffer.Clear();
  const auto error = doc.Serialize(buffer);
  if (error != sonic_json::kErrorNone)
    Fail(std::string("serialize failure: ") + sonic_json::ErrorMsg(error));
  const size_t size = buffer.Size();
  const char* data = buffer.ToString();
  return size + static_cast<unsigned char>(data[0]) +
         static_cast<unsigned char>(data[size / 2]) +
         static_cast<unsigned char>(data[size - 1]);
}
template <typename Operation>
void Measure(const Options& options, const Input& input, uint64_t output_hash,
             Operation operation) {
  uint64_t checksum = 0;
  for (size_t i = 0; i < 16; ++i) checksum += operation();
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
  std::cout << input.name << ',' << input.json.size() << ',' << iterations
            << ',' << std::fixed << std::setprecision(2) << elapsed / iterations
            << ',' << checksum << ',' << output_hash << '\n';
}
template <ParseFlags flags>
void RunParse(const Options& options, const Input& input) {
  Document doc;
  doc.Parse<flags>(input.json.data(), input.json.size());
  CheckParse(doc);
  WriteBuffer buffer;
  Serialize(doc, buffer);
  const uint64_t output_hash = Hash(buffer.ToString(), buffer.Size());
  Measure(options, input, output_hash, [&] {
    doc.Parse<flags>(input.json.data(), input.json.size());
    CheckParse(doc);
    return doc.Size() + Fingerprint(doc[0]) + Fingerprint(doc[doc.Size() - 1]);
  });
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
    } else if (option == "--filter") {
      options.filter = value;
    } else {
      Fail("unknown option " + option);
    }
  }
  return options;
}
}  // namespace

int main(int argc, char** argv) {
  const auto options = ReadOptions(argc, argv);
  std::cout << "case,input_bytes,iterations,ns_per_op,checksum,output_hash\n";
  for (const auto& input : Inputs()) {
    if (!options.filter.empty() &&
        input.name.find(options.filter) == std::string::npos)
      continue;
    if (input.mode == Mode::Raw) {
      RunParse<ParseFlags::kParseIntegerAsRaw>(options, input);
    } else if (input.mode == Mode::Overflow) {
      RunParse<ParseFlags::kParseOverflowNumAsNumStr>(options, input);
    } else if (input.mode == Mode::Parse) {
      RunParse<ParseFlags::kParseDefault>(options, input);
    } else {
      Document doc;
      doc.Parse(input.json.data(), input.json.size());
      CheckParse(doc);
      WriteBuffer buffer;
      Serialize(doc, buffer);
      const uint64_t output_hash = Hash(buffer.ToString(), buffer.Size());
      Measure(options, input, output_hash,
              [&] { return Serialize(doc, buffer); });
    }
  }
}
