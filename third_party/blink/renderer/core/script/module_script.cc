// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/script/module_script.h"

#include "third_party/blink/renderer/bindings/core/v8/script_value.h"
#include "third_party/blink/renderer/core/script/module_record_resolver.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "v8/include/v8.h"

namespace blink {

ModuleScript::ModuleScript(Modulator* settings_object,
                           ModuleRecord record,
                           const KURL& source_url,
                           const KURL& base_url,
                           const ScriptFetchOptions& fetch_options)
    : Script(fetch_options, base_url),
      settings_object_(settings_object),
      source_url_(source_url) {
  if (record.IsNull()) {
    // We allow empty records for module infra tests which never touch records.
    // This should never happen outside unit tests.
    return;
  }

  DCHECK(settings_object);
  v8::Isolate* isolate = settings_object_->GetScriptState()->GetIsolate();
  v8::HandleScope scope(isolate);
  record_.Set(isolate, record.NewLocal(isolate));
}

ModuleRecord ModuleScript::Record() const {
  if (record_.IsEmpty())
    return ModuleRecord();

  v8::Isolate* isolate = settings_object_->GetScriptState()->GetIsolate();
  v8::HandleScope scope(isolate);
  return ModuleRecord(isolate, record_.NewLocal(isolate), SourceURL());
}

bool ModuleScript::HasEmptyRecord() const {
  return record_.IsEmpty();
}

void ModuleScript::SetParseErrorAndClearRecord(ScriptValue error) {
  DCHECK(!error.IsEmpty());

  record_.Clear();
  ScriptState::Scope scope(error.GetScriptState());
  parse_error_.Set(error.GetIsolate(), error.V8Value());
}

ScriptValue ModuleScript::CreateParseError() const {
  ScriptState* script_state = settings_object_->GetScriptState();
  v8::Isolate* isolate = script_state->GetIsolate();
  ScriptState::Scope scope(script_state);
  ScriptValue error(script_state, parse_error_.NewLocal(isolate));
  DCHECK(!error.IsEmpty());
  return error;
}

void ModuleScript::SetErrorToRethrow(ScriptValue error) {
  ScriptState::Scope scope(error.GetScriptState());
  error_to_rethrow_.Set(error.GetIsolate(), error.V8Value());
}

ScriptValue ModuleScript::CreateErrorToRethrow() const {
  ScriptState* script_state = settings_object_->GetScriptState();
  v8::Isolate* isolate = script_state->GetIsolate();
  ScriptState::Scope scope(script_state);
  ScriptValue error(script_state, error_to_rethrow_.NewLocal(isolate));
  DCHECK(!error.IsEmpty());
  return error;
}

KURL ModuleScript::ResolveModuleSpecifier(const String& module_request,
                                          String* failure_reason) const {
  auto found = specifier_to_url_cache_.find(module_request);
  if (found != specifier_to_url_cache_.end())
    return found->value;

  KURL url = SettingsObject()->ResolveModuleSpecifier(module_request, BaseURL(),
                                                      failure_reason);
  // Cache the result only on success, so that failure_reason is set for
  // subsequent calls too.
  if (url.IsValid())
    specifier_to_url_cache_.insert(module_request, url);
  return url;
}

void ModuleScript::Trace(Visitor* visitor) {
  visitor->Trace(settings_object_);
  visitor->Trace(record_.UnsafeCast<v8::Value>());
  visitor->Trace(parse_error_);
  visitor->Trace(error_to_rethrow_);
  Script::Trace(visitor);
}

void ModuleScript::RunScript(LocalFrame* frame, const SecurityOrigin*) {
  DVLOG(1) << *this << "::RunScript()";
  SettingsObject()->ExecuteModule(this,
                                  Modulator::CaptureEvalErrorFlag::kReport);
}

std::ostream& operator<<(std::ostream& stream,
                         const ModuleScript& module_script) {
  stream << "ModuleScript[" << &module_script;
  if (module_script.HasEmptyRecord())
    stream << ", empty-record";

  if (module_script.HasErrorToRethrow())
    stream << ", error-to-rethrow";

  if (module_script.HasParseError())
    stream << ", parse-error";

  return stream << "]";
}

}  // namespace blink
