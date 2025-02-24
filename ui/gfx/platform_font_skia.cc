// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/platform_font_skia.h"

#include <algorithm>
#include <string>

#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/strings/string_piece.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "third_party/skia/include/core/SkFont.h"
#include "third_party/skia/include/core/SkFontMetrics.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkString.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/font.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/font_render_params.h"
#include "ui/gfx/skia_font_delegate.h"
#include "ui/gfx/text_utils.h"

namespace gfx {
namespace {

// The font family name which is used when a user's application font for
// GNOME/KDE is a non-scalable one. The name should be listed in the
// IsFallbackFontAllowed function in skia/ext/SkFontHost_fontconfig_direct.cpp.
#if defined(OS_ANDROID)
const char* kFallbackFontFamilyName = "serif";
#else
const char* kFallbackFontFamilyName = "sans";
#endif

// The default font, used for the default constructor.
base::LazyInstance<scoped_refptr<PlatformFontSkia>>::Leaky g_default_font =
    LAZY_INSTANCE_INITIALIZER;

// Creates a SkTypeface for the passed-in Font::FontStyle and family. If a
// fallback typeface is used instead of the requested family, |family| will be
// updated to contain the fallback's family name.
sk_sp<SkTypeface> CreateSkTypeface(bool italic,
                                   gfx::Font::Weight weight,
                                   std::string* family,
                                   bool* out_success) {
  DCHECK(family);
  TRACE_EVENT0("fonts", "gfx::CreateSkTypeface");

  const int font_weight = (weight == Font::Weight::INVALID)
                              ? static_cast<int>(Font::Weight::NORMAL)
                              : static_cast<int>(weight);
  SkFontStyle sk_style(
      font_weight, SkFontStyle::kNormal_Width,
      italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
  sk_sp<SkTypeface> typeface;
  {
    TRACE_EVENT1(TRACE_DISABLED_BY_DEFAULT("fonts"), "SkTypeface::MakeFromName",
                 "family", *family);
    typeface = SkTypeface::MakeFromName(family->c_str(), sk_style);
  }
  if (!typeface) {
    TRACE_EVENT1(TRACE_DISABLED_BY_DEFAULT("fonts"), "SkTypeface::MakeFromName",
                 "family", kFallbackFontFamilyName);
    // A non-scalable font such as .pcf is specified. Fall back to a default
    // scalable font.
    typeface = sk_sp<SkTypeface>(
        SkTypeface::MakeFromName(kFallbackFontFamilyName, sk_style));
    if (!typeface) {
      *out_success = false;
      return nullptr;
    }
    *family = kFallbackFontFamilyName;
  }
  *out_success = true;
  return typeface;
}

}  // namespace

std::string* PlatformFontSkia::default_font_description_ = NULL;

////////////////////////////////////////////////////////////////////////////////
// PlatformFontSkia, public:

PlatformFontSkia::PlatformFontSkia() {
  CHECK(InitDefaultFont()) << "Could not find the default font";
  InitFromPlatformFont(g_default_font.Get().get());
}

PlatformFontSkia::PlatformFontSkia(const std::string& font_name,
                                   int font_size_pixels) {
  FontRenderParamsQuery query;
  query.families.push_back(font_name);
  query.pixel_size = font_size_pixels;
  query.weight = Font::Weight::NORMAL;
  InitFromDetails(nullptr, font_name, font_size_pixels, Font::NORMAL,
                  query.weight, gfx::GetFontRenderParams(query, nullptr));
}

////////////////////////////////////////////////////////////////////////////////
// PlatformFontSkia, PlatformFont implementation:

// static
bool PlatformFontSkia::InitDefaultFont() {
  if (g_default_font.Get())
    return true;

  bool success = false;
  std::string family = kFallbackFontFamilyName;
  int size_pixels = PlatformFont::kDefaultBaseFontSize;
  int style = Font::NORMAL;
  Font::Weight weight = Font::Weight::NORMAL;
  FontRenderParams params;

  // On Linux, SkiaFontDelegate is used to query the native toolkit (e.g.
  // GTK+) for the default UI font.
  const SkiaFontDelegate* delegate = SkiaFontDelegate::instance();
  if (delegate) {
    delegate->GetDefaultFontDescription(&family, &size_pixels, &style, &weight,
                                        &params);
  } else if (default_font_description_) {
#if defined(OS_CHROMEOS)
    // On ChromeOS, a FontList font description string is stored as a
    // translatable resource and passed in via SetDefaultFontDescription().
    FontRenderParamsQuery query;
    CHECK(FontList::ParseDescription(*default_font_description_,
                                     &query.families, &query.style,
                                     &query.pixel_size, &query.weight))
        << "Failed to parse font description " << *default_font_description_;
    params = gfx::GetFontRenderParams(query, &family);
    size_pixels = query.pixel_size;
    style = query.style;
    weight = query.weight;
#else
    NOTREACHED();
#endif
  }

  sk_sp<SkTypeface> typeface =
      CreateSkTypeface(style & Font::ITALIC, weight, &family, &success);
  if (!success)
    return false;
  g_default_font.Get() = new PlatformFontSkia(
      std::move(typeface), family, size_pixels, style, weight, params);
  return true;
}

// static
void PlatformFontSkia::ReloadDefaultFont() {
  // Reset the scoped_refptr.
  g_default_font.Get() = nullptr;
}

// static
void PlatformFontSkia::SetDefaultFontDescription(
    const std::string& font_description) {
  delete default_font_description_;
  default_font_description_ = new std::string(font_description);
}

Font PlatformFontSkia::DeriveFont(int size_delta,
                                  int style,
                                  Font::Weight weight) const {
  const int new_size = font_size_pixels_ + size_delta;
  DCHECK_GT(new_size, 0);

  // If the style changed, we may need to load a new face.
  std::string new_family = font_family_;
  bool success = true;
  sk_sp<SkTypeface> typeface =
      (weight == weight_ && style == style_)
          ? typeface_
          : CreateSkTypeface(style, weight, &new_family, &success);
  if (!success) {
    LOG(ERROR) << "Could not find any font: " << new_family << ", "
               << kFallbackFontFamilyName << ". Falling back to the default";
    return Font(new PlatformFontSkia);
  }

  FontRenderParamsQuery query;
  query.families.push_back(new_family);
  query.pixel_size = new_size;
  query.style = style;

  return Font(new PlatformFontSkia(std::move(typeface), new_family, new_size,
                                   style, weight,
                                   gfx::GetFontRenderParams(query, NULL)));
}

int PlatformFontSkia::GetHeight() {
  ComputeMetricsIfNecessary();
  return height_pixels_;
}

Font::Weight PlatformFontSkia::GetWeight() const {
  return weight_;
}

int PlatformFontSkia::GetBaseline() {
  ComputeMetricsIfNecessary();
  return ascent_pixels_;
}

int PlatformFontSkia::GetCapHeight() {
  ComputeMetricsIfNecessary();
  return cap_height_pixels_;
}

int PlatformFontSkia::GetExpectedTextWidth(int length) {
  ComputeMetricsIfNecessary();
  return round(static_cast<float>(length) * average_width_pixels_);
}

int PlatformFontSkia::GetStyle() const {
  return style_;
}

const std::string& PlatformFontSkia::GetFontName() const {
  return font_family_;
}

std::string PlatformFontSkia::GetActualFontNameForTesting() const {
  SkString family_name;
  typeface_->getFamilyName(&family_name);
  return family_name.c_str();
}

int PlatformFontSkia::GetFontSize() const {
  return font_size_pixels_;
}

const FontRenderParams& PlatformFontSkia::GetFontRenderParams() {
  TRACE_EVENT0("fonts", "PlatformFontSkia::GetFontRenderParams");
  float current_scale_factor = GetFontRenderParamsDeviceScaleFactor();
  if (current_scale_factor != device_scale_factor_) {
    FontRenderParamsQuery query;
    query.families.push_back(font_family_);
    query.pixel_size = font_size_pixels_;
    query.style = style_;
    query.weight = weight_;
    query.device_scale_factor = current_scale_factor;
    font_render_params_ = gfx::GetFontRenderParams(query, nullptr);
    device_scale_factor_ = current_scale_factor;
  }
  return font_render_params_;
}

////////////////////////////////////////////////////////////////////////////////
// PlatformFontSkia, private:

PlatformFontSkia::PlatformFontSkia(sk_sp<SkTypeface> typeface,
                                   const std::string& family,
                                   int size_pixels,
                                   int style,
                                   Font::Weight weight,
                                   const FontRenderParams& render_params) {
  InitFromDetails(std::move(typeface), family, size_pixels, style, weight,
                  render_params);
}

PlatformFontSkia::~PlatformFontSkia() {}

void PlatformFontSkia::InitFromDetails(sk_sp<SkTypeface> typeface,
                                       const std::string& font_family,
                                       int font_size_pixels,
                                       int style,
                                       Font::Weight weight,
                                       const FontRenderParams& render_params) {
  TRACE_EVENT0("fonts", "PlatformFontSkia::InitFromDetails");
  DCHECK_GT(font_size_pixels, 0);

  font_family_ = font_family;
  bool success = true;
  typeface_ = typeface ? std::move(typeface)
                       : CreateSkTypeface(style & Font::ITALIC, weight,
                                          &font_family_, &success);

  if (!success) {
    LOG(ERROR) << "Could not find any font: " << font_family << ", "
               << kFallbackFontFamilyName << ". Falling back to the default";

    InitFromPlatformFont(g_default_font.Get().get());
    return;
  }

  font_size_pixels_ = font_size_pixels;
  style_ = style;
  weight_ = weight;
  device_scale_factor_ = GetFontRenderParamsDeviceScaleFactor();
  font_render_params_ = render_params;
}

void PlatformFontSkia::InitFromPlatformFont(const PlatformFontSkia* other) {
  TRACE_EVENT0("fonts", "PlatformFontSkia::InitFromPlatformFont");
  typeface_ = other->typeface_;
  font_family_ = other->font_family_;
  font_size_pixels_ = other->font_size_pixels_;
  style_ = other->style_;
  weight_ = other->weight_;
  device_scale_factor_ = other->device_scale_factor_;
  font_render_params_ = other->font_render_params_;

  if (!other->metrics_need_computation_) {
    metrics_need_computation_ = false;
    ascent_pixels_ = other->ascent_pixels_;
    height_pixels_ = other->height_pixels_;
    cap_height_pixels_ = other->cap_height_pixels_;
    average_width_pixels_ = other->average_width_pixels_;
  }
}

void PlatformFontSkia::ComputeMetricsIfNecessary() {
  if (metrics_need_computation_) {
    TRACE_EVENT0("fonts", "PlatformFontSkia::ComputeMetricsIfNecessary");

    metrics_need_computation_ = false;

    SkFont font(typeface_, font_size_pixels_);
    font.setEdging(SkFont::Edging::kAlias);
    font.setEmbolden(weight_ >= Font::Weight::BOLD && !typeface_->isBold());
    font.setSkewX((Font::ITALIC & style_) && !typeface_->isItalic()
                      ? -SK_Scalar1 / 4
                      : 0);
    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    ascent_pixels_ = SkScalarCeilToInt(-metrics.fAscent);
    height_pixels_ = ascent_pixels_ + SkScalarCeilToInt(metrics.fDescent);
    cap_height_pixels_ = SkScalarCeilToInt(metrics.fCapHeight);

    if (metrics.fAvgCharWidth) {
      average_width_pixels_ = SkScalarToDouble(metrics.fAvgCharWidth);
    } else {
      // Some Skia fonts manager do not compute the average character size
      // (e.g. Direct Write). The default behavior when the metric is not
      // available is to use the max char width.
      average_width_pixels_ = SkScalarToDouble(metrics.fMaxCharWidth);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// PlatformFont, public:

#if !defined(OS_WIN)
// static
PlatformFont* PlatformFont::CreateDefault() {
  return new PlatformFontSkia;
}

// static
PlatformFont* PlatformFont::CreateFromNameAndSize(const std::string& font_name,
                                                  int font_size) {
  TRACE_EVENT0("fonts", "PlatformFont::CreateFromNameAndSize");
  return new PlatformFontSkia(font_name, font_size);
}
#endif  // !defined(OS_WIN)

}  // namespace gfx
