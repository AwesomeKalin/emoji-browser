// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/send_tab_to_self/send_tab_to_self_bubble_view_impl.h"

#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/send_tab_to_self/send_tab_to_self_bubble_controller.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/browser/ui/views/page_action/omnibox_page_action_icon_container_view.h"
#include "chrome/browser/ui/views/send_tab_to_self/send_tab_to_self_bubble_device_button.h"
#include "chrome/grit/generated_resources.h"
#include "components/send_tab_to_self/target_device_info.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/ui_base_types.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/fill_layout.h"

namespace send_tab_to_self {

SendTabToSelfBubbleViewImpl::SendTabToSelfBubbleViewImpl(
    views::View* anchor_view,
    const gfx::Point& anchor_point,
    content::WebContents* web_contents,
    SendTabToSelfBubbleController* controller)
    : LocationBarBubbleDelegateView(anchor_view, anchor_point, web_contents),
      web_contents_(web_contents),
      controller_(controller),
      weak_factory_(this) {
  DCHECK(controller);
}

SendTabToSelfBubbleViewImpl::~SendTabToSelfBubbleViewImpl() {}

void SendTabToSelfBubbleViewImpl::Hide() {
  if (controller_) {
    controller_->OnBubbleClosed();
    controller_ = nullptr;
  }
  CloseBubble();
}

bool SendTabToSelfBubbleViewImpl::ShouldShowCloseButton() const {
  return true;
}

base::string16 SendTabToSelfBubbleViewImpl::GetWindowTitle() const {
  return controller_->GetWindowTitle();
}

void SendTabToSelfBubbleViewImpl::WindowClosing() {
  if (controller_) {
    controller_->OnBubbleClosed();
    controller_ = nullptr;
  }
}

int SendTabToSelfBubbleViewImpl::GetDialogButtons() const {
  return ui::DIALOG_BUTTON_NONE;
}

bool SendTabToSelfBubbleViewImpl::Close() {
  return Cancel();
}

void SendTabToSelfBubbleViewImpl::ButtonPressed(views::Button* sender,
                                                const ui::Event& event) {
  base::PostTaskWithTraits(
      FROM_HERE, {content::BrowserThread::UI},
      base::BindOnce(&SendTabToSelfBubbleViewImpl::DevicePressed,
                     weak_factory_.GetWeakPtr(), sender->tag()));
}

gfx::Size SendTabToSelfBubbleViewImpl::CalculatePreferredSize() const {
  const int width = ChromeLayoutProvider::Get()->GetDistanceMetric(
      DISTANCE_BUBBLE_PREFERRED_WIDTH);
  return gfx::Size(width, GetHeightForWidth(width));
}

void SendTabToSelfBubbleViewImpl::OnPaint(gfx::Canvas* canvas) {
  views::BubbleDialogDelegateView::OnPaint(canvas);
}

void SendTabToSelfBubbleViewImpl::Show(DisplayReason reason) {
  ShowForReason(reason);
  // Keeps the send tab to self icon in omnibox and be highlighted while
  // showing the bubble.
  views::Button* highlight_button =
      BrowserView::GetBrowserViewForBrowser(
          chrome::FindBrowserWithWebContents(web_contents_))
          ->toolbar_button_provider()
          ->GetOmniboxPageActionIconContainerView()
          ->GetPageActionIconView(PageActionIconType::kSendTabToSelf);
  highlight_button->SetVisible(true);
  SetHighlightedButton(highlight_button);
}

const std::vector<std::unique_ptr<SendTabToSelfBubbleDeviceButton>>&
SendTabToSelfBubbleViewImpl::GetDeviceButtonsForTest() {
  return device_buttons_;
}

void SendTabToSelfBubbleViewImpl::Init() {
  auto* provider = ChromeLayoutProvider::Get();
  set_margins(
      gfx::Insets(provider->GetDistanceMetric(
                      views::DISTANCE_DIALOG_CONTENT_MARGIN_TOP_CONTROL),
                  0,
                  provider->GetDistanceMetric(
                      views::DISTANCE_DIALOG_CONTENT_MARGIN_BOTTOM_CONTROL),
                  0));
  SetLayoutManager(std::make_unique<views::FillLayout>());

  CreateScrollView();

  PopulateScrollView(controller_->GetValidDevices());
}

void SendTabToSelfBubbleViewImpl::CreateScrollView() {
  scroll_view_ = new views::ScrollView();
  AddChildView(scroll_view_);
  scroll_view_->ClipHeightTo(0, kDeviceButtonHeight * kMaximumButtons);
}

void SendTabToSelfBubbleViewImpl::PopulateScrollView(
    const std::map<std::string, TargetDeviceInfo> devices) {
  device_buttons_.clear();
  auto device_list_view = std::make_unique<views::View>();
  device_list_view->SetLayoutManager(
      std::make_unique<views::BoxLayout>(views::BoxLayout::kVertical));
  int tag = 0;
  for (const auto& device : devices) {
    auto device_button = std::make_unique<SendTabToSelfBubbleDeviceButton>(
        this, device.first, device.second,
        /** button_tag */ tag++);
    device_buttons_.push_back(std::move(device_button));
    device_list_view->AddChildView(device_buttons_.back().get());
  }
  scroll_view_->SetContents(std::move(device_list_view));

  MaybeSizeToContents();
  Layout();
}

void SendTabToSelfBubbleViewImpl::DevicePressed(size_t index) {
  if (!controller_) {
    return;
  }
  SendTabToSelfBubbleDeviceButton* device_button =
      device_buttons_.at(index).get();
  controller_->OnDeviceSelected(device_button->device_name(),
                                device_button->device_guid());
  Hide();
}

void SendTabToSelfBubbleViewImpl::MaybeSizeToContents() {
  // The widget may be null if this is called while the dialog is opening.
  if (GetWidget()) {
    SizeToContents();
  }
}

}  // namespace send_tab_to_self
