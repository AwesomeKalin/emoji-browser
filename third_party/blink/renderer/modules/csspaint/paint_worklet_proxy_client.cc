// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/csspaint/paint_worklet_proxy_client.h"

#include <memory>

#include "base/single_thread_task_runner.h"
#include "third_party/blink/renderer/core/css/cssom/css_style_value.h"
#include "third_party/blink/renderer/core/css/cssom/paint_worklet_input.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/frame/web_frame_widget_base.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/core/workers/worker_thread.h"
#include "third_party/blink/renderer/modules/csspaint/css_paint_definition.h"
#include "third_party/blink/renderer/modules/csspaint/paint_worklet.h"
#include "third_party/blink/renderer/platform/graphics/image.h"
#include "third_party/blink/renderer/platform/graphics/paint_worklet_paint_dispatcher.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"

namespace blink {

const char PaintWorkletProxyClient::kSupplementName[] =
    "PaintWorkletProxyClient";

// static
PaintWorkletProxyClient* PaintWorkletProxyClient::From(WorkerClients* clients) {
  return Supplement<WorkerClients>::From<PaintWorkletProxyClient>(clients);
}

// static
PaintWorkletProxyClient* PaintWorkletProxyClient::Create(Document* document,
                                                         int worklet_id) {
  WebLocalFrameImpl* local_frame =
      WebLocalFrameImpl::FromFrame(document->GetFrame());
  PaintWorklet* paint_worklet = PaintWorklet::From(*document->domWindow());
  scoped_refptr<PaintWorkletPaintDispatcher> compositor_paint_dispatcher =
      local_frame->LocalRootFrameWidget()->EnsureCompositorPaintDispatcher();
  return MakeGarbageCollected<PaintWorkletProxyClient>(
      worklet_id, paint_worklet, std::move(compositor_paint_dispatcher));
}

PaintWorkletProxyClient::PaintWorkletProxyClient(
    int worklet_id,
    PaintWorklet* paint_worklet,
    scoped_refptr<PaintWorkletPaintDispatcher> paint_dispatcher)
    : paint_dispatcher_(std::move(paint_dispatcher)),
      worklet_id_(worklet_id),
      state_(RunState::kUninitialized),
      main_thread_runner_(Thread::MainThread()->GetTaskRunner()),
      paint_worklet_(paint_worklet) {
  DCHECK(IsMainThread());
}

void PaintWorkletProxyClient::AddGlobalScope(WorkletGlobalScope* global_scope) {
  DCHECK(global_scope);
  DCHECK(global_scope->IsContextThread());
  if (state_ == RunState::kDisposed)
    return;
  DCHECK(state_ == RunState::kUninitialized);

  global_scopes_.push_back(To<PaintWorkletGlobalScope>(global_scope));

  // Wait for all global scopes to be set before registering.
  if (global_scopes_.size() < PaintWorklet::kNumGlobalScopesPerThread) {
    return;
  }

  // All the global scopes that share a single PaintWorkletProxyClient run on
  // the same thread with the same scheduler. As such we can just grab a task
  // runner from the last one to register.
  scoped_refptr<base::SingleThreadTaskRunner> global_scope_runner =
      global_scope->GetThread()->GetTaskRunner(TaskType::kMiscPlatformAPI);
  state_ = RunState::kWorking;

  paint_dispatcher_->RegisterPaintWorkletPainter(this, global_scope_runner);
}

void PaintWorkletProxyClient::RegisterCSSPaintDefinition(
    const String& name,
    CSSPaintDefinition* definition,
    ExceptionState& exception_state) {
  if (document_definition_map_.Contains(name)) {
    DocumentPaintDefinition* document_definition =
        document_definition_map_.at(name);
    if (!document_definition)
      return;
    if (!document_definition->RegisterAdditionalPaintDefinition(*definition)) {
      document_definition_map_.Set(name, nullptr);
      exception_state.ThrowDOMException(
          DOMExceptionCode::kNotSupportedError,
          "A class with name:'" + name +
              "' was registered with a different definition.");
      return;
    }
  } else {
    auto document_definition = std::make_unique<DocumentPaintDefinition>(
        definition->NativeInvalidationProperties(),
        definition->CustomInvalidationProperties(),
        definition->InputArgumentTypes(),
        definition->GetPaintRenderingContext2DSettings()->alpha());
    document_definition_map_.insert(name, std::move(document_definition));
  }

  DocumentPaintDefinition* document_definition =
      document_definition_map_.at(name);
  // Notify the main thread only once all global scopes have registered the same
  // named paint definition (with the same definition as well).
  if (document_definition->GetRegisteredDefinitionCount() ==
      PaintWorklet::kNumGlobalScopesPerThread) {
    const Vector<AtomicString>& custom_properties =
        definition->CustomInvalidationProperties();
    // Make a deep copy of the |custom_properties| into a Vector<String> so that
    // CrossThreadCopier can pass that cross thread boundaries.
    Vector<String> passed_custom_properties;
    for (const auto& property : custom_properties)
      passed_custom_properties.push_back(property.GetString());

    PostCrossThreadTask(
        *main_thread_runner_, FROM_HERE,
        CrossThreadBindOnce(
            &PaintWorklet::RegisterMainThreadDocumentPaintDefinition,
            paint_worklet_, name, definition->NativeInvalidationProperties(),
            WTF::Passed(std::move(passed_custom_properties)),
            definition->InputArgumentTypes(),
            definition->GetPaintRenderingContext2DSettings()->alpha()));
  }
}

void PaintWorkletProxyClient::Dispose() {
  if (state_ == RunState::kWorking) {
    paint_dispatcher_->UnregisterPaintWorkletPainter(worklet_id_);
  }
  paint_dispatcher_ = nullptr;

  state_ = RunState::kDisposed;

  // At worklet scope termination break the reference cycle between
  // PaintWorkletGlobalScope and PaintWorkletProxyClient.
  global_scopes_.clear();
}

void PaintWorkletProxyClient::Trace(blink::Visitor* visitor) {
  Supplement<WorkerClients>::Trace(visitor);
  PaintWorkletPainter::Trace(visitor);
}

sk_sp<PaintRecord> PaintWorkletProxyClient::Paint(
    const CompositorPaintWorkletInput* compositor_input) {
  // TODO: Can this happen? We don't register till all are here.
  if (global_scopes_.IsEmpty())
    return sk_make_sp<PaintRecord>();

  // PaintWorklets are stateless by spec. There are two ways script might try to
  // inject state:
  //   * From one PaintWorklet to another, in the same frame.
  //   * Inside the same PaintWorklet, across frames.
  //
  // To discourage both of these, we randomize selection of the global scope.
  // TODO(smcgruer): Once we are passing bundles of PaintWorklets here, we
  // should shuffle the bundle randomly and then assign half to the first global
  // scope, and half to the rest.
  DCHECK_EQ(global_scopes_.size(), PaintWorklet::kNumGlobalScopesPerThread);
  PaintWorkletGlobalScope* global_scope = global_scopes_[base::RandInt(
      0, (PaintWorklet::kNumGlobalScopesPerThread)-1)];

  const PaintWorkletInput* input =
      static_cast<const PaintWorkletInput*>(compositor_input);
  CSSPaintDefinition* definition =
      global_scope->FindDefinition(input->NameCopy());
  PaintWorkletStylePropertyMap* style_map =
      MakeGarbageCollected<PaintWorkletStylePropertyMap>(input->StyleMapData());

  CSSStyleValueVector paint_arguments;
  for (const auto& style_value : input->ParsedInputArguments()) {
    paint_arguments.push_back(style_value->ToCSSStyleValue());
  }

  sk_sp<PaintRecord> result =
      definition->Paint(FloatSize(input->GetSize()), input->EffectiveZoom(),
                        style_map, &paint_arguments);

  // CSSPaintDefinition::Paint returns nullptr if it fails, but for
  // OffThread-PaintWorklet we prefer to insert empty PaintRecords into the
  // cache. Do the conversion here.
  // TODO(smcgruer): Once OffThread-PaintWorklet launches, we can make
  // CSSPaintDefinition::Paint return empty PaintRecords.
  if (!result)
    result = sk_make_sp<PaintRecord>();
  return result;
}

void ProvidePaintWorkletProxyClientTo(WorkerClients* clients,
                                      PaintWorkletProxyClient* client) {
  clients->ProvideSupplement(client);
}

}  // namespace blink
