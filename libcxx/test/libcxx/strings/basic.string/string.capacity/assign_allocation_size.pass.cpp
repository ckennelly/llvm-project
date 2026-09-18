//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// <string>

// Assigning a value to a string replaces the whole value of the string instead of extending it, so the buffer it
// allocates is sized for the value assigned instead of following the amortized growth factor, which would leave the
// string over-allocated.

// The test compares capacities the built library computes against capacities the headers compute, and the allocation
// size libc++ rounds a capacity up to is derived from __STDCPP_DEFAULT_NEW_ALIGNMENT__. The two only agree if this
// translation unit sees the same value for the macro as the built library did, so enable aligned-new for GCC, which
// otherwise doesn't define it at all.
// ADDITIONAL_COMPILE_FLAGS(gcc): -faligned-new

// Some of the assignments tested here are instantiated in the dylib, so we need to use an up-to-date one
// XFAIL: using-built-library-before-llvm-24

#include <cassert>
#include <cstddef>
#include <string>

#include "test_macros.h"

#if TEST_STD_VER >= 17
#  include <string_view>
#endif

// Large enough to bracket the small string optimization, the allocation size rounding and the amortized growth factor.
const std::size_t max_size = 512;

template <class CharT>
void test() {
  typedef std::basic_string<CharT> Str;
  const CharT value_char = static_cast<CharT>('a');
  const CharT other_char = static_cast<CharT>('b');

  for (std::size_t size = 0; size <= max_size; ++size) {
    const Str value(size, value_char);
    // The capacity of a string constructed to hold the value, which is the smallest capacity the implementation
    // allocates for it.
    const std::size_t capacity = value.capacity();

    { // operator=(const basic_string&)
      Str str;
      str = value;
      assert(str == value);
      assert(str.capacity() == capacity);
    }
    { // operator=(const value_type*)
      Str str;
      str = value.c_str();
      assert(str == value);
      assert(str.capacity() == capacity);
    }
    { // assign(const value_type*, size_type)
      Str str;
      str.assign(value.data(), value.size());
      assert(str == value);
      assert(str.capacity() == capacity);
    }
    { // assign(size_type, value_type)
      Str str;
      str.assign(value.size(), value_char);
      assert(str == value);
      assert(str.capacity() == capacity);
    }
    { // assign(InputIterator, InputIterator)
      Str str;
      str.assign(value.begin(), value.end());
      assert(str == value);
      assert(str.capacity() == capacity);
    }
    { // assign(const basic_string&, size_type, size_type)
      Str str;
      str.assign(value, 0, value.size());
      assert(str == value);
      assert(str.capacity() == capacity);
    }
#if TEST_STD_VER >= 17
    { // operator=(basic_string_view)
      Str str;
      str = std::basic_string_view<CharT>(value);
      assert(str == value);
      assert(str.capacity() == capacity);
    }
#endif
    { // a buffer too small for the value assigned is replaced by one sized for the value
      Str str(size / 2, other_char);
      if (str.capacity() < size) {
        str = value;
        assert(str == value);
        assert(str.capacity() == capacity);
      }
    }
    { // a buffer large enough for the value assigned is kept, whatever its size
      Str str;
      str.reserve(max_size);
      const std::size_t reserved = str.capacity();
      assert(reserved >= size);

      str = value;
      assert(str == value);
      assert(str.capacity() == reserved);
    }
  }
}

int main(int, char**) {
  test<char>();
#ifndef TEST_HAS_NO_WIDE_CHARACTERS
  test<wchar_t>();
#endif
#if TEST_STD_VER >= 11
  test<char16_t>();
  test<char32_t>();
#endif
#ifndef TEST_HAS_NO_CHAR8_T
  test<char8_t>();
#endif

  return 0;
}
