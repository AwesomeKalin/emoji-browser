// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_SEARCH_CHROME_COLORS_CHROME_COLORS_SERVICE_H_
#define CHROME_BROWSER_SEARCH_CHROME_COLORS_CHROME_COLORS_SERVICE_H_

#include "base/callback.h"
#include "chrome/browser/themes/theme_service.h"
#include "components/keyed_service/core/keyed_service.h"
#include "third_party/skia/include/core/SkColor.h"

class TestChromeColorsService;

namespace chrome_colors {

// Supports theme changes originating from the NTP customization menu. Users can
// apply a Chrome color or the default theme, which will then either be reverted
// or confirmed and made permanent. If third party themes are present, users
// will also have a choice to permanently uninstall it.
class ChromeColorsService : public KeyedService {
 public:
  explicit ChromeColorsService(Profile* profile);
  ~ChromeColorsService() override;

  // Applies a theme that can be reverted. Saves the previous theme state if
  // needed.
  void ApplyDefaultTheme();
  void ApplyAutogeneratedTheme(SkColor color);

  // Reverts to the previous theme state before Apply* was used.
  void RevertThemeChanges();

  // Confirms current theme changes. Since the theme is already installed by
  // Apply*, this only clears the previously saved state.
  void ConfirmThemeChanges();

 private:
  friend class ::TestChromeColorsService;

  // KeyedService implementation:
  void Shutdown() override;

  ThemeService* const theme_service_;

  // Callback that will revert the theme to the state it was at the time of this
  // callback's creation.
  base::OnceClosure revert_theme_changes_;
};

}  // namespace chrome_colors

#endif  // CHROME_BROWSER_SEARCH_CHROME_COLORS_CHROME_COLORS_SERVICE_H_
