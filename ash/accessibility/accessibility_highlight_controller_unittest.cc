// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/accessibility/accessibility_highlight_controller.h"

#include <cmath>
#include <memory>

#include "ash/accessibility/accessibility_controller.h"
#include "ash/accessibility/accessibility_cursor_ring_layer.h"
#include "ash/accessibility/accessibility_focus_ring_controller_impl.h"
#include "ash/shell.h"
#include "ash/test/ash_test_base.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/macros.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/aura/window.h"
#include "ui/base/ime/dummy_text_input_client.h"
#include "ui/compositor/compositor_switches.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/gfx/image/image.h"
#include "ui/snapshot/snapshot.h"
#include "ui/views/widget/widget.h"

namespace ash {

namespace {

class MockTextInputClient : public ui::DummyTextInputClient {
 public:
  MockTextInputClient() : ui::DummyTextInputClient(ui::TEXT_INPUT_TYPE_TEXT) {}
  ~MockTextInputClient() override = default;

  void SetCaretBounds(const gfx::Rect& bounds) { caret_bounds_ = bounds; }

 private:
  gfx::Rect GetCaretBounds() const override { return caret_bounds_; }

  gfx::Rect caret_bounds_;

  DISALLOW_COPY_AND_ASSIGN(MockTextInputClient);
};

}  // namespace

class AccessibilityHighlightControllerTest : public AshTestBase {
 protected:
  AccessibilityHighlightControllerTest() = default;
  ~AccessibilityHighlightControllerTest() override = default;

  void SetUp() override {
    base::CommandLine::ForCurrentProcess()->AppendSwitch(
        ::switches::kEnablePixelOutputInTests);
    AshTestBase::SetUp();
    Shell::Get()->accessibility_focus_ring_controller()->SetNoFadeForTesting();
  }

  void CaptureBeforeImage(const gfx::Rect& bounds) {
    Capture(bounds);
    if (before_bmp_.tryAllocPixels(image_.AsBitmap().info())) {
      image_.AsBitmap().readPixels(before_bmp_.info(), before_bmp_.getPixels(),
                                   before_bmp_.rowBytes(), 0, 0);
    }
  }

  void CaptureAfterImage(const gfx::Rect& bounds) {
    Capture(bounds);
    if (after_bmp_.tryAllocPixels(image_.AsBitmap().info())) {
      image_.AsBitmap().readPixels(after_bmp_.info(), after_bmp_.getPixels(),
                                   after_bmp_.rowBytes(), 0, 0);
    }
  }

  void ComputeImageStats() {
    diff_count_ = 0;
    double accum[4] = {0, 0, 0, 0};
    for (int x = 0; x < before_bmp_.width(); ++x) {
      for (int y = 0; y < before_bmp_.height(); ++y) {
        SkColor before_color = before_bmp_.getColor(x, y);
        SkColor after_color = after_bmp_.getColor(x, y);
        if (before_color != after_color) {
          diff_count_++;
          accum[0] += SkColorGetB(after_color);
          accum[1] += SkColorGetG(after_color);
          accum[2] += SkColorGetR(after_color);
          accum[3] += SkColorGetA(after_color);
        }
      }
    }
    average_diff_color_ =
        SkColorSetARGB(static_cast<unsigned char>(accum[3] / diff_count_),
                       static_cast<unsigned char>(accum[2] / diff_count_),
                       static_cast<unsigned char>(accum[1] / diff_count_),
                       static_cast<unsigned char>(accum[0] / diff_count_));
  }

  int diff_count() const { return diff_count_; }
  SkColor average_diff_color() const { return average_diff_color_; }

  void Capture(const gfx::Rect& bounds) {
    // Occasionally we don't get any pixels the first try.
    // Keep trying until we get the correct size butmap and
    // the first pixel is not transparent.
    while (true) {
      aura::Window* window = ash::Shell::GetPrimaryRootWindow();
      base::RunLoop run_loop;
      ui::GrabWindowSnapshotAndScaleAsync(
          window, bounds, bounds.size(),
          base::Bind(
              [](base::RunLoop* run_loop, gfx::Image* image,
                 gfx::Image got_image) {
                run_loop->Quit();
                *image = got_image;
              },
              &run_loop, &image_));
      run_loop.Run();
      SkBitmap bitmap = image_.AsBitmap();
      if (bitmap.width() != bounds.width() ||
          bitmap.height() != bounds.height()) {
        LOG(INFO) << "Bitmap not correct size, trying to capture again";
        continue;
      } else if (255 == SkColorGetA(bitmap.getColor(0, 0))) {
        LOG(INFO) << "Bitmap is transparent, trying to capture again";
        break;
      }
    }
  }

 private:
  gfx::Image image_;
  SkBitmap before_bmp_;
  SkBitmap after_bmp_;
  int diff_count_ = 0;
  SkColor average_diff_color_ = 0;

  DISALLOW_COPY_AND_ASSIGN(AccessibilityHighlightControllerTest);
};

TEST_F(AccessibilityHighlightControllerTest, TestCaretRingDrawsBluePixels) {
  // Create a white background window for captured image color smoke test.
  CreateTestWindowInShell(SK_ColorWHITE, -1,
                          Shell::GetPrimaryRootWindow()->bounds());

  gfx::Rect capture_bounds(200, 300, 100, 100);
  gfx::Rect caret_bounds(230, 330, 1, 25);

  CaptureBeforeImage(capture_bounds);

  AccessibilityHighlightController controller;
  controller.HighlightCaret(true);
  MockTextInputClient text_input_client;
  text_input_client.SetCaretBounds(caret_bounds);
  controller.OnCaretBoundsChanged(&text_input_client);

  CaptureAfterImage(capture_bounds);
  ComputeImageStats();

  // This is a smoke test to assert that something is drawn in the right part of
  // the screen of approximately the right size and color.
  // There's deliberately some tolerance for tiny errors.
  EXPECT_NEAR(1487, diff_count(), 50);
  EXPECT_NEAR(175, SkColorGetR(average_diff_color()), 5);
  EXPECT_NEAR(175, SkColorGetG(average_diff_color()), 5);
  EXPECT_NEAR(255, SkColorGetB(average_diff_color()), 5);
}

TEST_F(AccessibilityHighlightControllerTest, TestFocusRingDrawsPixels) {
  // Create a white background window for captured image color smoke test.
  CreateTestWindowInShell(SK_ColorWHITE, -1,
                          Shell::GetPrimaryRootWindow()->bounds());

  gfx::Rect capture_bounds(200, 300, 100, 100);
  gfx::Rect focus_bounds(230, 330, 40, 40);

  CaptureBeforeImage(capture_bounds);

  AccessibilityHighlightController controller;
  controller.HighlightFocus(true);
  controller.SetFocusHighlightRect(focus_bounds);

  CaptureAfterImage(capture_bounds);
  ComputeImageStats();

  // This is a smoke test to assert that something is drawn in the right part of
  // the screen of approximately the right size and color.
  // There's deliberately some tolerance for tiny errors.
  EXPECT_NEAR(1608, diff_count(), 50);
  EXPECT_NEAR(255, SkColorGetR(average_diff_color()), 5);
  EXPECT_NEAR(201, SkColorGetG(average_diff_color()), 5);
  EXPECT_NEAR(152, SkColorGetB(average_diff_color()), 5);
}

// Integration test of cursor handling between AccessibilityHighlightController
// and AccessibilityFocusRingController.
TEST_F(AccessibilityHighlightControllerTest, CursorWorksOnMultipleDisplays) {
  UpdateDisplay("400x400,500x500");
  aura::Window::Windows root_windows = ash::Shell::Get()->GetAllRootWindows();
  ASSERT_EQ(2u, root_windows.size());

  AccessibilityHighlightController highlight_controller;
  highlight_controller.HighlightCursor(true);
  gfx::Point location(90, 90);
  ui::MouseEvent event0(ui::ET_MOUSE_MOVED, location, location,
                        ui::EventTimeForNow(), 0, 0);
  ui::Event::DispatcherApi event_mod(&event0);
  event_mod.set_target(root_windows[0]);
  highlight_controller.OnMouseEvent(&event0);

  AccessibilityFocusRingControllerImpl* focus_ring_controller =
      Shell::Get()->accessibility_focus_ring_controller();
  auto* cursor_layer = focus_ring_controller->cursor_layer_for_testing();
  EXPECT_EQ(root_windows[0], cursor_layer->root_window());
  EXPECT_LT(
      std::abs(cursor_layer->layer()->GetTargetBounds().x() - location.x()),
      50);
  EXPECT_LT(
      std::abs(cursor_layer->layer()->GetTargetBounds().y() - location.y()),
      50);

  ui::MouseEvent event1(ui::ET_MOUSE_MOVED, location, location,
                        ui::EventTimeForNow(), 0, 0);
  ui::Event::DispatcherApi event_mod1(&event1);
  event_mod1.set_target(root_windows[1]);
  highlight_controller.OnMouseEvent(&event1);

  cursor_layer = focus_ring_controller->cursor_layer_for_testing();
  EXPECT_EQ(root_windows[1], cursor_layer->root_window());
  EXPECT_LT(
      std::abs(cursor_layer->layer()->GetTargetBounds().x() - location.x()),
      50);
  EXPECT_LT(
      std::abs(cursor_layer->layer()->GetTargetBounds().y() - location.y()),
      50);
}

// Integration test of caret handling between AccessibilityHighlightController
// and AccessibilityFocusRingController.
TEST_F(AccessibilityHighlightControllerTest, CaretRingDrawnOnlyWithinBounds) {
  // Given caret bounds that are not within the active window, expect that the
  // caret ring highlight is not drawn.
  std::unique_ptr<views::Widget> window = CreateTestWidget();
  window->SetBounds(gfx::Rect(5, 5, 300, 300));

  AccessibilityHighlightController highlight_controller;
  MockTextInputClient text_input_client;
  highlight_controller.HighlightCaret(true);
  gfx::Rect caret_bounds(10, 10, 40, 40);
  text_input_client.SetCaretBounds(caret_bounds);
  highlight_controller.OnCaretBoundsChanged(&text_input_client);

  AccessibilityFocusRingControllerImpl* focus_ring_controller =
      Shell::Get()->accessibility_focus_ring_controller();
  auto* caret_layer = focus_ring_controller->caret_layer_for_testing();
  EXPECT_EQ(
      std::abs(caret_layer->layer()->GetTargetBounds().x() - caret_bounds.x()),
      20);
  EXPECT_EQ(
      std::abs(caret_layer->layer()->GetTargetBounds().y() - caret_bounds.y()),
      20);

  gfx::Rect not_visible_bounds(301, 301, 10, 10);
  text_input_client.SetCaretBounds(not_visible_bounds);
  highlight_controller.OnCaretBoundsChanged(&text_input_client);

  EXPECT_FALSE(focus_ring_controller->caret_layer_for_testing());
}

// Tests that a zero-width text caret still results in a visible highlight.
// https://crbug.com/882762
TEST_F(AccessibilityHighlightControllerTest, ZeroWidthCaretRingVisible) {
  AccessibilityHighlightController highlight_controller;
  MockTextInputClient text_input_client;
  highlight_controller.HighlightCaret(true);

  // Simulate a zero-width text caret.
  gfx::Rect zero_width(0, 16);
  text_input_client.SetCaretBounds(zero_width);
  highlight_controller.OnCaretBoundsChanged(&text_input_client);

  // Caret ring is created.
  EXPECT_TRUE(Shell::Get()
                  ->accessibility_focus_ring_controller()
                  ->caret_layer_for_testing());

  // Simulate an empty text caret.
  gfx::Rect empty;
  text_input_client.SetCaretBounds(empty);
  highlight_controller.OnCaretBoundsChanged(&text_input_client);

  // Caret ring is gone.
  EXPECT_FALSE(Shell::Get()
                   ->accessibility_focus_ring_controller()
                   ->caret_layer_for_testing());
}

// Tests setting the caret bounds explicitly via AccessibilityController, rather
// than via the input method observer. This path is used in production in mash.
TEST_F(AccessibilityHighlightControllerTest, SetCaretBounds) {
  std::unique_ptr<views::Widget> window = CreateTestWidget();
  window->SetBounds(gfx::Rect(5, 5, 300, 300));

  AccessibilityController* accessibility_controller =
      Shell::Get()->accessibility_controller();
  accessibility_controller->SetCaretHighlightEnabled(true);

  // Bounds inside the active window create a highlight.
  accessibility_controller->SetCaretBounds(gfx::Rect(10, 10, 1, 16));
  AccessibilityFocusRingControllerImpl* focus_ring_controller =
      Shell::Get()->accessibility_focus_ring_controller();
  EXPECT_TRUE(focus_ring_controller->caret_layer_for_testing());

  // Empty bounds remove the highlight.
  accessibility_controller->SetCaretBounds(gfx::Rect());
  EXPECT_FALSE(focus_ring_controller->caret_layer_for_testing());
}

}  // namespace ash
