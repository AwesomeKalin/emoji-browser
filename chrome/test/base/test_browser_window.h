// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_TEST_BASE_TEST_BROWSER_WINDOW_H_
#define CHROME_TEST_BASE_TEST_BROWSER_WINDOW_H_

#include <memory>
#include <string>
#include <vector>

#include "base/macros.h"
#include "build/build_config.h"
#include "chrome/browser/download/test_download_shelf.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_dialogs.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/location_bar/location_bar.h"
#include "chrome/browser/ui/page_action/page_action_icon_container.h"
#include "chrome/common/buildflags.h"

#if !defined(OS_ANDROID)
#include "chrome/browser/apps/intent_helper/apps_navigation_types.h"
#endif  //  !defined(OS_ANDROID)

class LocationBarTesting;
class OmniboxView;

namespace extensions {
class Extension;
}

namespace send_tab_to_self {
class SendTabToSelfBubbleController;
class SendTabToSelfBubbleView;
}  // namespace send_tab_to_self

// An implementation of BrowserWindow used for testing. TestBrowserWindow only
// contains a valid LocationBar, all other getters return NULL.
// See BrowserWithTestWindowTest for an example of using this class.
class TestBrowserWindow : public BrowserWindow {
 public:
  TestBrowserWindow();
  ~TestBrowserWindow() override;

  // BrowserWindow:
  void Show() override {}
  void ShowInactive() override {}
  void Hide() override {}
  bool IsVisible() const override;
  void SetBounds(const gfx::Rect& bounds) override {}
  void Close() override {}
  void Activate() override {}
  void Deactivate() override {}
  bool IsActive() const override;
  void FlashFrame(bool flash) override {}
  bool IsAlwaysOnTop() const override;
  void SetAlwaysOnTop(bool always_on_top) override {}
  gfx::NativeWindow GetNativeWindow() const override;
  void SetTopControlsShownRatio(content::WebContents* web_contents,
                                float ratio) override;
  bool DoBrowserControlsShrinkRendererSize(
      const content::WebContents* contents) const override;
  int GetTopControlsHeight() const override;
  void SetTopControlsGestureScrollInProgress(bool in_progress) override;
  StatusBubble* GetStatusBubble() override;
  void UpdateTitleBar() override {}
  void UpdateFrameColor() override {}
  void BookmarkBarStateChanged(
      BookmarkBar::AnimateChangeType change_type) override {}
  void UpdateDevTools() override {}
  void UpdateLoadingAnimations(bool should_animate) override {}
  void SetStarredState(bool is_starred) override {}
  void SetTranslateIconToggled(bool is_lit) override {}
  void OnActiveTabChanged(content::WebContents* old_contents,
                          content::WebContents* new_contents,
                          int index,
                          int reason) override {}
  void OnTabDetached(content::WebContents* contents, bool was_active) override {
  }
  void OnTabRestored(int command_id) override {}
  void ZoomChangedForActiveTab(bool can_show_bubble) override {}
  gfx::Rect GetRestoredBounds() const override;
  ui::WindowShowState GetRestoredState() const override;
  gfx::Rect GetBounds() const override;
  gfx::Size GetContentsSize() const override;
  void SetContentsSize(const gfx::Size& size) override;
  bool IsMaximized() const override;
  bool IsMinimized() const override;
  void Maximize() override {}
  void Minimize() override {}
  void Restore() override {}
  bool ShouldHideUIForFullscreen() const override;
  bool IsFullscreen() const override;
  bool IsFullscreenBubbleVisible() const override;
  LocationBar* GetLocationBar() const override;
  PageActionIconContainer* GetOmniboxPageActionIconContainer() override;
  PageActionIconContainer* GetToolbarPageActionIconContainer() override;
  void SetFocusToLocationBar(bool select_all) override {}
  void UpdateReloadStopState(bool is_loading, bool force) override {}
  void UpdateToolbar(content::WebContents* contents) override {}
  void UpdateToolbarVisibility(bool visible, bool animate) override {}
  void ResetToolbarTabState(content::WebContents* contents) override {}
  void FocusToolbar() override {}
  ToolbarActionsBar* GetToolbarActionsBar() override;
  ExtensionsContainer* GetExtensionsContainer() override;
  void ToolbarSizeChanged(bool is_animating) override {}
  void TabDraggingStatusChanged(bool is_dragging) override {}
  void FocusAppMenu() override {}
  void FocusBookmarksToolbar() override {}
  void FocusInactivePopupForAccessibility() override {}
  void RotatePaneFocus(bool forwards) override {}
  void ShowAppMenu() override {}
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      const content::NativeWebKeyboardEvent& event) override;
  bool HandleKeyboardEvent(
      const content::NativeWebKeyboardEvent& event) override;
  bool IsBookmarkBarVisible() const override;
  bool IsBookmarkBarAnimating() const override;
  bool IsTabStripEditable() const override;
  bool IsToolbarVisible() const override;
  bool IsToolbarShowing() const override;
  void ShowUpdateChromeDialog() override {}
  void ShowBookmarkBubble(const GURL& url, bool already_bookmarked) override {}
#if !defined(OS_ANDROID)
  void ShowIntentPickerBubble(std::vector<apps::IntentPickerAppInfo> app_info,
                              bool show_stay_in_chrome,
                              bool show_remember_selection,
                              IntentPickerResponse callback) override {}
#endif  //  !define(OS_ANDROID)
  autofill::SaveCardBubbleView* ShowSaveCreditCardBubble(
      content::WebContents* contents,
      autofill::SaveCardBubbleController* controller,
      bool user_gesture) override;
  autofill::LocalCardMigrationBubble* ShowLocalCardMigrationBubble(
      content::WebContents* contents,
      autofill::LocalCardMigrationBubbleController* controller,
      bool user_gesture) override;
  send_tab_to_self::SendTabToSelfBubbleView* ShowSendTabToSelfBubble(
      content::WebContents* contents,
      send_tab_to_self::SendTabToSelfBubbleController* controller,
      bool is_user_gesture) override;
  ShowTranslateBubbleResult ShowTranslateBubble(
      content::WebContents* contents,
      translate::TranslateStep step,
      const std::string& source_language,
      const std::string& target_language,
      translate::TranslateErrors::Type error_type,
      bool is_user_gesture) override;
#if BUILDFLAG(ENABLE_ONE_CLICK_SIGNIN)
  void ShowOneClickSigninConfirmation(
      const base::string16& email,
      base::OnceCallback<void(bool)> confirmed_callback) override {}
#endif
  bool IsDownloadShelfVisible() const override;
  DownloadShelf* GetDownloadShelf() override;
  void ConfirmBrowserCloseWithPendingDownloads(
      int download_count,
      Browser::DownloadClosePreventionType dialog_type,
      bool app_modal,
      const base::Callback<void(bool)>& callback) override {}
  void UserChangedTheme(BrowserThemeChangeType theme_change_type) override {}
  void CutCopyPaste(int command_id) override {}
  FindBar* CreateFindBar() override;
  web_modal::WebContentsModalDialogHost* GetWebContentsModalDialogHost()
      override;
  void ShowAvatarBubbleFromAvatarButton(
      AvatarBubbleMode mode,
      const signin::ManageAccountsParams& manage_accounts_params,
      signin_metrics::AccessPoint access_point,
      bool is_source_keyboard) override {}

#if defined(OS_CHROMEOS) || defined(OS_MACOSX) || defined(OS_WIN) || \
    defined(OS_LINUX)
  void ShowHatsBubbleFromAppMenuButton() override {}
#endif

  void ExecuteExtensionCommand(const extensions::Extension* extension,
                               const extensions::Command& command) override;
  ExclusiveAccessContext* GetExclusiveAccessContext() override;
  void ShowImeWarningBubble(
      const extensions::Extension* extension,
      const base::Callback<void(ImeWarningBubblePermissionStatus status)>&
          callback) override {}
  std::string GetWorkspace() const override;
  bool IsVisibleOnAllWorkspaces() const override;
  void ShowEmojiPanel() override {}

  void ShowInProductHelpPromo(InProductHelpFeature iph_feature) override {}

 protected:
  void DestroyBrowser() override {}

 private:
  class TestLocationBar : public LocationBar {
   public:
    TestLocationBar() : LocationBar(NULL) {}
    ~TestLocationBar() override {}

    // LocationBar:
    GURL GetDestinationURL() const override;
    WindowOpenDisposition GetWindowOpenDisposition() const override;
    ui::PageTransition GetPageTransition() const override;
    base::TimeTicks GetMatchSelectionTimestamp() const override;
    void AcceptInput() override {}
    void AcceptInput(base::TimeTicks match_selection_timestamp) override {}
    void FocusLocation(bool select_all) override {}
    void FocusSearch() override {}
    void UpdateContentSettingsIcons() override {}
    void UpdateSaveCreditCardIcon() override {}
    void UpdateLocalCardMigrationIcon() override {}
    void UpdateBookmarkStarVisibility() override {}
    void SaveStateToContents(content::WebContents* contents) override {}
    void Revert() override {}
    const OmniboxView* GetOmniboxView() const override;
    OmniboxView* GetOmniboxView() override;
    LocationBarTesting* GetLocationBarForTesting() override;

   private:
    DISALLOW_COPY_AND_ASSIGN(TestLocationBar);
  };

  class TestOmniboxPageActionIconContainer : public PageActionIconContainer {
   public:
    TestOmniboxPageActionIconContainer() {}
    ~TestOmniboxPageActionIconContainer() override {}

    // PageActionIconContainer:
    void UpdatePageActionIcon(PageActionIconType type) override {}
    void ExecutePageActionIconForTesting(PageActionIconType type) override {}

   private:
    DISALLOW_COPY_AND_ASSIGN(TestOmniboxPageActionIconContainer);
  };

  TestDownloadShelf download_shelf_;
  TestLocationBar location_bar_;
  TestOmniboxPageActionIconContainer omnibox_page_action_icon_container_;

  DISALLOW_COPY_AND_ASSIGN(TestBrowserWindow);
};

// Handles destroying a TestBrowserWindow when the Browser it is attached to is
// destroyed.
class TestBrowserWindowOwner : public BrowserListObserver {
 public:
  explicit TestBrowserWindowOwner(TestBrowserWindow* window);
  ~TestBrowserWindowOwner() override;

 private:
  // Overridden from BrowserListObserver:
  void OnBrowserRemoved(Browser* browser) override;
  std::unique_ptr<TestBrowserWindow> window_;

  DISALLOW_COPY_AND_ASSIGN(TestBrowserWindowOwner);
};

// Helper that handle the lifetime of TestBrowserWindow instances.
std::unique_ptr<Browser> CreateBrowserWithTestWindowForParams(
    Browser::CreateParams* params);

#endif  // CHROME_TEST_BASE_TEST_BROWSER_WINDOW_H_
