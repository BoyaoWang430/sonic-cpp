/*
 * Copyright 2026 ByteDance Inc.
 * SPDX-License-Identifier: Apache-2.0
 */

// RISC-V memory and public API benchmark; compile from the repository root.
// clang-format off
// clang++ -std=c++17 -O3 -DNDEBUG -mcpu=jupiter -Iinclude -I. \
//   benchmark/memory.cpp -o /tmp/memory
// /tmp/memory --milliseconds 150 --filter lookup
// /tmp/memory --iterations 1000
// clang-format on
// LLVM toolchain 1.0.0.102 accepts -mcpu=jupiter without enabling RVV.
// Checksums consume operation results; they are not hashes of complete output.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "sonic/internal/arch/riscv/base.h"
#include "sonic/sonic.h"

namespace {

namespace rv = sonic_json::internal::riscv;
using Clock = std::chrono::steady_clock;

struct Options {
  uint64_t iterations = 0;
  double milliseconds = 150;
  std::string filter;
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(1);
}

Options ReadOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (++i == argc) Fail("missing value for " + option);
    const char* value = argv[i];
    char* end = nullptr;
    if (option == "--iterations") {
      for (const char* p = value; *p; ++p) {
        if (*p < '0' || *p > '9') Fail("invalid iterations");
      }
      errno = 0;
      options.iterations = std::strtoull(value, nullptr, 10);
      if (errno == ERANGE || !options.iterations)
        Fail("invalid iterations");
    } else if (option == "--milliseconds") {
      if ((*value < '0' || *value > '9') && *value != '.')
        Fail("invalid milliseconds");
      errno = 0;
      options.milliseconds = std::strtod(value, &end);
      if (errno == ERANGE || *end ||
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

template <class Operation>
void Measure(const Options& options, const std::string& name, size_t bytes,
             Operation operation) {
  if (!options.filter.empty() && name.find(options.filter) == std::string::npos)
    return;
  uint64_t checksum = 0;
  for (uint64_t i = 0; i < 256; ++i) checksum += operation(i);

  uint64_t iterations = options.iterations;
  if (iterations == 0) {
    uint64_t calibration = 256;
    double elapsed = 0;
    do {
      const auto start = Clock::now();
      for (uint64_t i = 0; i < calibration; ++i) checksum += operation(i);
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
  for (uint64_t i = 0; i < iterations; ++i) checksum += operation(i);
  const double elapsed =
      std::chrono::duration<double, std::nano>(Clock::now() - start).count();
  std::cout << name << ',' << bytes << ',' << iterations << ',' << std::fixed
            << std::setprecision(2) << elapsed / iterations << ',' << checksum
            << '\n';
}

// Prevent whole-benchmark hoisting while keeping the inline helper expansion
// that consumers get inside their key and copy loops.
inline void Escape(const void* p) { asm volatile("" : : "r"(p) : "memory"); }

void Compare(const Options& options) {
  for (size_t length : {1ul, 2ul, 3ul, 4ul, 7ul, 8ul, 12ul, 16ul, 20ul, 24ul,
                        32ul, 33ul, 64ul}) {
    const size_t stride = (length + 31) & ~size_t(15);
    std::vector<uint8_t> a(16 * stride), b(16 * stride);
    for (size_t i = 0; i < a.size(); ++i)
      a[i] = b[i] = static_cast<uint8_t>(37 * i + length);
    for (const char* mode : {"equal", "first", "last"}) {
      b = a;
      for (size_t alignment = 0; alignment < 16; ++alignment) {
        const size_t offset = alignment * stride + alignment;
        if (mode[0] == 'f') b[offset] ^= 1;
        if (mode[0] == 'l') b[offset + length - 1] ^= 1;
      }
      Measure(options, "eq/" + std::to_string(length) + '/' + mode, length,
              [&](uint64_t i) {
                Escape(a.data());
                Escape(b.data());
                const size_t alignment = i & 15;
                const size_t offset = alignment * stride + alignment;
                return uint64_t(rv::InlinedMemcmpEq(a.data() + offset,
                                                    b.data() + offset, length));
              });
    }
  }
}

template <size_t Chunk>
void Copy(const Options& options) {
  for (size_t count : {0ul, 1ul, 2ul, 3ul, 4ul, 8ul, 16ul}) {
    const size_t bytes = count * Chunk;
    std::vector<uint8_t> source(bytes + 16), destination(bytes + 16);
    for (size_t i = 0; i < source.size(); ++i)
      source[i] = static_cast<uint8_t>(37 * i);
    Measure(options,
            "copy/" + std::to_string(Chunk) + '/' + std::to_string(count),
            bytes, [&](uint64_t i) {
              const size_t offset = i & 15;
              Escape(source.data());
              rv::Xmemcpy<Chunk>(destination.data() + offset,
                                 source.data() + offset, count);
              Escape(destination.data());
              return uint64_t(destination[offset]);
            });
  }
}

std::string MakeKey(size_t length, size_t id) {
  std::string key(length, 'k');
  for (size_t j = 0; j < 4 && j < length; ++j) {
    key[length - j - 1] = static_cast<char>('a' + (id & 15));
    id >>= 4;
  }
  return key;
}

void Lookup(const Options& options) {
  using namespace sonic_json;
  for (size_t length : {4ul, 8ul, 16ul, 24ul, 32ul, 48ul}) {
    for (size_t members : {4ul, 16ul, 64ul}) {
      Document document;
      document.SetObject();
      for (size_t i = 0; i < members; ++i) {
        const auto key = MakeKey(length, i);
        const auto added =
            document.AddMember(StringView(key.data(), key.size()),
                               Node(uint64_t(i)), document.GetAllocator());
        if (added == document.MemberEnd())
          Fail("allocation failure while constructing lookup fixture");
      }
      const std::string keys[] = {MakeKey(length, members - 1),
                                  MakeKey(length, members)};
      Measure(
          options,
          "lookup/" + std::to_string(length) + '/' + std::to_string(members),
          length * members, [&](uint64_t i) {
            const auto& key = keys[i & 1];
            Escape(key.data());
            const auto it = document.FindMember(key.data(), key.size());
            return it == document.MemberEnd() ? uint64_t(0)
                                              : it->value.GetUint64() + 1;
          });
    }
  }
}

void Parse(const Options& options) {
  using namespace sonic_json;
  for (size_t count : {0ul, 1ul, 2ul, 3ul, 4ul, 8ul, 32ul}) {
    std::string array = "[", object = "{";
    for (size_t i = 0; i < count; ++i) {
      if (i) {
        array += ',';
        object += ',';
      }
      array += std::to_string(i);
      object += "\"key" + std::to_string(i) + "\":" + std::to_string(i);
    }
    array += ']';
    object += '}';
    for (const auto& item :
         {std::make_pair("array", array), std::make_pair("object", object)}) {
      Measure(
          options,
          std::string("parse/") + item.first + '/' + std::to_string(count),
          item.second.size(), [&](uint64_t) {
            Document document;
            document.Parse(item.second);
            if (document.HasParseError())
              Fail("parse failure in " + std::string(item.first) + " fixture");
            return uint64_t(document.Size());
          });
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = ReadOptions(argc, argv);
  std::cout << "case,input_bytes,iterations,ns_per_op,checksum\n";
  Compare(options);
  Copy<16>(options);
  Copy<32>(options);
  Lookup(options);
  Parse(options);
}
