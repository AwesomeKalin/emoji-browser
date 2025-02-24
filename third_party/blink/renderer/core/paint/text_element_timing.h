// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_PAINT_TEXT_ELEMENT_TIMING_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_PAINT_TEXT_ELEMENT_TIMING_H_

#include "base/memory/weak_ptr.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/timing/window_performance.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/wtf/deque.h"

namespace blink {

class IntRect;
class LocalFrameView;
class PropertyTreeState;
class TextRecord;

// TextElementTiming is responsible for tracking the paint timings for groups of
// text nodes associated with elements of a given window.
class CORE_EXPORT TextElementTiming final
    : public GarbageCollectedFinalized<TextElementTiming>,
      public Supplement<LocalDOMWindow> {
  USING_GARBAGE_COLLECTED_MIXIN(TextElementTiming);

 public:
  static const char kSupplementName[];

  explicit TextElementTiming(LocalDOMWindow&);

  static TextElementTiming& From(LocalDOMWindow&);

  static inline bool NeededForElementTiming(Node* node) {
    return !node->IsInShadowTree() && node->IsElementNode() &&
           !ToElement(node)
                ->FastGetAttribute(html_names::kElementtimingAttr)
                .IsEmpty();
  }

  static FloatRect ComputeIntersectionRect(
      Node*,
      const IntRect& aggregated_visual_rect,
      const PropertyTreeState&,
      LocalFrameView*);

  // Called when the swap promise queued by TextPaintTimingDetector has been
  // resolved. Dispatches PerformanceElementTiming entries to WindowPerformance.
  void OnTextNodesPainted(const Deque<base::WeakPtr<TextRecord>>&);

  void Trace(blink::Visitor* visitor) override;

  Member<WindowPerformance> performance_;

  DISALLOW_COPY_AND_ASSIGN(TextElementTiming);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_PAINT_TEXT_ELEMENT_TIMING_H_
