// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/accessibility/autoclick_scroll_view.h"

#include "ash/autoclick/autoclick_controller.h"
#include "ash/resources/vector_icons/vector_icons.h"
#include "ash/shell.h"
#include "ash/strings/grit/ash_strings.h"
#include "ash/system/tray/tray_constants.h"
#include "ash/system/unified/custom_shape_button.h"
#include "ash/system/unified/top_shortcut_button.h"
#include "base/macros.h"
#include "base/timer/timer.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/gfx/paint_vector_icon.h"
#include "ui/gfx/vector_icon_types.h"
#include "ui/views/animation/ink_drop_mask.h"
#include "ui/views/masked_targeter_delegate.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"

namespace ash {

namespace {

// Constants for color, size and position.
constexpr SkColor kScrollpadStrokeColor = SkColorSetARGB(0x1A, 255, 255, 255);
constexpr SkColor kScrollpadActiveColor =
    SkColorSetA(kUnifiedMenuButtonColor, 0x29);
constexpr int kScrollButtonCloseSizeDips = 48;
constexpr int kScrollpadStrokeWidthDips = 2;
constexpr int kScrollPadButtonHypotenuseDips = 192;
constexpr int kScrollPadIconPadding = 30;

}  // namespace

// The close button for the automatic clicks scroll bubble.
class AutoclickScrollCloseButton : public TopShortcutButton,
                                   public views::ButtonListener {
 public:
  explicit AutoclickScrollCloseButton()
      : TopShortcutButton(this, IDS_ASH_AUTOCLICK_SCROLL_CLOSE) {
    views::View::SetID(
        static_cast<int>(AutoclickScrollView::ButtonId::kCloseScroll));
    EnableCanvasFlippingForRTLUI(false);
    SetPreferredSize(
        gfx::Size(kScrollButtonCloseSizeDips, kScrollButtonCloseSizeDips));
    SetImage(views::Button::STATE_NORMAL,
             gfx::CreateVectorIcon(kAutoclickCloseIcon, kUnifiedMenuIconColor));
  }

  ~AutoclickScrollCloseButton() override = default;

  // views::ButtonListener:
  void ButtonPressed(views::Button* sender, const ui::Event& event) override {
    Shell::Get()->autoclick_controller()->DoScrollAction(
        AutoclickController::ScrollPadAction::kScrollClose);
  }

  // views::View:
  void OnMouseEntered(const ui::MouseEvent& event) override {
    hovered_ = true;
    SchedulePaint();
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    hovered_ = false;
    SchedulePaint();
  }

  // views::ImageButton:
  std::unique_ptr<views::InkDropMask> CreateInkDropMask() const override {
    gfx::Rect bounds = GetContentsBounds();
    return std::make_unique<views::CircleInkDropMask>(
        size(), bounds.CenterPoint(), bounds.width() / 2);
  }

  // TopShortcutButton:
  void PaintButtonContents(gfx::Canvas* canvas) override {
    if (hovered_) {
      gfx::Rect rect(GetContentsBounds());
      cc::PaintFlags flags;
      flags.setAntiAlias(true);
      flags.setStyle(cc::PaintFlags::kFill_Style);
      flags.setColor(kScrollpadActiveColor);
      canvas->DrawCircle(gfx::PointF(rect.CenterPoint()),
                         kScrollButtonCloseSizeDips / 2, flags);
    }
    views::ImageButton::PaintButtonContents(canvas);
  }

 private:
  bool hovered_ = false;
  DISALLOW_COPY_AND_ASSIGN(AutoclickScrollCloseButton);
};

// A single scroll button (up/down/left/right) for automatic clicks scroll
// bubble. Subclasses a MaskedTargeterDelegate in order to only get events over
// the button's custom shape, rather than over the whole rectangle which
// encloses the button.
class AutoclickScrollButton : public CustomShapeButton,
                              public views::MaskedTargeterDelegate,
                              public views::ButtonListener {
 public:
  AutoclickScrollButton(AutoclickController::ScrollPadAction action,
                        const gfx::VectorIcon& icon,
                        int accessible_name_id,
                        AutoclickScrollView::ButtonId id)
      : CustomShapeButton(this), action_(action) {
    views::View::SetID(static_cast<int>(id));
    SetTooltipText(l10n_util::GetStringUTF16(accessible_name_id));
    // Disable canvas flipping, as scroll left should always be left no matter
    // the language orientation.
    EnableCanvasFlippingForRTLUI(false);
    scroll_hover_timer_ = std::make_unique<base::RetainingOneShotTimer>(
        FROM_HERE,
        base::TimeDelta::FromMilliseconds(
            int64_t{AutoclickScrollView::kAutoclickScrollDelayMs}),
        base::BindRepeating(&AutoclickScrollButton::DoScrollAction,
                            base::Unretained(this)));
    SetImage(views::Button::STATE_NORMAL,
             gfx::CreateVectorIcon(icon, kUnifiedMenuIconColor));
    if (action_ == AutoclickController::ScrollPadAction::kScrollLeft ||
        action_ == AutoclickController::ScrollPadAction::kScrollRight) {
      size_ = gfx::Size(kScrollPadButtonHypotenuseDips / 2,
                        kScrollPadButtonHypotenuseDips);
    } else if (action_ == AutoclickController::ScrollPadAction::kScrollUp ||
               action_ == AutoclickController::ScrollPadAction::kScrollDown) {
      size_ = gfx::Size(kScrollPadButtonHypotenuseDips,
                        kScrollPadButtonHypotenuseDips / 2);
    }
    SetPreferredSize(size_);

    auto path = std::make_unique<SkPath>(
        CreateCustomShapePath(gfx::Rect(GetPreferredSize())));
    SetProperty(views::kHighlightPathKey, path.release());

    set_clip_path(CreateCustomShapePath(gfx::Rect(GetPreferredSize())));
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  }

  ~AutoclickScrollButton() override {
    Shell::Get()->autoclick_controller()->OnExitedScrollButton();
  }

  void ProcessAction(AutoclickController::ScrollPadAction action) {
    Shell::Get()->autoclick_controller()->DoScrollAction(action);
    // TODO(katie): Log UMA for scroll user action.
  }

  void DoScrollAction() {
    ProcessAction(action_);
    // Reset the timer to continue to do the action as long as we are hovering.
    scroll_hover_timer_->Reset();
  }

  // views::ButtonListener:
  void ButtonPressed(views::Button* sender, const ui::Event& event) override {
    ProcessAction(action_);
  }

  // CustomShapeButton:
  SkPath CreateCustomShapePath(const gfx::Rect& bounds) const override {
    return ComputePath(true /* all_edges */);
  }

  // Computes the path which is the outline of this button. If |all_edges|,
  // returns a path which fully encloses the shape, otherwise just returns a
  // path that can be used for drawing the edges but avoids overlap with
  // neighboring buttons.
  SkPath ComputePath(bool all_edges) const {
    int height = kScrollPadButtonHypotenuseDips;
    int width = height / 2;
    int half_width = width / 2;
    SkPath path;
    if (all_edges) {
      path.moveTo(0, 0);
      path.lineTo(0, height);
    } else {
      path.moveTo(0, height);
    }
    // Move to the edge of the close button.
    float offset = (kScrollButtonCloseSizeDips / 2.) / sqrt(2.);
    path.lineTo(SkIntToScalar(width - int(offset)),
                SkIntToScalar(width + int(offset)));
    gfx::Rect oval =
        gfx::Rect(width - kScrollButtonCloseSizeDips / 2,
                  width - kScrollButtonCloseSizeDips / 2,
                  kScrollButtonCloseSizeDips, kScrollButtonCloseSizeDips);
    path.arcTo(gfx::RectToSkRect(oval), 135, 90, false);
    if (all_edges) {
      path.lineTo(0, 0);
    }

    if (action_ == AutoclickController::ScrollPadAction::kScrollLeft)
      return path;

    SkMatrix matrix;
    if (action_ == AutoclickController::ScrollPadAction::kScrollUp) {
      matrix.setRotate(90, half_width, width);
      matrix.postTranslate(half_width, -half_width);
    } else if (action_ == AutoclickController::ScrollPadAction::kScrollRight) {
      matrix.setRotate(180, half_width, width);
    } else if (action_ == AutoclickController::ScrollPadAction::kScrollDown) {
      matrix.setRotate(270, half_width, width);
      matrix.postTranslate(half_width, -half_width);
    }
    path.transform(matrix);
    return path;
  }

  void PaintButtonContents(gfx::Canvas* canvas) override {
    gfx::Rect rect(GetContentsBounds());
    cc::PaintFlags flags;
    flags.setAntiAlias(true);

    if (active_) {
      flags.setColor(kScrollpadActiveColor);
      flags.setStyle(cc::PaintFlags::kFill_Style);
      canvas->DrawPath(CreateCustomShapePath(rect), flags);
    }

    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setStrokeWidth(kScrollpadStrokeWidthDips);
    flags.setColor(kScrollpadStrokeColor);
    canvas->DrawPath(ComputePath(false /* only drawn edges */), flags);

    gfx::ImageSkia img = GetImageToPaint();
    int halfImageSize = img.width() / 2;
    gfx::Point position;
    if (action_ == AutoclickController::ScrollPadAction::kScrollLeft) {
      position = gfx::Point(kScrollPadIconPadding,
                            kScrollPadButtonHypotenuseDips / 2 - halfImageSize);
    } else if (action_ == AutoclickController::ScrollPadAction::kScrollRight) {
      position = gfx::Point(kScrollPadButtonHypotenuseDips / 2 - img.width() -
                                kScrollPadIconPadding,
                            kScrollPadButtonHypotenuseDips / 2 - halfImageSize);
    } else if (action_ == AutoclickController::ScrollPadAction::kScrollUp) {
      position = gfx::Point(kScrollPadButtonHypotenuseDips / 2 - halfImageSize,
                            kScrollPadIconPadding);
    } else if (action_ == AutoclickController::ScrollPadAction::kScrollDown) {
      position = gfx::Point(kScrollPadButtonHypotenuseDips / 2 - halfImageSize,
                            kScrollPadButtonHypotenuseDips / 2 - img.width() -
                                kScrollPadIconPadding);
    }
    canvas->DrawImageInt(img, position.x(), position.y());
  }

  // views::MaskedTargeterDelegate:
  bool GetHitTestMask(SkPath* mask) const override {
    DCHECK(mask);
    gfx::Rect rect(GetContentsBounds());
    mask->addPath(CreateCustomShapePath(rect));
    return true;
  }

  // views::View:
  void OnMouseEntered(const ui::MouseEvent& event) override {
    // For the four scroll buttons, set the button to a hovered/active state
    // when this happens. Also tell something (autoclick controller? autoclick
    // scroll controller?) to start the timer that will cause a lot of scrolls
    // to occur, "clicking" on itself multiple times or otherwise.
    active_ = true;
    scroll_hover_timer_->Reset();
    Shell::Get()->autoclick_controller()->OnEnteredScrollButton();
    SchedulePaint();
  }

  // TODO(katie): Determine if this is reliable enough, or if it might not fire
  // in some cases.
  void OnMouseExited(const ui::MouseEvent& event) override {
    // For the four scroll buttons, unset the hover state when this happens.
    active_ = false;
    if (scroll_hover_timer_->IsRunning())
      scroll_hover_timer_->Stop();

    // Allow the Autoclick timer and widget to restart.
    Shell::Get()->autoclick_controller()->OnExitedScrollButton();
    SchedulePaint();
  }

 private:
  const AutoclickController::ScrollPadAction action_;
  gfx::Size size_;
  std::unique_ptr<base::RetainingOneShotTimer> scroll_hover_timer_;
  bool active_ = false;

  DISALLOW_COPY_AND_ASSIGN(AutoclickScrollButton);
};

// ------ AutoclickScrollBubbleView  ------ //

AutoclickScrollBubbleView::AutoclickScrollBubbleView(
    TrayBubbleView::InitParams init_params)
    : TrayBubbleView(init_params) {}

AutoclickScrollBubbleView::~AutoclickScrollBubbleView() {}

void AutoclickScrollBubbleView::UpdateAnchorRect(const gfx::Rect& rect) {
  ui::ScopedLayerAnimationSettings settings(
      GetWidget()->GetLayer()->GetAnimator());
  settings.SetPreemptionStrategy(
      ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);
  SetAnchorRect(rect);
}

bool AutoclickScrollBubbleView::IsAnchoredToStatusArea() const {
  return false;
}

const char* AutoclickScrollBubbleView::GetClassName() const {
  return "AutoclickScrollBubbleView";
}

// ------ AutoclickScrollView  ------ //

AutoclickScrollView::AutoclickScrollView()
    : scroll_up_button_(new AutoclickScrollButton(
          AutoclickController::ScrollPadAction::kScrollUp,
          kAutoclickScrollUpIcon,
          IDS_ASH_AUTOCLICK_SCROLL_UP,
          ButtonId::kScrollUp)),
      scroll_down_button_(new AutoclickScrollButton(
          AutoclickController::ScrollPadAction::kScrollDown,
          kAutoclickScrollDownIcon,
          IDS_ASH_AUTOCLICK_SCROLL_DOWN,
          ButtonId::kScrollDown)),
      scroll_left_button_(new AutoclickScrollButton(
          AutoclickController::ScrollPadAction::kScrollLeft,
          kAutoclickScrollLeftIcon,
          IDS_ASH_AUTOCLICK_SCROLL_LEFT,
          ButtonId::kScrollLeft)),
      scroll_right_button_(new AutoclickScrollButton(
          AutoclickController::ScrollPadAction::kScrollRight,
          kAutoclickScrollRightIcon,
          IDS_ASH_AUTOCLICK_SCROLL_RIGHT,
          ButtonId::kScrollRight)),
      close_scroll_button_(new AutoclickScrollCloseButton()) {
  SetPreferredSize(gfx::Size(kScrollPadButtonHypotenuseDips,
                             kScrollPadButtonHypotenuseDips));
  AddChildView(close_scroll_button_);
  AddChildView(scroll_up_button_);
  AddChildView(scroll_down_button_);
  AddChildView(scroll_left_button_);
  AddChildView(scroll_right_button_);
}

void AutoclickScrollView::Layout() {
  scroll_up_button_->SetBounds(0, 0, kScrollPadButtonHypotenuseDips,
                               kScrollPadButtonHypotenuseDips / 2);
  scroll_down_button_->SetBounds(0, kScrollPadButtonHypotenuseDips / 2,
                                 kScrollPadButtonHypotenuseDips,
                                 kScrollPadButtonHypotenuseDips / 2);
  scroll_left_button_->SetBounds(0, 0, kScrollPadButtonHypotenuseDips / 2,
                                 kScrollPadButtonHypotenuseDips);
  scroll_right_button_->SetBounds(kScrollPadButtonHypotenuseDips / 2, 0,
                                  kScrollPadButtonHypotenuseDips / 2,
                                  kScrollPadButtonHypotenuseDips);
  close_scroll_button_->SetBounds(
      kScrollPadButtonHypotenuseDips / 2 - kScrollButtonCloseSizeDips / 2,
      kScrollPadButtonHypotenuseDips / 2 - kScrollButtonCloseSizeDips / 2,
      kScrollButtonCloseSizeDips, kScrollButtonCloseSizeDips);
}

}  // namespace ash
