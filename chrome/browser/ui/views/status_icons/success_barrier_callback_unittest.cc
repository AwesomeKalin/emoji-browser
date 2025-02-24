// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/status_icons/success_barrier_callback.h"

#include "base/bind.h"
#include "chrome/browser/ui/views/status_icons/status_icon_linux_dbus.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

void Callback(bool* p_success, int* calls, bool success) {
  *p_success = success;
  (*calls)++;
}

}  // namespace

TEST(SuccessBarrierCallbackTest, RunAfterNumClosures) {
  bool success = false;
  int calls = 0;
  auto callback =
      SuccessBarrierCallback(3, base::BindOnce(Callback, &success, &calls));
  callback.Run(true);
  EXPECT_EQ(calls, 0);
  callback.Run(true);
  EXPECT_EQ(calls, 0);
  callback.Run(true);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(success);

  // Further calls should have no effect.
  callback.Run(true);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(success);
  callback.Run(false);
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(success);
}

TEST(SuccessBarrierCallbackTest, RunFailureOnce) {
  bool success = false;
  int calls = 0;
  auto callback =
      SuccessBarrierCallback(3, base::BindOnce(Callback, &success, &calls));
  callback.Run(true);
  EXPECT_EQ(calls, 0);
  callback.Run(false);
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(success);

  // Further calls should have no effect.
  callback.Run(true);
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(success);
  callback.Run(false);
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(success);
}
