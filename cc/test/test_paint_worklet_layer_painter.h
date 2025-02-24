// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_TEST_TEST_PAINT_WORKLET_LAYER_PAINTER_H_
#define CC_TEST_TEST_PAINT_WORKLET_LAYER_PAINTER_H_

#include "cc/paint/paint_worklet_layer_painter.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace cc {

class PaintWorkletInput;

class TestPaintWorkletLayerPainter : public PaintWorkletLayerPainter {
 public:
  TestPaintWorkletLayerPainter();
  ~TestPaintWorkletLayerPainter() override;

  sk_sp<PaintRecord> Paint(const PaintWorkletInput*) override;
  void DispatchWorklets(PaintWorkletJobMap, DoneCallback) override;
};

}  // namespace cc

#endif  // CC_TEST_TEST_PAINT_WORKLET_LAYER_PAINTER_H_
