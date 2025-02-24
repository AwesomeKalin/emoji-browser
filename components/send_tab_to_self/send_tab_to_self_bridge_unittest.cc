// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/send_tab_to_self/send_tab_to_self_bridge.h"

#include <map>
#include <set>
#include <utility>

#include "base/bind.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_task_environment.h"
#include "base/test/simple_test_clock.h"
#include "components/history/core/browser/history_service.h"
#include "components/send_tab_to_self/features.h"
#include "components/send_tab_to_self/proto/send_tab_to_self.pb.h"
#include "components/send_tab_to_self/target_device_info.h"
#include "components/sync/model/entity_change.h"
#include "components/sync/model/metadata_batch.h"
#include "components/sync/model/mock_model_type_change_processor.h"
#include "components/sync/model/model_type_store_test_util.h"
#include "components/sync/model_impl/in_memory_metadata_change_list.h"
#include "components/sync/protocol/model_type_state.pb.h"
#include "components/sync/test/test_matchers.h"
#include "components/sync_device_info/device_info.h"
#include "components/sync_device_info/fake_device_info_tracker.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace send_tab_to_self {

namespace {

using testing::_;
using testing::AllOf;
using testing::ElementsAre;
using testing::IsEmpty;
using testing::Pair;
using testing::Return;
using testing::SizeIs;
using testing::UnorderedElementsAre;

const char kGuidFormat[] = "guid %d";
const char kURLFormat[] = "https://www.url%d.com/";
const char kTitleFormat[] = "title %d";
const char kDeviceFormat[] = "device %d";
const char kLocalDeviceCacheGuid[] = "local_device_guid";
const char kLocalDeviceName[] = "local_device_name";

sync_pb::SendTabToSelfSpecifics CreateSpecifics(
    int suffix,
    base::Time shared_time = base::Time::Now(),
    base::Time navigation_time = base::Time::Now()) {
  sync_pb::SendTabToSelfSpecifics specifics;
  specifics.set_guid(base::StringPrintf(kGuidFormat, suffix));
  specifics.set_url(base::StringPrintf(kURLFormat, suffix));
  specifics.set_device_name(base::StringPrintf(kDeviceFormat, suffix));
  specifics.set_title(base::StringPrintf(kTitleFormat, suffix));
  specifics.set_target_device_sync_cache_guid(kLocalDeviceCacheGuid);
  specifics.set_shared_time_usec(
      shared_time.ToDeltaSinceWindowsEpoch().InMicroseconds());
  specifics.set_navigation_time_usec(
      navigation_time.ToDeltaSinceWindowsEpoch().InMicroseconds());
  return specifics;
}

sync_pb::ModelTypeState StateWithEncryption(
    const std::string& encryption_key_name) {
  sync_pb::ModelTypeState state;
  state.set_encryption_key_name(encryption_key_name);
  return state;
}

class MockSendTabToSelfModelObserver : public SendTabToSelfModelObserver {
 public:
  MOCK_METHOD0(SendTabToSelfModelLoaded, void());
  MOCK_METHOD1(EntriesAddedRemotely,
               void(const std::vector<const SendTabToSelfEntry*>&));
  MOCK_METHOD1(EntriesOpenedRemotely,
               void(const std::vector<const SendTabToSelfEntry*>&));

  MOCK_METHOD1(EntriesRemovedRemotely, void(const std::vector<std::string>&));
};

MATCHER_P(GuidIs, e, "") {
  return testing::ExplainMatchResult(e, arg->GetGUID(), result_listener);
}

class SendTabToSelfBridgeTest : public testing::Test {
 protected:
  SendTabToSelfBridgeTest()
      : store_(syncer::ModelTypeStoreTestUtil::CreateInMemoryStoreForTest()) {
    scoped_feature_list_.InitAndEnableFeature(kSendTabToSelfShowSendingUI);
    SetLocalDeviceCacheGuid(kLocalDeviceCacheGuid);
  }

  // Initialized the bridge based on the current local device and store. Can
  // only be called once per run, as it passes |store_|.
  void InitializeBridge() {
    ON_CALL(mock_processor_, IsTrackingMetadata()).WillByDefault(Return(true));
    bridge_ = std::make_unique<SendTabToSelfBridge>(
        mock_processor_.CreateForwardingProcessor(), &clock_,
        syncer::ModelTypeStoreTestUtil::MoveStoreToFactory(std::move(store_)),
        /*history_service=*/nullptr, &device_info_tracker_);
    bridge_->AddObserver(&mock_observer_);
    base::RunLoop().RunUntilIdle();
  }

  void ShutdownBridge() {
    bridge_->RemoveObserver(&mock_observer_);
    store_ =
        SendTabToSelfBridge::DestroyAndStealStoreForTest(std::move(bridge_));
    base::RunLoop().RunUntilIdle();
  }

  base::Time AdvanceAndGetTime(
      base::TimeDelta delta = base::TimeDelta::FromMilliseconds(10)) {
    clock_.Advance(delta);
    return clock_.Now();
  }

  void DisableBridge() {
    ON_CALL(mock_processor_, IsTrackingMetadata()).WillByDefault(Return(false));
  }

  std::unique_ptr<syncer::EntityData> MakeEntityData(
      const SendTabToSelfEntry& entry) {
    SendTabToSelfLocal specifics = entry.AsLocalProto();

    auto entity_data = std::make_unique<syncer::EntityData>();

    *(entity_data->specifics.mutable_send_tab_to_self()) =
        specifics.specifics();
    entity_data->name = entry.GetURL().spec();
    return entity_data;
  }

  // Helper method to reduce duplicated code between tests. Wraps the given
  // specifics objects in an EntityData and EntityChange of type ACTION_ADD, and
  // returns an EntityChangeList containing them all. Order is maintained.
  syncer::EntityChangeList EntityAddList(
      const std::vector<sync_pb::SendTabToSelfSpecifics>& specifics_list) {
    syncer::EntityChangeList changes;
    for (const auto& specifics : specifics_list) {
      auto entity_data = std::make_unique<syncer::EntityData>();

      *(entity_data->specifics.mutable_send_tab_to_self()) = specifics;
      entity_data->name = specifics.url();

      changes.push_back(syncer::EntityChange::CreateAdd(
          specifics.guid(), std::move(entity_data)));
    }
    return changes;
  }

  // For Model Tests.
  void AddSampleEntries() {
    // Adds timer to avoid having two entries with the same shared timestamp.
    bridge_->AddEntry(GURL("http://a.com"), "a", AdvanceAndGetTime(),
                      kLocalDeviceCacheGuid);
    bridge_->AddEntry(GURL("http://b.com"), "b", AdvanceAndGetTime(),
                      kLocalDeviceCacheGuid);
    bridge_->AddEntry(GURL("http://c.com"), "c", AdvanceAndGetTime(),
                      kLocalDeviceCacheGuid);
    bridge_->AddEntry(GURL("http://d.com"), "d", AdvanceAndGetTime(),
                      kLocalDeviceCacheGuid);
  }

  void SetLocalDeviceCacheGuid(const std::string& cache_guid) {
    ON_CALL(mock_processor_, TrackedCacheGuid())
        .WillByDefault(Return(cache_guid));
  }

  void AddTestDevice(const syncer::DeviceInfo* device) {
    device_info_tracker_.Add(device);
  }

  syncer::MockModelTypeChangeProcessor* processor() { return &mock_processor_; }

  SendTabToSelfBridge* bridge() { return bridge_.get(); }

  MockSendTabToSelfModelObserver* mock_observer() { return &mock_observer_; }

  base::SimpleTestClock* clock() { return &clock_; }

 private:
  base::SimpleTestClock clock_;

  // In memory model type store needs to be able to post tasks.
  base::test::ScopedTaskEnvironment task_environment_;

  std::unique_ptr<syncer::ModelTypeStore> store_;

  testing::NiceMock<syncer::MockModelTypeChangeProcessor> mock_processor_;

  syncer::FakeDeviceInfoTracker device_info_tracker_;

  std::unique_ptr<SendTabToSelfBridge> bridge_;

  testing::NiceMock<MockSendTabToSelfModelObserver> mock_observer_;

  base::test::ScopedFeatureList scoped_feature_list_;

  DISALLOW_COPY_AND_ASSIGN(SendTabToSelfBridgeTest);
};

TEST_F(SendTabToSelfBridgeTest, CheckEmpties) {
  InitializeBridge();

  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(_)).Times(0);
  EXPECT_EQ(0ul, bridge()->GetAllGuids().size());
  AddSampleEntries();
  EXPECT_EQ(4ul, bridge()->GetAllGuids().size());
}

TEST_F(SendTabToSelfBridgeTest, SyncAddOneEntry) {
  InitializeBridge();
  syncer::EntityChangeList remote_input;

  SendTabToSelfEntry entry("guid1", GURL("http://www.example.com/"), "title",
                           AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                           kLocalDeviceCacheGuid);

  remote_input.push_back(
      syncer::EntityChange::CreateAdd("guid1", MakeEntityData(entry)));
  auto metadata_change_list =
      std::make_unique<syncer::InMemoryMetadataChangeList>();
  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(SizeIs(1)));
  bridge()->MergeSyncData(std::move(metadata_change_list),
                          std::move(remote_input));
  EXPECT_EQ(1ul, bridge()->GetAllGuids().size());
}

TEST_F(SendTabToSelfBridgeTest, ApplySyncChangesAddTwoSpecifics) {
  InitializeBridge();

  const sync_pb::SendTabToSelfSpecifics specifics1 = CreateSpecifics(1);
  const sync_pb::SendTabToSelfSpecifics specifics2 = CreateSpecifics(2);

  sync_pb::ModelTypeState state = StateWithEncryption("ekn");
  std::unique_ptr<syncer::MetadataChangeList> metadata_changes =
      bridge()->CreateMetadataChangeList();
  metadata_changes->UpdateModelTypeState(state);

  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(SizeIs(2)));

  auto error = bridge()->ApplySyncChanges(
      std::move(metadata_changes), EntityAddList({specifics1, specifics2}));
  EXPECT_FALSE(error);
}

TEST_F(SendTabToSelfBridgeTest, ApplySyncChangesOneAdd) {
  InitializeBridge();
  SendTabToSelfEntry entry("guid1", GURL("http://www.example.com/"), "title",
                           AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                           kLocalDeviceCacheGuid);

  syncer::EntityChangeList add_changes;

  add_changes.push_back(
      syncer::EntityChange::CreateAdd("guid1", MakeEntityData(entry)));
  auto metadata_change_list =
      std::make_unique<syncer::InMemoryMetadataChangeList>();

  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(SizeIs(1)));
  bridge()->ApplySyncChanges(std::move(metadata_change_list),
                             std::move(add_changes));
  EXPECT_EQ(1ul, bridge()->GetAllGuids().size());
}

// Tests that the send tab to self entry is correctly removed.
TEST_F(SendTabToSelfBridgeTest, ApplySyncChangesOneDeletion) {
  InitializeBridge();
  SendTabToSelfEntry entry("guid1", GURL("http://www.example.com/"), "title",
                           AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                           kLocalDeviceCacheGuid);

  syncer::EntityChangeList add_changes;

  add_changes.push_back(
      syncer::EntityChange::CreateAdd("guid1", MakeEntityData(entry)));

  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(SizeIs(1)));
  bridge()->ApplySyncChanges(bridge()->CreateMetadataChangeList(),
                             std::move(add_changes));
  EXPECT_EQ(1ul, bridge()->GetAllGuids().size());
  syncer::EntityChangeList delete_changes;
  delete_changes.push_back(syncer::EntityChange::CreateDelete("guid1"));

  EXPECT_CALL(*mock_observer(), EntriesRemovedRemotely(SizeIs(1)));
  bridge()->ApplySyncChanges(bridge()->CreateMetadataChangeList(),
                             std::move(delete_changes));
  EXPECT_EQ(0ul, bridge()->GetAllGuids().size());
}

// Tests that the send tab to self entry is correctly removed.
TEST_F(SendTabToSelfBridgeTest, LocalHistoryDeletion) {
  InitializeBridge();
  SendTabToSelfEntry entry1("guid1", GURL("http://www.example.com/"), "title",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                            kLocalDeviceCacheGuid);

  SendTabToSelfEntry entry2("guid2", GURL("http://www.example2.com/"), "title2",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device2",
                            kLocalDeviceCacheGuid);

  SendTabToSelfEntry entry3("guid3", GURL("http://www.example3.com/"), "title3",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device3",
                            kLocalDeviceCacheGuid);

  syncer::EntityChangeList add_changes;

  add_changes.push_back(
      syncer::EntityChange::CreateAdd("guid1", MakeEntityData(entry1)));
  add_changes.push_back(
      syncer::EntityChange::CreateAdd("guid2", MakeEntityData(entry2)));
  add_changes.push_back(
      syncer::EntityChange::CreateAdd("guid3", MakeEntityData(entry3)));

  bridge()->ApplySyncChanges(bridge()->CreateMetadataChangeList(),
                             std::move(add_changes));

  ASSERT_EQ(3ul, bridge()->GetAllGuids().size());

  history::URLRows urls_to_remove;
  urls_to_remove.push_back(history::URLRow(GURL("http://www.example.com/")));
  urls_to_remove.push_back(history::URLRow(GURL("http://www.example2.com/")));

  EXPECT_CALL(*mock_observer(), EntriesRemovedRemotely(SizeIs(2)));

  bridge()->OnURLsDeleted(nullptr, history::DeletionInfo::ForUrls(
                                       urls_to_remove, std::set<GURL>()));
  EXPECT_EQ(1ul, bridge()->GetAllGuids().size());
}

TEST_F(SendTabToSelfBridgeTest, ApplySyncChangesEmpty) {
  InitializeBridge();
  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(_)).Times(0);

  auto error = bridge()->ApplySyncChanges(bridge()->CreateMetadataChangeList(),
                                          syncer::EntityChangeList());
  EXPECT_FALSE(error);
}

TEST_F(SendTabToSelfBridgeTest, AddEntryAndRestartBridge) {
  InitializeBridge();

  const sync_pb::SendTabToSelfSpecifics specifics = CreateSpecifics(1);
  sync_pb::ModelTypeState state = StateWithEncryption("ekn");
  std::unique_ptr<syncer::MetadataChangeList> metadata_changes =
      bridge()->CreateMetadataChangeList();
  metadata_changes->UpdateModelTypeState(state);

  auto error = bridge()->ApplySyncChanges(std::move(metadata_changes),
                                          EntityAddList({specifics}));
  ASSERT_FALSE(error);

  ShutdownBridge();

  EXPECT_CALL(*processor(),
              ModelReadyToSync(MetadataBatchContains(
                  syncer::HasEncryptionKeyName(state.encryption_key_name()),
                  /*entities=*/IsEmpty())));

  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(_)).Times(0);
  InitializeBridge();

  std::vector<std::string> guids = bridge()->GetAllGuids();
  ASSERT_EQ(1ul, guids.size());
  EXPECT_EQ(specifics.url(),
            bridge()->GetEntryByGUID(guids[0])->GetURL().spec());
}

TEST_F(SendTabToSelfBridgeTest, ApplySyncChangesInMemory) {
  InitializeBridge();

  const sync_pb::SendTabToSelfSpecifics specifics = CreateSpecifics(1);
  std::unique_ptr<syncer::MetadataChangeList> metadata_changes =
      bridge()->CreateMetadataChangeList();

  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(SizeIs(1)));

  auto error_on_add = bridge()->ApplySyncChanges(
      bridge()->CreateMetadataChangeList(), EntityAddList({specifics}));

  EXPECT_FALSE(error_on_add);

  EXPECT_EQ(1ul, bridge()->GetAllGuids().size());

  EXPECT_CALL(*mock_observer(), EntriesRemovedRemotely(SizeIs(1)));

  syncer::EntityChangeList entity_change_list;
  entity_change_list.push_back(
      syncer::EntityChange::CreateDelete(specifics.guid()));
  auto error_on_delete = bridge()->ApplySyncChanges(
      bridge()->CreateMetadataChangeList(), std::move(entity_change_list));

  EXPECT_FALSE(error_on_delete);
  EXPECT_EQ(0ul, bridge()->GetAllGuids().size());
}

TEST_F(SendTabToSelfBridgeTest, ApplyDeleteNonexistent) {
  InitializeBridge();
  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(_)).Times(0);

  std::unique_ptr<syncer::MetadataChangeList> metadata_changes =
      bridge()->CreateMetadataChangeList();

  EXPECT_CALL(*processor(), Delete(_, _)).Times(0);

  syncer::EntityChangeList entity_change_list;
  entity_change_list.push_back(syncer::EntityChange::CreateDelete("guid"));
  auto error = bridge()->ApplySyncChanges(std::move(metadata_changes),
                                          std::move(entity_change_list));
  EXPECT_FALSE(error);
}

TEST_F(SendTabToSelfBridgeTest, PreserveDissmissalAfterRestartBridge) {
  InitializeBridge();

  const sync_pb::SendTabToSelfSpecifics specifics = CreateSpecifics(1);
  std::unique_ptr<syncer::MetadataChangeList> metadata_changes =
      bridge()->CreateMetadataChangeList();

  auto error = bridge()->ApplySyncChanges(std::move(metadata_changes),
                                          EntityAddList({specifics}));
  ASSERT_FALSE(error);

  EXPECT_CALL(*processor(), Put(_, _, _)).Times(0);
  EXPECT_CALL(*processor(), Delete(_, _)).Times(0);

  bridge()->DismissEntry(specifics.guid());

  ShutdownBridge();

  InitializeBridge();

  std::vector<std::string> guids = bridge()->GetAllGuids();
  ASSERT_EQ(1ul, guids.size());
  EXPECT_TRUE(bridge()->GetEntryByGUID(guids[0])->GetNotificationDismissed());
}

TEST_F(SendTabToSelfBridgeTest, ExpireEntryDuringInit) {
  InitializeBridge();

  const sync_pb::SendTabToSelfSpecifics expired_specifics =
      CreateSpecifics(1, AdvanceAndGetTime(), AdvanceAndGetTime());

  AdvanceAndGetTime(kExpiryTime / 2.0);

  const sync_pb::SendTabToSelfSpecifics not_expired_specifics =
      CreateSpecifics(2, AdvanceAndGetTime(), AdvanceAndGetTime());

  sync_pb::ModelTypeState state = StateWithEncryption("ekn");
  std::unique_ptr<syncer::MetadataChangeList> metadata_changes =
      bridge()->CreateMetadataChangeList();
  metadata_changes->UpdateModelTypeState(state);

  auto error = bridge()->ApplySyncChanges(
      std::move(metadata_changes),
      EntityAddList({expired_specifics, not_expired_specifics}));
  ASSERT_FALSE(error);

  AdvanceAndGetTime(kExpiryTime / 2.0);

  EXPECT_CALL(*mock_observer(), EntriesRemovedRemotely(SizeIs(1)));

  ShutdownBridge();
  InitializeBridge();

  std::vector<std::string> guids = bridge()->GetAllGuids();
  EXPECT_EQ(1ul, guids.size());
  EXPECT_EQ(not_expired_specifics.url(),
            bridge()->GetEntryByGUID(guids[0])->GetURL().spec());
}

TEST_F(SendTabToSelfBridgeTest, AddExpiredEntry) {
  InitializeBridge();

  sync_pb::ModelTypeState state = StateWithEncryption("ekn");
  std::unique_ptr<syncer::MetadataChangeList> metadata_changes =
      bridge()->CreateMetadataChangeList();
  metadata_changes->UpdateModelTypeState(state);

  const sync_pb::SendTabToSelfSpecifics expired_specifics =
      CreateSpecifics(1, AdvanceAndGetTime(), AdvanceAndGetTime());

  AdvanceAndGetTime(kExpiryTime);

  const sync_pb::SendTabToSelfSpecifics not_expired_specifics =
      CreateSpecifics(2, AdvanceAndGetTime(), AdvanceAndGetTime());

  auto error = bridge()->ApplySyncChanges(
      std::move(metadata_changes),
      EntityAddList({expired_specifics, not_expired_specifics}));

  ASSERT_FALSE(error);

  std::vector<std::string> guids = bridge()->GetAllGuids();
  EXPECT_EQ(1ul, guids.size());
  EXPECT_EQ(not_expired_specifics.url(),
            bridge()->GetEntryByGUID(guids[0])->GetURL().spec());
}

TEST_F(SendTabToSelfBridgeTest, AddInvalidEntries) {
  InitializeBridge();
  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(_)).Times(0);

  // Add Entry should succeed in this case.
  EXPECT_NE(nullptr,
            bridge()->AddEntry(GURL("http://www.example.com/"), "d",
                               AdvanceAndGetTime(), kLocalDeviceCacheGuid));

  // Add Entry should fail on invalid URLs.
  EXPECT_EQ(nullptr, bridge()->AddEntry(GURL(), "d", AdvanceAndGetTime(),
                                        kLocalDeviceCacheGuid));
  EXPECT_EQ(nullptr,
            bridge()->AddEntry(GURL("http://?k=v"), "d", AdvanceAndGetTime(),
                               kLocalDeviceCacheGuid));
  EXPECT_EQ(nullptr,
            bridge()->AddEntry(GURL("http//google.com"), "d",
                               AdvanceAndGetTime(), kLocalDeviceCacheGuid));

  // Add Entry should succeed on an invalid navigation_time, since that is the
  // case for sending links.
  EXPECT_NE(nullptr, bridge()->AddEntry(GURL("http://www.example.com/"), "d",
                                        base::Time(), kLocalDeviceCacheGuid));
}

TEST_F(SendTabToSelfBridgeTest, IsBridgeReady) {
  InitializeBridge();
  ASSERT_TRUE(bridge()->IsReady());

  DisableBridge();
  ASSERT_FALSE(bridge()->IsReady());
}

TEST_F(SendTabToSelfBridgeTest, AddDuplicateEntries) {
  InitializeBridge();

  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(_)).Times(0);

  base::Time navigation_time = AdvanceAndGetTime();
  // The de-duplication code does not use the title as a comparator.
  // So they are intentionally different here.
  bridge()->AddEntry(GURL("http://a.com"), "a", navigation_time,
                     kLocalDeviceCacheGuid);
  bridge()->AddEntry(GURL("http://a.com"), "b", navigation_time,
                     kLocalDeviceCacheGuid);
  EXPECT_EQ(1ul, bridge()->GetAllGuids().size());

  bridge()->AddEntry(GURL("http://a.com"), "a", AdvanceAndGetTime(),
                     kLocalDeviceCacheGuid);
  bridge()->AddEntry(GURL("http://b.com"), "b", AdvanceAndGetTime(),
                     kLocalDeviceCacheGuid);
  EXPECT_EQ(3ul, bridge()->GetAllGuids().size());
}

TEST_F(SendTabToSelfBridgeTest,
       NotifyRemoteSendTabToSelfEntryAdded_BroadcastDisabled) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{kSendTabToSelfShowSendingUI},
      /*disabled_features=*/{kSendTabToSelfBroadcast});

  InitializeBridge();
  SetLocalDeviceCacheGuid("Device1");

  // Add on entry targeting this device and another targeting another device.
  syncer::EntityChangeList remote_input;
  SendTabToSelfEntry entry1("guid1", GURL("http://www.example.com/"), "title",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                            "Device1");
  SendTabToSelfEntry entry2("guid2", GURL("http://www.example.com/"), "title",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                            "Device2");
  remote_input.push_back(
      syncer::EntityChange::CreateAdd("guid1", MakeEntityData(entry1)));
  remote_input.push_back(
      syncer::EntityChange::CreateAdd("guid2", MakeEntityData(entry2)));

  auto metadata_change_list =
      std::make_unique<syncer::InMemoryMetadataChangeList>();

  // There should only be one entry sent to the observers.
  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(SizeIs(1)));
  bridge()->MergeSyncData(std::move(metadata_change_list),
                          std::move(remote_input));

  EXPECT_EQ(2ul, bridge()->GetAllGuids().size());
}

TEST_F(SendTabToSelfBridgeTest,
       NotifyRemoteSendTabToSelfEntryAdded_BroadcastEnabled) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{kSendTabToSelfShowSendingUI,
                            kSendTabToSelfBroadcast},
      /*disabled_features=*/{});

  InitializeBridge();
  SetLocalDeviceCacheGuid("Device1");

  // Add on entry targeting this device and another targeting another device.
  syncer::EntityChangeList remote_input;
  SendTabToSelfEntry entry1("guid1", GURL("http://www.example.com/"), "title",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                            "Device1");
  SendTabToSelfEntry entry2("guid2", GURL("http://www.example.com/"), "title",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                            "Device2");
  remote_input.push_back(
      syncer::EntityChange::CreateAdd("guid1", MakeEntityData(entry1)));
  remote_input.push_back(
      syncer::EntityChange::CreateAdd("guid2", MakeEntityData(entry2)));

  auto metadata_change_list =
      std::make_unique<syncer::InMemoryMetadataChangeList>();

  // The 2 entries should be sent to the observers.
  EXPECT_CALL(*mock_observer(), EntriesAddedRemotely(SizeIs(2)));
  bridge()->MergeSyncData(std::move(metadata_change_list),
                          std::move(remote_input));

  EXPECT_EQ(2ul, bridge()->GetAllGuids().size());
}

// Tests that only the most recent device's guid is returned when multiple
// devices have the same name.
TEST_F(SendTabToSelfBridgeTest,
       GetTargetDeviceNameToCacheInfoMap_OneDevicePerName) {
  const std::string kRecentGuid = "guid1";
  const std::string kOldGuid = "guid2";
  const std::string kOlderGuid = "guid3";

  InitializeBridge();

  // Create multiple DeviceInfo objects with the same name but different guids.
  syncer::DeviceInfo recent_device(
      kRecentGuid, "device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      clock()->Now() - base::TimeDelta::FromDays(1),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&recent_device);

  syncer::DeviceInfo old_device(
      kOldGuid, "device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now() - base::TimeDelta::FromDays(3),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&old_device);

  syncer::DeviceInfo older_device(
      kOlderGuid, "device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now() - base::TimeDelta::FromDays(5),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&older_device);

  TargetDeviceInfo target_device_info(recent_device.guid(),
                                      recent_device.device_type(),
                                      recent_device.last_updated_timestamp());

  EXPECT_THAT(
      bridge()->GetTargetDeviceNameToCacheInfoMap(),
      ElementsAre(Pair(recent_device.client_name(), target_device_info)));
}

// Tests that only devices that have the send tab to self receiving feature
// enabled are returned.
TEST_F(SendTabToSelfBridgeTest,
       GetTargetDeviceNameToCacheInfoMap_OnlyReceivingEnabled) {
  InitializeBridge();

  syncer::DeviceInfo enabled_device(
      "enabled_guid", "enabled_device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now(),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&enabled_device);

  syncer::DeviceInfo disabled_device(
      "disabled_guid", "disabled_device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now(),
      /*send_tab_to_self_receiving_enabled=*/false);
  AddTestDevice(&disabled_device);

  TargetDeviceInfo target_device_info(enabled_device.guid(),
                                      enabled_device.device_type(),
                                      enabled_device.last_updated_timestamp());

  EXPECT_THAT(
      bridge()->GetTargetDeviceNameToCacheInfoMap(),
      ElementsAre(Pair(enabled_device.client_name(), target_device_info)));
}

// Tests that only devices that are not expired are returned.
TEST_F(SendTabToSelfBridgeTest,
       GetTargetDeviceNameToCacheInfoMap_NoExpiredDevices) {
  InitializeBridge();

  syncer::DeviceInfo expired_device(
      "expired_guid", "expired_device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now() - base::TimeDelta::FromDays(11),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&expired_device);

  syncer::DeviceInfo valid_device(
      "valid_guid", "valid_device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now() - base::TimeDelta::FromDays(1),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&valid_device);

  TargetDeviceInfo target_device_info(valid_device.guid(),
                                      valid_device.device_type(),
                                      valid_device.last_updated_timestamp());

  EXPECT_THAT(
      bridge()->GetTargetDeviceNameToCacheInfoMap(),
      ElementsAre(Pair(valid_device.client_name(), target_device_info)));
}

// Tests that the local device is not returned.
TEST_F(SendTabToSelfBridgeTest,
       GetTargetDeviceNameToCacheInfoMap_NoLocalDevice) {
  InitializeBridge();
  bridge()->SetLocalDeviceNameForTest(kLocalDeviceName);

  syncer::DeviceInfo local_device(
      kLocalDeviceCacheGuid, kLocalDeviceName, "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now(),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&local_device);

  syncer::DeviceInfo other_local_device(
      "other_local_guid", kLocalDeviceName, "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now(),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&local_device);

  syncer::DeviceInfo other_device(
      "other_guid", "other_device_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now(),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&other_device);

  TargetDeviceInfo target_device_info(other_device.guid(),
                                      other_device.device_type(),
                                      other_device.last_updated_timestamp());

  EXPECT_THAT(
      bridge()->GetTargetDeviceNameToCacheInfoMap(),
      ElementsAre(Pair(other_device.client_name(), target_device_info)));
}

// Tests that the local device is not returned.
TEST_F(SendTabToSelfBridgeTest,
       GetTargetDeviceNameToCacheInfoMap_Updated_DeviceExpired) {
  InitializeBridge();

  // Set a device that is about to expire and a more recent device.
  syncer::DeviceInfo older_device(
      "older_guid", "older_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now() - base::TimeDelta::FromDays(9),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&older_device);

  syncer::DeviceInfo recent_device(
      "recent_guid", "recent_name", "72", "agent",
      sync_pb::SyncEnums_DeviceType_TYPE_LINUX, "scoped_is",
      /*last_updated_timestamp=*/clock()->Now() - base::TimeDelta::FromDays(1),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&recent_device);

  TargetDeviceInfo older_device_info(older_device.guid(),
                                     older_device.device_type(),
                                     older_device.last_updated_timestamp());
  TargetDeviceInfo recent_device_info(recent_device.guid(),
                                      recent_device.device_type(),
                                      recent_device.last_updated_timestamp());

  // Set the map by calling it. Make sure it has the 2 devices.
  EXPECT_THAT(bridge()->GetTargetDeviceNameToCacheInfoMap(),
              UnorderedElementsAre(
                  Pair(older_device.client_name(), older_device_info),
                  Pair(recent_device.client_name(), recent_device_info)));

  // Advance the time so that the older device expires.
  clock()->Advance(base::TimeDelta::FromDays(5));

  // Make sure only the recent device is in the map.
  EXPECT_THAT(
      bridge()->GetTargetDeviceNameToCacheInfoMap(),
      ElementsAre(Pair(recent_device.client_name(), recent_device_info)));
}

// Tests that the local device is not returned.
TEST_F(SendTabToSelfBridgeTest,
       GetTargetDeviceNameToCacheInfoMap_Updated_NewEntries) {
  InitializeBridge();

  // Set a valid device.
  syncer::DeviceInfo device(
      "guid", "name", "72", "agent", sync_pb::SyncEnums_DeviceType_TYPE_LINUX,
      "scoped_is", /*last_updated_timestamp=*/clock()->Now(),
      /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&device);

  // Set the map by calling it. Make sure it has the device.
  TargetDeviceInfo device_info(device.guid(), device.device_type(),
                               device.last_updated_timestamp());

  EXPECT_THAT(bridge()->GetTargetDeviceNameToCacheInfoMap(),
              ElementsAre(Pair(device.client_name(), device_info)));

  // Add a new device.
  syncer::DeviceInfo new_device("new_guid", "new_name", "72", "agent",
                                sync_pb::SyncEnums_DeviceType_TYPE_LINUX,
                                "scoped_is",
                                /*last_updated_timestamp=*/clock()->Now(),
                                /*send_tab_to_self_receiving_enabled=*/true);
  AddTestDevice(&new_device);

  // Make sure both devices are in the map.
  TargetDeviceInfo new_device_info(new_device.guid(), new_device.device_type(),
                                   new_device.last_updated_timestamp());

  EXPECT_THAT(
      bridge()->GetTargetDeviceNameToCacheInfoMap(),
      UnorderedElementsAre(Pair(device.client_name(), device_info),
                           Pair(new_device.client_name(), new_device_info)));
}

TEST_F(SendTabToSelfBridgeTest, NotifyRemoteSendTabToSelfEntryOpened) {
  base::test::ScopedFeatureList scoped_features;
  scoped_features.InitWithFeatures(
      /*enabled_features=*/{kSendTabToSelfShowSendingUI,
                            kSendTabToSelfBroadcast},
      /*disabled_features=*/{});

  InitializeBridge();
  SetLocalDeviceCacheGuid("Device1");

  // Add on entry targeting this device and another targeting another device.
  syncer::EntityChangeList remote_input;
  SendTabToSelfEntry entry1("guid1", GURL("http://www.example.com/"), "title",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                            "Device1");
  SendTabToSelfEntry entry2("guid2", GURL("http://www.example.com/"), "title",
                            AdvanceAndGetTime(), AdvanceAndGetTime(), "device",
                            "Device2");
  remote_input.push_back(
      syncer::EntityChange::CreateAdd("guid1", MakeEntityData(entry1)));
  remote_input.push_back(
      syncer::EntityChange::CreateAdd("guid2", MakeEntityData(entry2)));

  entry1.MarkOpened();
  remote_input.push_back(
      syncer::EntityChange::CreateUpdate("guid1", MakeEntityData(entry1)));

  auto metadata_change_list =
      std::make_unique<syncer::InMemoryMetadataChangeList>();

  // an entry with "guid1" should be sent to the observers.
  EXPECT_CALL(*mock_observer(), EntriesOpenedRemotely(AllOf(
                                    SizeIs(1), ElementsAre(GuidIs("guid1")))));
  bridge()->MergeSyncData(std::move(metadata_change_list),
                          std::move(remote_input));

  EXPECT_EQ(2ul, bridge()->GetAllGuids().size());
}

}  // namespace

}  // namespace send_tab_to_self
