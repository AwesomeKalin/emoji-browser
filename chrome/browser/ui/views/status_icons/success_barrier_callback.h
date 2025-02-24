// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_STATUS_ICONS_SUCCESS_BARRIER_CALLBACK_H_
#define CHROME_BROWSER_UI_VIEWS_STATUS_ICONS_SUCCESS_BARRIER_CALLBACK_H_

#include <cstddef>

#include "base/callback_forward.h"

// A callback wrapper that must be called |num_calls| times with an argument of
// true (indicates success) for |done_callback| to be called with true.  If the
// wrapper is called with false, |done_callback| is immediately run with an
// argument of false.  Further calls after |done_callback| has already been run
// will have no effect.
base::RepeatingCallback<void(bool)> SuccessBarrierCallback(
    size_t num_calls,
    base::OnceCallback<void(bool)> done_callback);

#endif  // CHROME_BROWSER_UI_VIEWS_STATUS_ICONS_SUCCESS_BARRIER_CALLBACK_H_
