// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/common.h"

#include <algorithm>

#include "tcmalloc/experiment.h"
#include "tcmalloc/internal/environment.h"
#include "tcmalloc/internal/optimization.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/runtime_size_classes.h"
#include "tcmalloc/sampler.h"

GOOGLE_MALLOC_SECTION_BEGIN
namespace tcmalloc {
namespace tcmalloc_internal {

absl::string_view MemoryTagToLabel(MemoryTag tag) {
  switch (tag) {
    case MemoryTag::kNormal:
      return "NORMAL";
    case MemoryTag::kNormalP1:
      return "NORMAL_P1";
    case MemoryTag::kSampled:
      return "SAMPLED";
    case MemoryTag::kCold:
      return "COLD";
    default:
      ASSUME(false);
  }
}

// Load sizes classes from environment variable if present
// and valid, then returns True. If not found or valid, returns
// False.
bool SizeMap::MaybeRunTimeSizeClasses() {
  SizeClassInfo parsed[kNumClasses];
  int num_classes = MaybeSizeClassesFromEnv(kMaxSize, kNumClasses, parsed);
  if (!ValidSizeClasses(num_classes, parsed)) {
    return false;
  }

  if (num_classes != kSizeClassesCount) {
    // TODO(b/122839049) - Add tests for num_classes < kSizeClassesCount before
    // allowing that case.
    Log(kLog, __FILE__, __LINE__, "Can't change the number of size classes",
        num_classes, kSizeClassesCount);
    return false;
  }

  SetSizeClasses(num_classes, parsed);
  Log(kLog, __FILE__, __LINE__, "Loaded valid Runtime Size classes");
  return true;
}

void SizeMap::SetSizeClasses(int num_classes, const SizeClassInfo* parsed,
                             bool reduce_below64_classes) {
  CHECK_CONDITION(ValidSizeClasses(num_classes, parsed));

  class_to_size_[0] = 0;
  class_to_pages_[0] = 0;
  num_objects_to_move_[0] = 0;

  int curr = 1;
  for (int c = 1; c < num_classes; c++) {
    if (reduce_below64_classes &&
        !size_map_internal::IsReducedBelow64SizeClass(parsed[c].size))
      continue;
    class_to_size_[curr] = parsed[c].size;
    class_to_pages_[curr] = parsed[c].pages;
    num_objects_to_move_[curr] = parsed[c].num_to_move;
    ++curr;
  }

  // Fill any unspecified size classes with 0.
  for (int x = curr; x < kNumBaseClasses; x++) {
    class_to_size_[x] = 0;
    class_to_pages_[x] = 0;
    num_objects_to_move_[x] = 0;
  }

  // Copy selected size classes into the upper registers.
  for (int i = 1; i < (kNumClasses / kNumBaseClasses); i++) {
    std::copy(&class_to_size_[0], &class_to_size_[kNumBaseClasses],
              &class_to_size_[kNumBaseClasses * i]);
    std::copy(&class_to_pages_[0], &class_to_pages_[kNumBaseClasses],
              &class_to_pages_[kNumBaseClasses * i]);
    std::copy(&num_objects_to_move_[0], &num_objects_to_move_[kNumBaseClasses],
              &num_objects_to_move_[kNumBaseClasses * i]);
  }
}

// Return true if all size classes meet the requirements for alignment
// ordering and min and max values.
bool SizeMap::ValidSizeClasses(int num_classes, const SizeClassInfo* parsed) {
  if (num_classes <= 0) {
    return false;
  }
  if (kHasExpandedClasses && num_classes > kNumBaseClasses) {
    num_classes = kNumBaseClasses;
  }

  for (int c = 1; c < num_classes; c++) {
    size_t class_size = parsed[c].size;
    size_t pages = parsed[c].pages;
    size_t num_objects_to_move = parsed[c].num_to_move;
    // Each size class must be larger than the previous size class.
    if (class_size <= parsed[c - 1].size) {
      Log(kLog, __FILE__, __LINE__, "Non-increasing size class", c,
          parsed[c - 1].size, class_size);
      return false;
    }
    if (class_size > kMaxSize) {
      Log(kLog, __FILE__, __LINE__, "size class too big", c, class_size,
          kMaxSize);
      return false;
    }
    // Check required alignment
    size_t alignment = 128;
    if (class_size <= kMultiPageSize) {
      alignment = kAlignment;
    } else if (class_size <= SizeMap::kMaxSmallSize) {
      alignment = kMultiPageAlignment;
    }
    if ((class_size & (alignment - 1)) != 0) {
      Log(kLog, __FILE__, __LINE__, "Not aligned properly", c, class_size,
          alignment);
      return false;
    }
    if (class_size <= kMultiPageSize && pages != 1) {
      Log(kLog, __FILE__, __LINE__, "Multiple pages not allowed", class_size,
          pages, kMultiPageSize);
      return false;
    }
    if (pages >= 256) {
      Log(kLog, __FILE__, __LINE__, "pages limited to 255", pages);
      return false;
    }
    if (num_objects_to_move > kMaxObjectsToMove) {
      Log(kLog, __FILE__, __LINE__, "num objects to move too large",
          num_objects_to_move, kMaxObjectsToMove);
      return false;
    }
  }
  // Last size class must be kMaxSize.  This is not strictly
  // class_to_size_[kNumBaseClasses - 1] because several size class
  // configurations populate fewer distinct size classes and fill the tail of
  // the array with zeroes.
  if (parsed[num_classes - 1].size != kMaxSize) {
    Log(kLog, __FILE__, __LINE__, "last class doesn't cover kMaxSize",
        num_classes - 1, parsed[num_classes - 1].size, kMaxSize);
    return false;
  }
  return true;
}

// Initialize the mapping arrays
void SizeMap::Init() {
  // Do some sanity checking on add_amount[]/shift_amount[]/class_array[]
  if (ClassIndex(0) != 0) {
    Crash(kCrash, __FILE__, __LINE__, "Invalid class index for size 0",
          ClassIndex(0));
  }
  if (ClassIndex(kMaxSize) >= sizeof(class_array_)) {
    Crash(kCrash, __FILE__, __LINE__, "Invalid class index for kMaxSize",
          ClassIndex(kMaxSize));
  }

  static_assert(kAlignment <= 16, "kAlignment is too large");

  if (IsExperimentActive(Experiment::TEST_ONLY_TCMALLOC_POW2_SIZECLASS)) {
    SetSizeClasses(kExperimentalPow2SizeClassesCount,
                   kExperimentalPow2SizeClasses);
  } else if (IsExperimentActive(Experiment::TCMALLOC_POW2_BELOW_64) ||
             IsExperimentActive(
                 Experiment::TEST_ONLY_TCMALLOC_POW2_BELOW64_SIZECLASS)) {
    SetSizeClasses(kExperimentalPow2Below64SizeClassesCount,
                   kExperimentalPow2Below64SizeClasses);
  } else if (IsExperimentActive(Experiment::TCMALLOC_CFL_AWARE_SIZE_CLASS) ||
             IsExperimentActive(
                 Experiment::TEST_ONLY_TCMALLOC_CFL_AWARE_SIZECLASS)) {
    SetSizeClasses(kExperimentalCFLAwareSizeClassesCount,
                   kExperimentalCFLAwareSizeClasses);
  } else if (IsExperimentActive(Experiment::TCMALLOC_REDUCED_BELOW_64) ||
             IsExperimentActive(
                 Experiment::TEST_ONLY_TCMALLOC_REDUCED_BELOW64_SIZECLASS)) {
    SetSizeClasses(kSizeClassesCount, kSizeClasses,
                   /*reduce_below64_classes=*/true);
  } else {
    SetSizeClasses(kSizeClassesCount, kSizeClasses);
  }
  MaybeRunTimeSizeClasses();

  int next_size = 0;
  for (int c = 1; c < kNumClasses; c++) {
    const int max_size_in_class = class_to_size_[c];

    for (int s = next_size; s <= max_size_in_class; s += kAlignment) {
      class_array_[ClassIndex(s)] = c;
    }
    next_size = max_size_in_class + kAlignment;
    if (next_size > kMaxSize) {
      break;
    }
  }

  if (!kHasExpandedClasses) {
    return;
  }

  memset(cold_sizes_, 0, sizeof(cold_sizes_));
  cold_sizes_count_ = 0;

  if (!ColdFeatureActive()) {
    std::copy(&class_array_[0], &class_array_[kClassArraySize],
              &class_array_[kClassArraySize]);
    return;
  }

  // TODO(b/124707070): Systematically identify candidates for cold allocation
  // and include them explicitly in size_classes.cc.
  static constexpr size_t kColdCandidates[] = {
      2048,  4096,  6144,  7168,  8192,   16384,
      20480, 32768, 40960, 65536, 131072, 262144,
  };
  static_assert(ABSL_ARRAYSIZE(kColdCandidates) <= ABSL_ARRAYSIZE(cold_sizes_),
                "kColdCandidates is too large.");

  // Point all lookups in the upper register of class_array_ (allocations
  // seeking cold memory) to the lower size classes.  This gives us an easy
  // fallback for sizes that are too small for moving to cold memory (due to
  // intrusive span metadata).
  std::copy(&class_array_[0], &class_array_[kClassArraySize],
            &class_array_[kClassArraySize]);

  for (size_t max_size_in_class : kColdCandidates) {
    ASSERT(max_size_in_class != 0);

    // Find the size class.  Some of our kColdCandidates may not map to actual
    // size classes in our current configuration.
    bool found = false;
    int c;
    for (c = kExpandedClassesStart; c < kNumClasses; c++) {
      if (class_to_size_[c] == max_size_in_class) {
        found = true;
        break;
      }
    }

    if (!found) {
      continue;
    }

    // Verify the candidate can fit into a single span's kCacheSize, otherwise,
    // we use an intrusive freelist which triggers memory accesses.
    if (Length(class_to_pages_[c]).in_bytes() / max_size_in_class >
        Span::kCacheSize) {
      continue;
    }

    cold_sizes_[cold_sizes_count_] = c;
    cold_sizes_count_++;

    for (int s = next_size; s <= max_size_in_class; s += kAlignment) {
      class_array_[ClassIndex(s) + kClassArraySize] = c;
    }
    next_size = max_size_in_class + kAlignment;
    if (next_size > kMaxSize) {
      break;
    }
  }
}

// This only provides correct answer for TCMalloc-allocated memory,
// and may give a false positive for non-allocated block.
extern "C" bool TCMalloc_Internal_PossiblyCold(const void* ptr) {
  return IsColdMemory(ptr);
}

}  // namespace tcmalloc_internal
}  // namespace tcmalloc
GOOGLE_MALLOC_SECTION_END
