/*
 * Copyright 2022 ByteDance Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VEC_LEN
#error "Define vector length firstly!"
#endif

template <typename T>
sonic_force_inline uint64_t GetStringBits(const uint8_t* data,
                                          uint64_t& prev_instring,
                                          uint64_t& prev_escaped) {
  const T v(data);
  uint64_t escaped = 0;
  uint64_t bs_bits = v.eq('\\');
  if (bs_bits) {
    escaped = common::GetEscaped<64>(prev_escaped, bs_bits);
  } else {
    escaped = prev_escaped;
    prev_escaped = 0;
  }
  uint64_t quote_bits = v.eq('"') & ~escaped;
  uint64_t in_string = PrefixXor(quote_bits) ^ prev_instring;
  prev_instring = uint64_t(static_cast<int64_t>(in_string) >> 63);
  return in_string;
}

// GetNextToken find the next characters in tokens and update the position to
// it.
template <size_t N>
sonic_force_inline uint8_t GetNextToken(const uint8_t* data, size_t& pos,
                                        size_t len, const char (&tokens)[N]) {
  while (pos <= len && len - pos >= VEC_LEN) {
    const uint64_t lo = swar::load_u64(data + pos);
    const uint64_t hi = swar::load_u64(data + pos + 8);
    uint64_t lo_bits = 0, hi_bits = 0;
    for (size_t i = 0; i < N - 1; i++) {
      lo_bits |= swar::eq_byte_msb(lo, static_cast<uint8_t>(tokens[i]));
      hi_bits |= swar::eq_byte_msb(hi, static_cast<uint8_t>(tokens[i]));
    }
    if (lo_bits | hi_bits) {
      pos += lo_bits ? (TrailingZeroes(lo_bits) >> 3)
                     : 8 + (TrailingZeroes(hi_bits) >> 3);
      return data[pos];
    }
    pos += VEC_LEN;
  }
  while (pos < len) {
    for (size_t i = 0; i < N - 1; i++) {
      if (data[pos] == tokens[i]) {
        return tokens[i];
      }
    }
    pos++;
  }
  return '\0';
}

// pos is the after the ending quote
sonic_force_inline int SkipString(const uint8_t* data, size_t& pos,
                                  size_t len) {
  const static int kEscaped = 2;
  const static int kNormal = 1;
  const static int kUnclosed = 0;
  int ret = kNormal;
  while (pos <= len && len - pos >= VEC_LEN) {
    const uint64_t lo = swar::load_u64(data + pos);
    const uint64_t hi = swar::load_u64(data + pos + 8);
    const uint64_t lo_events =
        swar::eq_byte_msb(lo, '\\') | swar::eq_byte_msb(lo, '"');
    const uint64_t hi_events =
        swar::eq_byte_msb(hi, '\\') | swar::eq_byte_msb(hi, '"');
    if ((lo_events | hi_events) == 0) {
      pos += VEC_LEN;
      continue;
    }
    pos += lo_events ? (TrailingZeroes(lo_events) >> 3)
                     : 8 + (TrailingZeroes(hi_events) >> 3);
    if (data[pos] == '"') {
      ++pos;
      return ret;
    }
    ret = kEscaped;
    pos += 2;
    while (pos < len && data[pos] == '\\') pos += 2;
  }
  while (pos < len) {
    if (data[pos] == '\\') {
      if (pos + 1 >= len) {
        return kUnclosed;
      }
      ret = kEscaped;
      pos += 2;
      continue;
    }
    if (data[pos++] == '"') {
      return ret;
    }
  };
  return kUnclosed;
}

// return true if container is closed.
template <typename T>
sonic_force_inline bool skip_container(const uint8_t* data, size_t& pos,
                                       size_t len, uint8_t left,
                                       uint8_t right) {
  uint64_t prev_instring = 0, prev_escaped = 0, instring;
  int rbrace_num = 0, lbrace_num = 0, last_lbrace_num;
  const uint8_t* p;
  while (pos + 64 <= len) {
    p = data + pos;
#define SKIP_LOOP()                                                    \
  {                                                                    \
    instring = GetStringBits<T>(p, prev_instring, prev_escaped);       \
    T v(p);                                                            \
    last_lbrace_num = lbrace_num;                                      \
    uint64_t rbrace = v.eq(right) & ~instring;                         \
    uint64_t lbrace = v.eq(left) & ~instring;                          \
    /* traverse each '}' */                                            \
    while (rbrace > 0) {                                               \
      rbrace_num++;                                                    \
      lbrace_num = last_lbrace_num + CountOnes((rbrace - 1) & lbrace); \
      bool is_closed = lbrace_num < rbrace_num;                        \
      if (is_closed) {                                                 \
        sonic_assert(rbrace_num == lbrace_num + 1);                    \
        pos += TrailingZeroes(rbrace) + 1;                             \
        return true;                                                   \
      }                                                                \
      rbrace &= (rbrace - 1);                                          \
    }                                                                  \
    lbrace_num = last_lbrace_num + CountOnes(lbrace);                  \
  }
    SKIP_LOOP();
    pos += 64;
  }
  uint8_t buf[64] = {0};
  std::memcpy(buf, data + pos, len - pos);
  p = buf;
  SKIP_LOOP();
#undef SKIP_LOOP
  return false;
}

sonic_force_inline uint8_t skip_space_safe(const uint8_t* data, size_t& pos,
                                           size_t len, size_t&, uint64_t&) {
  if (pos >= len) return len == 0 ? 0 : data[len - 1];
  if (!IsSpace(data[pos++])) return data[pos - 1];
  while (len - pos >= 16) {
    uint64_t nonspace = GetNonSpaceBits(data + pos);
    if (nonspace) {
      pos += TrailingZeroes(nonspace);
      return data[pos++];
    }
    pos += 16;
  }
  while (pos < len && IsSpace(data[pos++]));
  // If not found, still return the last space character.
  return data[pos - 1];
}
