// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/paint/text_element_timing.h"

#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/paint/text_paint_timing_detector.h"
#include "third_party/blink/renderer/core/timing/dom_window_performance.h"
#include "third_party/blink/renderer/platform/geometry/int_rect.h"
#include "third_party/blink/renderer/platform/graphics/paint/float_clip_rect.h"
#include "third_party/blink/renderer/platform/graphics/paint/geometry_mapper.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

namespace blink {

// static
const char TextElementTiming::kSupplementName[] = "TextElementTiming";

// static
TextElementTiming& TextElementTiming::From(LocalDOMWindow& window) {
  TextElementTiming* timing =
      Supplement<LocalDOMWindow>::From<TextElementTiming>(window);
  if (!timing) {
    timing = MakeGarbageCollected<TextElementTiming>(window);
    ProvideTo(window, timing);
  }
  return *timing;
}

TextElementTiming::TextElementTiming(LocalDOMWindow& window)
    : Supplement<LocalDOMWindow>(window),
      performance_(DOMWindowPerformance::performance(window)) {
  DCHECK(RuntimeEnabledFeatures::ElementTimingEnabled(
      GetSupplementable()->document()));
}

// static
FloatRect TextElementTiming::ComputeIntersectionRect(
    Node* node,
    const IntRect& aggregated_visual_rect,
    const PropertyTreeState& property_tree_state,
    LocalFrameView* frame_view) {
  if (!NeededForElementTiming(node))
    return FloatRect();

  FloatClipRect float_clip_visual_rect =
      FloatClipRect(FloatRect(aggregated_visual_rect));
  GeometryMapper::LocalToAncestorVisualRect(
      property_tree_state,
      frame_view->GetLayoutView()->FirstFragment().LocalBorderBoxProperties(),
      float_clip_visual_rect);
  return float_clip_visual_rect.Rect();
}

void TextElementTiming::OnTextNodesPainted(
    const Deque<base::WeakPtr<TextRecord>>& text_nodes_painted) {
  DCHECK(performance_);
  // If the entries cannot be exposed via PerformanceObserver nor added to the
  // buffer, bail out.
  if (!performance_->HasObserverFor(PerformanceEntry::kElement) &&
      performance_->IsElementTimingBufferFull()) {
    return;
  }
  for (const auto record : text_nodes_painted) {
    Node* node = DOMNodeIds::NodeForId(record->node_id);
    if (!node || node->IsInShadowTree())
      continue;

    // Text aggregators should be Elements!
    DCHECK(node->IsElementNode());
    Element* element = ToElement(node);
    const AtomicString& attr =
        element->FastGetAttribute(html_names::kElementtimingAttr);
    if (attr.IsEmpty())
      continue;

    const AtomicString& id = element->GetIdAttribute();
    DEFINE_STATIC_LOCAL(const AtomicString, kTextPaint, ("text-paint"));
    performance_->AddElementTiming(
        kTextPaint, g_empty_string, record->element_timing_rect_,
        record->paint_time, base::TimeTicks(), attr, IntSize(), id, element);
  }
}

void TextElementTiming::Trace(blink::Visitor* visitor) {
  Supplement<LocalDOMWindow>::Trace(visitor);
  visitor->Trace(performance_);
}

}  // namespace blink
