// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/heap/heap_stats_collector.h"

#include <cmath>

#include "base/logging.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"

namespace blink {

void ThreadHeapStatsCollector::IncreaseCompactionFreedSize(size_t bytes) {
  DCHECK(is_started_);
  current_.compaction_freed_bytes += bytes;
}

void ThreadHeapStatsCollector::IncreaseCompactionFreedPages(size_t pages) {
  DCHECK(is_started_);
  current_.compaction_freed_pages += pages;
}

void ThreadHeapStatsCollector::IncreaseAllocatedObjectSize(size_t bytes) {
  // The current GC may not have been started. This is ok as recording considers
  // the whole time range between garbage collections.
  pos_delta_allocated_bytes_since_prev_gc_ += bytes;
}

void ThreadHeapStatsCollector::IncreaseAllocatedObjectSizeForTesting(
    size_t bytes) {
  IncreaseAllocatedObjectSize(bytes);
  AllocatedObjectSizeSafepointImpl();
}

void ThreadHeapStatsCollector::DecreaseAllocatedObjectSize(size_t bytes) {
  // See IncreaseAllocatedObjectSize.
  neg_delta_allocated_bytes_since_prev_gc_ += bytes;
}

void ThreadHeapStatsCollector::DecreaseAllocatedObjectSizeForTesting(
    size_t bytes) {
  DecreaseAllocatedObjectSize(bytes);
  AllocatedObjectSizeSafepointImpl();
}

void ThreadHeapStatsCollector::AllocatedObjectSizeSafepoint() {
  if (std::abs(pos_delta_allocated_bytes_since_prev_gc_ -
               neg_delta_allocated_bytes_since_prev_gc_) > kUpdateThreshold) {
    AllocatedObjectSizeSafepointImpl();
  }
}

void ThreadHeapStatsCollector::AllocatedObjectSizeSafepointImpl() {
  allocated_bytes_since_prev_gc_ +=
      static_cast<int64_t>(pos_delta_allocated_bytes_since_prev_gc_) -
      static_cast<int64_t>(neg_delta_allocated_bytes_since_prev_gc_);

  // These observer methods may start or finalize GC. In case they trigger a
  // final GC pause, the delta counters are reset there and the following
  // observer calls are called with '0' updates.
  ForAllObservers([this](ThreadHeapStatsObserver* observer) {
    // Recompute delta here so that a GC finalization is able to clear the
    // delta for other observer calls.
    int64_t delta = pos_delta_allocated_bytes_since_prev_gc_ -
                    neg_delta_allocated_bytes_since_prev_gc_;
    if (delta < 0) {
      observer->DecreaseAllocatedObjectSize(static_cast<size_t>(-delta));
    } else {
      observer->IncreaseAllocatedObjectSize(static_cast<size_t>(delta));
    }
  });
  pos_delta_allocated_bytes_since_prev_gc_ = 0;
  neg_delta_allocated_bytes_since_prev_gc_ = 0;
}

void ThreadHeapStatsCollector::IncreaseAllocatedSpace(size_t bytes) {
  allocated_space_bytes_ += bytes;
  ForAllObservers([bytes](ThreadHeapStatsObserver* observer) {
    observer->IncreaseAllocatedSpace(bytes);
  });
}

void ThreadHeapStatsCollector::DecreaseAllocatedSpace(size_t bytes) {
  allocated_space_bytes_ -= bytes;
  ForAllObservers([bytes](ThreadHeapStatsObserver* observer) {
    observer->DecreaseAllocatedSpace(bytes);
  });
}

void ThreadHeapStatsCollector::IncreaseWrapperCount(size_t count) {
  wrapper_count_ += count;
}

void ThreadHeapStatsCollector::DecreaseWrapperCount(size_t count) {
  wrapper_count_ -= count;
}

void ThreadHeapStatsCollector::IncreaseCollectedWrapperCount(size_t count) {
  collected_wrapper_count_ += count;
}

void ThreadHeapStatsCollector::NotifyMarkingStarted(BlinkGC::GCReason reason) {
  DCHECK(!is_started_);
  DCHECK_EQ(0.0, current_.marking_time_in_ms());
  is_started_ = true;
  current_.reason = reason;
}

void ThreadHeapStatsCollector::NotifyMarkingCompleted(size_t marked_bytes) {
  allocated_bytes_since_prev_gc_ +=
      static_cast<int64_t>(pos_delta_allocated_bytes_since_prev_gc_) -
      static_cast<int64_t>(neg_delta_allocated_bytes_since_prev_gc_);
  current_.marked_bytes = marked_bytes;
  current_.object_size_in_bytes_before_sweeping = object_size_in_bytes();
  current_.allocated_space_in_bytes_before_sweeping = allocated_space_bytes();
  current_.partition_alloc_bytes_before_sweeping =
      WTF::Partitions::TotalSizeOfCommittedPages();
  current_.wrapper_count_before_sweeping = wrapper_count_;
  allocated_bytes_since_prev_gc_ = 0;
  collected_wrapper_count_ = 0;
  pos_delta_allocated_bytes_since_prev_gc_ = 0;
  neg_delta_allocated_bytes_since_prev_gc_ = 0;

  ForAllObservers([marked_bytes](ThreadHeapStatsObserver* observer) {
    observer->ResetAllocatedObjectSize(marked_bytes);
  });
}

void ThreadHeapStatsCollector::NotifySweepingCompleted() {
  is_started_ = false;
  current_.live_object_rate =
      current_.object_size_in_bytes_before_sweeping
          ? static_cast<double>(current().marked_bytes) /
                current_.object_size_in_bytes_before_sweeping
          : 0.0;
  current_.gc_nested_in_v8_ = gc_nested_in_v8_;
  previous_ = std::move(current_);
  // Reset the current state.
  static_assert(!std::is_polymorphic<Event>::value,
                "Event should not be polymorphic");
  memset(&current_, 0, sizeof(current_));
  gc_nested_in_v8_ = base::TimeDelta();
}

void ThreadHeapStatsCollector::UpdateReason(BlinkGC::GCReason reason) {
  current_.reason = reason;
}

size_t ThreadHeapStatsCollector::object_size_in_bytes() const {
  DCHECK_GE(static_cast<int64_t>(previous().marked_bytes) +
                allocated_bytes_since_prev_gc_,
            0);
  return static_cast<size_t>(static_cast<int64_t>(previous().marked_bytes) +
                             allocated_bytes_since_prev_gc_);
}

double ThreadHeapStatsCollector::estimated_marking_time_in_seconds() const {
  // Assume 8ms time for an initial heap. 8 ms is long enough for low-end mobile
  // devices to mark common real-world object graphs.
  constexpr double kInitialMarkingTimeInSeconds = 0.008;

  const double prev_marking_speed =
      previous().marking_time_in_bytes_per_second();
  return prev_marking_speed ? prev_marking_speed * object_size_in_bytes()
                            : kInitialMarkingTimeInSeconds;
}

base::TimeDelta ThreadHeapStatsCollector::estimated_marking_time() const {
  return base::TimeDelta::FromSecondsD(estimated_marking_time_in_seconds());
}

double ThreadHeapStatsCollector::Event::marking_time_in_ms() const {
  return (scope_data[kIncrementalMarkingStartMarking] +
          scope_data[kIncrementalMarkingStep] +
          scope_data[kIncrementalMarkingFinalizeMarking] +
          scope_data[kAtomicPhaseMarking])
      .InMillisecondsF();
}

double ThreadHeapStatsCollector::Event::marking_time_in_bytes_per_second()
    const {
  return marked_bytes ? marking_time_in_ms() / 1000 / marked_bytes : 0.0;
}

base::TimeDelta ThreadHeapStatsCollector::Event::sweeping_time() const {
  return scope_data[kCompleteSweep] + scope_data[kEagerSweep] +
         scope_data[kLazySweepInIdle] + scope_data[kLazySweepOnAllocation];
}

int64_t ThreadHeapStatsCollector::allocated_bytes_since_prev_gc() const {
  return allocated_bytes_since_prev_gc_;
}

size_t ThreadHeapStatsCollector::marked_bytes() const {
  return current_.marked_bytes;
}

size_t ThreadHeapStatsCollector::allocated_space_bytes() const {
  return allocated_space_bytes_;
}

size_t ThreadHeapStatsCollector::collected_wrapper_count() const {
  return collected_wrapper_count_;
}

size_t ThreadHeapStatsCollector::wrapper_count() const {
  return wrapper_count_;
}

void ThreadHeapStatsCollector::RegisterObserver(
    ThreadHeapStatsObserver* observer) {
  DCHECK(!observers_.Contains(observer));
  observers_.push_back(observer);
}

void ThreadHeapStatsCollector::UnregisterObserver(
    ThreadHeapStatsObserver* observer) {
  wtf_size_t index = observers_.Find(observer);
  DCHECK_NE(WTF::kNotFound, index);
  observers_.EraseAt(index);
}

template <typename Callback>
void ThreadHeapStatsCollector::ForAllObservers(Callback callback) {
  for (ThreadHeapStatsObserver* observer : observers_) {
    callback(observer);
  }
}

}  // namespace blink
