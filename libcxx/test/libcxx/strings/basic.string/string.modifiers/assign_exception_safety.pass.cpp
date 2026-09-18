//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// <string>

// Assignment allocates the buffer for the value assigned and fills it before releasing the buffer the string holds, so
// an allocation that throws leaves the string unchanged.

// UNSUPPORTED: no-exceptions

#include <cassert>
#include <cstddef>
#include <new>
#include <string>

#include "test_allocator.h"

typedef std::basic_string<char, std::char_traits<char>, test_allocator<char> > Str;

// The size of the value assigned, which has to be too large for the buffer the fixture holds so that the assignment
// allocates.
const std::size_t assigned_size = 1024;

// A string holding a heap allocation, whose allocator throws for the next allocation.
struct Fixture {
  test_allocator_statistics stats;
  Str str;
  std::size_t size;
  std::size_t capacity;
  const char* data;

  Fixture() : str(64, 'a', test_allocator<char>(&stats)) {
    size     = str.size();
    capacity = str.capacity();
    data     = str.data();
    assert(capacity < assigned_size);

    stats.throw_after = 0;
  }

  // The string still holds the buffer it was constructed with, with its value in it.
  void check() {
    assert(stats.alloc_count == 1); // the buffer wasn't released
    assert(str.size() == size);
    assert(str.capacity() == capacity);
    assert(str.data() == data);
    for (std::size_t i = 0; i != size; ++i)
      assert(str[i] == 'a');
  }
};

void assign_string(Str& str, const Str& value) { str = value; }
void assign_pointer(Str& str, const Str& value) { str = value.c_str(); }
void assign_pointer_size(Str& str, const Str& value) { str.assign(value.data(), value.size()); }
void assign_fill(Str& str, const Str& value) { str.assign(value.size(), 'b'); }
void assign_iterators(Str& str, const Str& value) { str.assign(value.begin(), value.end()); }
void assign_substring(Str& str, const Str& value) { str.assign(value, 0, value.size()); }

int main(int, char**) {
  // The value uses an allocator without statistics attached, so allocating for it never throws.
  const Str value(assigned_size, 'b');

  void (*const assignments[])(Str&, const Str&) = {
      assign_string, assign_pointer, assign_pointer_size, assign_fill, assign_iterators, assign_substring};

  for (std::size_t i = 0; i != sizeof(assignments) / sizeof(*assignments); ++i) {
    Fixture fixture;
    try {
      assignments[i](fixture.str, value);
      assert(false); // the assignment has to reallocate, and the allocation throws
    } catch (const std::bad_alloc&) {
    }
    fixture.check();
  }

  return 0;
}
