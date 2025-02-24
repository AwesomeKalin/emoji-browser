// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/metrics/previous_session_info.h"

#import <UIKit/UIKit.h>

#include "base/logging.h"
#include "base/strings/sys_string_conversions.h"
#include "base/system/sys_info.h"
#include "components/version_info/version_info.h"
#import "ios/chrome/browser/metrics/previous_session_info_private.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

using previous_session_info_constants::DeviceBatteryState;
using previous_session_info_constants::DeviceThermalState;

namespace {

// Translates a UIDeviceBatteryState value to DeviceBatteryState value.
DeviceBatteryState GetBatteryStateFromUIDeviceBatteryState(
    UIDeviceBatteryState device_battery_state) {
  switch (device_battery_state) {
    case UIDeviceBatteryStateUnknown:
      return DeviceBatteryState::kUnknown;
    case UIDeviceBatteryStateUnplugged:
      return DeviceBatteryState::kUnplugged;
    case UIDeviceBatteryStateCharging:
      return DeviceBatteryState::kCharging;
    case UIDeviceBatteryStateFull:
      return DeviceBatteryState::kFull;
  }

  return DeviceBatteryState::kUnknown;
}

// Translates a NSProcessInfoThermalState value to DeviceThermalState value.
DeviceThermalState GetThermalStateFromNSProcessInfoThermalState(
    NSProcessInfoThermalState process_info_thermal_state) {
  switch (process_info_thermal_state) {
    case NSProcessInfoThermalStateNominal:
      return DeviceThermalState::kNominal;
    case NSProcessInfoThermalStateFair:
      return DeviceThermalState::kFair;
    case NSProcessInfoThermalStateSerious:
      return DeviceThermalState::kSerious;
    case NSProcessInfoThermalStateCritical:
      return DeviceThermalState::kCritical;
  }

  return DeviceThermalState::kUnknown;
}

// NSUserDefaults keys.
// - The (string) application version.
NSString* const kLastRanVersion = @"LastRanVersion";
// - The (string) device language.
NSString* const kLastRanLanguage = @"LastRanLanguage";
// - The (integer) available device storage, in kilobytes.
NSString* const kPreviousSessionInfoAvailableDeviceStorage =
    @"PreviousSessionInfoAvailableDeviceStorage";
// - The (float) battery charge level.
NSString* const kPreviousSessionInfoBatteryLevel =
    @"PreviousSessionInfoBatteryLevel";
// - The (integer) underlying value of the DeviceBatteryState enum representing
//   the device battery state.
NSString* const kPreviousSessionInfoBatteryState =
    @"PreviousSessionInfoBatteryState";
// - The (string) OS version.
NSString* const kPreviousSessionInfoOSVersion = @"PreviousSessionInfoOSVersion";
// - The (integer) underlying value of the DeviceThermalState enum representing
//   the device thermal state.
NSString* const kPreviousSessionInfoThermalState =
    @"PreviousSessionInfoThermalState";
// - A (boolean) describing whether or not low power mode is enabled.
NSString* const kPreviousSessionInfoLowPowerMode =
    @"PreviousSessionInfoLowPowerMode";

}  // namespace

namespace previous_session_info_constants {
NSString* const kDidSeeMemoryWarningShortlyBeforeTerminating =
    @"DidSeeMemoryWarning";
}  // namespace previous_session_info_constants

@interface PreviousSessionInfo ()

// Whether beginRecordingCurrentSession was called.
@property(nonatomic, assign) BOOL didBeginRecordingCurrentSession;

// Redefined to be read-write.
@property(nonatomic, assign) NSInteger availableDeviceStorage;
@property(nonatomic, assign) float deviceBatteryLevel;
@property(nonatomic, assign) DeviceBatteryState deviceBatteryState;
@property(nonatomic, assign) DeviceThermalState deviceThermalState;
@property(nonatomic, assign) BOOL deviceWasInLowPowerMode;
@property(nonatomic, assign) BOOL didSeeMemoryWarningShortlyBeforeTerminating;
@property(nonatomic, assign) BOOL isFirstSessionAfterOSUpgrade;
@property(nonatomic, assign) BOOL isFirstSessionAfterUpgrade;
@property(nonatomic, assign) BOOL isFirstSessionAfterLanguageChange;

@end

@implementation PreviousSessionInfo

@synthesize availableDeviceStorage = _availableDeviceStorage;
@synthesize deviceBatteryLevel = _deviceBatteryLevel;
@synthesize deviceBatteryState = _deviceBatteryState;
@synthesize deviceThermalState = _deviceThermalState;
@synthesize deviceWasInLowPowerMode = _deviceWasInLowPowerMode;
@synthesize didBeginRecordingCurrentSession = _didBeginRecordingCurrentSession;
@synthesize didSeeMemoryWarningShortlyBeforeTerminating =
    _didSeeMemoryWarningShortlyBeforeTerminating;
@synthesize isFirstSessionAfterOSUpgrade = _isFirstSessionAfterOSUpgrade;
@synthesize isFirstSessionAfterUpgrade = _isFirstSessionAfterUpgrade;
@synthesize isFirstSessionAfterLanguageChange =
    _isFirstSessionAfterLanguageChange;

// Singleton PreviousSessionInfo.
static PreviousSessionInfo* gSharedInstance = nil;

+ (instancetype)sharedInstance {
  if (!gSharedInstance) {
    gSharedInstance = [[PreviousSessionInfo alloc] init];

    // Load the persisted information.
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    gSharedInstance.availableDeviceStorage =
        [defaults integerForKey:kPreviousSessionInfoAvailableDeviceStorage];
    gSharedInstance.didSeeMemoryWarningShortlyBeforeTerminating =
        [defaults boolForKey:previous_session_info_constants::
                                 kDidSeeMemoryWarningShortlyBeforeTerminating];
    gSharedInstance.deviceWasInLowPowerMode =
        [defaults boolForKey:kPreviousSessionInfoLowPowerMode];
    gSharedInstance.deviceBatteryState = static_cast<DeviceBatteryState>(
        [defaults integerForKey:kPreviousSessionInfoBatteryState]);
    gSharedInstance.deviceBatteryLevel =
        [defaults floatForKey:kPreviousSessionInfoBatteryLevel];
    gSharedInstance.deviceThermalState = static_cast<DeviceThermalState>(
        [defaults integerForKey:kPreviousSessionInfoThermalState]);

    NSString* versionOfOSAtLastRun =
        [defaults stringForKey:kPreviousSessionInfoOSVersion];
    NSString* currentOSVersion =
        base::SysUTF8ToNSString(base::SysInfo::OperatingSystemVersion());
    gSharedInstance.isFirstSessionAfterOSUpgrade =
        ![versionOfOSAtLastRun isEqualToString:currentOSVersion];

    NSString* lastRanVersion = [defaults stringForKey:kLastRanVersion];
    NSString* currentVersion =
        base::SysUTF8ToNSString(version_info::GetVersionNumber());
    gSharedInstance.isFirstSessionAfterUpgrade =
        ![lastRanVersion isEqualToString:currentVersion];

    NSString* lastRanLanguage = [defaults stringForKey:kLastRanLanguage];
    NSString* currentLanguage = [[NSLocale preferredLanguages] objectAtIndex:0];
    gSharedInstance.isFirstSessionAfterLanguageChange =
        ![lastRanLanguage isEqualToString:currentLanguage];
  }
  return gSharedInstance;
}

+ (void)resetSharedInstanceForTesting {
  gSharedInstance = nil;
}

- (void)beginRecordingCurrentSession {
  if (self.didBeginRecordingCurrentSession)
    return;
  self.didBeginRecordingCurrentSession = YES;

  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];

  // Set the current Chrome version.
  NSString* currentVersion =
      base::SysUTF8ToNSString(version_info::GetVersionNumber());
  [defaults setObject:currentVersion forKey:kLastRanVersion];

  // Set the current OS version.
  NSString* currentOSVersion =
      base::SysUTF8ToNSString(base::SysInfo::OperatingSystemVersion());
  [defaults setObject:currentOSVersion forKey:kPreviousSessionInfoOSVersion];

  // Set the current language.
  NSString* currentLanguage = [[NSLocale preferredLanguages] objectAtIndex:0];
  [defaults setObject:currentLanguage forKey:kLastRanLanguage];

  // Clear the memory warning flag.
  [defaults
      removeObjectForKey:previous_session_info_constants::
                             kDidSeeMemoryWarningShortlyBeforeTerminating];

  [UIDevice currentDevice].batteryMonitoringEnabled = YES;
  [self updateStoredBatteryLevel];
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(updateStoredBatteryLevel)
             name:UIDeviceBatteryLevelDidChangeNotification
           object:nil];

  [self updateStoredBatteryState];
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(updateStoredBatteryState)
             name:UIDeviceBatteryStateDidChangeNotification
           object:nil];

  [self updateStoredLowPowerMode];
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(updateStoredLowPowerMode)
             name:NSProcessInfoPowerStateDidChangeNotification
           object:nil];

  [self updateStoredThermalState];
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(updateStoredThermalState)
             name:NSProcessInfoThermalStateDidChangeNotification
           object:nil];

  // Save critical state information for crash detection.
  [defaults synchronize];
}

- (void)updateAvailableDeviceStorage:(NSInteger)availableStorage {
  if (!self.didBeginRecordingCurrentSession)
    return;

  [[NSUserDefaults standardUserDefaults]
      setInteger:availableStorage
          forKey:kPreviousSessionInfoAvailableDeviceStorage];
}

- (void)updateStoredBatteryLevel {
  [[NSUserDefaults standardUserDefaults]
      setFloat:[UIDevice currentDevice].batteryLevel
        forKey:kPreviousSessionInfoBatteryLevel];
}

- (void)updateStoredBatteryState {
  UIDevice* device = [UIDevice currentDevice];
  // Translate value to an app defined enum as the system could change the
  // underlying values of UIDeviceBatteryState between OS versions.
  DeviceBatteryState batteryState =
      GetBatteryStateFromUIDeviceBatteryState(device.batteryState);
  NSInteger batteryStateValue =
      static_cast<std::underlying_type<DeviceBatteryState>::type>(batteryState);

  [[NSUserDefaults standardUserDefaults]
      setInteger:batteryStateValue
          forKey:kPreviousSessionInfoBatteryState];
}

- (void)updateStoredLowPowerMode {
  BOOL isLowPoweredModeEnabled =
      [[NSProcessInfo processInfo] isLowPowerModeEnabled];
  [[NSUserDefaults standardUserDefaults]
      setInteger:isLowPoweredModeEnabled
          forKey:kPreviousSessionInfoLowPowerMode];
}

- (void)updateStoredThermalState {
  NSProcessInfo* processInfo = [NSProcessInfo processInfo];
  // Translate value to an app defined enum as the system could change the
  // underlying values of NSProcessInfoThermalState between OS versions.
  DeviceThermalState thermalState =
      GetThermalStateFromNSProcessInfoThermalState([processInfo thermalState]);
  NSInteger thermalStateValue =
      static_cast<std::underlying_type<DeviceThermalState>::type>(thermalState);

  [[NSUserDefaults standardUserDefaults]
      setInteger:thermalStateValue
          forKey:kPreviousSessionInfoThermalState];
}

- (void)setMemoryWarningFlag {
  if (!self.didBeginRecordingCurrentSession)
    return;

  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults setBool:YES
             forKey:previous_session_info_constants::
                        kDidSeeMemoryWarningShortlyBeforeTerminating];
  // Save critical state information for crash detection.
  [defaults synchronize];
}

- (void)resetMemoryWarningFlag {
  if (!self.didBeginRecordingCurrentSession)
    return;

  NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
  [defaults
      removeObjectForKey:previous_session_info_constants::
                             kDidSeeMemoryWarningShortlyBeforeTerminating];
  // Save critical state information for crash detection.
  [defaults synchronize];
}

@end
