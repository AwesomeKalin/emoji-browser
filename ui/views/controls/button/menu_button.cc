// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/controls/button/menu_button.h"

#include <memory>

#include "ui/events/event.h"
#include "ui/views/controls/button/button_controller_delegate.h"
#include "ui/views/controls/button/menu_button_controller.h"

namespace views {

MenuButton::MenuButton(const base::string16& text,
                       MenuButtonListener* menu_button_listener,
                       int button_context)
    : LabelButton(nullptr, text, button_context) {
  SetHorizontalAlignment(gfx::ALIGN_LEFT);
  std::unique_ptr<MenuButtonController> menu_button_controller =
      std::make_unique<MenuButtonController>(this, menu_button_listener,
                                             CreateButtonControllerDelegate());
  menu_button_controller_ = menu_button_controller.get();
  SetButtonController(std::move(menu_button_controller));
}
MenuButton::~MenuButton() = default;

bool MenuButton::Activate(const ui::Event* event) {
  return button_controller()->Activate(event);
}

bool MenuButton::IsTriggerableEventType(const ui::Event& event) {
  return button_controller()->IsTriggerableEventType(event);
}

void MenuButton::NotifyClick(const ui::Event& event) {
  // Notify MenuButtonListener via MenuButtonController, instead of
  // ButtonListener::ButtonPressed.
  button_controller()->Activate(&event);
}

BEGIN_METADATA(MenuButton)
METADATA_PARENT_CLASS(LabelButton)
END_METADATA()

}  // namespace views
