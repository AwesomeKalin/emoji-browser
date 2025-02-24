// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_TIME_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_TIME_H_

#include "base/time/time.h"
#include "third_party/blink/renderer/platform/wtf/wtf_export.h"

namespace WTF {
// Provides thin wrappers around the following basic time types from
// base/time package:
//
//  - WTF::TimeDelta is an alias for base::TimeDelta and represents a duration
//    of time.
//  - WTF::TimeTicks wraps base::TimeTicks and represents a monotonic time
//    value.
//  - WTF::Time is an alias for base::Time and represents a wall time value.
//
// For usage guideline please see the documentation in base/time/time.h

using TimeDelta = base::TimeDelta;
using TimeTicks = base::TimeTicks;
using Time = base::Time;
using TimeTicks = base::TimeTicks;

// Returns the current UTC time in seconds, counted from January 1, 1970.
// Precision varies depending on platform but is usually as good or better
// than a millisecond.
WTF_EXPORT double CurrentTime();

// Same thing, in milliseconds.
inline double CurrentTimeMS() {
  return CurrentTime() * 1000.0;
}

using TimeFunction = double (*)();

// Monotonically increasing clock time since an arbitrary and unspecified origin
// time. Mockable using SetTimeFunctionsForTesting().
WTF_EXPORT TimeTicks CurrentTimeTicks();
// Convenience functions that return seconds and milliseconds since the origin
// time. Prefer CurrentTimeTicks() where possible to avoid potential unit
// confusion errors.
WTF_EXPORT double CurrentTimeTicksInSeconds();
WTF_EXPORT double CurrentTimeTicksInMilliseconds();

}  // namespace WTF

using WTF::CurrentTime;
using WTF::CurrentTimeMS;
using WTF::CurrentTimeTicks;
using WTF::CurrentTimeTicksInMilliseconds;
using WTF::CurrentTimeTicksInSeconds;
using WTF::Time;
using WTF::TimeDelta;
using WTF::TimeTicks;

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_WTF_TIME_H_
