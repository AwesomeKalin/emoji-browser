// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/search/chrome_colors/chrome_colors_service.h"
#include "chrome/browser/search/chrome_colors/chrome_colors_factory.h"
#include "chrome/browser/themes/theme_service.h"
#include "chrome/browser/themes/theme_service_factory.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "testing/gtest/include/gtest/gtest.h"

class TestChromeColorsService : public BrowserWithTestWindowTest {
 protected:
  TestChromeColorsService() {}

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();
    chrome_colors_service_ =
        chrome_colors::ChromeColorsFactory::GetForProfile(profile());
  }

  bool HasThemeRevertCallback() {
    return !chrome_colors_service_->revert_theme_changes_.is_null();
  }

  chrome_colors::ChromeColorsService* chrome_colors_service_;
};

TEST_F(TestChromeColorsService, ApplyAndConfirmAutogeneratedTheme) {
  ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile());
  ASSERT_TRUE(theme_service->UsingDefaultTheme());

  SkColor theme_color1 = SkColorSetRGB(100, 0, 200);
  chrome_colors_service_->ApplyAutogeneratedTheme(theme_color1);
  EXPECT_TRUE(theme_service->UsingAutogenerated());
  EXPECT_TRUE(HasThemeRevertCallback());

  SkColor theme_color2 = SkColorSetRGB(100, 0, 200);
  chrome_colors_service_->ApplyAutogeneratedTheme(theme_color2);
  EXPECT_TRUE(theme_service->UsingAutogenerated());
  EXPECT_TRUE(HasThemeRevertCallback());

  // Last color is saved.
  chrome_colors_service_->ConfirmThemeChanges();
  EXPECT_TRUE(theme_service->UsingAutogenerated());
  EXPECT_EQ(theme_color2, theme_service->GetThemeColor());
  EXPECT_FALSE(HasThemeRevertCallback());
}

TEST_F(TestChromeColorsService, ApplyAndRevertAutogeneratedTheme) {
  ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile());
  ASSERT_TRUE(theme_service->UsingDefaultTheme());

  SkColor theme_color1 = SkColorSetRGB(100, 0, 200);
  chrome_colors_service_->ApplyAutogeneratedTheme(theme_color1);
  EXPECT_TRUE(theme_service->UsingAutogenerated());
  EXPECT_TRUE(HasThemeRevertCallback());

  SkColor theme_color2 = SkColorSetRGB(100, 0, 200);
  chrome_colors_service_->ApplyAutogeneratedTheme(theme_color2);
  EXPECT_TRUE(theme_service->UsingAutogenerated());
  EXPECT_TRUE(HasThemeRevertCallback());

  // State before first apply is restored.
  chrome_colors_service_->RevertThemeChanges();
  EXPECT_FALSE(theme_service->UsingAutogenerated());
  EXPECT_FALSE(HasThemeRevertCallback());
}

TEST_F(TestChromeColorsService,
       ApplyAndConfirmAutogeneratedTheme_withPreviousTheme) {
  ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile());
  SkColor prev_theme_color = SkColorSetRGB(200, 0, 200);
  theme_service->BuildFromColor(prev_theme_color);
  ASSERT_EQ(prev_theme_color, theme_service->GetThemeColor());

  SkColor new_theme_color = SkColorSetRGB(100, 0, 200);
  chrome_colors_service_->ApplyAutogeneratedTheme(new_theme_color);
  EXPECT_EQ(new_theme_color, theme_service->GetThemeColor());
  EXPECT_TRUE(HasThemeRevertCallback());

  chrome_colors_service_->ConfirmThemeChanges();
  EXPECT_TRUE(theme_service->UsingAutogenerated());
  EXPECT_EQ(new_theme_color, theme_service->GetThemeColor());
  EXPECT_FALSE(HasThemeRevertCallback());
}

TEST_F(TestChromeColorsService,
       ApplyAndRevertAutogeneratedTheme_withPreviousTheme) {
  ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile());
  SkColor prev_theme_color = SkColorSetRGB(200, 0, 200);
  theme_service->BuildFromColor(prev_theme_color);
  ASSERT_EQ(prev_theme_color, theme_service->GetThemeColor());

  SkColor new_theme_color = SkColorSetRGB(100, 0, 200);
  chrome_colors_service_->ApplyAutogeneratedTheme(new_theme_color);
  EXPECT_EQ(new_theme_color, theme_service->GetThemeColor());
  EXPECT_TRUE(HasThemeRevertCallback());

  chrome_colors_service_->RevertThemeChanges();
  EXPECT_TRUE(theme_service->UsingAutogenerated());
  EXPECT_EQ(prev_theme_color, theme_service->GetThemeColor());
  EXPECT_FALSE(HasThemeRevertCallback());
}

TEST_F(TestChromeColorsService, ApplyAndConfirmDefaultTheme_withPreviousTheme) {
  ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile());
  SkColor prev_theme_color = SkColorSetRGB(200, 0, 200);
  theme_service->BuildFromColor(prev_theme_color);
  ASSERT_EQ(prev_theme_color, theme_service->GetThemeColor());
  ASSERT_FALSE(theme_service->UsingDefaultTheme());

  chrome_colors_service_->ApplyDefaultTheme();
  EXPECT_TRUE(theme_service->UsingDefaultTheme());
  EXPECT_TRUE(HasThemeRevertCallback());

  chrome_colors_service_->ConfirmThemeChanges();
  EXPECT_TRUE(theme_service->UsingDefaultTheme());
  EXPECT_NE(prev_theme_color, theme_service->GetThemeColor());
  EXPECT_FALSE(HasThemeRevertCallback());
}

TEST_F(TestChromeColorsService, ApplyAndRevertDefaultTheme_withPreviousTheme) {
  ThemeService* theme_service = ThemeServiceFactory::GetForProfile(profile());
  SkColor prev_theme_color = SkColorSetRGB(200, 0, 200);
  theme_service->BuildFromColor(prev_theme_color);
  ASSERT_EQ(prev_theme_color, theme_service->GetThemeColor());
  ASSERT_FALSE(theme_service->UsingDefaultTheme());

  chrome_colors_service_->ApplyDefaultTheme();
  EXPECT_TRUE(theme_service->UsingDefaultTheme());
  EXPECT_TRUE(HasThemeRevertCallback());

  chrome_colors_service_->RevertThemeChanges();
  EXPECT_FALSE(theme_service->UsingDefaultTheme());
  EXPECT_EQ(prev_theme_color, theme_service->GetThemeColor());
  EXPECT_FALSE(HasThemeRevertCallback());
}
