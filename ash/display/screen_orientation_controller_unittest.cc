// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/display/screen_orientation_controller.h"

#include <memory>
#include <vector>

#include "ash/accelerometer/accelerometer_reader.h"
#include "ash/accelerometer/accelerometer_types.h"
#include "ash/display/screen_orientation_controller.h"
#include "ash/display/screen_orientation_controller_test_api.h"
#include "ash/home_screen/home_screen_controller.h"
#include "ash/kiosk_next/kiosk_next_shell_test_util.h"
#include "ash/kiosk_next/mock_kiosk_next_shell_client.h"
#include "ash/public/cpp/app_types.h"
#include "ash/public/cpp/ash_features.h"
#include "ash/public/cpp/ash_switches.h"
#include "ash/shell.h"
#include "ash/system/screen_layout_observer.h"
#include "ash/test/ash_test_base.h"
#include "ash/test/ash_test_helper.h"
#include "ash/test_shell_delegate.h"
#include "ash/window_factory.h"
#include "ash/wm/tablet_mode/tablet_mode_controller.h"
#include "ash/wm/window_state.h"
#include "base/command_line.h"
#include "base/macros.h"
#include "base/test/scoped_feature_list.h"
#include "ui/aura/client/aura_constants.h"
#include "ui/aura/window.h"
#include "ui/compositor/layer_type.h"
#include "ui/display/display.h"
#include "ui/display/display_switches.h"
#include "ui/display/manager/display_manager.h"
#include "ui/display/manager/managed_display_info.h"
#include "ui/display/test/display_manager_test_api.h"
#include "ui/message_center/message_center.h"
#include "ui/wm/public/activation_client.h"

namespace ash {
namespace {

const float kDegreesToRadians = 3.1415926f / 180.0f;
const float kMeanGravity = -9.8066f;

display::ManagedDisplayInfo CreateDisplayInfo(int64_t id,
                                              const gfx::Rect& bounds) {
  display::ManagedDisplayInfo info(id, "dummy", false);
  info.SetBounds(bounds);
  return info;
}

void EnableTabletMode(bool enable) {
  Shell::Get()->tablet_mode_controller()->SetEnabledForTest(enable);
}

bool RotationLocked() {
  return Shell::Get()->screen_orientation_controller()->rotation_locked();
}

bool UserRotationLocked() {
  return Shell::Get()->screen_orientation_controller()->user_rotation_locked();
}

void SetDisplayRotationById(int64_t display_id,
                            display::Display::Rotation rotation) {
  Shell::Get()->display_manager()->SetDisplayRotation(
      display_id, rotation, display::Display::RotationSource::USER);
}

void SetInternalDisplayRotation(display::Display::Rotation rotation) {
  SetDisplayRotationById(display::Display::InternalDisplayId(), rotation);
}

void TriggerLidUpdate(const gfx::Vector3dF& lid) {
  scoped_refptr<AccelerometerUpdate> update(new AccelerometerUpdate());
  update->Set(ACCELEROMETER_SOURCE_SCREEN, false, lid.x(), lid.y(), lid.z());
  Shell::Get()->screen_orientation_controller()->OnAccelerometerUpdated(update);
}

// Shows |child| and adds |child| to |parent|.
void AddWindowAndShow(aura::Window* child, aura::Window* parent) {
  child->Show();
  parent->AddChild(child);
}

// Adds |child| to |parent| and activates |parent|.
void AddWindowAndActivateParent(aura::Window* child, aura::Window* parent) {
  AddWindowAndShow(child, parent);
  Shell::Get()->activation_client()->ActivateWindow(parent);
}

void Lock(aura::Window* window, OrientationLockType orientation_lock) {
  Shell::Get()->screen_orientation_controller()->LockOrientationForWindow(
      window, orientation_lock);
}

void Unlock(aura::Window* window) {
  Shell::Get()->screen_orientation_controller()->UnlockOrientationForWindow(
      window);
}

// Creates a window of type WINDOW_TYPE_CONTROL.
std::unique_ptr<aura::Window> CreateControlWindow() {
  std::unique_ptr<aura::Window> window = window_factory::NewWindow(
      nullptr, aura::client::WindowType::WINDOW_TYPE_CONTROL);
  window->Init(ui::LAYER_NOT_DRAWN);
  window->set_owned_by_parent(false);
  return window;
}

}  // namespace

class ScreenOrientationControllerTest : public AshTestBase {
 public:
  ScreenOrientationControllerTest() = default;
  ~ScreenOrientationControllerTest() override = default;

  // AshTestBase:
  void SetUp() override {
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        ::switches::kUseFirstDisplayAsInternal);
    AshTestBase::SetUp();
  }

 protected:
  aura::Window* CreateAppWindowInShellWithId(int id) {
    aura::Window* window = CreateTestWindowInShellWithId(id);
    window->SetProperty(aura::client::kAppType,
                        static_cast<int>(AppType::CHROME_APP));
    return window;
  }

  void SetSystemRotationLocked(bool rotation_locked) {
    ScreenOrientationControllerTestApi(
        Shell::Get()->screen_orientation_controller())
        .SetRotationLocked(rotation_locked);
  }

  void SetUserRotationLocked(bool rotation_locked) {
    if (Shell::Get()->screen_orientation_controller()->user_rotation_locked() !=
        rotation_locked) {
      Shell::Get()->screen_orientation_controller()->ToggleUserRotationLock();
    }
  }

  OrientationLockType UserLockedOrientation() const {
    ScreenOrientationControllerTestApi test_api(
        Shell::Get()->screen_orientation_controller());
    return test_api.UserLockedOrientation();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(ScreenOrientationControllerTest);
};

// Tests that a Window can lock rotation.
TEST_F(ScreenOrientationControllerTest, LockOrientation) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  ASSERT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  ASSERT_FALSE(RotationLocked());

  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kLandscape);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());
}

// Tests that a Window can unlock rotation.
TEST_F(ScreenOrientationControllerTest, Unlock) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  ASSERT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  ASSERT_FALSE(RotationLocked());

  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kLandscape);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());

  Unlock(child_window.get());
  EXPECT_FALSE(RotationLocked());
}

// Tests that a Window is able to change the orientation of the display after
// having locked rotation.
TEST_F(ScreenOrientationControllerTest, OrientationChanges) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  ASSERT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  ASSERT_FALSE(RotationLocked());

  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kPortrait);
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());

  Lock(child_window.get(), OrientationLockType::kLandscape);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that orientation can only be set by the first Window that has set a
// rotation lock.
TEST_F(ScreenOrientationControllerTest, SecondContentCannotChangeOrientation) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window1 = CreateControlWindow();
  std::unique_ptr<aura::Window> child_window2 = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window1(CreateAppWindowInShellWithId(0));
  std::unique_ptr<aura::Window> focus_window2(CreateAppWindowInShellWithId(1));

  AddWindowAndActivateParent(child_window1.get(), focus_window1.get());
  AddWindowAndShow(child_window2.get(), focus_window2.get());
  Lock(child_window1.get(), OrientationLockType::kLandscape);
  Lock(child_window2.get(), OrientationLockType::kPortrait);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that only the Window that set a rotation lock can perform an unlock.
TEST_F(ScreenOrientationControllerTest, SecondContentCannotUnlock) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window1 = CreateControlWindow();
  std::unique_ptr<aura::Window> child_window2 = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window1(CreateAppWindowInShellWithId(0));
  std::unique_ptr<aura::Window> focus_window2(CreateAppWindowInShellWithId(1));

  AddWindowAndActivateParent(child_window1.get(), focus_window1.get());
  AddWindowAndShow(child_window2.get(), focus_window2.get());
  Lock(child_window1.get(), OrientationLockType::kLandscape);
  Unlock(child_window2.get());
  EXPECT_TRUE(RotationLocked());
}

// Tests that a rotation lock is applied only while the Window are a part of the
// active window.
TEST_F(ScreenOrientationControllerTest, ActiveWindowChangesUpdateLock) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window1(CreateAppWindowInShellWithId(0));
  std::unique_ptr<aura::Window> focus_window2(CreateAppWindowInShellWithId(1));

  AddWindowAndActivateParent(child_window.get(), focus_window1.get());
  Lock(child_window.get(), OrientationLockType::kLandscape);
  ASSERT_TRUE(RotationLocked());

  ::wm::ActivationClient* activation_client = Shell::Get()->activation_client();
  activation_client->ActivateWindow(focus_window2.get());
  EXPECT_FALSE(RotationLocked());

  activation_client->ActivateWindow(focus_window1.get());
  EXPECT_TRUE(RotationLocked());
}

// Tests that switching between windows with different orientation locks change
// the orientation.
TEST_F(ScreenOrientationControllerTest, ActiveWindowChangesUpdateOrientation) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window1 = CreateControlWindow();
  std::unique_ptr<aura::Window> child_window2 = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window1(CreateAppWindowInShellWithId(0));
  std::unique_ptr<aura::Window> focus_window2(CreateAppWindowInShellWithId(1));
  AddWindowAndActivateParent(child_window1.get(), focus_window1.get());
  AddWindowAndShow(child_window2.get(), focus_window2.get());

  Lock(child_window1.get(), OrientationLockType::kLandscape);
  Lock(child_window2.get(), OrientationLockType::kPortrait);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  ::wm::ActivationClient* activation_client = Shell::Get()->activation_client();
  activation_client->ActivateWindow(focus_window2.get());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());

  activation_client->ActivateWindow(focus_window1.get());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that a rotation lock is removed when the setting window is hidden, and
// that it is reapplied when the window becomes visible.
TEST_F(ScreenOrientationControllerTest, VisibilityChangesLock) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kLandscape);
  EXPECT_TRUE(RotationLocked());

  child_window->Hide();
  EXPECT_FALSE(RotationLocked());

  child_window->Show();
  EXPECT_TRUE(RotationLocked());
}

// Tests that when a window is destroyed that its rotation lock is removed, and
// window activations no longer change the lock
TEST_F(ScreenOrientationControllerTest, WindowDestructionRemovesLock) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window1(CreateAppWindowInShellWithId(0));
  std::unique_ptr<aura::Window> focus_window2(CreateAppWindowInShellWithId(1));

  AddWindowAndActivateParent(child_window.get(), focus_window1.get());
  Lock(child_window.get(), OrientationLockType::kLandscape);
  ASSERT_TRUE(RotationLocked());

  focus_window1->RemoveChild(child_window.get());
  child_window.reset();
  EXPECT_FALSE(RotationLocked());

  ::wm::ActivationClient* activation_client = Shell::Get()->activation_client();
  activation_client->ActivateWindow(focus_window2.get());
  EXPECT_FALSE(RotationLocked());

  activation_client->ActivateWindow(focus_window1.get());
  EXPECT_FALSE(RotationLocked());
}

// Tests that accelerometer readings in each of the screen angles will trigger a
// rotation of the internal display.
TEST_F(ScreenOrientationControllerTest, DisplayRotation) {
  EnableTabletMode(true);

  // Now test rotating in all directions.
  TriggerLidUpdate(gfx::Vector3dF(-kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, -kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that low angles are ignored by the accelerometer (i.e. when the device
// is almost laying flat).
TEST_F(ScreenOrientationControllerTest, RotationIgnoresLowAngles) {
  EnableTabletMode(true);

  TriggerLidUpdate(gfx::Vector3dF(0.0f, -kMeanGravity, -kMeanGravity));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(-2.0f, 0.0f, -kMeanGravity));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, 2.0f, -kMeanGravity));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(2.0f, 0.0f, -kMeanGravity));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, -2.0f, -kMeanGravity));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that the display will stick to the current orientation beyond the
// halfway point, preventing frequent updates back and forth.
TEST_F(ScreenOrientationControllerTest, RotationSticky) {
  EnableTabletMode(true);

  gfx::Vector3dF gravity(0.0f, -kMeanGravity, 0.0f);
  TriggerLidUpdate(gravity);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  // Turn past half-way point to next direction and rotation should remain
  // the same.
  float degrees = 50.0;
  gravity.set_x(-sin(degrees * kDegreesToRadians) * kMeanGravity);
  gravity.set_y(-cos(degrees * kDegreesToRadians) * kMeanGravity);
  TriggerLidUpdate(gravity);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  // Turn more and the screen should rotate.
  degrees = 70.0;
  gravity.set_x(-sin(degrees * kDegreesToRadians) * kMeanGravity);
  gravity.set_y(-cos(degrees * kDegreesToRadians) * kMeanGravity);
  TriggerLidUpdate(gravity);
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());

  // Turn back just beyond the half-way point and the new rotation should
  // still be in effect.
  degrees = 40.0;
  gravity.set_x(-sin(degrees * kDegreesToRadians) * kMeanGravity);
  gravity.set_y(-cos(degrees * kDegreesToRadians) * kMeanGravity);
  TriggerLidUpdate(gravity);
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());
}

// Tests that the display will stick to its current orientation when the
// rotation lock has been set.
TEST_F(ScreenOrientationControllerTest, RotationLockPreventsRotation) {
  EnableTabletMode(true);
  SetUserRotationLocked(true);

  // Turn past the threshold for rotation.
  float degrees = 90.0;
  gfx::Vector3dF gravity(-sin(degrees * kDegreesToRadians) * kMeanGravity,
                         -cos(degrees * kDegreesToRadians) * kMeanGravity,
                         0.0f);
  TriggerLidUpdate(gravity);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  SetUserRotationLocked(false);
  TriggerLidUpdate(gravity);
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());
}

// The ScreenLayoutObserver class that is responsible for adding/updating
// MessageCenter notifications is only added to the SystemTray on ChromeOS.
// Tests that the screen rotation notifications are suppressed when
// triggered by the accelerometer.
TEST_F(ScreenOrientationControllerTest, BlockRotationNotifications) {
  EnableTabletMode(true);
  Shell::Get()->screen_layout_observer()->set_show_notifications_for_testing(
      true);
  display::test::DisplayManagerTestApi(display_manager())
      .SetFirstDisplayAsInternalDisplay();

  message_center::MessageCenter* message_center =
      message_center::MessageCenter::Get();

  EXPECT_EQ(0u, message_center->NotificationCount());
  EXPECT_FALSE(message_center->HasPopupNotifications());

  // Make sure notifications are still displayed when
  // adjusting the screen rotation directly when in tablet mode
  ASSERT_NE(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  SetInternalDisplayRotation(display::Display::ROTATE_270);
  SetSystemRotationLocked(false);
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  EXPECT_EQ(1u, message_center->NotificationCount());
  EXPECT_TRUE(message_center->HasPopupNotifications());

  // Clear all notifications
  message_center->RemoveAllNotifications(
      false /* by_user */, message_center::MessageCenter::RemoveType::ALL);
  EXPECT_EQ(0u, message_center->NotificationCount());
  EXPECT_FALSE(message_center->HasPopupNotifications());

  // Make sure notifications are blocked when adjusting the screen rotation
  // via the accelerometer while in tablet mode
  // Rotate the screen 90 degrees
  ASSERT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(-kMeanGravity, 0.0f, 0.0f));
  ASSERT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());
  EXPECT_EQ(0u, message_center->NotificationCount());
  EXPECT_FALSE(message_center->HasPopupNotifications());

  // Make sure notifications are still displayed when
  // adjusting the screen rotation directly when not in tablet mode
  EnableTabletMode(false);
  // Reset the screen rotation.
  SetInternalDisplayRotation(display::Display::ROTATE_0);
  // Clear all notifications
  message_center->RemoveAllNotifications(
      false /* by_user */, message_center::MessageCenter::RemoveType::ALL);
  ASSERT_NE(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());
  ASSERT_EQ(0u, message_center->NotificationCount());
  ASSERT_FALSE(message_center->HasPopupNotifications());
  SetInternalDisplayRotation(display::Display::ROTATE_180);
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());
  EXPECT_EQ(1u, message_center->NotificationCount());
  EXPECT_TRUE(message_center->HasPopupNotifications());
}

// Tests that if a user has set a display rotation that it is restored upon
// exiting tablet mode.
TEST_F(ScreenOrientationControllerTest, ResetUserRotationUponExit) {
  display::test::DisplayManagerTestApi(display_manager())
      .SetFirstDisplayAsInternalDisplay();

  SetInternalDisplayRotation(display::Display::ROTATE_90);
  EnableTabletMode(true);

  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());

  EnableTabletMode(false);
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());
}

// Tests that if a user changes the display rotation, while rotation is locked,
// that the updates are recorded. Upon exiting tablet mode the latest user
// rotation should be applied.
TEST_F(ScreenOrientationControllerTest, UpdateUserRotationWhileRotationLocked) {
  EnableTabletMode(true);
  SetInternalDisplayRotation(display::Display::ROTATE_270);
  // User sets rotation to the same rotation that the display was at when
  // tablet mode was activated.
  SetInternalDisplayRotation(display::Display::ROTATE_0);
  EnableTabletMode(false);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that when the orientation lock is set to Landscape, that rotation can
// be done between the two angles of the orientation.
TEST_F(ScreenOrientationControllerTest, LandscapeOrientationAllowsRotation) {
  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  EnableTabletMode(true);

  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kLandscape);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());

  // Inverse of orientation is allowed
  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());

  // Display rotations between are not allowed
  TriggerLidUpdate(gfx::Vector3dF(kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(-kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());
}

// Tests that when the orientation lock is set to Portrait, that rotation can be
// done between the two angles of the orientation.
TEST_F(ScreenOrientationControllerTest, PortraitOrientationAllowsRotation) {
  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  EnableTabletMode(true);

  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kPortrait);
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());

  // Inverse of orientation is allowed
  TriggerLidUpdate(gfx::Vector3dF(-kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());

  // Display rotations between are not allowed
  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, -kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());
}

// Tests that for an orientation lock which does not allow rotation, that the
// display rotation remains constant.
TEST_F(ScreenOrientationControllerTest, OrientationLockDisallowsRotation) {
  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  EnableTabletMode(true);

  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kPortraitPrimary);
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());

  // Rotation does not change.
  TriggerLidUpdate(gfx::Vector3dF(kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, -kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
}

// Tests that after a Window has applied an orientation lock which supports
// rotation, that a user rotation lock does not allow rotation.
TEST_F(ScreenOrientationControllerTest, UserRotationLockDisallowsRotation) {
  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));
  EnableTabletMode(true);

  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  Lock(child_window.get(), OrientationLockType::kLandscape);
  Unlock(child_window.get());

  SetUserRotationLocked(true);
  EXPECT_TRUE(RotationLocked());
  EXPECT_TRUE(UserRotationLocked());

  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Verifies rotating an inactive Display is successful.
TEST_F(ScreenOrientationControllerTest, RotateInactiveDisplay) {
  const int64_t kInternalDisplayId = 9;
  const int64_t kExternalDisplayId = 10;
  const display::Display::Rotation kNewRotation = display::Display::ROTATE_180;

  const display::ManagedDisplayInfo internal_display_info =
      CreateDisplayInfo(kInternalDisplayId, gfx::Rect(0, 0, 500, 500));
  const display::ManagedDisplayInfo external_display_info =
      CreateDisplayInfo(kExternalDisplayId, gfx::Rect(1, 1, 500, 500));

  std::vector<display::ManagedDisplayInfo> display_info_list_two_active;
  display_info_list_two_active.push_back(internal_display_info);
  display_info_list_two_active.push_back(external_display_info);

  std::vector<display::ManagedDisplayInfo> display_info_list_one_active;
  display_info_list_one_active.push_back(external_display_info);

  // The display::ManagedDisplayInfo list with two active displays needs to be
  // added first so that the DisplayManager can track the
  // |internal_display_info| as inactive instead of non-existent.
  display_manager()->UpdateDisplaysWith(display_info_list_two_active);
  display_manager()->UpdateDisplaysWith(display_info_list_one_active);

  display::test::ScopedSetInternalDisplayId set_internal(display_manager(),
                                                         kInternalDisplayId);

  ASSERT_NE(kNewRotation, display_manager()
                              ->GetDisplayInfo(kInternalDisplayId)
                              .GetActiveRotation());
  ScreenOrientationControllerTestApi(
      Shell::Get()->screen_orientation_controller())
      .SetDisplayRotation(kNewRotation,
                          display::Display::RotationSource::ACTIVE);

  EXPECT_EQ(kNewRotation, display_manager()
                              ->GetDisplayInfo(kInternalDisplayId)
                              .GetActiveRotation());
}

TEST_F(ScreenOrientationControllerTest, UserRotationLockedOrientation) {
  ScreenOrientationController* orientation_controller =
      Shell::Get()->screen_orientation_controller();
  orientation_controller->ToggleUserRotationLock();
  EXPECT_TRUE(orientation_controller->user_rotation_locked());
  EXPECT_EQ(OrientationLockType::kLandscapePrimary, UserLockedOrientation());

  orientation_controller->ToggleUserRotationLock();
  SetInternalDisplayRotation(display::Display::ROTATE_270);
  orientation_controller->ToggleUserRotationLock();
  EXPECT_EQ(OrientationLockType::kPortraitPrimary, UserLockedOrientation());

  orientation_controller->ToggleUserRotationLock();
  SetInternalDisplayRotation(display::Display::ROTATE_180);
  orientation_controller->ToggleUserRotationLock();
  EXPECT_EQ(OrientationLockType::kLandscapeSecondary, UserLockedOrientation());

  orientation_controller->ToggleUserRotationLock();
  SetInternalDisplayRotation(display::Display::ROTATE_90);
  orientation_controller->ToggleUserRotationLock();
  EXPECT_EQ(OrientationLockType::kPortraitSecondary, UserLockedOrientation());
  orientation_controller->ToggleUserRotationLock();

  SetInternalDisplayRotation(display::Display::ROTATE_270);

  UpdateDisplay("800x1280");
  orientation_controller->ToggleUserRotationLock();
  EXPECT_TRUE(orientation_controller->user_rotation_locked());
  EXPECT_EQ(OrientationLockType::kPortraitPrimary, UserLockedOrientation());

  orientation_controller->ToggleUserRotationLock();
  SetInternalDisplayRotation(display::Display::ROTATE_90);
  orientation_controller->ToggleUserRotationLock();
  EXPECT_EQ(OrientationLockType::kLandscapePrimary, UserLockedOrientation());

  orientation_controller->ToggleUserRotationLock();
  SetInternalDisplayRotation(display::Display::ROTATE_180);
  orientation_controller->ToggleUserRotationLock();
  EXPECT_EQ(OrientationLockType::kPortraitSecondary, UserLockedOrientation());

  orientation_controller->ToggleUserRotationLock();
  SetInternalDisplayRotation(display::Display::ROTATE_270);
  orientation_controller->ToggleUserRotationLock();
  EXPECT_EQ(OrientationLockType::kLandscapeSecondary, UserLockedOrientation());
  orientation_controller->ToggleUserRotationLock();
}

TEST_F(ScreenOrientationControllerTest, UserRotationLock) {
  EnableTabletMode(true);

  std::unique_ptr<aura::Window> child_window1 = CreateControlWindow();
  std::unique_ptr<aura::Window> child_window2 = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window1(CreateAppWindowInShellWithId(0));
  std::unique_ptr<aura::Window> focus_window2(CreateAppWindowInShellWithId(1));

  AddWindowAndActivateParent(child_window2.get(), focus_window2.get());
  AddWindowAndActivateParent(child_window1.get(), focus_window1.get());

  ASSERT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  ASSERT_FALSE(RotationLocked());
  ASSERT_FALSE(UserRotationLocked());

  ScreenOrientationController* orientation_controller =
      Shell::Get()->screen_orientation_controller();
  ASSERT_FALSE(orientation_controller->user_rotation_locked());
  orientation_controller->ToggleUserRotationLock();
  ASSERT_TRUE(orientation_controller->user_rotation_locked());

  Lock(child_window1.get(), OrientationLockType::kPortrait);

  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());

  ::wm::ActivationClient* activation_client = Shell::Get()->activation_client();
  // Activating any will switch to the natural orientation.
  activation_client->ActivateWindow(focus_window2.get());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  // Activating the portrait window will rotate to the portrait.
  activation_client->ActivateWindow(focus_window1.get());
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());

  // User locked to the 90 dig.
  orientation_controller->ToggleUserRotationLock();
  orientation_controller->ToggleUserRotationLock();

  // Switching to Any orientation will stay to the user locked orientation.
  activation_client->ActivateWindow(focus_window2.get());
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());

  // Application forced to be landscape.
  Lock(child_window2.get(), OrientationLockType::kLandscape);
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  Lock(child_window1.get(), OrientationLockType::kAny);
  activation_client->ActivateWindow(focus_window1.get());
  // Switching back to any will rotate to user rotation.
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
}

// Test fixture for Kiosk Next Home's rotation lock.
class ScreenOrientationControllerKioskNextTest
    : public ScreenOrientationControllerTest {
 public:
  ScreenOrientationControllerKioskNextTest() = default;
  ~ScreenOrientationControllerKioskNextTest() override = default;

  // AshTestBase:
  void SetUp() override {
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        ::switches::kUseFirstDisplayAsInternal);
    scoped_feature_list_.InitAndEnableFeature(features::kKioskNextShell);

    set_start_session(false);
    AshTestBase::SetUp();

    client_ = std::make_unique<MockKioskNextShellClient>();
  }

  void TearDown() override {
    client_.reset();
    home_screen_window_.reset();
    AshTestBase::TearDown();
  }

 protected:
  aura::Window* CreateHomeScreenWindow() {
    home_screen_window_.reset(CreateAppWindowInShellWithId(0));
    ash::Shell::GetContainer(ash::Shell::GetPrimaryRootWindow(),
                             ash::kShellWindowId_HomeScreenContainer)
        ->AddChild(home_screen_window_.get());
    return home_screen_window_.get();
  }

 private:
  std::unique_ptr<MockKioskNextShellClient> client_;
  base::test::ScopedFeatureList scoped_feature_list_;

  std::unique_ptr<aura::Window> home_screen_window_;

  DISALLOW_COPY_AND_ASSIGN(ScreenOrientationControllerKioskNextTest);
};

// Tests that the home screen can be inverted but not rotated to portrait.
TEST_F(ScreenOrientationControllerKioskNextTest,
       LandscapeOrientationAllowsRotation) {
  LogInKioskNextUser(GetSessionControllerClient());
  aura::Window* home_screen_window = CreateHomeScreenWindow();

  Shell::Get()->activation_client()->ActivateWindow(home_screen_window);

  // The home screen window is rotation-locked to landscape.
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());

  // Inverse of orientation is allowed.
  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());

  // Display rotations in between are not allowed.
  TriggerLidUpdate(gfx::Vector3dF(kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(-kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_180, GetCurrentInternalDisplayRotation());
}

// Tests that app windows outside of the home screen follow normal orientation
// rules.
TEST_F(ScreenOrientationControllerKioskNextTest,
       ActiveWindowChangesUpdateOrientation) {
  LogInKioskNextUser(GetSessionControllerClient());
  aura::Window* home_screen_window = CreateHomeScreenWindow();
  Shell::Get()->activation_client()->ActivateWindow(home_screen_window);

  // The home screen window is rotation-locked to landscape.
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));

  // The rotation is unlocked when an app window is opened.
  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  EXPECT_FALSE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(-kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());

  // The rotation becomes locked again when returning to Home.
  Shell::Get()->home_screen_controller()->GoHome(
      display::Display::InternalDisplayId());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

TEST_F(ScreenOrientationControllerKioskNextTest,
       ActiveWindowChangesUpdateLock) {
  LogInKioskNextUser(GetSessionControllerClient());
  aura::Window* home_screen_window = CreateHomeScreenWindow();
  Shell::Get()->activation_client()->ActivateWindow(home_screen_window);

  // The home screen window is rotation-locked to landscape.
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));

  // An app window can specify its own lock.
  Lock(child_window.get(), OrientationLockType::kPortrait);
  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  EXPECT_EQ(display::Display::ROTATE_270, GetCurrentInternalDisplayRotation());
  EXPECT_TRUE(RotationLocked());

  // The rotation returns to landscape when returning to Home.
  Shell::Get()->home_screen_controller()->GoHome(
      display::Display::InternalDisplayId());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that the active window removing a rotation lock doesn't affect the home
// screen window's lock.
TEST_F(ScreenOrientationControllerKioskNextTest,
       ActiveWindowRemovesUpdateLock) {
  LogInKioskNextUser(GetSessionControllerClient());
  aura::Window* home_screen_window = CreateHomeScreenWindow();
  Shell::Get()->activation_client()->ActivateWindow(home_screen_window);

  // The home screen window is rotation-locked to landscape.
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));

  // An app window can specify its own lock.
  Lock(child_window.get(), OrientationLockType::kLandscape);
  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  TriggerLidUpdate(gfx::Vector3dF(kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  // Returning home preserves the home screen lock, even after the previously
  // active window drops its lock.
  Shell::Get()->home_screen_controller()->GoHome(
      display::Display::InternalDisplayId());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  Unlock(child_window.get());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

// Tests that a user rotation lock doesn't override the home screen window's
// orientation lock.
TEST_F(ScreenOrientationControllerKioskNextTest, UserRotationLock) {
  LogInKioskNextUser(GetSessionControllerClient());
  aura::Window* home_screen_window = CreateHomeScreenWindow();
  ::wm::ActivationClient* activation_client = Shell::Get()->activation_client();
  activation_client->ActivateWindow(home_screen_window);

  std::unique_ptr<aura::Window> child_window = CreateControlWindow();
  std::unique_ptr<aura::Window> focus_window(CreateAppWindowInShellWithId(0));

  // The rotation is unlocked when an app window is opened.
  AddWindowAndActivateParent(child_window.get(), focus_window.get());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
  EXPECT_FALSE(RotationLocked());
  TriggerLidUpdate(gfx::Vector3dF(-kMeanGravity, 0.0f, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());

  // The user can lock the rotation.
  SetUserRotationLocked(true);
  EXPECT_TRUE(UserRotationLocked());
  EXPECT_TRUE(RotationLocked());

  // Rotating the screen doesn't update the orientation while locked.
  TriggerLidUpdate(gfx::Vector3dF(0.0f, kMeanGravity, 0.0f));
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());

  // The rotation returns to landscape when returning to Home.
  Shell::Get()->home_screen_controller()->GoHome(
      display::Display::InternalDisplayId());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());

  // The user rotation lock applies again when re-activating an app window.
  activation_client->ActivateWindow(focus_window.get());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_90, GetCurrentInternalDisplayRotation());

  // Removing the user rotation lock doesn't affect Home.
  Shell::Get()->home_screen_controller()->GoHome(
      display::Display::InternalDisplayId());
  SetUserRotationLocked(false);
  EXPECT_FALSE(UserRotationLocked());
  EXPECT_TRUE(RotationLocked());
  EXPECT_EQ(display::Display::ROTATE_0, GetCurrentInternalDisplayRotation());
}

}  // namespace ash
