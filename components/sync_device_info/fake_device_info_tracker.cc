// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/sync_device_info/fake_device_info_tracker.h"

#include "base/logging.h"
#include "components/sync_device_info/device_info.h"

namespace {

// static
std::unique_ptr<syncer::DeviceInfo> CloneDeviceInfo(
    const syncer::DeviceInfo& device_info) {
  return std::make_unique<syncer::DeviceInfo>(
      device_info.guid(), device_info.client_name(),
      device_info.chrome_version(), device_info.sync_user_agent(),
      device_info.device_type(), device_info.signin_scoped_device_id(),
      device_info.last_updated_timestamp(),
      device_info.send_tab_to_self_receiving_enabled());
}

}  // namespace

namespace syncer {

FakeDeviceInfoTracker::FakeDeviceInfoTracker() = default;

FakeDeviceInfoTracker::~FakeDeviceInfoTracker() = default;

bool FakeDeviceInfoTracker::IsSyncing() const {
  return !devices_.empty();
}

std::unique_ptr<DeviceInfo> FakeDeviceInfoTracker::GetDeviceInfo(
    const std::string& client_id) const {
  NOTREACHED();
  return nullptr;
}

std::vector<std::unique_ptr<DeviceInfo>>
FakeDeviceInfoTracker::GetAllDeviceInfo() const {
  std::vector<std::unique_ptr<DeviceInfo>> list;

  for (const DeviceInfo* device : devices_)
    list.push_back(CloneDeviceInfo(*device));

  return list;
}

void FakeDeviceInfoTracker::AddObserver(Observer* observer) {
  NOTREACHED();
}

void FakeDeviceInfoTracker::RemoveObserver(Observer* observer) {
  NOTREACHED();
}

int FakeDeviceInfoTracker::CountActiveDevices() const {
  return active_device_count_.value_or(devices_.size());
}

void FakeDeviceInfoTracker::ForcePulseForTest() {
  NOTREACHED();
}

void FakeDeviceInfoTracker::Add(const DeviceInfo* device) {
  devices_.push_back(device);
}

void FakeDeviceInfoTracker::OverrideActiveDeviceCount(int count) {
  active_device_count_ = count;
}

}  // namespace syncer
