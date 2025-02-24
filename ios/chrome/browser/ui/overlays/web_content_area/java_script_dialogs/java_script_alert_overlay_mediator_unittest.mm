// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_alert_overlay_mediator.h"

#include "base/strings/sys_string_conversions.h"
#include "components/strings/grit/components_strings.h"
#include "components/url_formatter/elide_url.h"
#import "ios/chrome/browser/overlays/public/overlay_request.h"
#import "ios/chrome/browser/overlays/public/web_content_area/java_script_alert_overlay.h"
#import "ios/chrome/browser/ui/alert_view_controller/alert_action.h"
#import "ios/chrome/browser/ui/alert_view_controller/test/fake_alert_consumer.h"
#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_dialog_overlay_mediator.h"
#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/test/java_script_dialog_overlay_mediator_test.h"
#include "ios/chrome/grit/ios_strings.h"
#include "testing/gtest_mac.h"
#include "ui/base/l10n/l10n_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using JavaScriptAlertOverlayMediatorTest = JavaScriptDialogOverlayMediatorTest;

// Tests that the consumer values are set correctly for alerts.
TEST_F(JavaScriptAlertOverlayMediatorTest, AlertSetup) {
  const GURL kUrl("https://chromium.test");
  const std::string kMessage("Message");
  std::unique_ptr<OverlayRequest> request =
      OverlayRequest::CreateWithConfig<JavaScriptAlertOverlayRequestConfig>(
          kUrl, /*is_main_frame=*/true, kMessage);
  SetMediator(
      [[JavaScriptAlertOverlayMediator alloc] initWithRequest:request.get()]);

  // Verify the consumer values.
  EXPECT_NSEQ(base::SysUTF8ToNSString(kMessage), consumer().message);
  EXPECT_EQ(0U, consumer().textFieldConfigurations.count);
  ASSERT_EQ(1U, consumer().actions.count);
  EXPECT_EQ(UIAlertActionStyleDefault, consumer().actions[0].style);
  EXPECT_NSEQ(l10n_util::GetNSString(IDS_OK), consumer().actions[0].title);
}
