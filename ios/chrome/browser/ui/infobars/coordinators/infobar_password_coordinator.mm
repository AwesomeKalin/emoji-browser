// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/infobars/coordinators/infobar_password_coordinator.h"

#include "base/strings/sys_string_conversions.h"
#include "ios/chrome/browser/infobars/infobar_controller_delegate.h"
#import "ios/chrome/browser/passwords/ios_chrome_password_infobar_metrics_recorder.h"
#import "ios/chrome/browser/passwords/ios_chrome_save_password_infobar_delegate.h"
#import "ios/chrome/browser/ui/commands/application_commands.h"
#import "ios/chrome/browser/ui/infobars/banners/infobar_banner_view_controller.h"
#import "ios/chrome/browser/ui/infobars/coordinators/infobar_coordinator_implementation.h"
#import "ios/chrome/browser/ui/infobars/infobar_badge_ui_delegate.h"
#import "ios/chrome/browser/ui/infobars/infobar_container.h"
#import "ios/chrome/browser/ui/infobars/modals/infobar_password_modal_delegate.h"
#import "ios/chrome/browser/ui/infobars/modals/infobar_password_table_view_controller.h"
#include "ios/chrome/grit/ios_strings.h"
#include "ui/base/l10n/l10n_util.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@interface InfobarPasswordCoordinator () <InfobarCoordinatorImplementation,
                                          InfobarPasswordModalDelegate>

// Delegate that holds the Infobar information and actions.
@property(nonatomic, readonly)
    IOSChromeSavePasswordInfoBarDelegate* passwordInfoBarDelegate;
// InfobarBannerViewController owned by this Coordinator.
@property(nonatomic, strong) InfobarBannerViewController* bannerViewController;
// InfobarPasswordTableViewController owned by this Coordinator.
@property(nonatomic, strong)
    InfobarPasswordTableViewController* modalViewController;
// The InfobarType for the banner presented by this Coordinator.
@property(nonatomic, assign, readonly) InfobarType infobarBannerType;

@end

@implementation InfobarPasswordCoordinator
// Synthesize since readonly property from superclass is changed to readwrite.
@synthesize bannerViewController = _bannerViewController;
// Synthesize since readonly property from superclass is changed to readwrite.
@synthesize modalViewController = _modalViewController;

- (instancetype)initWithInfoBarDelegate:(IOSChromeSavePasswordInfoBarDelegate*)
                                            passwordInfoBarDelegate
                                   type:(InfobarType)infobarType {
  self = [super initWithInfoBarDelegate:passwordInfoBarDelegate
                                   type:infobarType];
  if (self) {
    _passwordInfoBarDelegate = passwordInfoBarDelegate;
    // Set |_infobarBannerType| at init time since
    // passwordInfoBarDelegate->IsPasswordUpdate() can change after the user has
    // interacted with the ModalInfobar.
    _infobarBannerType = infobarType;
  }
  return self;
}

#pragma mark - ChromeCoordinator

- (void)start {
  if (!self.started) {
    self.started = YES;
    self.bannerViewController = [[InfobarBannerViewController alloc]
        initWithDelegate:self
                    type:self.infobarBannerType];
    self.bannerViewController.titleText = base::SysUTF16ToNSString(
        self.passwordInfoBarDelegate->GetMessageText());
    NSString* username = self.passwordInfoBarDelegate->GetUserNameText();
    NSString* password = self.passwordInfoBarDelegate->GetPasswordText();
    password = [@"" stringByPaddingToLength:[password length]
                                 withString:@"•"
                            startingAtIndex:0];
    self.bannerViewController.subTitleText =
        [NSString stringWithFormat:@"%@ %@", username, password];
    self.bannerViewController.buttonText =
        base::SysUTF16ToNSString(self.passwordInfoBarDelegate->GetButtonLabel(
            ConfirmInfoBarDelegate::BUTTON_OK));
    self.bannerViewController.iconImage =
        [UIImage imageNamed:@"infobar_passwords_icon"];
    NSString* hiddenPasswordText =
        l10n_util::GetNSString(IDS_IOS_SETTINGS_PASSWORD_HIDDEN_LABEL);
    self.bannerViewController.optionalAccessibilityLabel = [NSString
        stringWithFormat:@"%@,%@, %@", self.bannerViewController.titleText,
                         username, hiddenPasswordText];
  }
}

- (void)stop {
  if (self.started) {
    self.started = NO;
    // RemoveInfoBar() will delete the InfobarIOS that owns this Coordinator
    // from memory.
    self.delegate->RemoveInfoBar();
    _passwordInfoBarDelegate = nil;
    [self.infobarContainer childCoordinatorStopped];
  }
}

#pragma mark - InfobarCoordinatorImplementation

- (void)configureModalViewController {
  // Do not use |self.infobarBannerType| since the modal type might change each
  // time is presented. e.g. We present a Modal of type Save and tap on "Save".
  // The next time the Modal is presented we'll present a Modal of Type "Update"
  // since the credentials are currently saved.
  InfobarType infobarModalType =
      self.passwordInfoBarDelegate->IsPasswordUpdate()
          ? InfobarType::kInfobarTypePasswordUpdate
          : InfobarType::kInfobarTypePasswordSave;
  self.modalViewController = [[InfobarPasswordTableViewController alloc]
      initWithDelegate:self
                  type:infobarModalType];
  self.modalViewController.title =
      self.passwordInfoBarDelegate->GetInfobarModalTitleText();
  self.modalViewController.username =
      self.passwordInfoBarDelegate->GetUserNameText();
  NSString* password = self.passwordInfoBarDelegate->GetPasswordText();
  self.modalViewController.maskedPassword =
      [@"" stringByPaddingToLength:[password length]
                        withString:@"•"
                   startingAtIndex:0];
  self.modalViewController.unmaskedPassword = password;
  self.modalViewController.detailsTextMessage =
      self.passwordInfoBarDelegate->GetDetailsMessageText();
  self.modalViewController.saveButtonText =
      base::SysUTF16ToNSString(self.passwordInfoBarDelegate->GetButtonLabel(
          ConfirmInfoBarDelegate::BUTTON_OK));
  self.modalViewController.cancelButtonText =
      base::SysUTF16ToNSString(self.passwordInfoBarDelegate->GetButtonLabel(
          ConfirmInfoBarDelegate::BUTTON_CANCEL));
  self.modalViewController.URL = self.passwordInfoBarDelegate->GetURLHostText();
  self.modalViewController.currentCredentialsSaved =
      self.passwordInfoBarDelegate->IsCurrentPasswordSaved();

  [self recordModalPresentationMetricsUsingModalType:infobarModalType];
}

- (void)infobarBannerWasPresented {
  // There's a chance the Delegate was destroyed while the presentation was
  // taking place e.g. User navigated away. Check if the delegate still exists.
  if (self.passwordInfoBarDelegate)
    self.passwordInfoBarDelegate->InfobarPresenting(YES /*automatic*/);
}

- (void)infobarModalPresentedFromBanner:(BOOL)presentedFromBanner {
  // If the modal is being expanded from the banner we count that as the same
  // presentation from infobarBannerWasPresented.
  if (presentedFromBanner)
    return;

  self.passwordInfoBarDelegate->InfobarPresenting(NO /*automatic*/);
}

- (void)dismissBannerWhenInteractionIsFinished {
  [self.bannerViewController dismissWhenInteractionIsFinished];
}

- (void)performInfobarAction {
  self.passwordInfoBarDelegate->Accept();
}

- (void)infobarWasDismissed {
  // Release these strong ViewControllers at the time of infobar dismissal.
  self.bannerViewController = nil;
  self.modalViewController = nil;
  // While navigating away the Infobar delegate might be niled before the banner
  // has been dismissed. Check that the delegate still exists.
  if (self.passwordInfoBarDelegate)
    self.passwordInfoBarDelegate->InfobarDismissed();
}

- (CGFloat)infobarModalHeight {
  UITableView* tableView = self.modalViewController.tableView;
  [tableView setNeedsLayout];
  [tableView layoutIfNeeded];

  // Since the TableView is contained in a NavigationController get the
  // navigation bar height.
  CGFloat navigationBarHeight = self.modalViewController.navigationController
                                    .navigationBar.frame.size.height;

  return tableView.contentSize.height + navigationBarHeight;
}

#pragma mark - InfobarPasswordModalDelegate

- (void)updateCredentialsWithUsername:(NSString*)username
                             password:(NSString*)password {
  self.passwordInfoBarDelegate->UpdateCredentials(username, password);
  [self modalInfobarButtonWasAccepted:self];
}

- (void)neverSaveCredentialsForCurrentSite {
  self.passwordInfoBarDelegate->Cancel();
  // Completely remove the Infobar along with its badge after blacklisting the
  // Website.
  [self detachView];
}

- (void)presentPasswordSettings {
  DCHECK(self.dispatcher);
  [self
      dismissInfobarModal:self
                 animated:NO
               completion:^{
                 [self.dispatcher showSavedPasswordsSettingsFromViewController:
                                      self.baseViewController];
               }];
}

#pragma mark - Helpers

- (void)recordModalPresentationMetricsUsingModalType:
    (InfobarType)infobarModalType {
  IOSChromePasswordInfobarMetricsRecorder* passwordMetricsRecorder;
  switch (infobarModalType) {
    case InfobarType::kInfobarTypePasswordUpdate:
      passwordMetricsRecorder = [[IOSChromePasswordInfobarMetricsRecorder alloc]
          initWithType:PasswordInfobarType::kPasswordInfobarTypeUpdate];
      break;
    case InfobarType::kInfobarTypePasswordSave:
      passwordMetricsRecorder = [[IOSChromePasswordInfobarMetricsRecorder alloc]
          initWithType:PasswordInfobarType::kPasswordInfobarTypeSave];
      break;
    default:
      NOTREACHED();
      break;
  }
  switch (self.infobarBannerType) {
    case InfobarType::kInfobarTypePasswordUpdate:
      [passwordMetricsRecorder
          recordModalPresent:MobileMessagesPasswordsModalPresent::
                                 PresentedAfterUpdateBanner];
      break;
    case InfobarType::kInfobarTypePasswordSave:
      [passwordMetricsRecorder
          recordModalPresent:MobileMessagesPasswordsModalPresent::
                                 PresentedAfterSaveBanner];
      break;
    default:
      NOTREACHED();
      break;
  }
}

@end
