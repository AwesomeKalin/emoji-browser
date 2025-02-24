/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_DECODING_IMAGE_GENERATOR_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_DECODING_IMAGE_GENERATOR_H_

#include "base/macros.h"
#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/platform/graphics/paint/paint_image.h"
#include "third_party/blink/renderer/platform/image-decoders/segment_reader.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkYUVAIndex.h"

class SkData;

namespace blink {

class ImageFrameGenerator;

// Implements SkImageGenerator, used by SkPixelRef to populate a discardable
// memory with a decoded image frame. ImageFrameGenerator does the actual
// decoding.
class PLATFORM_EXPORT DecodingImageGenerator final
    : public PaintImageGenerator {
  USING_FAST_MALLOC(DecodingImageGenerator);

 public:
  // Aside from tests, this is used to create a decoder from SkData in Skia
  // (exported via WebImageGenerator and set via
  // SkGraphics::SetImageGeneratorFromEncodedDataFactory)
  static std::unique_ptr<SkImageGenerator> CreateAsSkImageGenerator(
      sk_sp<SkData>);

  static sk_sp<DecodingImageGenerator> Create(
      scoped_refptr<ImageFrameGenerator>,
      const SkImageInfo&,
      scoped_refptr<SegmentReader>,
      std::vector<FrameMetadata>,
      PaintImage::ContentId,
      bool all_data_received,
      bool is_eligible_for_accelerated_decoding,
      bool can_yuv_decode);

  ~DecodingImageGenerator() override;

  // PaintImageGenerator implementation.
  bool IsEligibleForAcceleratedDecoding() const override;
  sk_sp<SkData> GetEncodedData() const override;
  bool GetPixels(const SkImageInfo&,
                 void* pixels,
                 size_t row_bytes,
                 size_t frame_index,
                 PaintImage::GeneratorClientId client_id,
                 uint32_t lazy_pixel_ref) override;
  bool QueryYUVA8(SkYUVASizeInfo*,
                  SkYUVAIndex[SkYUVAIndex::kIndexCount],
                  SkYUVColorSpace*) const override;
  bool GetYUVA8Planes(const SkYUVASizeInfo&,
                      const SkYUVAIndex[SkYUVAIndex::kIndexCount],
                      void* planes[4],
                      size_t frame_index,
                      uint32_t lazy_pixel_ref) override;
  SkISize GetSupportedDecodeSize(const SkISize& requested_size) const override;
  PaintImage::ContentId GetContentIdForFrame(size_t frame_index) const override;

 private:
  DecodingImageGenerator(scoped_refptr<ImageFrameGenerator>,
                         const SkImageInfo&,
                         scoped_refptr<SegmentReader>,
                         std::vector<FrameMetadata>,
                         PaintImage::ContentId,
                         bool all_data_received,
                         bool is_eligible_for_accelerated_decoding,
                         bool can_yuv_decode);

  scoped_refptr<ImageFrameGenerator> frame_generator_;
  const scoped_refptr<SegmentReader> data_;  // Data source.
  const bool all_data_received_;
  const bool is_eligible_for_accelerated_decoding_;
  const bool can_yuv_decode_;
  const PaintImage::ContentId complete_frame_content_id_;

  DISALLOW_COPY_AND_ASSIGN(DecodingImageGenerator);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_DECODING_IMAGE_GENERATOR_H__
