// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/keyboard/ash_keyboard_controller.h"

#include <memory>
#include <set>
#include <utility>

#include "ash/keyboard/ui/container_behavior.h"
#include "ash/keyboard/ui/keyboard_controller.h"
#include "ash/keyboard/ui/test/keyboard_test_util.h"
#include "ash/public/cpp/keyboard/keyboard_controller.h"
#include "ash/public/cpp/test/test_keyboard_controller_observer.h"
#include "ash/shell.h"
#include "ash/test/ash_test_base.h"
#include "ash/test/ash_test_helper.h"
#include "base/bind.h"
#include "base/run_loop.h"
#include "base/stl_util.h"
#include "base/test/bind_test_util.h"
#include "base/test/scoped_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/aura/window.h"
#include "ui/display/manager/display_manager.h"
#include "ui/display/test/display_manager_test_api.h"

using keyboard::KeyboardConfig;
using keyboard::KeyboardEnableFlag;

namespace ash {

namespace {

class TestContainerBehavior : public keyboard::ContainerBehavior {
 public:
  TestContainerBehavior() : keyboard::ContainerBehavior(nullptr) {}
  ~TestContainerBehavior() override = default;

  // keyboard::ContainerBehavior
  void DoShowingAnimation(
      aura::Window* window,
      ui::ScopedLayerAnimationSettings* animation_settings) override {}

  void DoHidingAnimation(
      aura::Window* window,
      wm::ScopedHidingAnimationSettings* animation_settings) override {}

  void InitializeShowAnimationStartingState(aura::Window* container) override {}

  gfx::Rect AdjustSetBoundsRequest(
      const gfx::Rect& display_bounds,
      const gfx::Rect& requested_bounds_in_screen_coords) override {
    return gfx::Rect();
  }

  void SetCanonicalBounds(aura::Window* container,
                          const gfx::Rect& display_bounds) override {}

  bool IsOverscrollAllowed() const override { return true; }

  void SavePosition(const gfx::Rect& keyboard_bounds_in_screen,
                    const gfx::Size& screen_size) override {}

  bool HandlePointerEvent(const ui::LocatedEvent& event,
                          const display::Display& current_display) override {
    return false;
  }

  keyboard::ContainerType GetType() const override { return type_; }

  bool TextBlurHidesKeyboard() const override { return false; }

  void SetOccludedBounds(const gfx::Rect& bounds) override {
    occluded_bounds_ = bounds;
  }

  gfx::Rect GetOccludedBounds(
      const gfx::Rect& visual_bounds_in_window) const override {
    return occluded_bounds_;
  }

  bool OccludedBoundsAffectWorkspaceLayout() const override { return false; }

  void SetDraggableArea(const gfx::Rect& rect) override {
    draggable_area_ = rect;
  }

  const gfx::Rect& occluded_bounds() const { return occluded_bounds_; }
  const gfx::Rect& draggable_area() const { return draggable_area_; }

 private:
  keyboard::ContainerType type_ = keyboard::ContainerType::kFullWidth;
  gfx::Rect occluded_bounds_;
  gfx::Rect draggable_area_;
};

class AshKeyboardControllerTest : public AshTestBase {
 public:
  AshKeyboardControllerTest() = default;
  ~AshKeyboardControllerTest() override = default;

  void SetUp() override {
    AshTestBase::SetUp();

    // Set the initial observer config to the default config.
    test_observer()->set_config(ash_keyboard_controller()->GetKeyboardConfig());
  }

  void TearDown() override {
    AshTestBase::TearDown();
  }

  AshKeyboardController* ash_keyboard_controller() {
    return Shell::Get()->ash_keyboard_controller();
  }
  keyboard::KeyboardController* keyboard_controller() {
    return ash_keyboard_controller()->keyboard_controller();
  }
  TestKeyboardControllerObserver* test_observer() {
    return ash_test_helper()->test_keyboard_controller_observer();
  }

 protected:
  bool SetContainerType(keyboard::ContainerType container_type,
                        const base::Optional<gfx::Rect>& target_bounds) {
    bool result = false;
    base::RunLoop run_loop;
    ash_keyboard_controller()->SetContainerType(
        container_type, target_bounds,
        base::BindLambdaForTesting([&](bool success) {
          result = success;
          run_loop.QuitWhenIdle();
        }));
    run_loop.Run();
    return result;
  }

  aura::Window* GetPrimaryRootWindow() { return Shell::GetPrimaryRootWindow(); }

  aura::Window* GetSecondaryRootWindow() {
    aura::Window::Windows root_windows = Shell::GetAllRootWindows();
    return root_windows[0] == GetPrimaryRootWindow() ? root_windows[1]
                                                     : root_windows[0];
  }

  void CreateFocusedTestWindowInRootWindow(aura::Window* root_window) {
    // Owned by |root_window|.
    aura::Window* focusable_window =
        CreateTestWindowInShellWithBounds(root_window->GetBoundsInScreen());
    focusable_window->Focus();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(AshKeyboardControllerTest);
};

}  // namespace

TEST_F(AshKeyboardControllerTest, SetKeyboardConfig) {
  // Enable the keyboard so that config changes trigger observer events.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  ash_keyboard_controller()->GetKeyboardConfig();
  KeyboardConfig config = ash_keyboard_controller()->GetKeyboardConfig();
  // Set the observer config to the default config.
  test_observer()->set_config(config);

  // Change the keyboard config.
  bool old_auto_complete = config.auto_complete;
  config.auto_complete = !config.auto_complete;
  ash_keyboard_controller()->SetKeyboardConfig(std::move(config));

  // Test that the config changes.
  ash_keyboard_controller()->GetKeyboardConfig();
  EXPECT_NE(old_auto_complete,
            ash_keyboard_controller()->GetKeyboardConfig().auto_complete);

  // Test that the test observer received the change.
  EXPECT_NE(old_auto_complete, test_observer()->config().auto_complete);
}

TEST_F(AshKeyboardControllerTest, EnableFlags) {
  EXPECT_FALSE(ash_keyboard_controller()->IsKeyboardEnabled());
  // Enable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);
  std::set<keyboard::KeyboardEnableFlag> enable_flags =
      ash_keyboard_controller()->GetEnableFlags();
  EXPECT_TRUE(
      base::Contains(enable_flags, KeyboardEnableFlag::kExtensionEnabled));
  EXPECT_EQ(enable_flags, test_observer()->enable_flags());
  EXPECT_TRUE(ash_keyboard_controller()->IsKeyboardEnabled());

  // Set the enable override to disable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(KeyboardEnableFlag::kPolicyDisabled);
  enable_flags = ash_keyboard_controller()->GetEnableFlags();
  EXPECT_TRUE(
      base::Contains(enable_flags, KeyboardEnableFlag::kExtensionEnabled));
  EXPECT_TRUE(
      base::Contains(enable_flags, KeyboardEnableFlag::kPolicyDisabled));
  EXPECT_EQ(enable_flags, test_observer()->enable_flags());
  EXPECT_FALSE(ash_keyboard_controller()->IsKeyboardEnabled());

  // Clear the enable override; should enable the keyboard.
  ash_keyboard_controller()->ClearEnableFlag(
      KeyboardEnableFlag::kPolicyDisabled);
  enable_flags = ash_keyboard_controller()->GetEnableFlags();
  EXPECT_TRUE(
      base::Contains(enable_flags, KeyboardEnableFlag::kExtensionEnabled));
  EXPECT_FALSE(
      base::Contains(enable_flags, KeyboardEnableFlag::kPolicyDisabled));
  EXPECT_EQ(enable_flags, test_observer()->enable_flags());
  EXPECT_TRUE(ash_keyboard_controller()->IsKeyboardEnabled());
}

TEST_F(AshKeyboardControllerTest, RebuildKeyboardIfEnabled) {
  EXPECT_EQ(0, test_observer()->destroyed_count());

  // Enable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);
  EXPECT_EQ(0, test_observer()->destroyed_count());

  // Enable the keyboard again; this should not reload the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);
  EXPECT_EQ(0, test_observer()->destroyed_count());

  // Rebuild the keyboard. This should destroy the previous keyboard window.
  ash_keyboard_controller()->RebuildKeyboardIfEnabled();
  EXPECT_EQ(1, test_observer()->destroyed_count());

  // Disable the keyboard. The keyboard window should be destroyed.
  ash_keyboard_controller()->ClearEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);
  EXPECT_EQ(2, test_observer()->destroyed_count());
}

TEST_F(AshKeyboardControllerTest, ShowAndHideKeyboard) {
  // Enable the keyboard. This will create the keyboard window but not show it.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  ASSERT_TRUE(keyboard_controller()->GetKeyboardWindow());
  EXPECT_FALSE(keyboard_controller()->GetKeyboardWindow()->IsVisible());

  // The keyboard needs to be in a loaded state before being shown.
  ASSERT_TRUE(keyboard::test::WaitUntilLoaded());

  ash_keyboard_controller()->ShowKeyboard();
  EXPECT_TRUE(keyboard_controller()->GetKeyboardWindow()->IsVisible());

  ash_keyboard_controller()->HideKeyboard(HideReason::kUser);
  EXPECT_FALSE(keyboard_controller()->GetKeyboardWindow()->IsVisible());

  // TODO(stevenjb): Also use TestKeyboardControllerObserver and
  // IsKeyboardVisible to test visibility changes. https://crbug.com/849995.
}

TEST_F(AshKeyboardControllerTest, SetContainerType) {
  // Enable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);
  const auto default_behavior = keyboard::ContainerType::kFullWidth;
  EXPECT_EQ(default_behavior, keyboard_controller()->GetActiveContainerType());

  gfx::Rect target_bounds(0, 0, 10, 10);
  // Set the container type to kFloating.
  EXPECT_TRUE(
      SetContainerType(keyboard::ContainerType::kFloating, target_bounds));
  EXPECT_EQ(keyboard::ContainerType::kFloating,
            keyboard_controller()->GetActiveContainerType());
  // Ensure that the window size is correct (position is determined by Ash).
  EXPECT_EQ(
      target_bounds.size(),
      keyboard_controller()->GetKeyboardWindow()->GetTargetBounds().size());

  // Setting the container type to the current type should fail.
  EXPECT_FALSE(
      SetContainerType(keyboard::ContainerType::kFloating, base::nullopt));
  EXPECT_EQ(keyboard::ContainerType::kFloating,
            keyboard_controller()->GetActiveContainerType());
}

TEST_F(AshKeyboardControllerTest, SetKeyboardLocked) {
  ASSERT_FALSE(keyboard_controller()->keyboard_locked());
  ash_keyboard_controller()->SetKeyboardLocked(true);
  EXPECT_TRUE(keyboard_controller()->keyboard_locked());
  ash_keyboard_controller()->SetKeyboardLocked(false);
  EXPECT_FALSE(keyboard_controller()->keyboard_locked());
}

TEST_F(AshKeyboardControllerTest, SetOccludedBounds) {
  // Enable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  // Override the container behavior.
  auto scoped_behavior = std::make_unique<TestContainerBehavior>();
  TestContainerBehavior* behavior = scoped_behavior.get();
  keyboard_controller()->set_container_behavior_for_test(
      std::move(scoped_behavior));

  gfx::Rect bounds(10, 20, 30, 40);
  ash_keyboard_controller()->SetOccludedBounds({bounds});
  EXPECT_EQ(bounds, behavior->occluded_bounds());
}

TEST_F(AshKeyboardControllerTest, SetHitTestBounds) {
  // Enable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);
  ASSERT_FALSE(keyboard_controller()->GetKeyboardWindow()->targeter());

  // Setting the hit test bounds should set a WindowTargeter.
  ash_keyboard_controller()->SetHitTestBounds({gfx::Rect(10, 20, 30, 40)});
  ASSERT_TRUE(keyboard_controller()->GetKeyboardWindow()->targeter());
}

TEST_F(AshKeyboardControllerTest, SetDraggableArea) {
  // Enable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  // Override the container behavior.
  auto scoped_behavior = std::make_unique<TestContainerBehavior>();
  TestContainerBehavior* behavior = scoped_behavior.get();
  keyboard_controller()->set_container_behavior_for_test(
      std::move(scoped_behavior));

  gfx::Rect bounds(10, 20, 30, 40);
  ash_keyboard_controller()->SetDraggableArea(bounds);
  EXPECT_EQ(bounds, behavior->draggable_area());
}

TEST_F(AshKeyboardControllerTest, ChangingSessionRebuildsKeyboard) {
  // Enable the keyboard.
  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  // LOGGED_IN_NOT_ACTIVE session state needs to rebuild keyboard for supervised
  // user profile.
  Shell::Get()->ash_keyboard_controller()->OnSessionStateChanged(
      session_manager::SessionState::LOGGED_IN_NOT_ACTIVE);
  EXPECT_EQ(1, test_observer()->destroyed_count());

  // ACTIVE session state also needs to rebuild keyboard for guest user profile.
  Shell::Get()->ash_keyboard_controller()->OnSessionStateChanged(
      session_manager::SessionState::ACTIVE);
  EXPECT_EQ(2, test_observer()->destroyed_count());
}

TEST_F(AshKeyboardControllerTest, VisualBoundsInMultipleDisplays) {
  UpdateDisplay("800x600,1024x768");

  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  // Show the keyboard in the second display.
  keyboard_controller()->ShowKeyboardInDisplay(
      Shell::Get()->display_manager()->GetSecondaryDisplay());
  ASSERT_TRUE(keyboard::WaitUntilShown());

  gfx::Rect root_bounds = keyboard_controller()->visual_bounds_in_root();
  EXPECT_EQ(0, root_bounds.x());

  gfx::Rect screen_bounds = keyboard_controller()->GetVisualBoundsInScreen();
  EXPECT_EQ(800, screen_bounds.x());
}

TEST_F(AshKeyboardControllerTest, OccludedBoundsInMultipleDisplays) {
  UpdateDisplay("800x600,1024x768");

  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  // Show the keyboard in the second display.
  keyboard_controller()->ShowKeyboardInDisplay(
      Shell::Get()->display_manager()->GetSecondaryDisplay());
  ASSERT_TRUE(keyboard::WaitUntilShown());

  gfx::Rect screen_bounds =
      keyboard_controller()->GetWorkspaceOccludedBoundsInScreen();
  EXPECT_EQ(800, screen_bounds.x());
}

// Test for http://crbug.com/303429. |GetContainerForDisplay| should move
// keyboard to specified display even when it's not touchable.
TEST_F(AshKeyboardControllerTest, GetContainerForDisplay) {
  UpdateDisplay("500x500,500x500");

  // Make primary display touchable.
  display::test::DisplayManagerTestApi(Shell::Get()->display_manager())
      .SetTouchSupport(GetPrimaryDisplay().id(),
                       display::Display::TouchSupport::AVAILABLE);

  EXPECT_EQ(display::Display::TouchSupport::AVAILABLE,
            GetPrimaryDisplay().touch_support());
  EXPECT_NE(display::Display::TouchSupport::AVAILABLE,
            GetSecondaryDisplay().touch_support());

  // Move to primary display.
  EXPECT_EQ(GetPrimaryRootWindow(),
            ash_keyboard_controller()
                ->GetContainerForDisplay(GetPrimaryDisplay())
                ->GetRootWindow());

  // Move to secondary display.
  EXPECT_EQ(GetSecondaryRootWindow(),
            ash_keyboard_controller()
                ->GetContainerForDisplay(GetSecondaryDisplay())
                ->GetRootWindow());
}

// Test for http://crbug.com/297858. |GetContainerForDefaultDisplay| should
// return the primary display if no display has touch capability and
// no window is focused.
TEST_F(AshKeyboardControllerTest,
       DefaultContainerInPrimaryDisplayWhenNoDisplayHasTouch) {
  UpdateDisplay("500x500,500x500");

  EXPECT_NE(display::Display::TouchSupport::AVAILABLE,
            GetPrimaryDisplay().touch_support());
  EXPECT_NE(display::Display::TouchSupport::AVAILABLE,
            GetSecondaryDisplay().touch_support());

  EXPECT_EQ(GetPrimaryRootWindow(), ash_keyboard_controller()
                                        ->GetContainerForDefaultDisplay()
                                        ->GetRootWindow());
}

// Test for http://crbug.com/297858. |GetContainerForDefaultDisplay| should
// move keyboard to focused display if no display has touch capability.
TEST_F(AshKeyboardControllerTest,
       DefaultContainerIsInFocusedDisplayWhenNoDisplayHasTouch) {
  UpdateDisplay("500x500,500x500");

  EXPECT_NE(display::Display::TouchSupport::AVAILABLE,
            GetPrimaryDisplay().touch_support());
  EXPECT_NE(display::Display::TouchSupport::AVAILABLE,
            GetSecondaryDisplay().touch_support());

  CreateFocusedTestWindowInRootWindow(GetSecondaryRootWindow());
  EXPECT_EQ(GetSecondaryRootWindow(), ash_keyboard_controller()
                                          ->GetContainerForDefaultDisplay()
                                          ->GetRootWindow());
}

// Test for http://crbug.com/303429. |GetContainerForDefaultDisplay| should
// move keyboard to first touchable display when there is one.
TEST_F(AshKeyboardControllerTest, DefaultContainerIsInFirstTouchableDisplay) {
  UpdateDisplay("500x500,500x500");

  // Make secondary display touchable.
  display::test::DisplayManagerTestApi(Shell::Get()->display_manager())
      .SetTouchSupport(GetSecondaryDisplay().id(),
                       display::Display::TouchSupport::AVAILABLE);

  EXPECT_NE(display::Display::TouchSupport::AVAILABLE,
            GetPrimaryDisplay().touch_support());
  EXPECT_EQ(display::Display::TouchSupport::AVAILABLE,
            GetSecondaryDisplay().touch_support());

  EXPECT_EQ(GetSecondaryRootWindow(), ash_keyboard_controller()
                                          ->GetContainerForDefaultDisplay()
                                          ->GetRootWindow());
}

// Test for http://crbug.com/303429. |GetContainerForDefaultDisplay| should
// move keyboard to first touchable display when the focused display is not
// touchable.
TEST_F(
    AshKeyboardControllerTest,
    DefaultContainerIsInFirstTouchableDisplayIfFocusedDisplayIsNotTouchable) {
  UpdateDisplay("500x500,500x500");

  // Make secondary display touchable.
  display::test::DisplayManagerTestApi(Shell::Get()->display_manager())
      .SetTouchSupport(GetSecondaryDisplay().id(),
                       display::Display::TouchSupport::AVAILABLE);

  EXPECT_NE(display::Display::TouchSupport::AVAILABLE,
            GetPrimaryDisplay().touch_support());
  EXPECT_EQ(display::Display::TouchSupport::AVAILABLE,
            GetSecondaryDisplay().touch_support());

  // Focus on primary display.
  CreateFocusedTestWindowInRootWindow(GetPrimaryRootWindow());

  EXPECT_EQ(GetSecondaryRootWindow(), ash_keyboard_controller()
                                          ->GetContainerForDefaultDisplay()
                                          ->GetRootWindow());
}

// Test for http://crbug.com/303429. |GetContainerForDefaultDisplay| should
// move keyborad to first touchable display when there is one.
TEST_F(AshKeyboardControllerTest,
       DefaultContainerIsInFocusedDisplayIfTouchable) {
  UpdateDisplay("500x500,500x500");

  // Make both displays touchable.
  display::test::DisplayManagerTestApi(Shell::Get()->display_manager())
      .SetTouchSupport(GetPrimaryDisplay().id(),
                       display::Display::TouchSupport::AVAILABLE);
  display::test::DisplayManagerTestApi(Shell::Get()->display_manager())
      .SetTouchSupport(GetSecondaryDisplay().id(),
                       display::Display::TouchSupport::AVAILABLE);

  EXPECT_EQ(display::Display::TouchSupport::AVAILABLE,
            GetPrimaryDisplay().touch_support());
  EXPECT_EQ(display::Display::TouchSupport::AVAILABLE,
            GetSecondaryDisplay().touch_support());

  // Focus on secondary display.
  CreateFocusedTestWindowInRootWindow(GetSecondaryRootWindow());
  EXPECT_EQ(GetSecondaryRootWindow(), ash_keyboard_controller()
                                          ->GetContainerForDefaultDisplay()
                                          ->GetRootWindow());

  // Focus on primary display.
  CreateFocusedTestWindowInRootWindow(GetPrimaryRootWindow());
  EXPECT_EQ(GetPrimaryRootWindow(), ash_keyboard_controller()
                                        ->GetContainerForDefaultDisplay()
                                        ->GetRootWindow());
}

// Test for https://crbug.com/897007.
TEST_F(AshKeyboardControllerTest, ShowKeyboardInSecondaryDisplay) {
  UpdateDisplay("500x500,500x500");

  ash_keyboard_controller()->SetEnableFlag(
      KeyboardEnableFlag::kExtensionEnabled);

  // Show in secondary display.
  keyboard_controller()->ShowKeyboardInDisplay(GetSecondaryDisplay());
  EXPECT_EQ(GetSecondaryRootWindow(), keyboard_controller()->GetRootWindow());
  ASSERT_TRUE(keyboard::WaitUntilShown());
  EXPECT_TRUE(!keyboard_controller()->GetKeyboardWindow()->bounds().IsEmpty());
}

}  // namespace ash
