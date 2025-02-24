// Copyright 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox/win/src/process_mitigations.h"

#include <windows.h>

#include "base/files/file_util.h"
#include "base/numerics/safe_conversions.h"
#include "base/path_service.h"
#include "base/scoped_native_library.h"
#include "base/test/test_timeouts.h"
#include "base/win/windows_version.h"
#include "sandbox/win/src/nt_internals.h"
#include "sandbox/win/src/target_services.h"
#include "sandbox/win/tests/common/controller.h"
#include "sandbox/win/tests/integration_tests/hooking_dll.h"
#include "sandbox/win/tests/integration_tests/integration_tests_common.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

//------------------------------------------------------------------------------
// Internal Defines & Functions
//------------------------------------------------------------------------------

// API defined in winbase.h.
using GetProcessDEPPolicyFunction = decltype(&GetProcessDEPPolicy);

// API defined in processthreadsapi.h.
using GetProcessMitigationPolicyFunction =
    decltype(&GetProcessMitigationPolicy);
GetProcessMitigationPolicyFunction get_process_mitigation_policy;

// APIs defined in wingdi.h.
using AddFontMemResourceExFunction = decltype(&AddFontMemResourceEx);
using RemoveFontMemResourceExFunction = decltype(&RemoveFontMemResourceEx);

//------------------------------------------------------------------------------
// NonSystemFont test helper function.
//
// 1. Pick font file and set up sandbox to allow read access to it.
// 2. Trigger test child process (with or without mitigation enabled).
//------------------------------------------------------------------------------
void TestWin10NonSystemFont(bool is_success_test) {
  base::FilePath font_path;
  EXPECT_TRUE(base::PathService::Get(base::DIR_WINDOWS_FONTS, &font_path));
  // Arial font should always be available
  font_path = font_path.Append(L"arial.ttf");

  sandbox::TestRunner runner;
  EXPECT_TRUE(runner.AddFsRule(sandbox::TargetPolicy::FILES_ALLOW_READONLY,
                               font_path.value().c_str()));

  if (!is_success_test) {
    sandbox::TargetPolicy* policy = runner.GetPolicy();
    // Turn on the non-system font disable mitigation.
    EXPECT_EQ(policy->SetProcessMitigations(
                  sandbox::MITIGATION_NONSYSTEM_FONT_DISABLE),
              sandbox::SBOX_ALL_OK);
  }

  base::string16 test_command = L"CheckWin10FontLoad \"";
  test_command += font_path.value().c_str();
  test_command += L"\"";

  EXPECT_EQ((is_success_test ? sandbox::SBOX_TEST_SUCCEEDED
                             : sandbox::SBOX_TEST_FAILED),
            runner.RunTest(test_command.c_str()));
}

//------------------------------------------------------------------------------
// ForceMsSigned test helper function.
// - LoadLibrary fails with ERROR_INVALID_IMAGE_HASH if this mitigation is
//   enabled and the target is not appropriately signed.
// - Acquire the global g_hooking_dll_mutex mutex before calling
//   (as we meddle with a shared system resource).
// - Note: Do not use ASSERTs in this function, as a global mutex is held.
//
// Trigger test child process (with or without mitigation enabled).
//------------------------------------------------------------------------------
void TestWin10MsSigned(bool expect_success,
                       bool enable_mitigation,
                       bool use_ms_signed_binary) {
  sandbox::TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  if (enable_mitigation) {
    // Enable the ForceMsSigned mitigation.
    EXPECT_EQ(policy->SetDelayedProcessMitigations(
                  sandbox::MITIGATION_FORCE_MS_SIGNED_BINS),
              sandbox::SBOX_ALL_OK);
  }

  // Choose the appropriate DLL and make sure the sandbox allows access to it.
  base::FilePath dll_path;
  if (use_ms_signed_binary) {
    EXPECT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &dll_path));
    dll_path = dll_path.Append(L"gdi32.dll");
  } else {
    EXPECT_TRUE(base::PathService::Get(base::DIR_EXE, &dll_path));
    dll_path = dll_path.Append(hooking_dll::g_hook_dll_file);
  }
  EXPECT_TRUE(runner.AddFsRule(sandbox::TargetPolicy::FILES_ALLOW_READONLY,
                               dll_path.value().c_str()));

  // Set up test string.
  base::string16 test = L"TestDllLoad \"";
  test += dll_path.value().c_str();
  test += L"\"";

  // Note: ERROR_INVALID_IMAGE_HASH is being displayed in a system pop-up when
  //       the DLL load is attempted, but the value returned from the test
  //       process itself is SBOX_TEST_FAILED.
  EXPECT_EQ((expect_success ? sandbox::SBOX_TEST_SUCCEEDED
                            : sandbox::SBOX_TEST_FAILED),
            runner.RunTest(test.c_str()));
}

}  // namespace

namespace sandbox {

//------------------------------------------------------------------------------
// Exported functions called by child test processes.
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Common test function for checking that a policy was enabled.
// - Use enum TestPolicy defined in integration_tests_common.h to specify which
//   policy to check - passed as arg1.
//------------------------------------------------------------------------------
SBOX_TESTS_COMMAND int CheckPolicy(int argc, wchar_t** argv) {
  if (argc < 1)
    return SBOX_TEST_INVALID_PARAMETER;
  int test = ::_wtoi(argv[0]);
  if (!test)
    return SBOX_TEST_INVALID_PARAMETER;

  get_process_mitigation_policy =
      reinterpret_cast<GetProcessMitigationPolicyFunction>(::GetProcAddress(
          ::GetModuleHandleW(L"kernel32.dll"), "GetProcessMitigationPolicy"));
  if (!get_process_mitigation_policy)
    return SBOX_TEST_NOT_FOUND;

  switch (test) {
    //--------------------------------------------------
    // MITIGATION_DEP
    // MITIGATION_DEP_NO_ATL_THUNK
    //--------------------------------------------------
    case (TESTPOLICY_DEP): {
#if !defined(_WIN64)
      // DEP - always enabled on 64-bit.
      PROCESS_MITIGATION_DEP_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessDEPPolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.Enable || !policy.Permanent)
        return SBOX_TEST_FAILED;
#endif  // !defined(_WIN64)
      break;
    }
    //--------------------------------------------------
    // MITIGATION_RELOCATE_IMAGE
    // MITIGATION_RELOCATE_IMAGE_REQUIRED
    //--------------------------------------------------
    case (TESTPOLICY_ASLR): {
      PROCESS_MITIGATION_ASLR_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessASLRPolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.EnableForceRelocateImages || !policy.DisallowStrippedImages)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_STRICT_HANDLE_CHECKS
    //--------------------------------------------------
    case (TESTPOLICY_STRICTHANDLE): {
      PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessStrictHandleCheckPolicy,
                                         &policy, sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.RaiseExceptionOnInvalidHandleReference ||
          !policy.HandleExceptionsPermanentlyEnabled) {
        return SBOX_TEST_FAILED;
      }

      break;
    }
    //--------------------------------------------------
    // MITIGATION_WIN32K_DISABLE
    //--------------------------------------------------
    case (TESTPOLICY_WIN32K): {
      PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessSystemCallDisablePolicy,
                                         &policy, sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.DisallowWin32kSystemCalls)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_EXTENSION_POINT_DISABLE
    //--------------------------------------------------
    case (TESTPOLICY_EXTENSIONPOINT): {
      PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessExtensionPointDisablePolicy,
                                         &policy, sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.DisableExtensionPoints)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_DYNAMIC_CODE_DISABLE
    //--------------------------------------------------
    case (TESTPOLICY_DYNAMICCODE): {
      PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessDynamicCodePolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.ProhibitDynamicCode)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_NONSYSTEM_FONT_DISABLE
    //--------------------------------------------------
    case (TESTPOLICY_NONSYSFONT): {
      PROCESS_MITIGATION_FONT_DISABLE_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessFontDisablePolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.DisableNonSystemFonts)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_FORCE_MS_SIGNED_BINS
    //--------------------------------------------------
    case (TESTPOLICY_MSSIGNED): {
      PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessSignaturePolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.MicrosoftSignedOnly)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_IMAGE_LOAD_NO_REMOTE
    //--------------------------------------------------
    case (TESTPOLICY_LOADNOREMOTE): {
      PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessImageLoadPolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.NoRemoteImages)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_IMAGE_LOAD_NO_LOW_LABEL
    //--------------------------------------------------
    case (TESTPOLICY_LOADNOLOW): {
      PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessImageLoadPolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.NoLowMandatoryLabelImages)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_DYNAMIC_CODE_DISABLE_WITH_OPT_OUT
    //--------------------------------------------------
    case (TESTPOLICY_DYNAMICCODEOPTOUT): {
      PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessDynamicCodePolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.ProhibitDynamicCode || !policy.AllowThreadOptOut)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_IMAGE_LOAD_PREFER_SYS32
    //--------------------------------------------------
    case (TESTPOLICY_LOADPREFERSYS32): {
      PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy = {};
      if (!get_process_mitigation_policy(::GetCurrentProcess(),
                                         ProcessImageLoadPolicy, &policy,
                                         sizeof(policy))) {
        return SBOX_TEST_NOT_FOUND;
      }
      if (!policy.PreferSystem32Images)
        return SBOX_TEST_FAILED;

      break;
    }
    //--------------------------------------------------
    // MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION
    //--------------------------------------------------
    case (TESTPOLICY_RESTRICTINDIRECTBRANCHPREDICTION): {
      // TODO(pennymac): No Policy defines available yet!
      // Can't use GetProcessMitigationPolicy() API to check if enabled at this
      // time.  If the creation of THIS process succeeded, then the call to
      // UpdateProcThreadAttribute() with this mitigation succeeded.
      break;
    }
    default:
      return SBOX_TEST_INVALID_PARAMETER;
  }

  return SBOX_TEST_SUCCEEDED;
}

SBOX_TESTS_COMMAND int CheckDep(int argc, wchar_t** argv) {
  GetProcessDEPPolicyFunction get_process_dep_policy =
      reinterpret_cast<GetProcessDEPPolicyFunction>(::GetProcAddress(
          ::GetModuleHandleW(L"kernel32.dll"), "GetProcessDEPPolicy"));
  if (get_process_dep_policy) {
    BOOL is_permanent = false;
    DWORD dep_flags = 0;

    if (!get_process_dep_policy(::GetCurrentProcess(), &dep_flags,
                                &is_permanent)) {
      return SBOX_TEST_FIRST_ERROR;
    }

    if (!(dep_flags & PROCESS_DEP_ENABLE) || !is_permanent)
      return SBOX_TEST_SECOND_ERROR;

  } else {
    NtQueryInformationProcessFunction query_information_process = nullptr;
    ResolveNTFunctionPtr("NtQueryInformationProcess",
                         &query_information_process);
    if (!query_information_process)
      return SBOX_TEST_NOT_FOUND;

    ULONG size = 0;
    ULONG dep_flags = 0;
    if (!SUCCEEDED(query_information_process(::GetCurrentProcess(),
                                             ProcessExecuteFlags, &dep_flags,
                                             sizeof(dep_flags), &size))) {
      return SBOX_TEST_THIRD_ERROR;
    }

    static const int MEM_EXECUTE_OPTION_DISABLE = 2;
    static const int MEM_EXECUTE_OPTION_PERMANENT = 8;
    dep_flags &= 0xff;

    if (dep_flags !=
        (MEM_EXECUTE_OPTION_DISABLE | MEM_EXECUTE_OPTION_PERMANENT)) {
      return SBOX_TEST_FOURTH_ERROR;
    }
  }

  return SBOX_TEST_SUCCEEDED;
}

// ForceMsSigned tests:
// Try to load the DLL given in arg1.
SBOX_TESTS_COMMAND int TestDllLoad(int argc, wchar_t** argv) {
  if (argc < 1 || !argv[0])
    return SBOX_TEST_INVALID_PARAMETER;

  base::string16 dll = argv[0];
  base::ScopedNativeLibrary test_dll((base::FilePath(dll)));
  if (test_dll.is_valid())
    return SBOX_TEST_SUCCEEDED;

  // Note: GetLastError() does not get an accurate failure code
  //       at this point.
  return SBOX_TEST_FAILED;
}

// This test attempts a non-system font load.
//
// 1) Load gdi32.dll for required font APIs.
// 2) Load file contents of font file passed in arg1 into memory.
// 3) Call API to try loading a non-system font.
//
// Arg1: Full path to font file to try loading.
SBOX_TESTS_COMMAND int CheckWin10FontLoad(int argc, wchar_t** argv) {
  if (argc < 1)
    return SBOX_TEST_INVALID_PARAMETER;

  HMODULE gdi_module = ::LoadLibraryW(L"gdi32.dll");
  if (!gdi_module)
    return SBOX_TEST_NOT_FOUND;

  AddFontMemResourceExFunction add_font_mem_resource =
      reinterpret_cast<AddFontMemResourceExFunction>(
          ::GetProcAddress(gdi_module, "AddFontMemResourceEx"));

  RemoveFontMemResourceExFunction rem_font_mem_resource =
      reinterpret_cast<RemoveFontMemResourceExFunction>(
          ::GetProcAddress(gdi_module, "RemoveFontMemResourceEx"));

  if (!add_font_mem_resource || !rem_font_mem_resource)
    return SBOX_TEST_NOT_FOUND;

  // Open font file passed in as an argument.
  base::File file(base::FilePath(argv[0]),
                  base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid())
    // Failed to open the font file passed in.
    return SBOX_TEST_NOT_FOUND;

  std::vector<char> font_data;
  int64_t len = file.GetLength();
  if (len < 0)
    return SBOX_TEST_NOT_FOUND;
  font_data.resize(len);

  int read = file.Read(0, &font_data[0], base::checked_cast<int>(len));
  file.Close();

  if (read != len)
    return SBOX_TEST_NOT_FOUND;

  DWORD font_count = 0;
  HANDLE font_handle =
      add_font_mem_resource(&font_data[0], static_cast<DWORD>(font_data.size()),
                            nullptr, &font_count);

  if (font_handle) {
    rem_font_mem_resource(font_handle);
    return SBOX_TEST_SUCCEEDED;
  }

  return SBOX_TEST_FAILED;
}

// Common helper test for CreateProcess.
// Arg1: The process commandline to create.
// Arg2: Will process end on its own:
//  - "true" for a process expected to finish on its own (wait for it).
//  - "false" for a process that will require terminating.
// Arg3: [OPTIONAL] Expected or forced exit code:
//  - If arg2 is "true", this is the expected exit code (default 0).
//  - If arg2 is "false", terminate the process with this exit code (default 0).
//
// ***Make sure you've enabled basic process creation in the
// test sandbox settings via:
// sandbox::TargetPolicy::SetJobLevel(),
// sandbox::TargetPolicy::SetTokenLevel(),
// and TestRunner::SetDisableCsrss().
SBOX_TESTS_COMMAND int TestChildProcess(int argc, wchar_t** argv) {
  if (argc < 2 || argc > 3)
    return SBOX_TEST_INVALID_PARAMETER;

  bool process_finishes = true;
  base::string16 arg2 = argv[1];
  if (arg2.compare(L"false") == 0)
    process_finishes = false;

  int desired_exit_code = 0;
  if (argc == 3) {
    desired_exit_code = wcstoul(argv[2], nullptr, 0);
  }

  base::string16 cmd = argv[0];
  base::LaunchOptions options = base::LaunchOptionsForTest();
  base::Process setup_proc = base::LaunchProcess(cmd.c_str(), options);

  if (setup_proc.IsValid()) {
    if (process_finishes) {
      // Wait for process exit and compare exit code.
      int exit_code = 1;
      if (!setup_proc.WaitForExitWithTimeout(TestTimeouts::action_max_timeout(),
                                             &exit_code)) {
        // Unexpected failure.
        setup_proc.Terminate(0, false);
        return SBOX_TEST_TIMED_OUT;
      }
      if (exit_code != desired_exit_code)
        return SBOX_TEST_FAILED;
      return SBOX_TEST_SUCCEEDED;
    } else {
      // Terminate process with requested exit code.
      setup_proc.Terminate(desired_exit_code, false);
      return SBOX_TEST_SUCCEEDED;
    }
  }
  // Process failed to be created.
  // Note: GetLastError from CreateProcess returns 5, "ERROR_ACCESS_DENIED".
  return SBOX_TEST_FAILED;
}

//------------------------------------------------------------------------------
// Exported Mitigation Tests
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// DEP (MITIGATION_DEP and MITIGATION_DEP_NO_ATL_THUNK)
// Win7 x86
//------------------------------------------------------------------------------

#if !defined(_WIN64)
// DEP is always enabled on 64-bit.  Only test on x86.
TEST(ProcessMitigationsTest, CheckDepWin7) {
  if (base::win::GetVersion() > base::win::Version::WIN7)
    return;

  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  EXPECT_EQ(policy->SetProcessMitigations(MITIGATION_DEP |
                                          MITIGATION_DEP_NO_ATL_THUNK |
                                          MITIGATION_SEHOP),
            SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(L"CheckDep"));
}
#endif  // !defined(_WIN64)

//------------------------------------------------------------------------------
// DEP (MITIGATION_DEP and MITIGATION_DEP_NO_ATL_THUNK)
// >= Win8 x86
//------------------------------------------------------------------------------

#if !defined(_WIN64)
// DEP is always enabled on 64-bit.  Only test on x86.

// This test validates that setting the MITIGATION_DEP*
// mitigations enables the setting on a process.
TEST(ProcessMitigationsTest, CheckDepWin8PolicySuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN8)
    return;

  base::string16 test_command = L"CheckPolicy ";
  test_command += std::to_wstring(TESTPOLICY_DEP);

  //---------------------------------
  // 1) Test setting pre-startup.
  //---------------------------------
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  EXPECT_EQ(policy->SetProcessMitigations(MITIGATION_DEP |
                                          MITIGATION_DEP_NO_ATL_THUNK),
            SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(test_command.c_str()));

  //---------------------------------
  // 2) Test setting post-startup.
  //---------------------------------
  TestRunner runner2;
  sandbox::TargetPolicy* policy2 = runner2.GetPolicy();

  EXPECT_EQ(policy2->SetDelayedProcessMitigations(MITIGATION_DEP |
                                                  MITIGATION_DEP_NO_ATL_THUNK),
            SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner2.RunTest(test_command.c_str()));
}

#endif  // !defined(_WIN64)

//------------------------------------------------------------------------------
// ASLR
// - MITIGATION_RELOCATE_IMAGE
// - MITIGATION_RELOCATE_IMAGE_REQUIRED
// - MITIGATION_BOTTOM_UP_ASLR
// - MITIGATION_HIGH_ENTROPY_ASLR
// >= Win8
//------------------------------------------------------------------------------

TEST(ProcessMitigationsTest, CheckWin8AslrPolicySuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN8)
    return;

  base::string16 test_command = L"CheckPolicy ";
  test_command += std::to_wstring(TESTPOLICY_ASLR);

//---------------------------------------------
// Only test in release for now.
// TODO(pennymac): overhaul ASLR, crbug/834907.
//---------------------------------------------
#if defined(NDEBUG)
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  EXPECT_EQ(policy->SetProcessMitigations(
                MITIGATION_RELOCATE_IMAGE | MITIGATION_RELOCATE_IMAGE_REQUIRED |
                MITIGATION_BOTTOM_UP_ASLR | MITIGATION_HIGH_ENTROPY_ASLR),
            SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(test_command.c_str()));
#endif  // defined(NDEBUG)
}

//------------------------------------------------------------------------------
//  Strict handle checks (MITIGATION_STRICT_HANDLE_CHECKS)
// >= Win8
//------------------------------------------------------------------------------

TEST(ProcessMitigationsTest, CheckWin8StrictHandlePolicySuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN8)
    return;

  base::string16 test_command = L"CheckPolicy ";
  test_command += std::to_wstring(TESTPOLICY_STRICTHANDLE);

  //---------------------------------
  // 1) Test setting post-startup.
  // ** Can only be set post-startup.
  //---------------------------------
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  EXPECT_EQ(
      policy->SetDelayedProcessMitigations(MITIGATION_STRICT_HANDLE_CHECKS),
      SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(test_command.c_str()));
}

//------------------------------------------------------------------------------
// Disable non-system font loads (MITIGATION_NONSYSTEM_FONT_DISABLE)
// >= Win10
//------------------------------------------------------------------------------

// This test validates that setting the MITIGATION_NON_SYSTEM_FONTS_DISABLE
// mitigation enables the setting on a process.
TEST(ProcessMitigationsTest, CheckWin10NonSystemFontLockDownPolicySuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN10)
    return;

  base::string16 test_command = L"CheckPolicy ";
  test_command += std::to_wstring(TESTPOLICY_NONSYSFONT);

  //---------------------------------
  // 1) Test setting pre-startup.
  //---------------------------------
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  EXPECT_EQ(policy->SetProcessMitigations(MITIGATION_NONSYSTEM_FONT_DISABLE),
            SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(test_command.c_str()));

  //---------------------------------
  // 2) Test setting post-startup.
  //---------------------------------
  TestRunner runner2;
  sandbox::TargetPolicy* policy2 = runner2.GetPolicy();

  EXPECT_EQ(
      policy2->SetDelayedProcessMitigations(MITIGATION_NONSYSTEM_FONT_DISABLE),
      SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner2.RunTest(test_command.c_str()));
}

// This test validates that we can load a non-system font if the
// MITIGATION_NON_SYSTEM_FONTS_DISABLE mitigation is NOT set.
TEST(ProcessMitigationsTest, CheckWin10NonSystemFontLockDownLoadSuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN10)
    return;

  TestWin10NonSystemFont(true /* is_success_test */);
}

// This test validates that setting the MITIGATION_NON_SYSTEM_FONTS_DISABLE
// mitigation prevents the loading of a non-system font.
TEST(ProcessMitigationsTest, CheckWin10NonSystemFontLockDownLoadFailure) {
  if (base::win::GetVersion() < base::win::Version::WIN10)
    return;

  TestWin10NonSystemFont(false /* is_success_test */);
}

//------------------------------------------------------------------------------
// Force MS Signed Binaries (MITIGATION_FORCE_MS_SIGNED_BINS)
// >= Win10 TH2
//
// (Note: the signing options for "MS store-signed" and "MS, store, or WHQL"
//  are not supported or tested by the sandbox at the moment.)
//------------------------------------------------------------------------------

// This test validates that setting the MITIGATION_FORCE_MS_SIGNED_BINS
// mitigation enables the setting on a process.
TEST(ProcessMitigationsTest, CheckWin10MsSignedPolicySuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN10_TH2)
    return;

  base::string16 test_command = L"CheckPolicy ";
  test_command += std::to_wstring(TESTPOLICY_MSSIGNED);

//---------------------------------
// 1) Test setting post-startup.
// **Only test if NOT component build, otherwise component DLLs are not signed
//   by MS and prevent process setup.
// **Only test post-startup, otherwise this test executable has dependencies
//   on DLLs that are not signed by MS and they prevent process startup.
//---------------------------------
#if !defined(COMPONENT_BUILD)
  TestRunner runner2;
  sandbox::TargetPolicy* policy2 = runner2.GetPolicy();

  EXPECT_EQ(
      policy2->SetDelayedProcessMitigations(MITIGATION_FORCE_MS_SIGNED_BINS),
      SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner2.RunTest(test_command.c_str()));
#endif  // !defined(COMPONENT_BUILD)
}

// This test validates that we can load an unsigned DLL if the
// MITIGATION_FORCE_MS_SIGNED_BINS mitigation is NOT set.
TEST(ProcessMitigationsTest, CheckWin10MsSigned_Success) {
  if (base::win::GetVersion() < base::win::Version::WIN10_TH2)
    return;

  ScopedTestMutex mutex(hooking_dll::g_hooking_dll_mutex);

  TestWin10MsSigned(true /* expect_success */,
                    false /* enable_mitigation */,
                    false /* use_ms_signed_binary */);
}

// This test validates that setting the MITIGATION_FORCE_MS_SIGNED_BINS
// mitigation prevents the loading of an unsigned DLL.
TEST(ProcessMitigationsTest, CheckWin10MsSigned_Failure) {
  if (base::win::GetVersion() < base::win::Version::WIN10_TH2)
    return;

  ScopedTestMutex mutex(hooking_dll::g_hooking_dll_mutex);

  TestWin10MsSigned(false /* expect_success */,
                    true /* enable_mitigation */,
                    false /* use_ms_signed_binary */);
}

// This test validates that we can load a signed Microsoft DLL if the
// MITIGATION_FORCE_MS_SIGNED_BINS mitigation is NOT set.  Very basic
// sanity test.
TEST(ProcessMitigationsTest, CheckWin10MsSigned_MsBaseline) {
  if (base::win::GetVersion() < base::win::Version::WIN10_TH2)
    return;

  ScopedTestMutex mutex(hooking_dll::g_hooking_dll_mutex);

  TestWin10MsSigned(true /* expect_success */,
                    false /* enable_mitigation */,
                    true /* use_ms_signed_binary */);
}

// This test validates that setting the MITIGATION_FORCE_MS_SIGNED_BINS
// mitigation still allows the load of an MS-signed DLL.
TEST(ProcessMitigationsTest, CheckWin10MsSigned_MsSuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN10_TH2)
    return;

  ScopedTestMutex mutex(hooking_dll::g_hooking_dll_mutex);

  TestWin10MsSigned(true /* expect_success */,
                    true /* enable_mitigation */,
                    true /* use_ms_signed_binary */);
}

//------------------------------------------------------------------------------
// Disable child process creation.
// - JobLevel <= JOB_LIMITED_USER (on < WIN10_TH2).
// - JobLevel <= JOB_LIMITED_USER which also triggers setting
//   PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY to
//   PROCESS_CREATION_CHILD_PROCESS_RESTRICTED in
//   BrokerServicesBase::SpawnTarget (on >= WIN10_TH2).
//------------------------------------------------------------------------------

// This test validates that we can spawn a child process if
// MITIGATION_CHILD_PROCESS_CREATION_RESTRICTED mitigation is
// not set.
TEST(ProcessMitigationsTest, CheckChildProcessSuccess) {
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  // Set a policy that would normally allow for process creation.
  policy->SetJobLevel(JOB_INTERACTIVE, 0);
  policy->SetTokenLevel(USER_UNPROTECTED, USER_UNPROTECTED);
  runner.SetDisableCsrss(false);

  base::FilePath cmd;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &cmd));
  cmd = cmd.Append(L"calc.exe");

  base::string16 test_command = L"TestChildProcess \"";
  test_command += cmd.value().c_str();
  test_command += L"\" false";

  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(test_command.c_str()));
}

// This test validates that setting the
// MITIGATION_CHILD_PROCESS_CREATION_RESTRICTED mitigation prevents
// the spawning of child processes.
TEST(ProcessMitigationsTest, CheckChildProcessFailure) {
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  // Now set the job level to be <= JOB_LIMITED_USER
  // and ensure we can no longer create a child process.
  policy->SetJobLevel(JOB_LIMITED_USER, 0);
  policy->SetTokenLevel(USER_UNPROTECTED, USER_UNPROTECTED);
  runner.SetDisableCsrss(false);

  base::FilePath cmd;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &cmd));
  cmd = cmd.Append(L"calc.exe");

  base::string16 test_command = L"TestChildProcess \"";
  test_command += cmd.value().c_str();
  test_command += L"\" false";

  EXPECT_EQ(SBOX_TEST_FAILED, runner.RunTest(test_command.c_str()));
}

// This test validates that when the sandboxed target within a job spawns a
// child process and the target process exits abnormally, the broker correctly
// handles the JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS message.
// Because this involves spawning a child process from the target process and is
// very similar to the above CheckChildProcess* tests, this test is here rather
// than elsewhere closer to the other Job tests.
TEST(ProcessMitigationsTest, CheckChildProcessAbnormalExit) {
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  // Set a policy that would normally allow for process creation.
  policy->SetJobLevel(JOB_INTERACTIVE, 0);
  policy->SetTokenLevel(USER_UNPROTECTED, USER_UNPROTECTED);
  runner.SetDisableCsrss(false);

  base::FilePath cmd;
  EXPECT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &cmd));
  cmd = cmd.Append(L"calc.exe");

  base::string16 test_command = L"TestChildProcess \"";
  test_command += cmd.value().c_str();
  test_command += L"\" false ";
  test_command += std::to_wstring(STATUS_ACCESS_VIOLATION);

  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(test_command.c_str()));
}

//------------------------------------------------------------------------------
// Restrict indirect branch prediction
// (MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION)
// >= Win10 RS3
//------------------------------------------------------------------------------

// This test validates that setting the
// MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION mitigation enables the setting
// on a process.
TEST(ProcessMitigationsTest,
     CheckWin10RestrictIndirectBranchPredictionPolicySuccess) {
  if (base::win::GetVersion() < base::win::Version::WIN10_RS3)
    return;

  base::string16 test_command = L"CheckPolicy ";
  test_command += std::to_wstring(TESTPOLICY_RESTRICTINDIRECTBRANCHPREDICTION);

  //---------------------------------
  // 1) Test setting pre-startup.
  //---------------------------------
  TestRunner runner;
  sandbox::TargetPolicy* policy = runner.GetPolicy();

  EXPECT_EQ(policy->SetProcessMitigations(
                MITIGATION_RESTRICT_INDIRECT_BRANCH_PREDICTION),
            SBOX_ALL_OK);
  EXPECT_EQ(SBOX_TEST_SUCCEEDED, runner.RunTest(test_command.c_str()));

  //---------------------------------
  // 2) Test setting post-startup.
  //    ** Post-startup not supported.  Must be enabled on creation.
  //---------------------------------
}

}  // namespace sandbox
