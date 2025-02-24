// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/profiles/profile_key.h"

#include "base/logging.h"

ProfileKey::ProfileKey(const base::FilePath& path, ProfileKey* original_key)
    : SimpleFactoryKey(path, original_key != nullptr /* is_off_the_record */),
      prefs_(nullptr),
      original_key_(original_key) {}

ProfileKey::~ProfileKey() = default;

ProfileKey* ProfileKey::GetOriginalKey() {
  if (original_key_)
    return original_key_;
  return this;
}

PrefService* ProfileKey::GetPrefs() {
  DCHECK(prefs_);
  return prefs_;
}

void ProfileKey::SetPrefs(PrefService* prefs) {
  DCHECK(!prefs_);
  prefs_ = prefs;
}

// static
ProfileKey* ProfileKey::FromSimpleFactoryKey(SimpleFactoryKey* key) {
  return key ? static_cast<ProfileKey*>(key) : nullptr;
}
