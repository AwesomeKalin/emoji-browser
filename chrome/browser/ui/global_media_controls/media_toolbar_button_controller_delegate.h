// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_GLOBAL_MEDIA_CONTROLS_MEDIA_TOOLBAR_BUTTON_CONTROLLER_DELEGATE_H_
#define CHROME_BROWSER_UI_GLOBAL_MEDIA_CONTROLS_MEDIA_TOOLBAR_BUTTON_CONTROLLER_DELEGATE_H_

// Delegate for MediaToolbarButtonController that is told when to show by the
// controller.
class MediaToolbarButtonControllerDelegate {
 public:
  virtual void Show() = 0;

 protected:
  virtual ~MediaToolbarButtonControllerDelegate();
};

#endif  // CHROME_BROWSER_UI_GLOBAL_MEDIA_CONTROLS_MEDIA_TOOLBAR_BUTTON_CONTROLLER_DELEGATE_H_
