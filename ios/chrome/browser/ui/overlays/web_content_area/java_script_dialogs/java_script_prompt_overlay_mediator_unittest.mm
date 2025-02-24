// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_prompt_overlay_mediator.h"

#include "base/strings/sys_string_conversions.h"
#include "components/strings/grit/components_strings.h"
#include "components/url_formatter/elide_url.h"
#import "ios/chrome/browser/overlays/public/overlay_request.h"
#import "ios/chrome/browser/overlays/public/overlay_response.h"
#import "ios/chrome/browser/overlays/public/web_content_area/java_script_prompt_overlay.h"
#import "ios/chrome/browser/ui/alert_view_controller/alert_action.h"
#import "ios/chrome/browser/ui/alert_view_controller/test/fake_alert_consumer.h"
#import "ios/chrome/browser/ui/elements/text_field_configuration.h"
#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_dialog_overlay_mediator.h"
#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/test/java_script_dialog_overlay_mediator_test.h"
#include "ios/chrome/grit/ios_strings.h"
#include "testing/gtest_mac.h"
#include "ui/base/l10n/l10n_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace {
// User input string provided by FakePromptOverlayMediatorDataSource.
NSString* const kFakeUserInput = @"Fake User Input";
}

// Fake version of the mediator data source.
@interface FakePromptOverlayMediatorDataSource
    : NSObject <JavaScriptPromptOverlayMediatorDataSource>
@property(nonatomic, copy) NSString* promptInput;
@end

@implementation FakePromptOverlayMediatorDataSource

- (NSString*)promptInputForMediator:(JavaScriptPromptOverlayMediator*)mediator {
  return self.promptInput;
}

@end

class JavaScriptPromptOverlayMediatorTest
    : public JavaScriptDialogOverlayMediatorTest {
 public:
  JavaScriptPromptOverlayMediatorTest()
      : url_("https://chromium.test"),
        message_("Message"),
        default_prompt_value_("Default Prompt Value"),
        data_source_([[FakePromptOverlayMediatorDataSource alloc] init]) {}

  // Creates a mediator and sets it for testing.
  void CreateMediator() {
    request_ =
        OverlayRequest::CreateWithConfig<JavaScriptPromptOverlayRequestConfig>(
            url_, /*is_main_frame=*/true, message_, default_prompt_value_);
    JavaScriptPromptOverlayMediator* mediator =
        [[JavaScriptPromptOverlayMediator alloc]
            initWithRequest:request_.get()];
    mediator.dataSource = data_source_;
    SetMediator(mediator);
  }

  const GURL& url() const { return url_; }
  const std::string& message() const { return message_; }
  const std::string& default_prompt_value() const {
    return default_prompt_value_;
  }
  const OverlayRequest* request() const { return request_.get(); }
  FakePromptOverlayMediatorDataSource* data_source() { return data_source_; }

 private:
  const GURL url_;
  const std::string message_;
  const std::string default_prompt_value_;
  std::unique_ptr<OverlayRequest> request_;
  FakePromptOverlayMediatorDataSource* data_source_ = nil;
};

// Tests that the consumer values are set correctly for main frame prompts.
TEST_F(JavaScriptPromptOverlayMediatorTest, PromptSetup) {
  CreateMediator();

  // Verify the consumer values.
  EXPECT_NSEQ(base::SysUTF8ToNSString(message()), consumer().message);
  ASSERT_EQ(1U, consumer().textFieldConfigurations.count);
  EXPECT_NSEQ(base::SysUTF8ToNSString(default_prompt_value()),
              consumer().textFieldConfigurations[0].text);
  EXPECT_FALSE(!!consumer().textFieldConfigurations[0].placeholder);
  EXPECT_NSEQ(kJavaScriptPromptTextFieldAccessibiltyIdentifier,
              consumer().textFieldConfigurations[0].accessibilityIdentifier);
  ASSERT_EQ(2U, consumer().actions.count);
  EXPECT_EQ(UIAlertActionStyleDefault, consumer().actions[0].style);
  EXPECT_NSEQ(l10n_util::GetNSString(IDS_OK), consumer().actions[0].title);
  EXPECT_EQ(UIAlertActionStyleCancel, consumer().actions[1].style);
  EXPECT_NSEQ(l10n_util::GetNSString(IDS_CANCEL), consumer().actions[1].title);
}

// Tests that the correct response is provided for the confirm action.
TEST_F(JavaScriptPromptOverlayMediatorTest, ConfirmResponse) {
  data_source().promptInput = kFakeUserInput;
  CreateMediator();
  ASSERT_EQ(2U, consumer().actions.count);
  ASSERT_FALSE(!!request()->response());

  // Execute the confirm action and verify the response.
  AlertAction* confirm_action = consumer().actions[0];
  confirm_action.handler(confirm_action);
  OverlayResponse* confirm_response = request()->response();
  ASSERT_TRUE(!!confirm_response);
  JavaScriptPromptOverlayResponseInfo* confirm_response_info =
      confirm_response->GetInfo<JavaScriptPromptOverlayResponseInfo>();
  ASSERT_TRUE(confirm_response_info);
  EXPECT_NSEQ(kFakeUserInput,
              base::SysUTF8ToNSString(confirm_response_info->text_input()));
}

// Tests that an empty string is provided in the response when the confirm
// action is executed and the datasource returns nil.
TEST_F(JavaScriptPromptOverlayMediatorTest, EmptyConfirmResponse) {
  data_source().promptInput = nil;
  CreateMediator();
  ASSERT_EQ(2U, consumer().actions.count);
  ASSERT_FALSE(!!request()->response());

  // Execute the confirm action and verify the response.
  AlertAction* confirm_action = consumer().actions[0];
  confirm_action.handler(confirm_action);
  OverlayResponse* confirm_response = request()->response();
  ASSERT_TRUE(!!confirm_response);
  JavaScriptPromptOverlayResponseInfo* confirm_response_info =
      confirm_response->GetInfo<JavaScriptPromptOverlayResponseInfo>();
  ASSERT_TRUE(confirm_response_info);
  EXPECT_NSEQ(@"",
              base::SysUTF8ToNSString(confirm_response_info->text_input()));
}

// Tests that the correct response is provided for the cancel action.
TEST_F(JavaScriptPromptOverlayMediatorTest, CancelResponse) {
  data_source().promptInput = kFakeUserInput;
  CreateMediator();
  ASSERT_EQ(2U, consumer().actions.count);
  ASSERT_FALSE(!!request()->response());

  // Execute the cancel action and verify that there is no response for
  // cancelled prompts.
  AlertAction* cancel_action = consumer().actions[1];
  cancel_action.handler(cancel_action);
  OverlayResponse* cancel_response = request()->response();
  EXPECT_FALSE(!!cancel_response);
}
