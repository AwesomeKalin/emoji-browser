// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_REPORTING_UTIL_H_
#define CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_REPORTING_UTIL_H_

#include <string>

class PrefService;

namespace base {
class Clock;
class Time;
}  // namespace base

namespace component_updater {
class ComponentUpdateService;
}  // namespace component_updater

namespace crostini {

// If Crostini usage reporting is enabled, store the time window of
// the last app launch and the Termina version in prefs.
void WriteMetricsForReportingToPrefsIfEnabled(
    PrefService* profile_prefs,
    const component_updater::ComponentUpdateService* update_service,
    const base::Clock* clock);

// "Coarsens" the given time to the start of a three-day time window.
// Used for privacy reasons.
base::Time GetThreeDayWindowStart(const base::Time& actual_time);

// Retrieve the cros-termina version from the browser's component list.
std::string GetTerminaVersion(
    const component_updater::ComponentUpdateService* update_service);

}  // namespace crostini

#endif  // CHROME_BROWSER_CHROMEOS_CROSTINI_CROSTINI_REPORTING_UTIL_H_
