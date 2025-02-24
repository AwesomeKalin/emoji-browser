// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_CREDENTIAL_PROVIDER_GAIACP_OS_USER_MANAGER_H_
#define CHROME_CREDENTIAL_PROVIDER_GAIACP_OS_USER_MANAGER_H_

#include "base/strings/string16.h"
#include "base/win/scoped_handle.h"
#include "base/win/windows_types.h"

typedef wchar_t* BSTR;

namespace credential_provider {

// Manages OS users on the system.
class [[clang::lto_visibility_public]] OSUserManager {
 public:
  // Minimum length for password buffer when calling GenerateRandomPassword().
  static const int kMinPasswordLength = 24;

  static OSUserManager* Get();

  virtual ~OSUserManager();

  // Generates a cryptographically secure random password.
  virtual HRESULT GenerateRandomPassword(wchar_t* password, int length);

  // Creates a new OS user on the system with the given credentials.  If
  // |add_to_users_group| is true, the Os user is added to the machine's
  // "Users" group which allows interactive logon.  The OS user's SID is
  // returned in |sid|.
  virtual HRESULT AddUser(const wchar_t* username,
                          const wchar_t* password,
                          const wchar_t* fullname,
                          const wchar_t* comment,
                          bool add_to_users_group,
                          BSTR* sid,
                          DWORD* error);

  // Changes the password of the given OS user.
  virtual HRESULT ChangeUserPassword(
      const wchar_t* domain, const wchar_t* username,
      const wchar_t* old_password, const wchar_t* new_password);

  // Force changes the password of the given OS user. This will cause them to
  // lose all encrypted data.
  virtual HRESULT SetUserPassword(
      const wchar_t* domain, const wchar_t* username, const wchar_t* password);

  // Checks if the given user's password matches |password|. Returns S_OK if it
  // matches, S_FALSE if not. Otherwise will return the windows error code.
  virtual HRESULT IsWindowsPasswordValid(
      const wchar_t* domain, const wchar_t* username, const wchar_t* password);

  // Creates a logon token for the given user.  If |interactive| is true the
  // token is of type interactive otherwise it is of type batch.
  virtual HRESULT CreateLogonToken(
      const wchar_t* domain, const wchar_t* username, const wchar_t* password,
      bool interactive, base::win::ScopedHandle* token);

  // Gets the SID of the given OS user.  The caller owns the pointer |sid| and
  // should free it with a call to LocalFree().
  virtual HRESULT GetUserSID(const wchar_t* domain, const wchar_t* username,
                             PSID* sid);
  // Gets the SID in string format of the given OS user.
  HRESULT GetUserSID(const wchar_t* domain, const wchar_t* username,
                     base::string16* sid_string);

  // Finds a user created from a gaia account by its SID.  Returns S_OK if a
  // user with the given SID exists, HRESULT_FROM_WIN32(ERROR_NONE_MAPPED)
  // if not, or an arbitrary error otherwise.  If |username| is non-null and
  // |username_size| is greater than zero, the username associated with the
  // SID is returned. If |domain| is non-null and |domain_size| is greater
  // than zero, the domain associated with the SID is returned.
  virtual HRESULT FindUserBySID(const wchar_t* sid, wchar_t* username,
                                DWORD username_size, wchar_t* domain,
                                DWORD domain_size);

  // Verify if a user with provided sid is domain joined.
  virtual bool IsUserDomainJoined(const base::string16& sid);

  // Removes the user from the machine.
  virtual HRESULT RemoveUser(const wchar_t* username, const wchar_t* password);

  // Gets the full name of the user from their user info.
  virtual HRESULT GetUserFullname(
      const wchar_t* domain, const wchar_t* username, base::string16* fullname);

  // Changes the user's valid access hours to effectively allow or disallow them
  // from signing in to the system. If |allow| is false then the user is not
  // allowed to sign on at any hour of the day. If |allow| is true, then the
  // user is allowed to sign on at any time of day.
  virtual HRESULT ModifyUserAccessWithLogonHours(
      const wchar_t* domain, const wchar_t* username, bool allow);
  static base::string16 GetLocalDomain();

  // This method is called from dllmain.cc when setting fakes from one modul
  // to another.
  static void SetInstanceForTesting(OSUserManager* instance);

 protected:
  OSUserManager() {}

  // Returns the storage used for the instance pointer.
  static OSUserManager** GetInstanceStorage();
};

}  // namespace credential_provider

#endif  // CHROME_CREDENTIAL_PROVIDER_GAIACP_OS_USER_MANAGER_H_
