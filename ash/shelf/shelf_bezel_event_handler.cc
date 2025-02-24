// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/shelf/shelf_bezel_event_handler.h"

#include "ash/shelf/shelf.h"
#include "ash/shelf/shelf_layout_manager.h"
#include "ash/shelf/shelf_widget.h"
#include "ash/shell.h"
#include "ui/aura/window.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/events/event.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/wm/core/coordinate_conversion.h"

namespace ash {

ShelfBezelEventHandler::ShelfBezelEventHandler(Shelf* shelf)
    : shelf_(shelf), in_touch_drag_(false) {
  Shell::Get()->AddPreTargetHandler(this);
}

ShelfBezelEventHandler::~ShelfBezelEventHandler() {
  Shell::Get()->RemovePreTargetHandler(this);
}

void ShelfBezelEventHandler::OnGestureEvent(ui::GestureEvent* event) {
  gfx::Point point_in_screen(event->location());
  aura::Window* target = static_cast<aura::Window*>(event->target());
  ::wm::ConvertPointToScreen(target, &point_in_screen);
  gfx::Rect screen = display::Screen::GetScreen()
                         ->GetDisplayNearestPoint(point_in_screen)
                         .bounds();
  if ((!screen.Contains(point_in_screen) &&
       IsShelfOnBezel(screen, point_in_screen)) ||
      in_touch_drag_) {
    if (shelf_->ProcessGestureEvent(*event)) {
      switch (event->type()) {
        case ui::ET_GESTURE_SCROLL_BEGIN:
          in_touch_drag_ = true;
          break;
        case ui::ET_GESTURE_SCROLL_END:
        case ui::ET_SCROLL_FLING_START:
          in_touch_drag_ = false;
          break;
        default:
          break;
      }
      event->StopPropagation();
    }
  }
}

void ShelfBezelEventHandler::OnTouchEvent(ui::TouchEvent* event) {
  if (shelf_->auto_hide_behavior() != SHELF_AUTO_HIDE_BEHAVIOR_ALWAYS)
    return;

  // The event target should be the shelf widget.
  aura::Window* target = static_cast<aura::Window*>(event->target());
  if (target != Shelf::ForWindow(target)->shelf_widget()->GetNativeView())
    return;

  // The touch-pressing event may hide the shelf. Lock the shelf's auto hide
  // state to give the shelf a chance to handle the touch event before it being
  // hidden.
  ShelfLayoutManager* shelf_layout_manager = shelf_->shelf_layout_manager();
  if (event->type() == ui::ET_TOUCH_PRESSED && shelf_->IsVisible()) {
    shelf_layout_manager->LockAutoHideState(true);
  } else if (event->type() == ui::ET_TOUCH_RELEASED ||
             event->type() == ui::ET_TOUCH_CANCELLED) {
    shelf_layout_manager->LockAutoHideState(false);
  }
}

bool ShelfBezelEventHandler::IsShelfOnBezel(const gfx::Rect& screen,
                                            const gfx::Point& point) const {
  switch (shelf_->alignment()) {
    case SHELF_ALIGNMENT_BOTTOM:
    case SHELF_ALIGNMENT_BOTTOM_LOCKED:
      return point.y() >= screen.bottom();
    case SHELF_ALIGNMENT_LEFT:
      return point.x() <= screen.x();
    case SHELF_ALIGNMENT_RIGHT:
      return point.x() >= screen.right();
  }
  NOTREACHED();
  return false;
}

}  // namespace ash
