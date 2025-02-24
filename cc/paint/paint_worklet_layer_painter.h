// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_PAINT_WORKLET_LAYER_PAINTER_H_
#define CC_PAINT_PAINT_WORKLET_LAYER_PAINTER_H_

#include "base/callback.h"
#include "cc/paint/paint_export.h"
#include "cc/paint/paint_record.h"
#include "cc/paint/paint_worklet_job.h"

namespace cc {

class PaintWorkletInput;

// PaintWorkletLayerPainter bridges between the compositor and the PaintWorklet
// thread, providing hooks for the compositor to paint PaintWorklet content that
// Blink has deferred on.
class CC_PAINT_EXPORT PaintWorkletLayerPainter {
 public:
  virtual ~PaintWorkletLayerPainter() {}

  // Synchronously paints a PaintWorklet instance (represented by a
  // PaintWorkletInput), returning the resultant PaintRecord.
  //
  // TODO(crbug.com/907897): Once we remove the raster thread path, we will only
  // be using |DispatchWorklets| and this can be removed.
  virtual sk_sp<PaintRecord> Paint(const PaintWorkletInput*) = 0;

  // Asynchronously paints a set of PaintWorklet instances. The results are
  // returned via the provided callback, on the same thread that originally
  // called this method.
  using DoneCallback = base::OnceCallback<void(PaintWorkletJobMap)>;
  virtual void DispatchWorklets(PaintWorkletJobMap, DoneCallback) = 0;
};

}  // namespace cc

#endif  // CC_PAINT_PAINT_WORKLET_LAYER_PAINTER_H_
