// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/notifications/scheduler/internal/init_aware_scheduler.h"

#include <memory>

#include "base/bind_helpers.h"
#include "base/test/scoped_task_environment.h"
#include "chrome/browser/notifications/scheduler/public/notification_params.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;

namespace notifications {
namespace {

class MockNotificationScheduler : public NotificationScheduler {
 public:
  MockNotificationScheduler() = default;
  ~MockNotificationScheduler() override = default;

  MOCK_METHOD1(Init, void(InitCallback));
  MOCK_METHOD1(Schedule, void(std::unique_ptr<NotificationParams>));
  MOCK_METHOD0(OnStartTask, void());
  MOCK_METHOD0(OnStopTask, void());

 private:
  DISALLOW_COPY_AND_ASSIGN(MockNotificationScheduler);
};

class InitAwareNotificationSchedulerTest : public testing::Test {
 public:
  InitAwareNotificationSchedulerTest() : scheduler_impl_(nullptr) {}
  ~InitAwareNotificationSchedulerTest() override = default;

  void SetUp() override {
    auto scheduler = std::make_unique<MockNotificationScheduler>();
    scheduler_impl_ = scheduler.get();
    init_aware_scheduler_ =
        std::make_unique<InitAwareNotificationScheduler>(std::move(scheduler));
  }

 protected:
  std::unique_ptr<NotificationParams> BuildParams() {
    return std::make_unique<NotificationParams>(
        SchedulerClientType::kUnknown, NotificationData(), ScheduleParams());
  }

  NotificationScheduler* init_aware_scheduler() {
    return init_aware_scheduler_.get();
  }
  MockNotificationScheduler* scheduler_impl() { return scheduler_impl_; }

 private:
  base::test::ScopedTaskEnvironment scoped_task_environment_;
  MockNotificationScheduler* scheduler_impl_;
  std::unique_ptr<NotificationScheduler> init_aware_scheduler_;

  DISALLOW_COPY_AND_ASSIGN(InitAwareNotificationSchedulerTest);
};

// Checks std::unique_ptr<NotificationParams> has specific guid.
MATCHER_P(GuidIs, expected_guid, "") {
  return arg->guid == expected_guid;
}

// Verifies cached calls are flushed into the actual implementation.
TEST_F(InitAwareNotificationSchedulerTest, FlushCachedCalls) {
  auto params = BuildParams();
  std::string guid = params->guid;
  EXPECT_FALSE(guid.empty());
  {
    InSequence sequence;
    EXPECT_CALL(*scheduler_impl(), Init(_))
        .WillOnce(Invoke([](NotificationScheduler::InitCallback cb) {
          std::move(cb).Run(true /*success*/);
        }));
    EXPECT_CALL(*scheduler_impl(), Schedule(GuidIs(guid)));

    // Schedule() call before Init() will be cached.
    init_aware_scheduler()->Schedule(std::move(params));
    init_aware_scheduler()->Init(base::DoNothing());
  }
}

// Verifies that API calls after successful initialization will call into the
// actual implementation.
TEST_F(InitAwareNotificationSchedulerTest, CallAfterInitSuccess) {
  auto params = BuildParams();
  std::string guid = params->guid;
  EXPECT_FALSE(guid.empty());
  {
    InSequence sequence;
    EXPECT_CALL(*scheduler_impl(), Init(_))
        .WillOnce(Invoke([](NotificationScheduler::InitCallback cb) {
          std::move(cb).Run(true /*success*/);
        }));
    EXPECT_CALL(*scheduler_impl(), Schedule(GuidIs(guid)));

    // Schedule() call after Init().
    init_aware_scheduler()->Init(base::DoNothing());
    init_aware_scheduler()->Schedule(std::move(params));
  }
}

// Verifies no calls are flushed to actual implementation if initialization
// failed.
TEST_F(InitAwareNotificationSchedulerTest, NoFlushOnInitFailure) {
  auto params1 = BuildParams();
  auto params2 = BuildParams();

  EXPECT_CALL(*scheduler_impl(), Init(_))
      .WillOnce(Invoke([](NotificationScheduler::InitCallback cb) {
        std::move(cb).Run(false /*success*/);
      }));
  EXPECT_CALL(*scheduler_impl(), Schedule(_)).Times(0);

  init_aware_scheduler()->Schedule(std::move(params1));
  init_aware_scheduler()->Init(base::DoNothing());
  init_aware_scheduler()->Schedule(std::move(params2));
}

}  // namespace
}  // namespace notifications
