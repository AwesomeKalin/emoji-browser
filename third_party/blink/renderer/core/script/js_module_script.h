// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_SCRIPT_JS_MODULE_SCRIPT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_SCRIPT_JS_MODULE_SCRIPT_H_

#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/script/modulator.h"
#include "third_party/blink/renderer/core/script/module_script.h"
#include "third_party/blink/renderer/platform/bindings/name_client.h"
#include "third_party/blink/renderer/platform/bindings/parkable_string.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/kurl_hash.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/text/text_position.h"

namespace blink {

// JSModuleScript is a model object for the "JavaScript module script" spec
// concept. https://html.spec.whatwg.org/C/#javascript-module-script
class CORE_EXPORT JSModuleScript final : public ModuleScript,
                                         public NameClient {
 public:
  // https://html.spec.whatwg.org/C/#creating-a-javascript-module-script
  static JSModuleScript* Create(
      const ParkableString& source_text,
      SingleCachedMetadataHandler*,
      ScriptSourceLocationType,
      Modulator*,
      const KURL& source_url,
      const KURL& base_url,
      const ScriptFetchOptions&,
      const TextPosition& start_position = TextPosition::MinimumPosition());

  // Mostly corresponds to Create() but accepts ModuleRecord as the argument
  // and allows null ModuleRecord.
  static JSModuleScript* CreateForTest(
      Modulator*,
      ModuleRecord,
      const KURL& base_url,
      const ScriptFetchOptions& = ScriptFetchOptions());

  // Do not call this constructor. Use Create() instead. This is public only for
  // MakeGarbageCollected.
  JSModuleScript(Modulator* settings_object,
                 ModuleRecord record,
                 const KURL& source_url,
                 const KURL& base_url,
                 const ScriptFetchOptions&,
                 const ParkableString& source_text,
                 const TextPosition& start_position,
                 ModuleRecordProduceCacheData*);
  ~JSModuleScript() override = default;

  void ProduceCache() override;

  void Trace(Visitor*) override;
  const char* NameInHeapSnapshot() const override { return "JSModuleScript"; }

 private:
  friend class ModuleScriptTest;

  static JSModuleScript* CreateInternal(
      const ParkableString& source_text,
      Modulator*,
      ModuleRecord,
      const KURL& source_url,
      const KURL& base_url,
      const ScriptFetchOptions&,
      const TextPosition&,
      ModuleRecordProduceCacheData* produce_cache_data);

  const TextPosition& StartPosition() const { return start_position_; }
  void RunScriptOnWorker(WorkerGlobalScope&) override;
  String InlineSourceTextForCSP() const override;

  // For CSP check.
  const ParkableString source_text_;

  const TextPosition start_position_;

  // Only for ProduceCache(). JSModuleScript keeps |produce_cache_data| because:
  // - CompileModule() and ProduceCache() should be called at different
  //   timings, and
  // - There are no persistent object that can hold this in
  //   bindings/core/v8 side. ModuleRecord should be short-lived and is
  //   constructed every time in JSModuleScript::Record().
  //
  // Cleared once ProduceCache() is called, to avoid
  // calling V8CodeCache::ProduceCache() multiple times, as a JSModuleScript
  // can appear multiple times in multiple module graphs.
  Member<ModuleRecordProduceCacheData> produce_cache_data_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_SCRIPT_JS_MODULE_SCRIPT_H_
