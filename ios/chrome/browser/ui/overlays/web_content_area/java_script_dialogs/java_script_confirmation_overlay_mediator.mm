// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_confirmation_overlay_mediator.h"

#include "base/strings/sys_string_conversions.h"
#include "components/strings/grit/components_strings.h"
#import "ios/chrome/browser/overlays/public/overlay_request.h"
#import "ios/chrome/browser/overlays/public/overlay_response.h"
#import "ios/chrome/browser/overlays/public/web_content_area/java_script_confirmation_overlay.h"
#import "ios/chrome/browser/ui/alert_view_controller/alert_action.h"
#import "ios/chrome/browser/ui/alert_view_controller/alert_consumer.h"
#import "ios/chrome/browser/ui/overlays/web_content_area/java_script_dialogs/java_script_dialog_overlay_mediator+subclassing.h"
#include "ios/chrome/grit/ios_strings.h"
#include "ui/base/l10n/l10n_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@interface JavaScriptConfirmationOverlayMediator ()
// The confirmation config.
@property(nonatomic, readonly)
    JavaScriptConfirmationOverlayRequestConfig* config;
@end

@implementation JavaScriptConfirmationOverlayMediator

#pragma mark - Accessors

- (JavaScriptConfirmationOverlayRequestConfig*)config {
  return self.request->GetConfig<JavaScriptConfirmationOverlayRequestConfig>();
}

- (void)setConsumer:(id<AlertConsumer>)consumer {
  if (self.consumer == consumer)
    return;
  [super setConsumer:consumer];
  [self.consumer setMessage:base::SysUTF8ToNSString(self.config->message())];
  __weak __typeof__(self) weakSelf = self;
  [self.consumer setActions:@[
    [AlertAction actionWithTitle:l10n_util::GetNSString(IDS_OK)
                           style:UIAlertActionStyleDefault
                         handler:^(AlertAction* action) {
                           __typeof__(self) strongSelf = weakSelf;
                           [strongSelf setConfirmationResponse:YES];
                           [strongSelf.delegate
                               stopDialogForMediator:strongSelf];
                         }],
    [AlertAction actionWithTitle:l10n_util::GetNSString(IDS_CANCEL)
                           style:UIAlertActionStyleCancel
                         handler:^(AlertAction* action) {
                           __typeof__(self) strongSelf = weakSelf;
                           [strongSelf setConfirmationResponse:NO];
                           [strongSelf.delegate
                               stopDialogForMediator:strongSelf];
                         }],
  ]];
}

#pragma mark - Response helpers

// Sets the OverlayResponse using the user's selection from the confirmation UI.
- (void)setConfirmationResponse:(BOOL)dialogConfirmed {
  self.request->set_response(
      OverlayResponse::CreateWithInfo<
          JavaScriptConfirmationOverlayResponseInfo>(dialogConfirmed));
}

@end

@implementation JavaScriptConfirmationOverlayMediator (Subclassing)

- (const JavaScriptDialogSource*)requestSource {
  return &self.config->source();
}

@end
