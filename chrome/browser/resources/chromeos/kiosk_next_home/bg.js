// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

chrome.app.runtime.onLaunched.addListener(() => {
  const windowOptions = {state: 'maximized', frame: 'none'};
  chrome.app.window.create('main.html', windowOptions, (newWindow) => {
    newWindow.maximize();
  });
});
