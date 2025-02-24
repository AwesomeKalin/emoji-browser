// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/tabs/tab_strip_model.h"

#include <stddef.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/optional.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "chrome/browser/ui/tabs/tab_group_id.h"
#include "chrome/browser/ui/tabs/test_tab_strip_model_delegate.h"
#include "chrome/test/base/testing_profile.h"
#include "components/web_modal/web_contents_modal_dialog_manager.h"
#include "components/web_modal/web_contents_modal_dialog_manager_delegate.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_side_navigation_test_utils.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"

using content::WebContents;

namespace {

const char kTabStripModelTestIDUserDataKey[] = "TabStripModelTestIDUserData";

class TabStripModelTestIDUserData : public base::SupportsUserData::Data {
 public:
  explicit TabStripModelTestIDUserData(int id) : id_(id) {}
  ~TabStripModelTestIDUserData() override {}
  int id() { return id_; }

 private:
  int id_;
};

class MockTabStripModelObserver : public TabStripModelObserver {
 public:
  MockTabStripModelObserver() = default;
  ~MockTabStripModelObserver() override = default;

  enum TabStripModelObserverAction {
    INSERT,
    CLOSE,
    DETACH,
    ACTIVATE,
    DEACTIVATE,
    SELECT,
    MOVE,
    CHANGE,
    PINNED,
    REPLACED,
    CLOSE_ALL,
    CLOSE_ALL_CANCELED,
    CLOSE_ALL_COMPLETED,
    GROUP_CHANGED,
  };

  struct State {
    State(WebContents* a_dst_contents,
          int a_dst_index,
          TabStripModelObserverAction a_action)
        : src_contents(nullptr),
          dst_contents(a_dst_contents),
          src_index(-1),
          dst_index(a_dst_index),
          change_reason(CHANGE_REASON_NONE),
          foreground(false),
          action(a_action) {}

    WebContents* src_contents;
    WebContents* dst_contents;
    int src_index;
    int dst_index;
    int change_reason;
    bool foreground;
    base::Optional<TabGroupId> old_group;
    base::Optional<TabGroupId> new_group;
    TabStripModelObserverAction action;
  };

  int GetStateCount() const { return static_cast<int>(states_.size()); }

  // Returns (by way of parameters) the number of state's with CLOSE_ALL,
  // CLOSE_ALL_CANCELED and CLOSE_ALL_COMPLETED.
  void GetCloseCounts(int* close_all_count,
                      int* close_all_canceled_count,
                      int* close_all_completed_count) {
    *close_all_count = *close_all_canceled_count = *close_all_completed_count =
        0;
    for (int i = 0; i < GetStateCount(); ++i) {
      switch (GetStateAt(i).action) {
        case CLOSE_ALL:
          (*close_all_count)++;
          break;
        case CLOSE_ALL_CANCELED:
          (*close_all_canceled_count)++;
          break;
        case CLOSE_ALL_COMPLETED:
          (*close_all_completed_count)++;
          break;
        default:
          break;
      }
    }
  }

  const State& GetStateAt(int index) const {
    DCHECK(index >= 0 && index < GetStateCount());
    return states_[index];
  }

  bool StateEquals(int index, const State& state) {
    const State& s = GetStateAt(index);
    return (s.src_contents == state.src_contents &&
            s.dst_contents == state.dst_contents &&
            s.src_index == state.src_index && s.dst_index == state.dst_index &&
            s.change_reason == state.change_reason &&
            s.foreground == state.foreground &&
            s.old_group == state.old_group && s.new_group == state.new_group &&
            s.action == state.action);
  }

  void PushInsertState(WebContents* contents, int index, bool foreground) {
    State s(contents, index, INSERT);
    s.foreground = foreground;
    states_.push_back(s);
  }

  void PushActivateState(WebContents* old_contents,
                         WebContents* new_contents,
                         int index,
                         int reason) {
    State s(new_contents, index, ACTIVATE);
    s.src_contents = old_contents;
    s.change_reason = reason;
    states_.push_back(s);
  }

  void PushDeactivateState(WebContents* contents,
                           const ui::ListSelectionModel& old_model) {
    states_.push_back(State(contents, old_model.active(), DEACTIVATE));
  }

  void PushSelectState(content::WebContents* new_contents,
                       const ui::ListSelectionModel& old_model,
                       const ui::ListSelectionModel& new_model) {
    State s(new_contents, new_model.active(), SELECT);
    s.src_index = old_model.active();
    states_.push_back(s);
  }

  void PushMoveState(WebContents* contents, int from_index, int to_index) {
    State s(contents, to_index, MOVE);
    s.src_index = from_index;
    states_.push_back(s);
  }

  void PushCloseState(WebContents* contents, int index) {
    states_.push_back(State(contents, index, CLOSE));
  }

  void PushDetachState(WebContents* contents, int index, bool was_active) {
    states_.push_back(State(contents, index, DETACH));
  }

  void PushReplaceState(WebContents* old_contents,
                        WebContents* new_contents,
                        int index) {
    State s(new_contents, index, REPLACED);
    s.src_contents = old_contents;
    states_.push_back(s);
  }

  void PushGroupChangeState(WebContents* contents,
                            int index,
                            base::Optional<TabGroupId> old_group,
                            base::Optional<TabGroupId> new_group) {
    State s(contents, index, GROUP_CHANGED);
    s.old_group = old_group;
    s.new_group = new_group;
    states_.push_back(s);
  }

  // TabStripModelObserver overrides:
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    switch (change.type()) {
      case TabStripModelChange::kInserted: {
        for (const auto& contents : change.GetInsert()->contents) {
          PushInsertState(contents.contents, contents.index,
                          selection.new_contents == contents.contents);
        }
        break;
      }
      case TabStripModelChange::kRemoved: {
        const bool will_be_deleted = change.GetRemove()->will_be_deleted;
        for (const auto& contents : change.GetRemove()->contents) {
          if (will_be_deleted)
            PushCloseState(contents.contents, contents.index);
          PushDetachState(contents.contents, contents.index,
                          selection.old_contents == contents.contents);
        }
        break;
      }
      case TabStripModelChange::kReplaced: {
        auto* replace = change.GetReplace();
        PushReplaceState(replace->old_contents, replace->new_contents,
                         replace->index);
        break;
      }
      case TabStripModelChange::kMoved: {
        auto* move = change.GetMove();
        PushMoveState(move->contents, move->from_index, move->to_index);
        break;
      }
      case TabStripModelChange::kGroupChanged: {
        auto* group_change = change.GetGroupChange();
        PushGroupChangeState(group_change->contents, group_change->index,
                             group_change->old_group, group_change->new_group);
        break;
      }
      case TabStripModelChange::kSelectionOnly:
        break;
    }

    if (selection.active_tab_changed()) {
      if (selection.old_contents && selection.selection_changed())
        PushDeactivateState(selection.old_contents, selection.old_model);

      PushActivateState(selection.old_contents, selection.new_contents,
                        selection.new_model.active(), selection.reason);
    }

    if (selection.selection_changed()) {
      PushSelectState(selection.new_contents, selection.old_model,
                      selection.new_model);
    }
  }

  void TabChangedAt(WebContents* contents,
                    int index,
                    TabChangeType change_type) override {
    states_.push_back(State(contents, index, CHANGE));
  }

  void TabPinnedStateChanged(TabStripModel* tab_strip_model,
                             WebContents* contents,
                             int index) override {
    states_.push_back(State(contents, index, PINNED));
  }

  void WillCloseAllTabs(TabStripModel* tab_strip_model) override {
    states_.push_back(State(nullptr, -1, CLOSE_ALL));
  }

  void CloseAllTabsStopped(TabStripModel* tab_strip_model,
                           CloseAllStoppedReason reason) override {
    if (reason == kCloseAllCanceled) {
      states_.push_back(State(nullptr, -1, CLOSE_ALL_CANCELED));
    } else if (reason == kCloseAllCompleted) {
      states_.push_back(State(nullptr, -1, CLOSE_ALL_COMPLETED));
    }
  }

  void ClearStates() { states_.clear(); }

 private:
  std::vector<State> states_;

  DISALLOW_COPY_AND_ASSIGN(MockTabStripModelObserver);
};

}  // namespace

class TabStripModelTest : public testing::Test {
 public:
  TabStripModelTest() : profile_(new TestingProfile) {}

  TestingProfile* profile() { return profile_.get(); }

  std::unique_ptr<WebContents> CreateWebContents() {
    return content::WebContentsTester::CreateTestWebContents(profile(),
                                                             nullptr);
  }

  std::unique_ptr<WebContents> CreateWebContentsWithSharedRPH(
      WebContents* web_contents) {
    WebContents::CreateParams create_params(
        profile(), web_contents->GetRenderViewHost()->GetSiteInstance());
    std::unique_ptr<WebContents> retval = WebContents::Create(create_params);
    EXPECT_EQ(retval->GetMainFrame()->GetProcess(),
              web_contents->GetMainFrame()->GetProcess());
    return retval;
  }

  std::unique_ptr<WebContents> CreateWebContentsWithID(int id) {
    std::unique_ptr<WebContents> contents = CreateWebContents();
    SetID(contents.get(), id);
    return contents;
  }

  // Sets the id of the specified contents.
  void SetID(WebContents* contents, int id) {
    contents->SetUserData(&kTabStripModelTestIDUserDataKey,
                          std::make_unique<TabStripModelTestIDUserData>(id));
  }

  // Returns the id of the specified contents.
  int GetID(WebContents* contents) {
    TabStripModelTestIDUserData* user_data =
        static_cast<TabStripModelTestIDUserData*>(
            contents->GetUserData(&kTabStripModelTestIDUserDataKey));

    return user_data ? user_data->id() : -1;
  }

  // Returns the state of the given tab strip as a string. The state consists
  // of the ID of each web contents followed by a 'p' if pinned. For example,
  // if the model consists of two tabs with ids 2 and 1, with the first
  // tab pinned, this returns "2p 1".
  std::string GetTabStripStateString(const TabStripModel& model) {
    std::string actual;
    for (int i = 0; i < model.count(); ++i) {
      if (i > 0)
        actual += " ";

      actual += base::NumberToString(GetID(model.GetWebContentsAt(i)));

      if (model.IsTabPinned(i))
        actual += "p";
    }
    return actual;
  }

  void PrepareTabs(TabStripModel* model, int tab_count) {
    for (int i = 0; i < tab_count; ++i)
      model->AppendWebContents(CreateWebContentsWithID(i), true);
  }

  void PrepareTabstripForSelectionTest(TabStripModel* model,
                                       int tab_count,
                                       int pinned_count,
                                       const std::string& selected_tabs) {
    PrepareTabs(model, tab_count);
    for (int i = 0; i < pinned_count; ++i)
      model->SetTabPinned(i, true);

    ui::ListSelectionModel selection_model;
    for (const base::StringPiece& sel : base::SplitStringPiece(
             selected_tabs, base::kWhitespaceASCII, base::TRIM_WHITESPACE,
             base::SPLIT_WANT_NONEMPTY)) {
      int value;
      ASSERT_TRUE(base::StringToInt(sel, &value));
      selection_model.AddIndexToSelection(value);
    }
    selection_model.set_active(selection_model.selected_indices()[0]);
    model->SetSelectionFromModel(selection_model);
  }

 private:
  content::TestBrowserThreadBundle test_browser_thread_bundle_;
  content::RenderViewHostTestEnabler rvh_test_enabler_;
  const std::unique_ptr<TestingProfile> profile_;

  DISALLOW_COPY_AND_ASSIGN(TabStripModelTest);
};

TEST_F(TabStripModelTest, TestBasicAPI) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  MockTabStripModelObserver observer;
  tabstrip.AddObserver(&observer);

  EXPECT_TRUE(tabstrip.empty());

  typedef MockTabStripModelObserver::State State;

  std::unique_ptr<WebContents> contents1 = CreateWebContentsWithID(1);
  WebContents* raw_contents1 = contents1.get();

  // Note! The ordering of these tests is important, each subsequent test
  // builds on the state established in the previous. This is important if you
  // ever insert tests rather than append.

  // Test AppendWebContents, ContainsIndex
  {
    EXPECT_FALSE(tabstrip.ContainsIndex(0));
    tabstrip.AppendWebContents(std::move(contents1), true);
    EXPECT_TRUE(tabstrip.ContainsIndex(0));
    EXPECT_EQ(1, tabstrip.count());
    EXPECT_EQ(3, observer.GetStateCount());
    State s1(raw_contents1, 0, MockTabStripModelObserver::INSERT);
    s1.foreground = true;
    EXPECT_TRUE(observer.StateEquals(0, s1));
    State s2(raw_contents1, 0, MockTabStripModelObserver::ACTIVATE);
    EXPECT_TRUE(observer.StateEquals(1, s2));
    State s3(raw_contents1, 0, MockTabStripModelObserver::SELECT);
    s3.src_index = ui::ListSelectionModel::kUnselectedIndex;
    EXPECT_TRUE(observer.StateEquals(2, s3));
    observer.ClearStates();
  }
  EXPECT_EQ("1", GetTabStripStateString(tabstrip));

  // Test InsertWebContentsAt, foreground tab.
  std::unique_ptr<WebContents> contents2 = CreateWebContentsWithID(2);
  content::WebContents* raw_contents2 = contents2.get();
  {
    tabstrip.InsertWebContentsAt(1, std::move(contents2),
                                 TabStripModel::ADD_ACTIVE);

    EXPECT_EQ(2, tabstrip.count());
    EXPECT_EQ(4, observer.GetStateCount());
    State s1(raw_contents2, 1, MockTabStripModelObserver::INSERT);
    s1.foreground = true;
    EXPECT_TRUE(observer.StateEquals(0, s1));
    State s2(raw_contents1, 0, MockTabStripModelObserver::DEACTIVATE);
    EXPECT_TRUE(observer.StateEquals(1, s2));
    State s3(raw_contents2, 1, MockTabStripModelObserver::ACTIVATE);
    s3.src_contents = raw_contents1;
    EXPECT_TRUE(observer.StateEquals(2, s3));
    State s4(raw_contents2, 1, MockTabStripModelObserver::SELECT);
    s4.src_index = 0;
    EXPECT_TRUE(observer.StateEquals(3, s4));
    observer.ClearStates();
  }
  EXPECT_EQ("1 2", GetTabStripStateString(tabstrip));

  // Test InsertWebContentsAt, background tab.
  std::unique_ptr<WebContents> contents3 = CreateWebContentsWithID(3);
  WebContents* raw_contents3 = contents3.get();
  {
    tabstrip.InsertWebContentsAt(2, std::move(contents3),
                                 TabStripModel::ADD_NONE);

    EXPECT_EQ(3, tabstrip.count());
    EXPECT_EQ(1, observer.GetStateCount());
    State s1(raw_contents3, 2, MockTabStripModelObserver::INSERT);
    s1.foreground = false;
    EXPECT_TRUE(observer.StateEquals(0, s1));
    observer.ClearStates();
  }
  EXPECT_EQ("1 2 3", GetTabStripStateString(tabstrip));

  // Test ActivateTabAt
  {
    tabstrip.ActivateTabAt(2, {TabStripModel::GestureType::kOther});
    EXPECT_EQ(3, observer.GetStateCount());
    State s1(raw_contents2, 1, MockTabStripModelObserver::DEACTIVATE);
    EXPECT_TRUE(observer.StateEquals(0, s1));
    State s2(raw_contents3, 2, MockTabStripModelObserver::ACTIVATE);
    s2.src_contents = raw_contents2;
    s2.change_reason = TabStripModelObserver::CHANGE_REASON_USER_GESTURE;
    EXPECT_TRUE(observer.StateEquals(1, s2));
    State s3(raw_contents3, 2, MockTabStripModelObserver::SELECT);
    s3.src_index = 1;
    EXPECT_TRUE(observer.StateEquals(2, s3));
    observer.ClearStates();
  }
  EXPECT_EQ("1 2 3", GetTabStripStateString(tabstrip));

  // Test DetachWebContentsAt
  {
    // Detach ...
    std::unique_ptr<content::WebContents> detached_with_ownership =
        tabstrip.DetachWebContentsAt(2);
    WebContents* detached = detached_with_ownership.get();
    // ... and append again because we want this for later.
    tabstrip.AppendWebContents(std::move(detached_with_ownership), true);
    EXPECT_EQ(8, observer.GetStateCount());
    State s1(detached, 2, MockTabStripModelObserver::DETACH);
    EXPECT_TRUE(observer.StateEquals(0, s1));
    State s2(detached, 2, MockTabStripModelObserver::DEACTIVATE);
    EXPECT_TRUE(observer.StateEquals(1, s2));
    State s3(raw_contents2, 1, MockTabStripModelObserver::ACTIVATE);
    s3.src_contents = raw_contents3;
    s3.change_reason = TabStripModelObserver::CHANGE_REASON_NONE;
    EXPECT_TRUE(observer.StateEquals(2, s3));
    State s4(raw_contents2, 1, MockTabStripModelObserver::SELECT);
    s4.src_index = 2;
    EXPECT_TRUE(observer.StateEquals(3, s4));
    State s5(detached, 2, MockTabStripModelObserver::INSERT);
    s5.foreground = true;
    EXPECT_TRUE(observer.StateEquals(4, s5));
    State s6(raw_contents2, 1, MockTabStripModelObserver::DEACTIVATE);
    EXPECT_TRUE(observer.StateEquals(5, s6));
    State s7(detached, 2, MockTabStripModelObserver::ACTIVATE);
    s7.src_contents = raw_contents2;
    s7.change_reason = TabStripModelObserver::CHANGE_REASON_NONE;
    EXPECT_TRUE(observer.StateEquals(6, s7));
    State s8(detached, 2, MockTabStripModelObserver::SELECT);
    s8.src_index = 1;
    EXPECT_TRUE(observer.StateEquals(7, s8));
    observer.ClearStates();
  }
  EXPECT_EQ("1 2 3", GetTabStripStateString(tabstrip));

  // Test CloseWebContentsAt
  {
    EXPECT_TRUE(tabstrip.CloseWebContentsAt(2, TabStripModel::CLOSE_NONE));
    EXPECT_EQ(2, tabstrip.count());

    EXPECT_EQ(5, observer.GetStateCount());
    State s1(raw_contents3, 2, MockTabStripModelObserver::CLOSE);
    EXPECT_TRUE(observer.StateEquals(0, s1));
    State s2(raw_contents3, 2, MockTabStripModelObserver::DETACH);
    EXPECT_TRUE(observer.StateEquals(1, s2));
    State s3(raw_contents3, 2, MockTabStripModelObserver::DEACTIVATE);
    EXPECT_TRUE(observer.StateEquals(2, s3));
    State s4(raw_contents2, 1, MockTabStripModelObserver::ACTIVATE);
    s4.src_contents = raw_contents3;
    s4.change_reason = TabStripModelObserver::CHANGE_REASON_NONE;
    EXPECT_TRUE(observer.StateEquals(3, s4));
    State s5(raw_contents2, 1, MockTabStripModelObserver::SELECT);
    s5.src_index = 2;
    EXPECT_TRUE(observer.StateEquals(4, s5));
    observer.ClearStates();
  }
  EXPECT_EQ("1 2", GetTabStripStateString(tabstrip));

  // Test MoveWebContentsAt, select_after_move == true
  {
    tabstrip.MoveWebContentsAt(1, 0, true);

    EXPECT_EQ(2, observer.GetStateCount());
    State s1(raw_contents2, 0, MockTabStripModelObserver::MOVE);
    s1.src_index = 1;
    EXPECT_TRUE(observer.StateEquals(0, s1));
    EXPECT_EQ(0, tabstrip.active_index());
    observer.ClearStates();
  }
  EXPECT_EQ("2 1", GetTabStripStateString(tabstrip));

  // Test MoveWebContentsAt, select_after_move == false
  {
    tabstrip.MoveWebContentsAt(1, 0, false);
    EXPECT_EQ(2, observer.GetStateCount());
    State s1(raw_contents1, 0, MockTabStripModelObserver::MOVE);
    s1.src_index = 1;
    EXPECT_TRUE(observer.StateEquals(0, s1));
    EXPECT_EQ(1, tabstrip.active_index());

    tabstrip.MoveWebContentsAt(0, 1, false);
    observer.ClearStates();
  }
  EXPECT_EQ("2 1", GetTabStripStateString(tabstrip));

  // Test Getters
  {
    EXPECT_EQ(raw_contents2, tabstrip.GetActiveWebContents());
    EXPECT_EQ(raw_contents2, tabstrip.GetWebContentsAt(0));
    EXPECT_EQ(raw_contents1, tabstrip.GetWebContentsAt(1));
    EXPECT_EQ(0, tabstrip.GetIndexOfWebContents(raw_contents2));
    EXPECT_EQ(1, tabstrip.GetIndexOfWebContents(raw_contents1));
  }

  // Test UpdateWebContentsStateAt
  {
    tabstrip.UpdateWebContentsStateAt(0, TabChangeType::kAll);
    EXPECT_EQ(1, observer.GetStateCount());
    State s1(raw_contents2, 0, MockTabStripModelObserver::CHANGE);
    EXPECT_TRUE(observer.StateEquals(0, s1));
    observer.ClearStates();
  }

  // Test SelectNextTab, SelectPreviousTab, SelectLastTab
  {
    // Make sure the second of the two tabs is selected first...
    tabstrip.ActivateTabAt(1, {TabStripModel::GestureType::kOther});
    tabstrip.SelectPreviousTab();
    EXPECT_EQ(0, tabstrip.active_index());
    tabstrip.SelectLastTab();
    EXPECT_EQ(1, tabstrip.active_index());
    tabstrip.SelectNextTab();
    EXPECT_EQ(0, tabstrip.active_index());
  }

  // Test CloseSelectedTabs
  {
    tabstrip.CloseSelectedTabs();
    // |CloseSelectedTabs| calls CloseWebContentsAt, we already tested that, now
    // just verify that the count and selected index have changed
    // appropriately...
    EXPECT_EQ(1, tabstrip.count());
    EXPECT_EQ(0, tabstrip.active_index());
  }

  observer.ClearStates();
  tabstrip.CloseAllTabs();

  int close_all_count = 0, close_all_canceled_count = 0,
      close_all_completed_count = 0;
  observer.GetCloseCounts(&close_all_count, &close_all_canceled_count,
                          &close_all_completed_count);
  EXPECT_EQ(1, close_all_count);
  EXPECT_EQ(0, close_all_canceled_count);
  EXPECT_EQ(1, close_all_completed_count);

  // TabStripModel should now be empty.
  EXPECT_TRUE(tabstrip.empty());

  // Opener methods are tested below...

  tabstrip.RemoveObserver(&observer);
}

TEST_F(TabStripModelTest, TestBasicOpenerAPI) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // This is a basic test of opener functionality. opener is created
  // as the first tab in the strip and then we create 5 other tabs in the
  // background with opener set as their opener.

  std::unique_ptr<WebContents> opener = CreateWebContents();
  WebContents* raw_opener = opener.get();
  tabstrip.AppendWebContents(std::move(opener), true);
  std::unique_ptr<WebContents> contents1 = CreateWebContents();
  WebContents* raw_contents1 = contents1.get();
  std::unique_ptr<WebContents> contents2 = CreateWebContents();
  std::unique_ptr<WebContents> contents3 = CreateWebContents();
  std::unique_ptr<WebContents> contents4 = CreateWebContents();
  std::unique_ptr<WebContents> contents5 = CreateWebContents();
  WebContents* raw_contents5 = contents5.get();

  // We use |InsertWebContentsAt| here instead of |AppendWebContents| so that
  // openership relationships are preserved.
  tabstrip.InsertWebContentsAt(tabstrip.count(), std::move(contents1),
                               TabStripModel::ADD_INHERIT_OPENER);
  tabstrip.InsertWebContentsAt(tabstrip.count(), std::move(contents2),
                               TabStripModel::ADD_INHERIT_OPENER);
  tabstrip.InsertWebContentsAt(tabstrip.count(), std::move(contents3),
                               TabStripModel::ADD_INHERIT_OPENER);
  tabstrip.InsertWebContentsAt(tabstrip.count(), std::move(contents4),
                               TabStripModel::ADD_INHERIT_OPENER);
  tabstrip.InsertWebContentsAt(tabstrip.count(), std::move(contents5),
                               TabStripModel::ADD_INHERIT_OPENER);

  // All the tabs should have the same opener.
  for (int i = 1; i < tabstrip.count(); ++i)
    EXPECT_EQ(raw_opener, tabstrip.GetOpenerOfWebContentsAt(i));

  // If there is a next adjacent item, then the index should be of that item.
  EXPECT_EQ(2, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 1));
  // If the last tab in the opener tree is closed, the preceding tab in the same
  // tree should be selected.
  EXPECT_EQ(4, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 5));

  // Tests the method that finds the last tab opened by the same opener in the
  // strip (this is the insertion index for the next background tab for the
  // specified opener).
  EXPECT_EQ(5, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener, 1));

  // For a tab that has opened no other tabs, the return value should always be
  // -1...
  EXPECT_EQ(-1, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_contents1, 3));
  EXPECT_EQ(-1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_contents1, 3));

  // ForgetAllOpeners should destroy all opener relationships.
  tabstrip.ForgetAllOpeners();
  EXPECT_EQ(-1, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 1));
  EXPECT_EQ(-1, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 5));
  EXPECT_EQ(-1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener, 1));

  // Specify the last tab as the opener of the others.
  for (int i = 0; i < tabstrip.count() - 1; ++i)
    tabstrip.SetOpenerOfWebContentsAt(i, raw_contents5);

  for (int i = 0; i < tabstrip.count() - 1; ++i)
    EXPECT_EQ(raw_contents5, tabstrip.GetOpenerOfWebContentsAt(i));

  // If there is a next adjacent item, then the index should be of that item.
  EXPECT_EQ(2, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_contents5, 1));

  // If the last tab in the opener tree is closed, the preceding tab in the same
  // opener tree should be selected.
  EXPECT_EQ(3, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_contents5, 4));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

static int GetInsertionIndex(TabStripModel* tabstrip) {
  return tabstrip->order_controller()->DetermineInsertionIndex(
      ui::PAGE_TRANSITION_LINK, false);
}

static void InsertWebContentses(TabStripModel* tabstrip,
                                std::unique_ptr<WebContents> contents1,
                                std::unique_ptr<WebContents> contents2,
                                std::unique_ptr<WebContents> contents3) {
  tabstrip->InsertWebContentsAt(GetInsertionIndex(tabstrip),
                                std::move(contents1),
                                TabStripModel::ADD_INHERIT_OPENER);
  tabstrip->InsertWebContentsAt(GetInsertionIndex(tabstrip),
                                std::move(contents2),
                                TabStripModel::ADD_INHERIT_OPENER);
  tabstrip->InsertWebContentsAt(GetInsertionIndex(tabstrip),
                                std::move(contents3),
                                TabStripModel::ADD_INHERIT_OPENER);
}

// Tests opening background tabs.
TEST_F(TabStripModelTest, TestLTRInsertionOptions) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  std::unique_ptr<WebContents> opener = CreateWebContents();
  tabstrip.AppendWebContents(std::move(opener), true);

  std::unique_ptr<WebContents> contents1 = CreateWebContents();
  WebContents* raw_contents1 = contents1.get();
  std::unique_ptr<WebContents> contents2 = CreateWebContents();
  WebContents* raw_contents2 = contents2.get();
  std::unique_ptr<WebContents> contents3 = CreateWebContents();
  WebContents* raw_contents3 = contents3.get();

  // Test LTR
  InsertWebContentses(&tabstrip, std::move(contents1), std::move(contents2),
                      std::move(contents3));
  EXPECT_EQ(raw_contents1, tabstrip.GetWebContentsAt(1));
  EXPECT_EQ(raw_contents2, tabstrip.GetWebContentsAt(2));
  EXPECT_EQ(raw_contents3, tabstrip.GetWebContentsAt(3));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// This test constructs a tabstrip, and then simulates loading several tabs in
// the background from link clicks on the first tab. Then it simulates opening
// a new tab from the first tab in the foreground via a link click, verifies
// that this tab is opened adjacent to the opener, then closes it.
// Finally it tests that a tab opened for some non-link purpose opens at the
// end of the strip, not bundled to any existing context.
TEST_F(TabStripModelTest, TestInsertionIndexDetermination) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  std::unique_ptr<WebContents> opener = CreateWebContents();
  WebContents* raw_opener = opener.get();
  tabstrip.AppendWebContents(std::move(opener), true);

  // Open some other random unrelated tab in the background to monkey with our
  // insertion index.
  std::unique_ptr<WebContents> other = CreateWebContents();
  WebContents* raw_other = other.get();
  tabstrip.AppendWebContents(std::move(other), false);

  std::unique_ptr<WebContents> contents1 = CreateWebContents();
  WebContents* raw_contents1 = contents1.get();
  std::unique_ptr<WebContents> contents2 = CreateWebContents();
  WebContents* raw_contents2 = contents2.get();
  std::unique_ptr<WebContents> contents3 = CreateWebContents();
  WebContents* raw_contents3 = contents3.get();

  // Start by testing LTR.
  InsertWebContentses(&tabstrip, std::move(contents1), std::move(contents2),
                      std::move(contents3));
  EXPECT_EQ(raw_opener, tabstrip.GetWebContentsAt(0));
  EXPECT_EQ(raw_contents1, tabstrip.GetWebContentsAt(1));
  EXPECT_EQ(raw_contents2, tabstrip.GetWebContentsAt(2));
  EXPECT_EQ(raw_contents3, tabstrip.GetWebContentsAt(3));
  EXPECT_EQ(raw_other, tabstrip.GetWebContentsAt(4));

  // The opener API should work...
  EXPECT_EQ(3, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 2));
  EXPECT_EQ(2, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 3));
  EXPECT_EQ(3, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener, 1));

  // Now open a foreground tab from a link. It should be opened adjacent to the
  // opener tab.
  std::unique_ptr<WebContents> fg_link_contents = CreateWebContents();
  WebContents* raw_fg_link_contents = fg_link_contents.get();
  int insert_index = tabstrip.order_controller()->DetermineInsertionIndex(
      ui::PAGE_TRANSITION_LINK, true);
  EXPECT_EQ(1, insert_index);
  tabstrip.InsertWebContentsAt(
      insert_index, std::move(fg_link_contents),
      TabStripModel::ADD_ACTIVE | TabStripModel::ADD_INHERIT_OPENER);
  EXPECT_EQ(1, tabstrip.active_index());
  EXPECT_EQ(raw_fg_link_contents, tabstrip.GetActiveWebContents());

  // Now close this contents. The selection should move to the opener contents.
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(0, tabstrip.active_index());

  // Now open a new empty tab. It should open at the end of the strip.
  std::unique_ptr<WebContents> fg_nonlink_contents = CreateWebContents();
  WebContents* raw_fg_nonlink_contents = fg_nonlink_contents.get();
  insert_index = tabstrip.order_controller()->DetermineInsertionIndex(
      ui::PAGE_TRANSITION_AUTO_BOOKMARK, true);
  EXPECT_EQ(tabstrip.count(), insert_index);
  // We break the opener relationship...
  tabstrip.InsertWebContentsAt(insert_index, std::move(fg_nonlink_contents),
                               TabStripModel::ADD_NONE);
  // Now select it, so that GestureType != kNone causes the opener
  // relationship to be forgotten...
  tabstrip.ActivateTabAt(tabstrip.count() - 1,
                         {TabStripModel::GestureType::kOther});
  EXPECT_EQ(tabstrip.count() - 1, tabstrip.active_index());
  EXPECT_EQ(raw_fg_nonlink_contents, tabstrip.GetActiveWebContents());

  // Verify that all opener relationships are forgotten.
  EXPECT_EQ(-1, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 2));
  EXPECT_EQ(-1, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 3));
  EXPECT_EQ(-1, tabstrip.GetIndexOfNextWebContentsOpenedBy(raw_opener, 3));
  EXPECT_EQ(-1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener, 1));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests that non-adjacent tabs with an opener are ignored when deciding where
// to position tabs.
TEST_F(TabStripModelTest, TestInsertionIndexDeterminationAfterDragged) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Start with three tabs, of which the first is active.
  std::unique_ptr<WebContents> opener1 = CreateWebContentsWithID(1);
  WebContents* raw_opener1 = opener1.get();
  tabstrip.AppendWebContents(std::move(opener1), true /* foreground */);
  tabstrip.AppendWebContents(CreateWebContentsWithID(2), false);
  tabstrip.AppendWebContents(CreateWebContentsWithID(3), false);
  EXPECT_EQ("1 2 3", GetTabStripStateString(tabstrip));
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));
  EXPECT_EQ(-1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));

  // Open a link in a new background tab.
  tabstrip.InsertWebContentsAt(GetInsertionIndex(&tabstrip),
                               CreateWebContentsWithID(11),
                               TabStripModel::ADD_INHERIT_OPENER);
  EXPECT_EQ("1 11 2 3", GetTabStripStateString(tabstrip));
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));
  EXPECT_EQ(1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));

  // Drag that tab (which activates it) one to the right.
  tabstrip.MoveWebContentsAt(1, 2, true /* select_after_move */);
  EXPECT_EQ("1 2 11 3", GetTabStripStateString(tabstrip));
  EXPECT_EQ(11, GetID(tabstrip.GetActiveWebContents()));
  // It should no longer be counted by GetIndexOfLastWebContentsOpenedBy,
  // since there is a tab in between, even though its opener is unchanged.
  // TODO(johnme): Maybe its opener should be reset when it's dragged away.
  EXPECT_EQ(-1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));
  EXPECT_EQ(raw_opener1, tabstrip.GetOpenerOfWebContentsAt(2));

  // Activate the parent tab again.
  tabstrip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));

  // Open another link in a new background tab.
  tabstrip.InsertWebContentsAt(GetInsertionIndex(&tabstrip),
                               CreateWebContentsWithID(12),
                               TabStripModel::ADD_INHERIT_OPENER);
  // Tab 12 should be next to 1, and considered opened by it.
  EXPECT_EQ("1 12 2 11 3", GetTabStripStateString(tabstrip));
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));
  EXPECT_EQ(1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests that grandchild tabs are considered to be opened by their grandparent
// tab when deciding where to position tabs.
TEST_F(TabStripModelTest, TestInsertionIndexDeterminationNestedOpener) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Start with two tabs, of which the first is active:
  std::unique_ptr<WebContents> opener1 = CreateWebContentsWithID(1);
  WebContents* raw_opener1 = opener1.get();
  tabstrip.AppendWebContents(std::move(opener1), true /* foreground */);
  tabstrip.AppendWebContents(CreateWebContentsWithID(2), false);
  EXPECT_EQ("1 2", GetTabStripStateString(tabstrip));
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));
  EXPECT_EQ(-1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));

  // Open a link in a new background child tab.
  std::unique_ptr<WebContents> child11 = CreateWebContentsWithID(11);
  WebContents* raw_child11 = child11.get();
  tabstrip.InsertWebContentsAt(GetInsertionIndex(&tabstrip), std::move(child11),
                               TabStripModel::ADD_INHERIT_OPENER);
  EXPECT_EQ("1 11 2", GetTabStripStateString(tabstrip));
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));
  EXPECT_EQ(1, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));

  // Activate the child tab:
  tabstrip.ActivateTabAt(1, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(11, GetID(tabstrip.GetActiveWebContents()));

  // Open a link in a new background grandchild tab.
  tabstrip.InsertWebContentsAt(GetInsertionIndex(&tabstrip),
                               CreateWebContentsWithID(111),
                               TabStripModel::ADD_INHERIT_OPENER);
  EXPECT_EQ("1 11 111 2", GetTabStripStateString(tabstrip));
  EXPECT_EQ(11, GetID(tabstrip.GetActiveWebContents()));
  // The grandchild tab should be counted by GetIndexOfLastWebContentsOpenedBy
  // as opened by both its parent (child11) and grandparent (opener1).
  EXPECT_EQ(2, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));
  EXPECT_EQ(2, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_child11, 1));

  // Activate the parent tab again:
  tabstrip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));

  // Open another link in a new background child tab (a sibling of child11).
  tabstrip.InsertWebContentsAt(GetInsertionIndex(&tabstrip),
                               CreateWebContentsWithID(12),
                               TabStripModel::ADD_INHERIT_OPENER);
  EXPECT_EQ("1 11 111 12 2", GetTabStripStateString(tabstrip));
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));
  // opener1 has three adjacent descendants (11, 111, 12)
  EXPECT_EQ(3, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));
  // child11 has only one adjacent descendant (111)
  EXPECT_EQ(2, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_child11, 1));

  // Closing a tab should cause its children to inherit the tab's opener.
  EXPECT_EQ(true, tabstrip.CloseWebContentsAt(
                      1, TabStripModel::CLOSE_USER_GESTURE |
                             TabStripModel::CLOSE_CREATE_HISTORICAL_TAB));
  EXPECT_EQ("1 111 12 2", GetTabStripStateString(tabstrip));
  EXPECT_EQ(1, GetID(tabstrip.GetActiveWebContents()));
  // opener1 is now the opener of 111, so has two adjacent descendants (111, 12)
  EXPECT_EQ(raw_opener1, tabstrip.GetOpenerOfWebContentsAt(1));
  EXPECT_EQ(2, tabstrip.GetIndexOfLastWebContentsOpenedBy(raw_opener1, 0));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests that selection is shifted to the correct tab when a tab is closed.
// If a tab is in the background when it is closed, the selection does not
// change.
// If a tab is in the foreground (selected),
//   If that tab does not have an opener, selection shifts to the right.
//   If the tab has an opener,
//     The next tab (scanning LTR) in the entire strip that has the same opener
//     is selected
//     If there are no other tabs that have the same opener,
//       The opener is selected
TEST_F(TabStripModelTest, TestSelectOnClose) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  std::unique_ptr<WebContents> opener = CreateWebContents();
  tabstrip.AppendWebContents(std::move(opener), true);

  std::unique_ptr<WebContents> contentses[3];
  for (int i = 0; i < 3; ++i)
    contentses[i] = CreateWebContents();

  // Note that we use Detach instead of Close throughout this test to avoid
  // having to keep reconstructing these WebContentses.

  // First test that closing tabs that are in the background doesn't adjust the
  // current selection.
  InsertWebContentses(&tabstrip, std::move(contentses[0]),
                      std::move(contentses[1]), std::move(contentses[2]));
  EXPECT_EQ(0, tabstrip.active_index());

  contentses[0] = tabstrip.DetachWebContentsAt(1);
  EXPECT_EQ(0, tabstrip.active_index());

  for (int i = tabstrip.count() - 1; i >= 1; --i)
    contentses[i] = tabstrip.DetachWebContentsAt(i);

  // Now test that when a tab doesn't have an opener, selection shifts to the
  // right when the tab is closed.
  InsertWebContentses(&tabstrip, std::move(contentses[0]),
                      std::move(contentses[1]), std::move(contentses[2]));
  EXPECT_EQ(0, tabstrip.active_index());

  tabstrip.ForgetAllOpeners();
  tabstrip.ActivateTabAt(1, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(1, tabstrip.active_index());
  contentses[0] = tabstrip.DetachWebContentsAt(1);
  EXPECT_EQ(1, tabstrip.active_index());
  contentses[1] = tabstrip.DetachWebContentsAt(1);
  EXPECT_EQ(1, tabstrip.active_index());
  contentses[2] = tabstrip.DetachWebContentsAt(1);
  EXPECT_EQ(0, tabstrip.active_index());

  EXPECT_EQ(1, tabstrip.count());

  // Now test that when a tab does have an opener, it selects the next tab
  // opened by the same opener scanning LTR when it is closed.
  InsertWebContentses(&tabstrip, std::move(contentses[0]),
                      std::move(contentses[1]), std::move(contentses[2]));
  EXPECT_EQ(0, tabstrip.active_index());
  tabstrip.ActivateTabAt(2, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(2, tabstrip.active_index());
  tabstrip.CloseWebContentsAt(2, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(2, tabstrip.active_index());
  tabstrip.CloseWebContentsAt(2, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(1, tabstrip.active_index());
  tabstrip.CloseWebContentsAt(1, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(0, tabstrip.active_index());
  // Finally test that when a tab has no "siblings" that the opener is
  // selected.
  std::unique_ptr<WebContents> other_contents = CreateWebContents();
  tabstrip.InsertWebContentsAt(1, std::move(other_contents),
                               TabStripModel::ADD_NONE);
  EXPECT_EQ(2, tabstrip.count());
  std::unique_ptr<WebContents> opened_contents = CreateWebContents();
  tabstrip.InsertWebContentsAt(
      2, std::move(opened_contents),
      TabStripModel::ADD_ACTIVE | TabStripModel::ADD_INHERIT_OPENER);
  EXPECT_EQ(2, tabstrip.active_index());
  tabstrip.CloseWebContentsAt(2, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(0, tabstrip.active_index());

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests IsContextMenuCommandEnabled and ExecuteContextMenuCommand with
// CommandCloseTab.
TEST_F(TabStripModelTest, CommandCloseTab) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Make sure can_close is honored.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 1, 0, "0"));
  EXPECT_TRUE(
      tabstrip.IsContextMenuCommandEnabled(0, TabStripModel::CommandCloseTab));
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandCloseTab);
  ASSERT_TRUE(tabstrip.empty());

  // Make sure close on a tab that is selected affects all the selected tabs.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 0, "0 1"));
  EXPECT_TRUE(
      tabstrip.IsContextMenuCommandEnabled(0, TabStripModel::CommandCloseTab));
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandCloseTab);
  // Should have closed tabs 0 and 1.
  EXPECT_EQ("2", GetTabStripStateString(tabstrip));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());

  // Select two tabs and make close on a tab that isn't selected doesn't affect
  // selected tabs.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 0, "0 1"));
  EXPECT_TRUE(
      tabstrip.IsContextMenuCommandEnabled(2, TabStripModel::CommandCloseTab));
  tabstrip.ExecuteContextMenuCommand(2, TabStripModel::CommandCloseTab);
  // Should have closed tab 2.
  EXPECT_EQ("0 1", GetTabStripStateString(tabstrip));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());

  // Tests with 3 tabs, one pinned, two tab selected, one of which is pinned.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 1, "0 1"));
  EXPECT_TRUE(
      tabstrip.IsContextMenuCommandEnabled(0, TabStripModel::CommandCloseTab));
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandCloseTab);
  // Should have closed tab 2.
  EXPECT_EQ("2", GetTabStripStateString(tabstrip));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests IsContextMenuCommandEnabled and ExecuteContextMenuCommand with
// CommandCloseTabs.
TEST_F(TabStripModelTest, CommandCloseOtherTabs) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Create three tabs, select two tabs, CommandCloseOtherTabs should be enabled
  // and close two tabs.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 0, "0 1"));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      0, TabStripModel::CommandCloseOtherTabs));
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandCloseOtherTabs);
  EXPECT_EQ("0 1", GetTabStripStateString(tabstrip));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());

  // Select two tabs, CommandCloseOtherTabs should be enabled and invoking it
  // with a non-selected index should close the two other tabs.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 0, "0 1"));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      2, TabStripModel::CommandCloseOtherTabs));
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandCloseOtherTabs);
  EXPECT_EQ("0 1", GetTabStripStateString(tabstrip));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());

  // Select all, CommandCloseOtherTabs should not be enabled.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 0, "0 1 2"));
  EXPECT_FALSE(tabstrip.IsContextMenuCommandEnabled(
      2, TabStripModel::CommandCloseOtherTabs));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());

  // Three tabs, pin one, select the two non-pinned.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 1, "1 2"));
  EXPECT_FALSE(tabstrip.IsContextMenuCommandEnabled(
      1, TabStripModel::CommandCloseOtherTabs));
  // If we don't pass in the pinned index, the command should be enabled.
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      0, TabStripModel::CommandCloseOtherTabs));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());

  // 3 tabs, one pinned.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 1, "1"));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      1, TabStripModel::CommandCloseOtherTabs));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      0, TabStripModel::CommandCloseOtherTabs));
  tabstrip.ExecuteContextMenuCommand(1, TabStripModel::CommandCloseOtherTabs);
  // The pinned tab shouldn't be closed.
  EXPECT_EQ("0p 1", GetTabStripStateString(tabstrip));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests IsContextMenuCommandEnabled and ExecuteContextMenuCommand with
// CommandCloseTabsToRight.
TEST_F(TabStripModelTest, CommandCloseTabsToRight) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Create three tabs, select last two tabs, CommandCloseTabsToRight should
  // only be enabled for the first tab.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 0, "1 2"));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      0, TabStripModel::CommandCloseTabsToRight));
  EXPECT_FALSE(tabstrip.IsContextMenuCommandEnabled(
      1, TabStripModel::CommandCloseTabsToRight));
  EXPECT_FALSE(tabstrip.IsContextMenuCommandEnabled(
      2, TabStripModel::CommandCloseTabsToRight));
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandCloseTabsToRight);
  EXPECT_EQ("0", GetTabStripStateString(tabstrip));
  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests IsContextMenuCommandEnabled and ExecuteContextMenuCommand with
// CommandTogglePinned.
TEST_F(TabStripModelTest, CommandTogglePinned) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Create three tabs with one pinned, pin the first two.
  ASSERT_NO_FATAL_FAILURE(
      PrepareTabstripForSelectionTest(&tabstrip, 3, 1, "0 1"));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      0, TabStripModel::CommandTogglePinned));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      1, TabStripModel::CommandTogglePinned));
  EXPECT_TRUE(tabstrip.IsContextMenuCommandEnabled(
      2, TabStripModel::CommandTogglePinned));
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandTogglePinned);
  EXPECT_EQ("0p 1p 2", GetTabStripStateString(tabstrip));

  // Execute CommandTogglePinned again, this should unpin.
  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandTogglePinned);
  EXPECT_EQ("0 1 2", GetTabStripStateString(tabstrip));

  // Pin the last.
  tabstrip.ExecuteContextMenuCommand(2, TabStripModel::CommandTogglePinned);
  EXPECT_EQ("2p 0 1", GetTabStripStateString(tabstrip));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests the following context menu commands:
//  - Close Tab
//  - Close Other Tabs
//  - Close Tabs To Right
TEST_F(TabStripModelTest, TestContextMenuCloseCommands) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  std::unique_ptr<WebContents> opener = CreateWebContents();
  WebContents* raw_opener = opener.get();
  tabstrip.AppendWebContents(std::move(opener), true);

  std::unique_ptr<WebContents> contents1 = CreateWebContents();
  std::unique_ptr<WebContents> contents2 = CreateWebContents();
  std::unique_ptr<WebContents> contents3 = CreateWebContents();

  InsertWebContentses(&tabstrip, std::move(contents1), std::move(contents2),
                      std::move(contents3));
  EXPECT_EQ(0, tabstrip.active_index());

  tabstrip.ExecuteContextMenuCommand(2, TabStripModel::CommandCloseTab);
  EXPECT_EQ(3, tabstrip.count());

  tabstrip.ExecuteContextMenuCommand(0, TabStripModel::CommandCloseTabsToRight);
  EXPECT_EQ(1, tabstrip.count());
  EXPECT_EQ(raw_opener, tabstrip.GetActiveWebContents());

  std::unique_ptr<WebContents> dummy = CreateWebContents();
  WebContents* raw_dummy = dummy.get();
  tabstrip.AppendWebContents(std::move(dummy), false);

  contents1 = CreateWebContents();
  contents2 = CreateWebContents();
  contents3 = CreateWebContents();
  InsertWebContentses(&tabstrip, std::move(contents1), std::move(contents2),
                      std::move(contents3));
  EXPECT_EQ(5, tabstrip.count());

  int dummy_index = tabstrip.count() - 1;
  tabstrip.ActivateTabAt(dummy_index, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(raw_dummy, tabstrip.GetActiveWebContents());

  tabstrip.ExecuteContextMenuCommand(dummy_index,
                                     TabStripModel::CommandCloseOtherTabs);
  EXPECT_EQ(1, tabstrip.count());
  EXPECT_EQ(raw_dummy, tabstrip.GetActiveWebContents());

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

TEST_F(TabStripModelTest, GetIndicesClosedByCommand) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  const auto indicesClosedAsString =
      [&tabstrip](int index, TabStripModel::ContextMenuCommand id) {
        std::vector<int> indices =
            tabstrip.GetIndicesClosedByCommand(index, id);
        std::string result;
        for (size_t i = 0; i < indices.size(); ++i) {
          if (i != 0)
            result += " ";
          result += base::NumberToString(indices[i]);
        }
        return result;
      };

  for (int i = 0; i < 5; ++i)
    tabstrip.AppendWebContents(CreateWebContents(), true);

  EXPECT_EQ("4 3 2 1",
            indicesClosedAsString(0, TabStripModel::CommandCloseTabsToRight));
  EXPECT_EQ("4 3 2",
            indicesClosedAsString(1, TabStripModel::CommandCloseTabsToRight));

  EXPECT_EQ("4 3 2 1",
            indicesClosedAsString(0, TabStripModel::CommandCloseOtherTabs));
  EXPECT_EQ("4 3 2 0",
            indicesClosedAsString(1, TabStripModel::CommandCloseOtherTabs));

  // Pin the first two tabs. Pinned tabs shouldn't be closed by the close other
  // commands.
  tabstrip.SetTabPinned(0, true);
  tabstrip.SetTabPinned(1, true);

  EXPECT_EQ("4 3 2",
            indicesClosedAsString(0, TabStripModel::CommandCloseTabsToRight));
  EXPECT_EQ("4 3",
            indicesClosedAsString(2, TabStripModel::CommandCloseTabsToRight));

  EXPECT_EQ("4 3 2",
            indicesClosedAsString(0, TabStripModel::CommandCloseOtherTabs));
  EXPECT_EQ("4 3",
            indicesClosedAsString(2, TabStripModel::CommandCloseOtherTabs));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests whether or not WebContentses are inserted in the correct position
// using this "smart" function with a simulated middle click action on a series
// of links on the home page.
TEST_F(TabStripModelTest, AddWebContents_MiddleClickLinksAndClose) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Open the Home Page.
  std::unique_ptr<WebContents> homepage_contents = CreateWebContents();
  WebContents* raw_homepage_contents = homepage_contents.get();
  tabstrip.AddWebContents(std::move(homepage_contents), -1,
                          ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                          TabStripModel::ADD_ACTIVE);

  // Open some other tab, by user typing.
  std::unique_ptr<WebContents> typed_page_contents = CreateWebContents();
  WebContents* raw_typed_page_contents = typed_page_contents.get();
  tabstrip.AddWebContents(std::move(typed_page_contents), -1,
                          ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);

  EXPECT_EQ(2, tabstrip.count());

  // Re-select the home page.
  tabstrip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});

  // Open a bunch of tabs by simulating middle clicking on links on the home
  // page.
  std::unique_ptr<WebContents> middle_click_contents1 = CreateWebContents();
  WebContents* raw_middle_click_contents1 = middle_click_contents1.get();
  tabstrip.AddWebContents(std::move(middle_click_contents1), -1,
                          ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);
  std::unique_ptr<WebContents> middle_click_contents2 = CreateWebContents();
  WebContents* raw_middle_click_contents2 = middle_click_contents2.get();
  tabstrip.AddWebContents(std::move(middle_click_contents2), -1,
                          ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);
  std::unique_ptr<WebContents> middle_click_contents3 = CreateWebContents();
  WebContents* raw_middle_click_contents3 = middle_click_contents3.get();
  tabstrip.AddWebContents(std::move(middle_click_contents3), -1,
                          ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);

  EXPECT_EQ(5, tabstrip.count());

  EXPECT_EQ(raw_homepage_contents, tabstrip.GetWebContentsAt(0));
  EXPECT_EQ(raw_middle_click_contents1, tabstrip.GetWebContentsAt(1));
  EXPECT_EQ(raw_middle_click_contents2, tabstrip.GetWebContentsAt(2));
  EXPECT_EQ(raw_middle_click_contents3, tabstrip.GetWebContentsAt(3));
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetWebContentsAt(4));

  // Select a tab in the middle of the tabs opened from the home page and start
  // closing them. Each WebContents should be closed, right to left. This test
  // is constructed to start at the middle WebContents in the tree to make sure
  // the cursor wraps around to the first WebContents in the tree before
  // closing the opener or any other WebContents.
  tabstrip.ActivateTabAt(2, {TabStripModel::GestureType::kOther});
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_middle_click_contents3, tabstrip.GetActiveWebContents());
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_middle_click_contents1, tabstrip.GetActiveWebContents());
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_homepage_contents, tabstrip.GetActiveWebContents());
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetActiveWebContents());

  EXPECT_EQ(1, tabstrip.count());

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests whether or not a WebContents created by a left click on a link
// that opens a new tab is inserted correctly adjacent to the tab that spawned
// it.
TEST_F(TabStripModelTest, AddWebContents_LeftClickPopup) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Open the Home Page.
  std::unique_ptr<WebContents> homepage_contents = CreateWebContents();
  WebContents* raw_homepage_contents = homepage_contents.get();
  tabstrip.AddWebContents(std::move(homepage_contents), -1,
                          ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                          TabStripModel::ADD_ACTIVE);

  // Open some other tab, by user typing.
  std::unique_ptr<WebContents> typed_page_contents = CreateWebContents();
  WebContents* raw_typed_page_contents = typed_page_contents.get();
  tabstrip.AddWebContents(std::move(typed_page_contents), -1,
                          ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);

  EXPECT_EQ(2, tabstrip.count());

  // Re-select the home page.
  tabstrip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});

  // Open a tab by simulating a left click on a link that opens in a new tab.
  std::unique_ptr<WebContents> left_click_contents = CreateWebContents();
  WebContents* raw_left_click_contents = left_click_contents.get();
  tabstrip.AddWebContents(std::move(left_click_contents), -1,
                          ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_ACTIVE);

  // Verify the state meets our expectations.
  EXPECT_EQ(3, tabstrip.count());
  EXPECT_EQ(raw_homepage_contents, tabstrip.GetWebContentsAt(0));
  EXPECT_EQ(raw_left_click_contents, tabstrip.GetWebContentsAt(1));
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetWebContentsAt(2));

  // The newly created tab should be selected.
  EXPECT_EQ(raw_left_click_contents, tabstrip.GetActiveWebContents());

  // After closing the selected tab, the selection should move to the left, to
  // the opener.
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_homepage_contents, tabstrip.GetActiveWebContents());

  EXPECT_EQ(2, tabstrip.count());

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests whether or not new tabs that should split context (typed pages,
// generated urls, also blank tabs) open at the end of the tabstrip instead of
// in the middle.
TEST_F(TabStripModelTest, AddWebContents_CreateNewBlankTab) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Open the Home Page.
  std::unique_ptr<WebContents> homepage_contents = CreateWebContents();
  WebContents* raw_homepage_contents = homepage_contents.get();
  tabstrip.AddWebContents(std::move(homepage_contents), -1,
                          ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                          TabStripModel::ADD_ACTIVE);

  // Open some other tab, by user typing.
  std::unique_ptr<WebContents> typed_page_contents = CreateWebContents();
  WebContents* raw_typed_page_contents = typed_page_contents.get();
  tabstrip.AddWebContents(std::move(typed_page_contents), -1,
                          ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);

  EXPECT_EQ(2, tabstrip.count());

  // Re-select the home page.
  tabstrip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});

  // Open a new blank tab in the foreground.
  std::unique_ptr<WebContents> new_blank_contents = CreateWebContents();
  WebContents* raw_new_blank_contents = new_blank_contents.get();
  tabstrip.AddWebContents(std::move(new_blank_contents), -1,
                          ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);

  // Verify the state of the tabstrip.
  EXPECT_EQ(3, tabstrip.count());
  EXPECT_EQ(raw_homepage_contents, tabstrip.GetWebContentsAt(0));
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetWebContentsAt(1));
  EXPECT_EQ(raw_new_blank_contents, tabstrip.GetWebContentsAt(2));

  // Now open a couple more blank tabs in the background.
  std::unique_ptr<WebContents> background_blank_contents1 = CreateWebContents();
  WebContents* raw_background_blank_contents1 =
      background_blank_contents1.get();
  tabstrip.AddWebContents(std::move(background_blank_contents1), -1,
                          ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_NONE);
  std::unique_ptr<WebContents> background_blank_contents2 = CreateWebContents();
  WebContents* raw_background_blank_contents2 =
      background_blank_contents2.get();
  tabstrip.AddWebContents(std::move(background_blank_contents2), -1,
                          ui::PAGE_TRANSITION_GENERATED,
                          TabStripModel::ADD_NONE);
  EXPECT_EQ(5, tabstrip.count());
  EXPECT_EQ(raw_homepage_contents, tabstrip.GetWebContentsAt(0));
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetWebContentsAt(1));
  EXPECT_EQ(raw_new_blank_contents, tabstrip.GetWebContentsAt(2));
  EXPECT_EQ(raw_background_blank_contents1, tabstrip.GetWebContentsAt(3));
  EXPECT_EQ(raw_background_blank_contents2, tabstrip.GetWebContentsAt(4));

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Tests whether opener state is correctly forgotten when the user switches
// context.
TEST_F(TabStripModelTest, AddWebContents_ForgetOpeners) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Open the home page.
  std::unique_ptr<WebContents> homepage_contents = CreateWebContents();
  WebContents* raw_homepage_contents = homepage_contents.get();
  tabstrip.AddWebContents(std::move(homepage_contents), -1,
                          ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                          TabStripModel::ADD_ACTIVE);

  // Open a blank new tab.
  std::unique_ptr<WebContents> typed_page_contents = CreateWebContents();
  WebContents* raw_typed_page_contents = typed_page_contents.get();
  tabstrip.AddWebContents(std::move(typed_page_contents), -1,
                          ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);

  EXPECT_EQ(2, tabstrip.count());

  // Re-select the first tab (home page).
  tabstrip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});

  // Open a bunch of tabs by simulating middle clicking on links on the home
  // page.
  std::unique_ptr<WebContents> middle_click_contents1 = CreateWebContents();
  WebContents* raw_middle_click_contents1 = middle_click_contents1.get();
  tabstrip.AddWebContents(std::move(middle_click_contents1), -1,
                          ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);
  std::unique_ptr<WebContents> middle_click_contents2 = CreateWebContents();
  WebContents* raw_middle_click_contents2 = middle_click_contents2.get();
  tabstrip.AddWebContents(std::move(middle_click_contents2), -1,
                          ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);
  std::unique_ptr<WebContents> middle_click_contents3 = CreateWebContents();
  WebContents* raw_middle_click_contents3 = middle_click_contents3.get();
  tabstrip.AddWebContents(std::move(middle_click_contents3), -1,
                          ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);

  // Break out of the context by selecting a tab in a different context.
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetWebContentsAt(4));
  tabstrip.SelectLastTab();
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetActiveWebContents());

  // Step back into the context by selecting a tab inside it.
  tabstrip.ActivateTabAt(2, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(raw_middle_click_contents2, tabstrip.GetActiveWebContents());

  // Now test that closing tabs selects to the right until there are no more,
  // then to the left, as if there were no context (context has been
  // successfully forgotten).
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_middle_click_contents3, tabstrip.GetActiveWebContents());
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_typed_page_contents, tabstrip.GetActiveWebContents());
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_middle_click_contents1, tabstrip.GetActiveWebContents());
  tabstrip.CloseSelectedTabs();
  EXPECT_EQ(raw_homepage_contents, tabstrip.GetActiveWebContents());

  EXPECT_EQ(1, tabstrip.count());

  tabstrip.CloseAllTabs();
  EXPECT_TRUE(tabstrip.empty());
}

// Added for http://b/issue?id=958960
TEST_F(TabStripModelTest, AppendContentsReselectionTest) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  EXPECT_TRUE(tabstrip.empty());

  // Open the Home Page.
  tabstrip.AddWebContents(CreateWebContents(), -1,
                          ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                          TabStripModel::ADD_ACTIVE);

  // Open some other tab, by user typing.
  tabstrip.AddWebContents(CreateWebContents(), -1, ui::PAGE_TRANSITION_TYPED,
                          TabStripModel::ADD_NONE);

  // The selected tab should still be the first.
  EXPECT_EQ(0, tabstrip.active_index());

  // Now simulate a link click that opens a new tab (by virtue of target=_blank)
  // and make sure the correct tab gets selected when the new tab is closed.
  tabstrip.AppendWebContents(CreateWebContents(), true);
  EXPECT_EQ(2, tabstrip.active_index());
  tabstrip.CloseWebContentsAt(2, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(0, tabstrip.active_index());

  // Clean up after ourselves.
  tabstrip.CloseAllTabs();
}

// Added for http://b/issue?id=1027661
TEST_F(TabStripModelTest, ReselectionConsidersChildrenTest) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  // Open page A
  std::unique_ptr<WebContents> page_a_contents = CreateWebContents();
  WebContents* raw_page_a_contents = page_a_contents.get();
  strip.AddWebContents(std::move(page_a_contents), -1,
                       ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                       TabStripModel::ADD_ACTIVE);

  // Simulate middle click to open page A.A and A.B
  std::unique_ptr<WebContents> page_a_a_contents = CreateWebContents();
  WebContents* raw_page_a_a_contents = page_a_a_contents.get();
  strip.AddWebContents(std::move(page_a_a_contents), -1,
                       ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);
  std::unique_ptr<WebContents> page_a_b_contents = CreateWebContents();
  WebContents* raw_page_a_b_contents = page_a_b_contents.get();
  strip.AddWebContents(std::move(page_a_b_contents), -1,
                       ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);

  // Select page A.A
  strip.ActivateTabAt(1, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(raw_page_a_a_contents, strip.GetActiveWebContents());

  // Simulate a middle click to open page A.A.A
  std::unique_ptr<WebContents> page_a_a_a_contents = CreateWebContents();
  WebContents* raw_page_a_a_a_contents = page_a_a_a_contents.get();
  strip.AddWebContents(std::move(page_a_a_a_contents), -1,
                       ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);

  EXPECT_EQ(raw_page_a_a_a_contents, strip.GetWebContentsAt(2));

  // Close page A.A
  strip.CloseWebContentsAt(strip.active_index(), TabStripModel::CLOSE_NONE);

  // Page A.A.A should be selected, NOT A.B
  EXPECT_EQ(raw_page_a_a_a_contents, strip.GetActiveWebContents());

  // Close page A.A.A
  strip.CloseWebContentsAt(strip.active_index(), TabStripModel::CLOSE_NONE);

  // Page A.B should be selected
  EXPECT_EQ(raw_page_a_b_contents, strip.GetActiveWebContents());

  // Close page A.B
  strip.CloseWebContentsAt(strip.active_index(), TabStripModel::CLOSE_NONE);

  // Page A should be selected
  EXPECT_EQ(raw_page_a_contents, strip.GetActiveWebContents());

  // Clean up.
  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddWebContents_NewTabAtEndOfStripInheritsOpener) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  // Open page A
  strip.AddWebContents(CreateWebContents(), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_ACTIVE);

  // Open pages B, C and D in the background from links on page A...
  for (int i = 0; i < 3; ++i) {
    strip.AddWebContents(CreateWebContents(), -1, ui::PAGE_TRANSITION_LINK,
                         TabStripModel::ADD_NONE);
  }

  // Switch to page B's tab.
  strip.ActivateTabAt(1, {TabStripModel::GestureType::kOther});

  // Open a New Tab at the end of the strip (simulate Ctrl+T)
  std::unique_ptr<WebContents> new_contents = CreateWebContents();
  WebContents* raw_new_contents = new_contents.get();
  strip.AddWebContents(std::move(new_contents), -1, ui::PAGE_TRANSITION_TYPED,
                       TabStripModel::ADD_ACTIVE);

  EXPECT_EQ(4, strip.GetIndexOfWebContents(raw_new_contents));
  EXPECT_EQ(4, strip.active_index());

  // Close the New Tab that was just opened. We should be returned to page B's
  // Tab...
  strip.CloseWebContentsAt(4, TabStripModel::CLOSE_NONE);

  EXPECT_EQ(1, strip.active_index());

  // Open a non-New Tab tab at the end of the strip, with a TYPED transition.
  // This is like typing a URL in the address bar and pressing Alt+Enter. The
  // behavior should be the same as above.
  std::unique_ptr<WebContents> page_e_contents = CreateWebContents();
  WebContents* raw_page_e_contents = page_e_contents.get();
  strip.AddWebContents(std::move(page_e_contents), -1,
                       ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);

  EXPECT_EQ(4, strip.GetIndexOfWebContents(raw_page_e_contents));
  EXPECT_EQ(4, strip.active_index());

  // Close the Tab. Selection should shift back to page B's Tab.
  strip.CloseWebContentsAt(4, TabStripModel::CLOSE_NONE);

  EXPECT_EQ(1, strip.active_index());

  // Open a non-New Tab tab at the end of the strip, with some other
  // transition. This is like right clicking on a bookmark and choosing "Open
  // in New Tab". No opener relationship should be preserved between this Tab
  // and the one that was active when the gesture was performed.
  std::unique_ptr<WebContents> page_f_contents = CreateWebContents();
  WebContents* raw_page_f_contents = page_f_contents.get();
  strip.AddWebContents(std::move(page_f_contents), -1,
                       ui::PAGE_TRANSITION_AUTO_BOOKMARK,
                       TabStripModel::ADD_ACTIVE);

  EXPECT_EQ(4, strip.GetIndexOfWebContents(raw_page_f_contents));
  EXPECT_EQ(4, strip.active_index());

  // Close the Tab. The next-adjacent should be selected.
  strip.CloseWebContentsAt(4, TabStripModel::CLOSE_NONE);

  EXPECT_EQ(3, strip.active_index());

  // Clean up.
  strip.CloseAllTabs();
}

// A test of navigations in a tab that is part of a tree of openers from some
// parent tab. If the navigations are link clicks, the opener relationships of
// the tab. If they are of any other type, they are not preserved.
TEST_F(TabStripModelTest, NavigationForgetsOpeners) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  // Open page A
  strip.AddWebContents(CreateWebContents(), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_ACTIVE);

  // Open pages B, C and D in the background from links on page A...
  std::unique_ptr<WebContents> page_c_contents = CreateWebContents();
  WebContents* raw_page_c_contents = page_c_contents.get();
  std::unique_ptr<WebContents> page_d_contents = CreateWebContents();
  WebContents* raw_page_d_contents = page_d_contents.get();
  strip.AddWebContents(CreateWebContents(), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);
  strip.AddWebContents(std::move(page_c_contents), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);
  strip.AddWebContents(std::move(page_d_contents), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);

  // Open page E in a different opener tree from page A.
  std::unique_ptr<WebContents> page_e_contents = CreateWebContents();
  WebContents* raw_page_e_contents = page_e_contents.get();
  strip.AddWebContents(std::move(page_e_contents), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_NONE);

  // Tell the TabStripModel that we are navigating page D via a link click.
  strip.ActivateTabAt(3, {TabStripModel::GestureType::kOther});
  strip.TabNavigating(raw_page_d_contents, ui::PAGE_TRANSITION_LINK);

  // Close page D, page C should be selected. (part of same opener tree).
  strip.CloseWebContentsAt(3, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(2, strip.active_index());

  // Tell the TabStripModel that we are navigating in page C via a bookmark.
  strip.TabNavigating(raw_page_c_contents, ui::PAGE_TRANSITION_AUTO_BOOKMARK);

  // Close page C, page E should be selected. (C is no longer part of the
  // A-B-C-D tree, selection moves to the right).
  strip.CloseWebContentsAt(2, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(raw_page_e_contents, strip.GetWebContentsAt(strip.active_index()));

  strip.CloseAllTabs();
}

// A test for the "quick look" use case where the user can open a new tab at the
// end of the tab strip, do one search, and then close the tab to get back to
// where they were.
TEST_F(TabStripModelTest, NavigationForgettingDoesntAffectNewTab) {
  content::BrowserSideNavigationSetUp();

  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  // Open a tab and several tabs from it, then select one of the tabs that was
  // opened.
  strip.AddWebContents(CreateWebContents(), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_ACTIVE);

  std::unique_ptr<WebContents> page_c_contents = CreateWebContents();
  WebContents* raw_page_c_contents = page_c_contents.get();
  std::unique_ptr<WebContents> page_d_contents = CreateWebContents();
  WebContents* raw_page_d_contents = page_d_contents.get();
  strip.AddWebContents(CreateWebContents(), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);
  strip.AddWebContents(std::move(page_c_contents), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);
  strip.AddWebContents(std::move(page_d_contents), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);

  strip.ActivateTabAt(2, {TabStripModel::GestureType::kOther});

  // TEST 1: A tab in the middle of a bunch of tabs is active and the user opens
  // a new tab at the end of the strip. Closing that new tab will select the tab
  // that they were last on.

  // Open a new tab at the end of the TabStrip.
  std::unique_ptr<WebContents> new_tab_contents = CreateWebContents();
  WebContents* raw_new_tab_contents = new_tab_contents.get();
  content::WebContentsTester::For(raw_new_tab_contents)
      ->NavigateAndCommit(GURL("chrome://newtab"));
  strip.AddWebContents(std::move(new_tab_contents), -1,
                       ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);

  // The opener should still be remembered after one navigation.
  content::NavigationSimulator::CreateBrowserInitiated(
      GURL("http://example.com"), raw_new_tab_contents)
      ->Start();
  strip.TabNavigating(raw_new_tab_contents, ui::PAGE_TRANSITION_TYPED);

  // At this point, if we close this tab the last selected one should be
  // re-selected.
  strip.CloseWebContentsAt(strip.count() - 1, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(raw_page_c_contents, strip.GetWebContentsAt(strip.active_index()));

  // TEST 2: As above, but the user selects another tab in the strip and thus
  // that new tab's opener relationship is forgotten.

  // Open a new tab again.
  strip.AddWebContents(CreateWebContents(), -1, ui::PAGE_TRANSITION_TYPED,
                       TabStripModel::ADD_ACTIVE);

  // Now select the first tab.
  strip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});

  // Now select the last tab.
  strip.ActivateTabAt(strip.count() - 1, {TabStripModel::GestureType::kOther});

  // Now close the last tab. The next adjacent should be selected.
  strip.CloseWebContentsAt(strip.count() - 1, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(raw_page_d_contents, strip.GetWebContentsAt(strip.active_index()));

  // TEST 3: As above, but the user does multiple navigations and thus the tab's
  // opener relationship is forgotten.
  strip.ActivateTabAt(2, {TabStripModel::GestureType::kOther});

  // Open a new tab but navigate away from the new tab page.
  new_tab_contents = CreateWebContents();
  raw_new_tab_contents = new_tab_contents.get();
  strip.AddWebContents(std::move(new_tab_contents), -1,
                       ui::PAGE_TRANSITION_TYPED, TabStripModel::ADD_ACTIVE);
  content::WebContentsTester::For(raw_new_tab_contents)
      ->NavigateAndCommit(GURL("http://example.org"));

  // Do another navigation. The opener should be forgotten.
  content::NavigationSimulator::CreateBrowserInitiated(
      GURL("http://example.com"), raw_new_tab_contents)
      ->Start();
  strip.TabNavigating(raw_new_tab_contents, ui::PAGE_TRANSITION_TYPED);

  // Close the tab. The next adjacent should be selected.
  strip.CloseWebContentsAt(strip.count() - 1, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(raw_page_d_contents, strip.GetWebContentsAt(strip.active_index()));

  strip.CloseAllTabs();
  content::BrowserSideNavigationTearDown();
}

namespace {

class UnloadListenerTabStripModelDelegate : public TestTabStripModelDelegate {
 public:
  UnloadListenerTabStripModelDelegate() : run_unload_(false) {}
  ~UnloadListenerTabStripModelDelegate() override {}

  void set_run_unload_listener(bool value) { run_unload_ = value; }

  bool RunUnloadListenerBeforeClosing(WebContents* contents) override {
    return run_unload_;
  }

 private:
  // Whether to report that we need to run an unload listener before closing.
  bool run_unload_;

  DISALLOW_COPY_AND_ASSIGN(UnloadListenerTabStripModelDelegate);
};

}  // namespace

// Tests that fast shutdown is attempted appropriately.
TEST_F(TabStripModelTest, FastShutdown) {
  UnloadListenerTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  MockTabStripModelObserver observer;
  tabstrip.AddObserver(&observer);

  EXPECT_TRUE(tabstrip.empty());

  // Make sure fast shutdown is attempted when tabs that share a RPH are shut
  // down.
  {
    std::unique_ptr<WebContents> contents1 = CreateWebContents();
    WebContents* raw_contents1 = contents1.get();
    std::unique_ptr<WebContents> contents2 =
        CreateWebContentsWithSharedRPH(contents1.get());

    SetID(contents1.get(), 1);
    SetID(contents2.get(), 2);

    tabstrip.AppendWebContents(std::move(contents1), true);
    tabstrip.AppendWebContents(std::move(contents2), true);

    // Turn on the fake unload listener so the tabs don't actually get shut
    // down when we call CloseAllTabs()---we need to be able to check that
    // fast shutdown was attempted.
    delegate.set_run_unload_listener(true);
    tabstrip.CloseAllTabs();
    // On a mock RPH this checks whether we *attempted* fast shutdown.
    // A real RPH would reject our attempt since there is an unload handler.
    EXPECT_TRUE(
        raw_contents1->GetMainFrame()->GetProcess()->FastShutdownStarted());
    EXPECT_EQ(2, tabstrip.count());

    delegate.set_run_unload_listener(false);
    tabstrip.CloseAllTabs();
    EXPECT_TRUE(tabstrip.empty());
  }

  // Make sure fast shutdown is not attempted when only some tabs that share a
  // RPH are shut down.
  {
    std::unique_ptr<WebContents> contents1 = CreateWebContents();
    WebContents* raw_contents1 = contents1.get();
    std::unique_ptr<WebContents> contents2 =
        CreateWebContentsWithSharedRPH(contents1.get());

    SetID(contents1.get(), 1);
    SetID(contents2.get(), 2);

    tabstrip.AppendWebContents(std::move(contents1), true);
    tabstrip.AppendWebContents(std::move(contents2), true);

    tabstrip.CloseWebContentsAt(1, TabStripModel::CLOSE_NONE);
    EXPECT_FALSE(
        raw_contents1->GetMainFrame()->GetProcess()->FastShutdownStarted());
    EXPECT_EQ(1, tabstrip.count());

    tabstrip.CloseAllTabs();
    EXPECT_TRUE(tabstrip.empty());
  }
}

// Tests various permutations of pinning tabs.
TEST_F(TabStripModelTest, Pinning) {
  TestTabStripModelDelegate delegate;
  TabStripModel tabstrip(&delegate, profile());
  MockTabStripModelObserver observer;
  tabstrip.AddObserver(&observer);

  EXPECT_TRUE(tabstrip.empty());

  typedef MockTabStripModelObserver::State State;

  std::unique_ptr<WebContents> contents1 = CreateWebContentsWithID(1);
  WebContents* raw_contents1 = contents1.get();
  std::unique_ptr<WebContents> contents3 = CreateWebContentsWithID(3);
  WebContents* raw_contents3 = contents3.get();

  // Note! The ordering of these tests is important, each subsequent test
  // builds on the state established in the previous. This is important if you
  // ever insert tests rather than append.

  // Initial state, three tabs, first selected.
  tabstrip.AppendWebContents(std::move(contents1), true);
  tabstrip.AppendWebContents(CreateWebContentsWithID(2), false);
  tabstrip.AppendWebContents(std::move(contents3), false);

  observer.ClearStates();

  // Pin the first tab, this shouldn't visually reorder anything.
  {
    tabstrip.SetTabPinned(0, true);

    // As the order didn't change, we should get a pinned notification.
    ASSERT_EQ(1, observer.GetStateCount());
    State state(raw_contents1, 0, MockTabStripModelObserver::PINNED);
    EXPECT_TRUE(observer.StateEquals(0, state));

    // And verify the state.
    EXPECT_EQ("1p 2 3", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  // Unpin the first tab.
  {
    tabstrip.SetTabPinned(0, false);

    // As the order didn't change, we should get a pinned notification.
    ASSERT_EQ(1, observer.GetStateCount());
    State state(raw_contents1, 0, MockTabStripModelObserver::PINNED);
    EXPECT_TRUE(observer.StateEquals(0, state));

    // And verify the state.
    EXPECT_EQ("1 2 3", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  // Pin the 3rd tab, which should move it to the front.
  {
    tabstrip.SetTabPinned(2, true);

    // The pinning should have resulted in a move and a pinned notification.
    ASSERT_EQ(3, observer.GetStateCount());
    State state(raw_contents3, 0, MockTabStripModelObserver::MOVE);
    state.src_index = 2;
    EXPECT_TRUE(observer.StateEquals(0, state));

    state = State(raw_contents3, 0, MockTabStripModelObserver::PINNED);
    EXPECT_TRUE(observer.StateEquals(2, state));

    // And verify the state.
    EXPECT_EQ("3p 1 2", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  // Pin the tab "1", which shouldn't move anything.
  {
    tabstrip.SetTabPinned(1, true);

    // As the order didn't change, we should get a pinned notification.
    ASSERT_EQ(1, observer.GetStateCount());
    State state(raw_contents1, 1, MockTabStripModelObserver::PINNED);
    EXPECT_TRUE(observer.StateEquals(0, state));

    // And verify the state.
    EXPECT_EQ("3p 1p 2", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  // Try to move tab "2" to the front, it should be ignored.
  {
    tabstrip.MoveWebContentsAt(2, 0, false);

    // As the order didn't change, we should get a pinned notification.
    ASSERT_EQ(0, observer.GetStateCount());

    // And verify the state.
    EXPECT_EQ("3p 1p 2", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  // Unpin tab "3", which implicitly moves it to the end.
  {
    tabstrip.SetTabPinned(0, false);

    ASSERT_EQ(3, observer.GetStateCount());
    State state(raw_contents3, 1, MockTabStripModelObserver::MOVE);
    state.src_index = 0;
    EXPECT_TRUE(observer.StateEquals(0, state));

    state = State(raw_contents3, 1, MockTabStripModelObserver::PINNED);
    EXPECT_TRUE(observer.StateEquals(2, state));

    // And verify the state.
    EXPECT_EQ("1p 3 2", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  // Unpin tab "3", nothing should happen.
  {
    tabstrip.SetTabPinned(1, false);

    ASSERT_EQ(0, observer.GetStateCount());

    EXPECT_EQ("1p 3 2", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  // Pin "3" and "1".
  {
    tabstrip.SetTabPinned(0, true);
    tabstrip.SetTabPinned(1, true);

    EXPECT_EQ("1p 3p 2", GetTabStripStateString(tabstrip));

    observer.ClearStates();
  }

  std::unique_ptr<WebContents> contents4 = CreateWebContentsWithID(4);
  WebContents* raw_contents4 = contents4.get();

  // Insert "4" between "1" and "3". As "1" and "4" are pinned, "4" should end
  // up after them.
  {
    tabstrip.InsertWebContentsAt(1, std::move(contents4),
                                 TabStripModel::ADD_NONE);

    ASSERT_EQ(1, observer.GetStateCount());
    State state(raw_contents4, 2, MockTabStripModelObserver::INSERT);
    EXPECT_TRUE(observer.StateEquals(0, state));

    EXPECT_EQ("1p 3p 4 2", GetTabStripStateString(tabstrip));
  }

  tabstrip.CloseAllTabs();
}

// Makes sure the TabStripModel calls the right observer methods during a
// replace.
TEST_F(TabStripModelTest, ReplaceSendsSelected) {
  typedef MockTabStripModelObserver::State State;

  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  std::unique_ptr<WebContents> first_contents = CreateWebContents();
  WebContents* raw_first_contents = first_contents.get();
  strip.AddWebContents(std::move(first_contents), -1, ui::PAGE_TRANSITION_TYPED,
                       TabStripModel::ADD_ACTIVE);

  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);

  std::unique_ptr<WebContents> new_contents = CreateWebContents();
  WebContents* raw_new_contents = new_contents.get();
  strip.ReplaceWebContentsAt(0, std::move(new_contents));

  ASSERT_EQ(2, observer.GetStateCount());

  // First event should be for replaced.
  State state(raw_new_contents, 0, MockTabStripModelObserver::REPLACED);
  state.src_contents = raw_first_contents;
  EXPECT_TRUE(observer.StateEquals(0, state));

  // And the second for selected.
  state = State(raw_new_contents, 0, MockTabStripModelObserver::ACTIVATE);
  state.src_contents = raw_first_contents;
  state.change_reason = TabStripModelObserver::CHANGE_REASON_REPLACED;
  EXPECT_TRUE(observer.StateEquals(1, state));

  // Now add another tab and replace it, making sure we don't get a selected
  // event this time.
  std::unique_ptr<WebContents> third_contents = CreateWebContents();
  WebContents* raw_third_contents = third_contents.get();
  strip.AddWebContents(std::move(third_contents), 1, ui::PAGE_TRANSITION_TYPED,
                       TabStripModel::ADD_NONE);

  observer.ClearStates();

  // And replace it.
  new_contents = CreateWebContents();
  raw_new_contents = new_contents.get();
  strip.ReplaceWebContentsAt(1, std::move(new_contents));

  ASSERT_EQ(1, observer.GetStateCount());

  state = State(raw_new_contents, 1, MockTabStripModelObserver::REPLACED);
  state.src_contents = raw_third_contents;
  EXPECT_TRUE(observer.StateEquals(0, state));

  strip.CloseAllTabs();
}

// Ensure pinned tabs are not mixed with non-pinned tabs when using
// MoveWebContentsAt.
TEST_F(TabStripModelTest, MoveWebContentsAtWithPinned) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  ASSERT_NO_FATAL_FAILURE(PrepareTabstripForSelectionTest(&strip, 6, 3, "0"));
  EXPECT_EQ("0p 1p 2p 3 4 5", GetTabStripStateString(strip));

  // Move middle tabs into the wrong area.
  strip.MoveWebContentsAt(1, 5, true);
  EXPECT_EQ("0p 2p 1p 3 4 5", GetTabStripStateString(strip));
  strip.MoveWebContentsAt(4, 1, true);
  EXPECT_EQ("0p 2p 1p 4 3 5", GetTabStripStateString(strip));

  // Test moving edge cases into the wrong area.
  strip.MoveWebContentsAt(5, 0, true);
  EXPECT_EQ("0p 2p 1p 5 4 3", GetTabStripStateString(strip));
  strip.MoveWebContentsAt(0, 5, true);
  EXPECT_EQ("2p 1p 0p 5 4 3", GetTabStripStateString(strip));

  // Test moving edge cases in the correct area.
  strip.MoveWebContentsAt(3, 5, true);
  EXPECT_EQ("2p 1p 0p 4 3 5", GetTabStripStateString(strip));
  strip.MoveWebContentsAt(2, 0, true);
  EXPECT_EQ("0p 2p 1p 4 3 5", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, MoveTabNext) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  ASSERT_NO_FATAL_FAILURE(PrepareTabstripForSelectionTest(&strip, 6, 3, "3"));
  EXPECT_EQ("0p 1p 2p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabNext();
  EXPECT_EQ("0p 1p 2p 4 3 5", GetTabStripStateString(strip));

  strip.MoveTabNext();
  EXPECT_EQ("0p 1p 2p 4 5 3", GetTabStripStateString(strip));

  strip.MoveTabNext();
  EXPECT_EQ("0p 1p 2p 4 5 3", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, MoveTabNext_Pinned) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  ASSERT_NO_FATAL_FAILURE(PrepareTabstripForSelectionTest(&strip, 6, 3, "0"));
  EXPECT_EQ("0p 1p 2p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabNext();
  EXPECT_EQ("1p 0p 2p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabNext();
  EXPECT_EQ("1p 2p 0p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabNext();
  EXPECT_EQ("1p 2p 0p 3 4 5", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, MoveTabPrevious) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  ASSERT_NO_FATAL_FAILURE(PrepareTabstripForSelectionTest(&strip, 6, 3, "5"));
  EXPECT_EQ("0p 1p 2p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabPrevious();
  EXPECT_EQ("0p 1p 2p 3 5 4", GetTabStripStateString(strip));

  strip.MoveTabPrevious();
  EXPECT_EQ("0p 1p 2p 5 3 4", GetTabStripStateString(strip));

  strip.MoveTabPrevious();
  EXPECT_EQ("0p 1p 2p 5 3 4", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, MoveTabPrevious_Pinned) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  ASSERT_NO_FATAL_FAILURE(PrepareTabstripForSelectionTest(&strip, 6, 3, "2"));
  EXPECT_EQ("0p 1p 2p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabPrevious();
  EXPECT_EQ("0p 2p 1p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabPrevious();
  EXPECT_EQ("2p 0p 1p 3 4 5", GetTabStripStateString(strip));

  strip.MoveTabPrevious();
  EXPECT_EQ("2p 0p 1p 3 4 5", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, MoveSelectedTabsTo) {
  struct TestData {
    // Number of tabs the tab strip should have.
    const int tab_count;

    // Number of pinned tabs.
    const int pinned_count;

    // Index of the tabs to select.
    const std::string selected_tabs;

    // Index to move the tabs to.
    const int target_index;

    // Expected state after the move (space separated list of indices).
    const std::string state_after_move;
  } test_data[] = {
      // 1 selected tab.
      {2, 0, "0", 1, "1 0"},
      {3, 0, "0", 2, "1 2 0"},
      {3, 0, "2", 0, "2 0 1"},
      {3, 0, "2", 1, "0 2 1"},
      {3, 0, "0 1", 0, "0 1 2"},

      // 2 selected tabs.
      {6, 0, "4 5", 1, "0 4 5 1 2 3"},
      {3, 0, "0 1", 1, "2 0 1"},
      {4, 0, "0 2", 1, "1 0 2 3"},
      {6, 0, "0 1", 3, "2 3 4 0 1 5"},

      // 3 selected tabs.
      {6, 0, "0 2 3", 3, "1 4 5 0 2 3"},
      {7, 0, "4 5 6", 1, "0 4 5 6 1 2 3"},
      {7, 0, "1 5 6", 4, "0 2 3 4 1 5 6"},

      // 5 selected tabs.
      {8, 0, "0 2 3 6 7", 3, "1 4 5 0 2 3 6 7"},

      // 7 selected tabs
      {16, 0, "0 1 2 3 4 7 9", 8, "5 6 8 10 11 12 13 14 0 1 2 3 4 7 9 15"},

      // With pinned tabs.
      {6, 2, "2 3", 2, "0p 1p 2 3 4 5"},
      {6, 2, "0 4", 3, "1p 0p 2 3 4 5"},
      {6, 3, "1 2 4", 0, "1p 2p 0p 4 3 5"},
      {8, 3, "1 3 4", 4, "0p 2p 1p 5 6 3 4 7"},

      {7, 4, "2 3 4", 3, "0p 1p 2p 3p 5 4 6"},
  };

  for (size_t i = 0; i < base::size(test_data); ++i) {
    TestTabStripModelDelegate delegate;
    TabStripModel strip(&delegate, profile());
    ASSERT_NO_FATAL_FAILURE(PrepareTabstripForSelectionTest(
        &strip, test_data[i].tab_count, test_data[i].pinned_count,
        test_data[i].selected_tabs));
    strip.MoveSelectedTabsTo(test_data[i].target_index);
    EXPECT_EQ(test_data[i].state_after_move, GetTabStripStateString(strip))
        << i;
    strip.CloseAllTabs();
  }
}

// Tests that moving a tab forgets all openers referencing it.
TEST_F(TabStripModelTest, MoveSelectedTabsTo_ForgetOpeners) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  // Open page A as a new tab and then A1 in the background from A.
  std::unique_ptr<WebContents> page_a_contents = CreateWebContents();
  WebContents* raw_page_a_contents = page_a_contents.get();
  strip.AddWebContents(std::move(page_a_contents), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_ACTIVE);
  std::unique_ptr<WebContents> page_a1_contents = CreateWebContents();
  WebContents* raw_page_a1_contents = page_a1_contents.get();
  strip.AddWebContents(std::move(page_a1_contents), -1,
                       ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);

  // Likewise, open pages B and B1.
  std::unique_ptr<WebContents> page_b_contents = CreateWebContents();
  WebContents* raw_page_b_contents = page_b_contents.get();
  strip.AddWebContents(std::move(page_b_contents), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_ACTIVE);
  std::unique_ptr<WebContents> page_b1_contents = CreateWebContents();
  WebContents* raw_page_b1_contents = page_b1_contents.get();
  strip.AddWebContents(std::move(page_b1_contents), -1,
                       ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);

  EXPECT_EQ(raw_page_a_contents, strip.GetWebContentsAt(0));
  EXPECT_EQ(raw_page_a1_contents, strip.GetWebContentsAt(1));
  EXPECT_EQ(raw_page_b_contents, strip.GetWebContentsAt(2));
  EXPECT_EQ(raw_page_b1_contents, strip.GetWebContentsAt(3));

  // Move page B to the start of the tab strip.
  strip.MoveSelectedTabsTo(0);

  // Open page B2 in the background from B. It should end up after B.
  std::unique_ptr<WebContents> page_b2_contents = CreateWebContents();
  WebContents* raw_page_b2_contents = page_b2_contents.get();
  strip.AddWebContents(std::move(page_b2_contents), -1,
                       ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);
  EXPECT_EQ(raw_page_b_contents, strip.GetWebContentsAt(0));
  EXPECT_EQ(raw_page_b2_contents, strip.GetWebContentsAt(1));
  EXPECT_EQ(raw_page_a_contents, strip.GetWebContentsAt(2));
  EXPECT_EQ(raw_page_a1_contents, strip.GetWebContentsAt(3));
  EXPECT_EQ(raw_page_b1_contents, strip.GetWebContentsAt(4));

  // Switch to A.
  strip.ActivateTabAt(2, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(raw_page_a_contents, strip.GetActiveWebContents());

  // Open page A2 in the background from A. It should end up after A1.
  std::unique_ptr<WebContents> page_a2_contents = CreateWebContents();
  WebContents* raw_page_a2_contents = page_a2_contents.get();
  strip.AddWebContents(std::move(page_a2_contents), -1,
                       ui::PAGE_TRANSITION_LINK, TabStripModel::ADD_NONE);
  EXPECT_EQ(raw_page_b_contents, strip.GetWebContentsAt(0));
  EXPECT_EQ(raw_page_b2_contents, strip.GetWebContentsAt(1));
  EXPECT_EQ(raw_page_a_contents, strip.GetWebContentsAt(2));
  EXPECT_EQ(raw_page_a1_contents, strip.GetWebContentsAt(3));
  EXPECT_EQ(raw_page_a2_contents, strip.GetWebContentsAt(4));
  EXPECT_EQ(raw_page_b1_contents, strip.GetWebContentsAt(5));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, CloseSelectedTabs) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  for (int i = 0; i < 3; ++i)
    strip.AppendWebContents(CreateWebContents(), true);
  strip.ToggleSelectionAt(1);
  strip.CloseSelectedTabs();
  EXPECT_EQ(1, strip.count());
  EXPECT_EQ(0, strip.active_index());
  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, MultipleSelection) {
  typedef MockTabStripModelObserver::State State;

  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;
  std::unique_ptr<WebContents> contents0 = CreateWebContents();
  WebContents* raw_contents0 = contents0.get();
  std::unique_ptr<WebContents> contents3 = CreateWebContents();
  WebContents* raw_contents3 = contents3.get();
  strip.AppendWebContents(std::move(contents0), false);
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AppendWebContents(std::move(contents3), false);
  strip.AddObserver(&observer);

  // Selection and active tab change.
  strip.ActivateTabAt(3, {TabStripModel::GestureType::kOther});
  ASSERT_EQ(2, observer.GetStateCount());
  ASSERT_EQ(observer.GetStateAt(0).action, MockTabStripModelObserver::ACTIVATE);
  State s1(raw_contents3, 3, MockTabStripModelObserver::SELECT);
  EXPECT_TRUE(observer.StateEquals(1, s1));
  observer.ClearStates();

  // Adding all tabs to selection, active tab is now at 0.
  strip.ExtendSelectionTo(0);
  ASSERT_EQ(3, observer.GetStateCount());
  ASSERT_EQ(observer.GetStateAt(0).action,
            MockTabStripModelObserver::DEACTIVATE);
  ASSERT_EQ(observer.GetStateAt(1).action, MockTabStripModelObserver::ACTIVATE);
  State s2(raw_contents0, 0, MockTabStripModelObserver::SELECT);
  s2.src_index = 3;
  EXPECT_TRUE(observer.StateEquals(2, s2));
  observer.ClearStates();

  // Toggle the active tab, should make the next index active.
  strip.ToggleSelectionAt(0);
  EXPECT_EQ(1, strip.active_index());
  EXPECT_EQ(3U, strip.selection_model().size());
  EXPECT_EQ(4, strip.count());
  ASSERT_EQ(3, observer.GetStateCount());
  ASSERT_EQ(observer.GetStateAt(0).action,
            MockTabStripModelObserver::DEACTIVATE);
  ASSERT_EQ(observer.GetStateAt(1).action, MockTabStripModelObserver::ACTIVATE);
  ASSERT_EQ(observer.GetStateAt(2).action, MockTabStripModelObserver::SELECT);
  observer.ClearStates();

  // Toggle the first tab back to selected and active.
  strip.ToggleSelectionAt(0);
  EXPECT_EQ(0, strip.active_index());
  EXPECT_EQ(4U, strip.selection_model().size());
  EXPECT_EQ(4, strip.count());
  ASSERT_EQ(3, observer.GetStateCount());
  ASSERT_EQ(observer.GetStateAt(0).action,
            MockTabStripModelObserver::DEACTIVATE);
  ASSERT_EQ(observer.GetStateAt(1).action, MockTabStripModelObserver::ACTIVATE);
  ASSERT_EQ(observer.GetStateAt(2).action, MockTabStripModelObserver::SELECT);
  observer.ClearStates();

  // Closing one of the selected tabs, not the active one.
  strip.CloseWebContentsAt(1, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(3, strip.count());
  ASSERT_EQ(3, observer.GetStateCount());
  ASSERT_EQ(observer.GetStateAt(0).action, MockTabStripModelObserver::CLOSE);
  ASSERT_EQ(observer.GetStateAt(1).action, MockTabStripModelObserver::DETACH);
  ASSERT_EQ(observer.GetStateAt(2).action, MockTabStripModelObserver::SELECT);
  observer.ClearStates();

  // Closing the active tab, while there are others tabs selected.
  strip.CloseWebContentsAt(0, TabStripModel::CLOSE_NONE);
  EXPECT_EQ(2, strip.count());
  ASSERT_EQ(5, observer.GetStateCount());
  ASSERT_EQ(observer.GetStateAt(0).action, MockTabStripModelObserver::CLOSE);
  ASSERT_EQ(observer.GetStateAt(1).action, MockTabStripModelObserver::DETACH);
  ASSERT_EQ(observer.GetStateAt(2).action,
            MockTabStripModelObserver::DEACTIVATE);
  ASSERT_EQ(observer.GetStateAt(3).action, MockTabStripModelObserver::ACTIVATE);
  ASSERT_EQ(observer.GetStateAt(4).action, MockTabStripModelObserver::SELECT);
  observer.ClearStates();

  // Active tab is at 0, deselecting all but the active tab.
  strip.ToggleSelectionAt(1);
  ASSERT_EQ(1, observer.GetStateCount());
  ASSERT_EQ(observer.GetStateAt(0).action, MockTabStripModelObserver::SELECT);
  observer.ClearStates();

  // Attempting to deselect the only selected and therefore active tab,
  // it is ignored (no notifications being sent) and tab at 0 remains selected
  // and active.
  strip.ToggleSelectionAt(0);
  ASSERT_EQ(0, observer.GetStateCount());

  strip.RemoveObserver(&observer);
  strip.CloseAllTabs();
}

// Verifies that if we change the selection from a multi selection to a single
// selection, but not in a way that changes the selected_index that
// TabSelectionChanged is invoked.
TEST_F(TabStripModelTest, MultipleToSingle) {
  typedef MockTabStripModelObserver::State State;

  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  std::unique_ptr<WebContents> contents2 = CreateWebContents();
  WebContents* raw_contents2 = contents2.get();
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AppendWebContents(std::move(contents2), false);
  strip.ToggleSelectionAt(0);
  strip.ToggleSelectionAt(1);

  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);
  // This changes the selection (0 is no longer selected) but the selected_index
  // still remains at 1.
  strip.ActivateTabAt(1, {TabStripModel::GestureType::kOther});
  ASSERT_EQ(1, observer.GetStateCount());
  State s(raw_contents2, 1, MockTabStripModelObserver::SELECT);
  s.src_index = 1;
  s.change_reason = TabStripModelObserver::CHANGE_REASON_NONE;
  EXPECT_TRUE(observer.StateEquals(0, s));
  strip.RemoveObserver(&observer);
  strip.CloseAllTabs();
}

namespace {

// Test Browser-like class for TabStripModelTest.TabBlockedState.
class TabBlockedStateTestBrowser
    : public TabStripModelObserver,
      public web_modal::WebContentsModalDialogManagerDelegate {
 public:
  explicit TabBlockedStateTestBrowser(TabStripModel* tab_strip_model)
      : tab_strip_model_(tab_strip_model) {
    tab_strip_model_->AddObserver(this);
  }

  ~TabBlockedStateTestBrowser() override {
    tab_strip_model_->RemoveObserver(this);
  }

 private:
  // TabStripModelObserver
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    if (change.type() != TabStripModelChange::kInserted)
      return;

    for (const auto& contents : change.GetInsert()->contents) {
      web_modal::WebContentsModalDialogManager* manager =
          web_modal::WebContentsModalDialogManager::FromWebContents(
              contents.contents);
      if (manager)
        manager->SetDelegate(this);
    }
  }

  // WebContentsModalDialogManagerDelegate
  void SetWebContentsBlocked(content::WebContents* contents,
                             bool blocked) override {
    int index = tab_strip_model_->GetIndexOfWebContents(contents);

    // Removal of tabs from the TabStripModel can cause observer callbacks to
    // invoke this method. The WebContents may no longer exist in the
    // TabStripModel.
    if (index == TabStripModel::kNoTab)
      return;

    tab_strip_model_->SetTabBlocked(index, blocked);
  }

  TabStripModel* tab_strip_model_;

  DISALLOW_COPY_AND_ASSIGN(TabBlockedStateTestBrowser);
};

class DummySingleWebContentsDialogManager
    : public web_modal::SingleWebContentsDialogManager {
 public:
  explicit DummySingleWebContentsDialogManager(
      gfx::NativeWindow dialog,
      web_modal::SingleWebContentsDialogManagerDelegate* delegate)
      : delegate_(delegate), dialog_(dialog) {}
  ~DummySingleWebContentsDialogManager() override {}

  void Show() override {}
  void Hide() override {}
  void Close() override { delegate_->WillClose(dialog_); }
  void Focus() override {}
  void Pulse() override {}
  void HostChanged(web_modal::WebContentsModalDialogHost* new_host) override {}
  gfx::NativeWindow dialog() override { return dialog_; }

 private:
  web_modal::SingleWebContentsDialogManagerDelegate* delegate_;
  gfx::NativeWindow dialog_;

  DISALLOW_COPY_AND_ASSIGN(DummySingleWebContentsDialogManager);
};

}  // namespace

// Verifies a newly inserted tab retains its previous blocked state.
// http://crbug.com/276334
TEST_F(TabStripModelTest, TabBlockedState) {
  // Start with a source tab strip.
  TestTabStripModelDelegate dummy_tab_strip_delegate;
  TabStripModel strip_src(&dummy_tab_strip_delegate, profile());
  TabBlockedStateTestBrowser browser_src(&strip_src);

  // Add a tab.
  std::unique_ptr<WebContents> contents1 = CreateWebContents();
  web_modal::WebContentsModalDialogManager::CreateForWebContents(
      contents1.get());
  strip_src.AppendWebContents(std::move(contents1), /*foreground=*/true);

  // Add another tab.
  std::unique_ptr<WebContents> contents2 = CreateWebContents();
  WebContents* raw_contents2 = contents2.get();
  web_modal::WebContentsModalDialogManager::CreateForWebContents(
      contents2.get());
  strip_src.AppendWebContents(std::move(contents2), false);

  // Create a destination tab strip.
  TabStripModel strip_dst(&dummy_tab_strip_delegate, profile());
  TabBlockedStateTestBrowser browser_dst(&strip_dst);

  // Setup a SingleWebContentsDialogManager for tab |contents2|.
  web_modal::WebContentsModalDialogManager* modal_dialog_manager =
      web_modal::WebContentsModalDialogManager::FromWebContents(raw_contents2);

  // Show a dialog that blocks tab |contents2|.
  // DummySingleWebContentsDialogManager doesn't care about the
  // dialog window value, so any dummy value works.
  DummySingleWebContentsDialogManager* native_manager =
      new DummySingleWebContentsDialogManager(gfx::kNullNativeWindow,
                                              modal_dialog_manager);
  modal_dialog_manager->ShowDialogWithManager(
      gfx::kNullNativeWindow,
      std::unique_ptr<web_modal::SingleWebContentsDialogManager>(
          native_manager));
  EXPECT_TRUE(strip_src.IsTabBlocked(1));

  // Detach the tab.
  std::unique_ptr<WebContents> moved_contents =
      strip_src.DetachWebContentsAt(1);
  EXPECT_EQ(raw_contents2, moved_contents.get());

  // Attach the tab to the destination tab strip.
  strip_dst.AppendWebContents(std::move(moved_contents), true);
  EXPECT_TRUE(strip_dst.IsTabBlocked(0));

  strip_dst.CloseAllTabs();
  strip_src.CloseAllTabs();
}

// Verifies ordering of tabs opened via a link from a pinned tab with a
// subsequent pinned tab.
TEST_F(TabStripModelTest, LinkClicksWithPinnedTabOrdering) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  // Open two pages, pinned.
  strip.AddWebContents(CreateWebContents(), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_ACTIVE | TabStripModel::ADD_PINNED);
  strip.AddWebContents(CreateWebContents(), -1,
                       ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                       TabStripModel::ADD_ACTIVE | TabStripModel::ADD_PINNED);

  // Activate the first tab (a).
  strip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});

  // Open two more tabs as link clicks. The first tab, c, should appear after
  // the pinned tabs followed by the second tab (d).
  std::unique_ptr<WebContents> page_c_contents = CreateWebContents();
  WebContents* raw_page_c_contents = page_c_contents.get();
  std::unique_ptr<WebContents> page_d_contents = CreateWebContents();
  WebContents* raw_page_d_contents = page_d_contents.get();
  strip.AddWebContents(std::move(page_c_contents), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);
  strip.AddWebContents(std::move(page_d_contents), -1, ui::PAGE_TRANSITION_LINK,
                       TabStripModel::ADD_NONE);

  EXPECT_EQ(2, strip.GetIndexOfWebContents(raw_page_c_contents));
  EXPECT_EQ(3, strip.GetIndexOfWebContents(raw_page_d_contents));
  strip.CloseAllTabs();
}

// This test covers a bug in TabStripModel::MoveWebContentsAt(). Specifically
// if |select_after_move| was true it checked if the index
// select_after_move (as an int) was selected rather than |to_position|.
TEST_F(TabStripModelTest, MoveWebContentsAt) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;

  strip.AppendWebContents(CreateWebContents(), false);
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AddObserver(&observer);

  strip.ActivateTabAt(1, {TabStripModel::GestureType::kOther});
  EXPECT_EQ(1, strip.active_index());

  strip.MoveWebContentsAt(2, 3, true);
  EXPECT_EQ(3, strip.active_index());

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, NewTabsUngrouped) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), false);

  EXPECT_FALSE(strip.GetTabGroupForTab(0).has_value());

  strip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});
  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), false);

  strip.AddToNewGroup({0});

  EXPECT_TRUE(strip.GetTabGroupForTab(0).has_value());

  strip.ActivateTabAt(0, {TabStripModel::GestureType::kOther});
  strip.CloseAllTabs();
}

namespace {

MockTabStripModelObserver::State ExpectedGroupChangeState(
    const TabStripModel& strip,
    int tab_index,
    base::Optional<TabGroupId> old_group,
    base::Optional<TabGroupId> new_group) {
  MockTabStripModelObserver::State state(
      strip.GetWebContentsAt(tab_index), tab_index,
      MockTabStripModelObserver::GROUP_CHANGED);
  state.old_group = old_group;
  state.new_group = new_group;
  return state;
}

}  // namespace

TEST_F(TabStripModelTest, AddTabToNewGroupUpdatesObservers) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);
  strip.AppendWebContents(CreateWebContents(), true);

  observer.ClearStates();
  strip.AddToNewGroup({0});
  EXPECT_EQ(1, observer.GetStateCount());
  observer.StateEquals(0, ExpectedGroupChangeState(strip, 0, base::nullopt,
                                                   strip.GetTabGroupForTab(0)));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, ReplacingTabGroupUpdatesObservers) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);
  strip.AppendWebContents(CreateWebContents(), true);

  observer.ClearStates();
  strip.AddToNewGroup({0});
  base::Optional<TabGroupId> first_group = strip.GetTabGroupForTab(0);
  strip.AddToNewGroup({0});
  EXPECT_EQ(2, observer.GetStateCount());
  observer.StateEquals(
      0, ExpectedGroupChangeState(strip, 0, base::nullopt, first_group));
  observer.StateEquals(1, ExpectedGroupChangeState(strip, 0, first_group,
                                                   strip.GetTabGroupForTab(0)));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroupMiddleOfExistingGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 4);
  strip.AddToNewGroup({0, 1, 2, 3});
  base::Optional<TabGroupId> first_group = strip.GetTabGroupForTab(0);

  strip.AddToNewGroup({1, 2});

  EXPECT_EQ(strip.GetTabGroupForTab(0), first_group);
  EXPECT_EQ(strip.GetTabGroupForTab(1), first_group);
  EXPECT_NE(strip.GetTabGroupForTab(2), first_group);
  EXPECT_EQ(strip.GetTabGroupForTab(2), strip.GetTabGroupForTab(3));
  EXPECT_EQ("0 3 1 2", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroupMiddleOfExistingGroupTwoGroups) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 4);
  strip.AddToNewGroup({0, 1, 2});
  strip.AddToNewGroup({3});

  strip.AddToNewGroup({1});
  EXPECT_NE(strip.GetTabGroupForTab(2), strip.GetTabGroupForTab(0));
  EXPECT_EQ("0 2 1 3", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroupReorders) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 3);

  strip.AddToNewGroup({0, 2});

  EXPECT_EQ("0 2 1", GetTabStripStateString(strip));
  EXPECT_EQ(strip.GetTabGroupForTab(0), strip.GetTabGroupForTab(1));
  EXPECT_TRUE(strip.GetTabGroupForTab(0).has_value());
  EXPECT_FALSE(strip.GetTabGroupForTab(2).has_value());

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroupPins) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 2);

  strip.SetTabPinned(0, true);
  strip.AddToNewGroup({0, 1});
  EXPECT_EQ("0p 1p", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroupPinsAndReorders) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 3);

  strip.SetTabPinned(0, true);
  strip.AddToNewGroup({0, 2});
  EXPECT_EQ("0p 2p 1", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroupMovePinnedTabRight) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 4);

  strip.SetTabPinned(0, true);
  strip.SetTabPinned(1, true);
  strip.SetTabPinned(2, true);
  strip.AddToNewGroup({0, 1, 2});

  strip.AddToNewGroup({1, 3});
  EXPECT_EQ("0p 2p 1p 3p", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToNewGroupMovePinnedTabLeft) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 5);

  strip.SetTabPinned(0, true);
  strip.SetTabPinned(1, true);
  strip.SetTabPinned(2, true);
  strip.SetTabPinned(3, true);
  strip.AddToNewGroup({0, 1, 2});

  strip.AddToNewGroup({1, 3, 4});
  EXPECT_EQ("0p 2p 1p 3p 4p", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroupIdempotent) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);
  strip.AppendWebContents(CreateWebContents(), true);

  strip.AddToNewGroup({0});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);
  strip.AddToExistingGroup({0}, group.value());

  observer.ClearStates();
  EXPECT_EQ(strip.GetTabGroupForTab(0), group);
  EXPECT_EQ(0, observer.GetStateCount());

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 2);

  strip.AddToNewGroup({0});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.AddToExistingGroup({1}, group.value());

  EXPECT_EQ(strip.GetTabGroupForTab(1), group);
  EXPECT_EQ("0 1", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroupUpdatesObservers) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);
  PrepareTabs(&strip, 2);

  strip.AddToNewGroup({0});
  observer.ClearStates();
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.AddToExistingGroup({1}, group.value());
  EXPECT_EQ(1, observer.GetStateCount());
  observer.StateEquals(
      0, ExpectedGroupChangeState(strip, 1, base::nullopt, group));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroupReordersToTheRight) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 2);

  strip.AddToNewGroup({1});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(1);

  strip.AddToExistingGroup({0}, group.value());
  EXPECT_EQ(strip.GetTabGroupForTab(0), group);
  EXPECT_EQ(strip.GetTabGroupForTab(1), group);
  EXPECT_EQ("1 0", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroupReordersToTheLeft) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 3);

  strip.AddToNewGroup({0});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.AddToExistingGroup({2}, group.value());
  EXPECT_EQ(strip.GetTabGroupForTab(0), group);
  EXPECT_EQ(strip.GetTabGroupForTab(1), group);
  EXPECT_FALSE(strip.GetTabGroupForTab(2).has_value());
  EXPECT_EQ("0 2 1", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroupReorders) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 4);

  strip.AddToNewGroup({1});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(1);

  strip.AddToExistingGroup({0, 3}, group.value());
  EXPECT_EQ(strip.GetTabGroupForTab(0), group);
  EXPECT_EQ(strip.GetTabGroupForTab(1), group);
  EXPECT_EQ(strip.GetTabGroupForTab(2), group);
  EXPECT_FALSE(strip.GetTabGroupForTab(3).has_value());
  EXPECT_EQ("1 0 3 2", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroupPins) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 2);

  strip.SetTabPinned(0, true);
  strip.AddToNewGroup({0});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.AddToExistingGroup({1}, group.value());
  EXPECT_EQ("0p 1p", GetTabStripStateString(strip));
  EXPECT_EQ(strip.GetTabGroupForTab(1), group);

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddTabToExistingGroupUnpins) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 2);

  strip.SetTabPinned(0, true);
  strip.AddToNewGroup({1});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(1);

  strip.AddToExistingGroup({0}, group.value());
  EXPECT_FALSE(strip.IsTabPinned(0));
  EXPECT_EQ(strip.GetTabGroupForTab(0), group);
  EXPECT_EQ("1 0", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroupNoopForUngroupedTab) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);
  strip.AppendWebContents(CreateWebContents(), true);

  observer.ClearStates();
  strip.RemoveFromGroup({0});
  EXPECT_EQ(0, observer.GetStateCount());

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AddToNewGroup({0});

  strip.RemoveFromGroup({0});

  EXPECT_FALSE(strip.GetTabGroupForTab(0).has_value());

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroupUpdatesObservers) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  MockTabStripModelObserver observer;
  strip.AddObserver(&observer);
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AppendWebContents(CreateWebContents(), false);

  strip.AddToNewGroup({0});
  observer.ClearStates();
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.RemoveFromGroup({0});
  EXPECT_EQ(1, observer.GetStateCount());
  observer.StateEquals(
      0, ExpectedGroupChangeState(strip, 0, group, base::nullopt));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroupReorders) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 2);
  strip.AddToNewGroup({0, 1});

  strip.RemoveFromGroup({0});

  EXPECT_FALSE(strip.GetTabGroupForTab(1).has_value());
  EXPECT_TRUE(strip.GetTabGroupForTab(0).has_value());
  EXPECT_EQ("1 0", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroupDoesntReorderIfNoGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 3);
  strip.AddToNewGroup({0});

  strip.RemoveFromGroup({0, 1});
  EXPECT_EQ("0 1 2", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroupMaintainsOrderOfSelectedTabs) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 4);
  strip.AddToNewGroup({0, 1, 2, 3});

  strip.RemoveFromGroup({0, 2});

  EXPECT_TRUE(strip.GetTabGroupForTab(0).has_value());
  EXPECT_TRUE(strip.GetTabGroupForTab(1).has_value());
  EXPECT_FALSE(strip.GetTabGroupForTab(2).has_value());
  EXPECT_FALSE(strip.GetTabGroupForTab(3).has_value());
  EXPECT_EQ("1 3 0 2", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroupMixtureOfGroups) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  PrepareTabs(&strip, 5);
  strip.AddToNewGroup({0, 1});
  strip.AddToNewGroup({2, 3});

  strip.RemoveFromGroup({0, 3, 4});

  EXPECT_TRUE(strip.GetTabGroupForTab(0).has_value());
  EXPECT_FALSE(strip.GetTabGroupForTab(1).has_value());
  EXPECT_TRUE(strip.GetTabGroupForTab(2).has_value());
  EXPECT_FALSE(strip.GetTabGroupForTab(3).has_value());
  EXPECT_FALSE(strip.GetTabGroupForTab(4).has_value());
  EXPECT_EQ("1 0 2 3 4", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, RemoveTabFromGroupDeletesGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AddToNewGroup({0});
  EXPECT_EQ(strip.ListTabGroups().size(), 1U);

  strip.RemoveFromGroup({0});

  EXPECT_EQ(strip.ListTabGroups().size(), 0U);

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddToNewGroupDeletesGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AddToNewGroup({0});
  EXPECT_EQ(strip.ListTabGroups().size(), 1U);
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.AddToNewGroup({0});

  EXPECT_EQ(strip.ListTabGroups().size(), 1U);
  EXPECT_NE(strip.GetTabGroupForTab(0), group);

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, AddToExistingGroupDeletesGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AddToNewGroup({0});
  strip.AddToNewGroup({1});
  EXPECT_EQ(strip.ListTabGroups().size(), 2U);
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(1);

  strip.AddToExistingGroup({1}, strip.GetTabGroupForTab(0).value());

  EXPECT_EQ(strip.ListTabGroups().size(), 1U);
  EXPECT_NE(strip.ListTabGroups()[0], group);

  strip.CloseAllTabs();
}

TEST_F(TabStripModelTest, CloseTabDeletesGroup) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AddToNewGroup({0});
  EXPECT_EQ(strip.ListTabGroups().size(), 1U);

  strip.CloseWebContentsAt(0, TabStripModel::CLOSE_USER_GESTURE);

  EXPECT_EQ(strip.ListTabGroups().size(), 0U);
}

TEST_F(TabStripModelTest, CloseTabNotifiesObserversOfGroupChange) {
  TestTabStripModelDelegate delegate;
  MockTabStripModelObserver observer;
  TabStripModel strip(&delegate, profile());
  strip.AddObserver(&observer);
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AddToNewGroup({0});
  base::Optional<TabGroupId> old_group = strip.GetTabGroupForTab(0);
  observer.ClearStates();

  strip.CloseWebContentsAt(0, TabStripModel::CLOSE_USER_GESTURE);

  EXPECT_GT(observer.GetStateCount(), 0);
  int num_group_changed_notifications = 0;
  for (int i = 0; i < observer.GetStateCount(); i++) {
    if (observer.GetStateAt(i).action ==
        MockTabStripModelObserver::GROUP_CHANGED) {
      observer.StateEquals(
          i, ExpectedGroupChangeState(strip, 0, old_group, base::nullopt));
      num_group_changed_notifications++;
    }
  }
  EXPECT_EQ(num_group_changed_notifications, 1);
}

// When inserting a WebContents, if a group is not specified, the new tab
// should be left ungrouped.
TEST_F(TabStripModelTest, InsertWebContentsAtDoesNotGroupByDefault) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContents(), true);
  strip.AppendWebContents(CreateWebContents(), false);
  strip.AddToNewGroup({0, 1});

  strip.InsertWebContentsAt(2, CreateWebContents(), TabStripModel::ADD_NONE);

  // The newly added tab should not be in the group.
  EXPECT_TRUE(strip.GetTabGroupForTab(0).has_value());
  EXPECT_TRUE(strip.GetTabGroupForTab(1).has_value());
  EXPECT_FALSE(strip.GetTabGroupForTab(2).has_value());

  strip.CloseAllTabs();
}

// When inserting a WebContents, if a group is specified, the new tab should be
// added to that group.
TEST_F(TabStripModelTest, InsertWebContentsAtWithGroupGroups) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContentsWithID(0), true);
  strip.AppendWebContents(CreateWebContentsWithID(1), false);
  strip.AddToNewGroup({0, 1});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.InsertWebContentsAt(1, CreateWebContentsWithID(2),
                            TabStripModel::ADD_NONE, group);

  EXPECT_EQ(strip.GetTabGroupForTab(0), group);
  EXPECT_EQ(strip.GetTabGroupForTab(1), group);
  EXPECT_EQ(strip.GetTabGroupForTab(2), group);
  EXPECT_EQ("0 2 1", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

// When inserting a WebContents, if a group is specified and the tabs in that
// group are pinned, the new tab should be pinned also.
TEST_F(TabStripModelTest, InsertWebContentsAtWithPinnedGroupPins) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());
  strip.AppendWebContents(CreateWebContentsWithID(0), true);
  strip.AppendWebContents(CreateWebContentsWithID(1), false);
  strip.SetTabPinned(0, true);
  strip.SetTabPinned(1, true);
  strip.AddToNewGroup({0, 1});
  base::Optional<TabGroupId> group = strip.GetTabGroupForTab(0);

  strip.InsertWebContentsAt(1, CreateWebContentsWithID(2),
                            TabStripModel::ADD_NONE, group);
  EXPECT_EQ("0p 2p 1p", GetTabStripStateString(strip));

  strip.CloseAllTabs();
}

namespace {

// Checks whether a sequence of tabs have contiguous groups. |group_per_tab|
// defines the group ID for each tab in the sequence.
bool GroupsAreContiguous(
    base::span<const base::Optional<TabGroupId>> group_per_tab) {
  base::flat_set<TabGroupId> seen_groups;
  base::Optional<TabGroupId> last_seen_group;
  for (int tab_index = 0; tab_index < static_cast<int>(group_per_tab.size());
       ++tab_index) {
    // If the current tab is grouped and its group ID is not the same as the
    // last seen tab, we have found a new or discontiguous group. Checking
    // |seen_groups| tells us which.
    if (group_per_tab[tab_index].has_value() &&
        group_per_tab[tab_index] != last_seen_group) {
      if (base::Contains(seen_groups, group_per_tab[tab_index].value()))
        return false;
      seen_groups.insert(group_per_tab[tab_index].value());
    }

    last_seen_group = group_per_tab[tab_index];
  }

  return true;
}

// Printing a base::Optional<TabGroupId> gives absolutely unreadable
// results. For more readable output, this assigns successive integers to each
// seen group, starting at 1. It prints those to the string, substituting "*"
// for non-grouped tabs.  Sample output: "* 1 1 * * 2 2 2"
std::string PrintGroupsToString(
    base::span<const base::Optional<TabGroupId>> group_per_tab) {
  if (group_per_tab.empty())
    return "";

  int next_group = 1;
  base::flat_map<TabGroupId, int> group_to_output;
  std::string result;
  for (const base::Optional<TabGroupId>& cur : group_per_tab) {
    if (!cur.has_value()) {
      result += "* ";
      continue;
    }

    if (!base::Contains(group_to_output, cur.value()))
      group_to_output[cur.value()] = next_group++;

    result += base::NumberToString(group_to_output[cur.value()]) + " ";
  }

  result.pop_back();
  return result;
}

class TabStripGroupContiguityChecker : public TabStripModelObserver {
 public:
  explicit TabStripGroupContiguityChecker(TabStripModel* model)
      : model_(model) {
    // This must be used on a TabStripModel with no tabs so we can accurately
    // track the group of each tab at each observer update.
    EXPECT_EQ(0, model_->count());
    model_->AddObserver(this);
  }
  ~TabStripGroupContiguityChecker() override { model_->RemoveObserver(this); }

  void OnTabStripModelChanged(
      TabStripModel* model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override {
    ASSERT_EQ(model_, model);
    // Update our list of tabs' groups, |group_of_tab_|, according to the
    // observer notification.
    switch (change.type()) {
      case TabStripModelChange::kInserted: {
        const TabStripModelChange::Insert* insert = change.GetInsert();
        for (const TabStripModelChange::ContentsWithIndex& data :
             insert->contents) {
          ASSERT_LE(data.index, static_cast<int>(group_of_tab_.size()));
          group_of_tab_.insert(group_of_tab_.begin() + data.index,
                               model_->GetTabGroupForTab(data.index));
        }
        break;
      }
      case TabStripModelChange::kRemoved: {
        const TabStripModelChange::Remove* remove = change.GetRemove();
        for (const TabStripModelChange::ContentsWithIndex& data :
             remove->contents) {
          ASSERT_LT(data.index, static_cast<int>(group_of_tab_.size()));
          group_of_tab_.erase(group_of_tab_.begin() + data.index);
        }
        break;
      }
      case TabStripModelChange::kMoved: {
        const TabStripModelChange::Move* move = change.GetMove();
        ASSERT_LT(move->from_index, static_cast<int>(group_of_tab_.size()));
        ASSERT_LT(move->to_index, static_cast<int>(group_of_tab_.size()));
        auto from_iter = group_of_tab_.begin() + move->from_index;
        auto to_iter = group_of_tab_.begin() + move->to_index;
        if (from_iter <= to_iter) {
          std::rotate(from_iter, from_iter + 1, to_iter + 1);
        } else {
          std::rotate(to_iter, from_iter, from_iter + 1);
        }
        break;
      }
      case TabStripModelChange::kGroupChanged: {
        const TabStripModelChange::GroupChange* group_change =
            change.GetGroupChange();
        ASSERT_LT(group_change->index, static_cast<int>(group_of_tab_.size()));
        EXPECT_EQ(group_change->old_group, group_of_tab_[group_change->index]);
        group_of_tab_[group_change->index] = group_change->new_group;
        break;
      }
      case TabStripModelChange::kSelectionOnly:
        break;
      case TabStripModelChange::kReplaced:
        break;
      default:
        NOTREACHED();
        break;
    }

    EXPECT_TRUE(GroupsAreContiguous(group_of_tab_))
        << "Groups: " << PrintGroupsToString(group_of_tab_);
  }

 private:
  TabStripModel* const model_;
  std::vector<base::Optional<TabGroupId>> group_of_tab_;
};

}  // namespace

// Group contiguity is an invariant of the TabStripModel. It should also send
// observer updates in a way that preserves this invariant. Otherwise,
// reconstructing the TabStripModel state from a sequence of observer
// notifications could result in an invalid state. This is important for
// sessions code.
//
// TODO(crbug.com/971491): this doesn't test all cases because some cases do
// result in an inconsistent state. For example, removing a tab in the middle of
// a group sends the ungroup update before the remove update. Once this is
// fixed, this test should be updated for full coverage.
TEST_F(TabStripModelTest, GroupsAreAlwaysContiguousForObservers) {
  TestTabStripModelDelegate delegate;
  TabStripModel strip(&delegate, profile());

  // This observers |strip|. On each update it receives, it updates its internal
  // model of the tab strip and checks that all groups are contiguous in it.
  TabStripGroupContiguityChecker checker(&strip);

  // Add a few tabs to start out.
  for (int i = 0; i < 6; ++i)
    strip.AppendWebContents(CreateWebContents(), true);

  // Since failures will happen from inside |checker|, we use |SCOPED_TRACE()|
  // to see where they come from.

  base::Optional<TabGroupId> group1, group2;
  {
    SCOPED_TRACE("Creating initial groups");
    // Do some grouping, getting some tab reordering along the way.
    group1 = strip.AddToNewGroup({1, 2});
    group2 = strip.AddToNewGroup({4, 5});
    strip.AddToExistingGroup({3}, group1.value());
    strip.AddToExistingGroup({0}, group2.value());
  }
  // Groups: 1 1 1 2 2 2

  {
    ASSERT_EQ(6, strip.count());
    SCOPED_TRACE("Remove tab at group beginning");
    strip.DetachWebContentsAt(3);
  }
  // Groups: 1 1 1 2 2

  {
    ASSERT_EQ(5, strip.count());
    SCOPED_TRACE("Insert ungrouped tab between groups");
    strip.InsertWebContentsAt(3, CreateWebContents(), TabStripModel::ADD_NONE);
  }
  // Groups: 1 1 1 * 2 2

  {
    ASSERT_EQ(6, strip.count());
    SCOPED_TRACE("Move ungrouped tab from between groups to end of group");
    strip.MoveWebContentsAt(3, 5, false);
  }
  // Groups: 1 1 1 2 2 *

  {
    ASSERT_EQ(6, strip.count());
    SCOPED_TRACE("Insert grouped tab into middle of group");
    strip.InsertWebContentsAt(4, CreateWebContents(), TabStripModel::ADD_NONE,
                              group2);
  }
  // Groups: 1 1 1 2 2 2 *

  // The following test currently fails, but at some point should pass:
  //
  // {
  //   ASSERT_EQ(7, strip.count());
  //   SCOPED_TRACE("Remove tab in middle of group");
  //   strip.DetachWebContentsAt(1);
  // }
  // // Groups: 1 1 2 2 2 *

  strip.CloseAllTabs();
}
