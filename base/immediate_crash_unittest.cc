// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/immediate_crash.h"

#include <stdint.h>

#include <algorithm>

#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/native_library.h"
#include "base/optional.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace base {

// iOS is excluded, since it doesn't support loading shared libraries.
#if defined(OS_WIN) || (defined(OS_MACOSX) && !defined(OS_IOS)) ||      \
    defined(OS_LINUX) || defined(OS_ANDROID) || defined(OS_CHROMEOS) || \
    defined(OS_FUCHSIA)

// Checks that the IMMEDIATE_CRASH() macro produces specific instructions; see
// comments in immediate_crash.h for the requirements.
TEST(ImmediateCrashTest, ExpectedOpcodeSequence) {
  // TestFunction1() and TestFunction2() are defined in a shared library in an
  // attempt to guarantee that they are located next to each other.
  NativeLibraryLoadError load_error;
  FilePath helper_library_path;
#if !defined(OS_ANDROID) && !defined(OS_FUCHSIA)
  // On Android M, DIR_EXE == /system/bin when running base_unittests.
  // On Fuchsia, NativeLibrary understands the native convention that libraries
  // are not colocated with the binary.
  ASSERT_TRUE(PathService::Get(DIR_EXE, &helper_library_path));
#endif
  helper_library_path = helper_library_path.AppendASCII(
      GetNativeLibraryName("immediate_crash_test_helper"));
#if defined(OS_ANDROID) && defined(COMPONENT_BUILD)
  helper_library_path = helper_library_path.ReplaceExtension(".cr.so");
#endif
  // TODO(dcheng): Shouldn't GetNativeLibraryName just return a FilePath?
  NativeLibrary helper_library =
      LoadNativeLibrary(helper_library_path, &load_error);
  ASSERT_TRUE(helper_library)
      << "shared library load failed: " << load_error.ToString();

  // TestFunction1() and TestFunction2() each contain two IMMEDIATE_CRASH()
  // invocations. IMMEDIATE_CRASH() should be treated as a noreturn sequence and
  // optimized into the function epilogue. The general strategy is to find the
  // return opcode, then scan the following bytes for the opcodes for two
  // consecutive IMMEDIATE_CRASH() sequences.
  void* a =
      GetFunctionPointerFromNativeLibrary(helper_library, "TestFunction1");
  ASSERT_TRUE(a);
  void* b =
      GetFunctionPointerFromNativeLibrary(helper_library, "TestFunction2");
  ASSERT_TRUE(b);

#if defined(ARCH_CPU_X86_FAMILY)

  // X86 opcode reference:
  // https://software.intel.com/en-us/download/intel-64-and-ia-32-architectures-sdm-combined-volumes-1-2a-2b-2c-2d-3a-3b-3c-3d-and-4
  span<const uint8_t> function_body =
      a < b ? make_span(static_cast<const uint8_t*>(a),
                        static_cast<const uint8_t*>(b))
            : make_span(static_cast<const uint8_t*>(b),
                        static_cast<const uint8_t*>(a));
  SCOPED_TRACE(HexEncode(function_body.data(), function_body.size_bytes()));

  // Look for RETN opcode (0xC3). Note that 0xC3 is a substring of several
  // other opcodes (VMRESUME, MOVNTI), and can also be encoded as part of an
  // argument to another opcode. None of these other cases are expected to be
  // present, so a simple byte scan should be Good Enough™.
  auto it = std::find(function_body.begin(), function_body.end(), 0xC3);
  ASSERT_NE(function_body.end(), it) << "Failed to find return! ";

  // Look for two IMMEDIATE_CRASH() opcode sequences.
  base::Optional<uint8_t> nonce;
  for (int i = 0; i < 2; ++i) {
    // INT 3
    EXPECT_EQ(0xCC, *++it);
    // UD2
    EXPECT_EQ(0x0F, *++it);
    EXPECT_EQ(0x0B, *++it);
    // PUSH
    EXPECT_EQ(0x6A, *++it);
    // Immediate nonce argument to PUSH
    if (!nonce) {
      nonce = *++it;
    } else {
      EXPECT_NE(*nonce, *++it);
    }
#if (defined(OS_WIN) && defined(ARCH_CPU_64_BITS)) || defined(OS_MACOSX)
    // On Windows x64 and Mac, __builtin_unreachable() generates UD2. See
    // https://crbug.com/958373.
    EXPECT_EQ(0x0F, *++it);
    EXPECT_EQ(0x0B, *++it);
#endif  // defined(OS_WIN) || defined(OS_MACOSX)
  }

#elif defined(ARCH_CPU_ARMEL)

  // Routines loaded from a shared library will have the LSB in the pointer set
  // if encoded as T32 instructions. The rest of this test assumes T32.
  ASSERT_TRUE(reinterpret_cast<uintptr_t>(a) & 0x1)
      << "Expected T32 opcodes but found A32 opcodes instead.";
  ASSERT_TRUE(reinterpret_cast<uintptr_t>(b) & 0x1)
      << "Expected T32 opcodes but found A32 opcodes instead.";

  // Mask off the lowest bit.
  a = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(a) & ~uintptr_t{0x1});
  b = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(b) & ~uintptr_t{0x1});

  // T32 opcode reference: https://developer.arm.com/docs/ddi0487/latest
  span<const uint16_t> function_body =
      a < b ? make_span(static_cast<const uint16_t*>(a),
                        static_cast<const uint16_t*>(b))
            : make_span(static_cast<const uint16_t*>(b),
                        static_cast<const uint16_t*>(a));
  SCOPED_TRACE(HexEncode(function_body.data(), function_body.size_bytes()));

  // Look for the standard return opcode sequence (BX LR).
  auto it = std::find(function_body.begin(), function_body.end(), 0x4770);
  ASSERT_NE(function_body.end(), it) << "Failed to find return! ";

  // Look for two IMMEDIATE_CRASH() opcode sequences.
  base::Optional<uint8_t> nonce;
  for (int i = 0; i < 2; ++i) {
    // BKPT #0
    EXPECT_EQ(0xBE00, *++it);
    // UDF #<nonce>
    EXPECT_EQ(0xDE00, *++it & 0xFF00);
    if (!nonce) {
      nonce = *it & 0x00FF;
    } else {
      EXPECT_NE(*nonce, *it & 0x00FF);
    }
  }

#elif defined(ARCH_CPU_ARM64)

  // A64 opcode reference: https://developer.arm.com/docs/ddi0487/latest
  span<const uint32_t> function_body =
      a < b ? make_span(static_cast<const uint32_t*>(a),
                        static_cast<const uint32_t*>(b))
            : make_span(static_cast<const uint32_t*>(b),
                        static_cast<const uint32_t*>(a));
  SCOPED_TRACE(HexEncode(function_body.data(), function_body.size_bytes()));

  // Look for RET. There appears to be multiple valid encodings, so this is
  // hardcoded to whatever clang currently emits...
  auto it = std::find(function_body.begin(), function_body.end(), 0XD65F03C0);
  ASSERT_NE(function_body.end(), it) << "Failed to find return! ";

  // Look for two IMMEDIATE_CRASH() opcode sequences.
  base::Optional<uint16_t> nonce;
  for (int i = 0; i < 2; ++i) {
    // BRK #0
    EXPECT_EQ(0XD4200000, *++it);
    // HLT #<nonce>
    EXPECT_EQ(0xD4400000, *++it & 0xFFE00000);
    if (!nonce) {
      nonce = (*it >> 5) & 0xFFFF;
    } else {
      EXPECT_NE(*nonce, (*it >> 5) & 0xFFFF);
    }
#if defined(OS_WIN)
    // On Windows ARM64 __builtin_unreachable() produces an extra 'brk #1'
    // instruction with clang-cl. https://crbug.com/973794.
    EXPECT_EQ(0XD4200020, *++it);
#endif
  }

#endif  // defined(ARCH_CPU_X86_FAMILY)

  UnloadNativeLibrary(helper_library);
}

#endif

}  // namespace base
