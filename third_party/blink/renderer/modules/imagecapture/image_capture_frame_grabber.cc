// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/imagecapture/image_capture_frame_grabber.h"

#include "media/base/bind_to_current_loop.h"
#include "media/base/video_frame.h"
#include "media/base/video_types.h"
#include "media/base/video_util.h"
#include "skia/ext/platform_canvas.h"
#include "third_party/blink/public/platform/web_media_stream_source.h"
#include "third_party/blink/public/platform/web_media_stream_track.h"
#include "third_party/blink/renderer/core/imagebitmap/image_bitmap.h"
#include "third_party/blink/renderer/platform/wtf/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/thread_safe_ref_counted.h"
#include "third_party/libyuv/include/libyuv.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"

namespace WTF {
// Template specialization of [1], needed to be able to pass callbacks
// that have ScopedWebCallbacks paramaters across threads.
//
// [1] third_party/blink/renderer/platform/wtf/cross_thread_copier.h.
template <typename T>
struct CrossThreadCopier<blink::ScopedWebCallbacks<T>>
    : public CrossThreadCopierPassThrough<blink::ScopedWebCallbacks<T>> {
  STATIC_ONLY(CrossThreadCopier);
  using Type = blink::ScopedWebCallbacks<T>;
  static blink::ScopedWebCallbacks<T> Copy(
      blink::ScopedWebCallbacks<T> pointer) {
    return pointer;
  }
};

}  // namespace WTF

namespace blink {

namespace {

void OnError(std::unique_ptr<ImageCaptureGrabFrameCallbacks> callbacks) {
  callbacks->OnError();
}

}  // anonymous namespace

// Ref-counted class to receive a single VideoFrame on IO thread, convert it and
// send it to |main_task_runner_|, where this class is created and destroyed.
class ImageCaptureFrameGrabber::SingleShotFrameHandler
    : public WTF::ThreadSafeRefCounted<SingleShotFrameHandler> {
 public:
  SingleShotFrameHandler() : first_frame_received_(false) {}

  // Receives a |frame| and converts its pixels into a SkImage via an internal
  // PaintSurface and SkPixmap. Alpha channel, if any, is copied.
  using SkImageDeliverCB = WTF::CrossThreadFunction<void(sk_sp<SkImage>)>;
  void OnVideoFrameOnIOThread(
      SkImageDeliverCB callback,
      scoped_refptr<base::SingleThreadTaskRunner> task_runner,
      scoped_refptr<media::VideoFrame> frame,
      base::TimeTicks current_time);

 private:
  friend class WTF::ThreadSafeRefCounted<SingleShotFrameHandler>;

  // Flag to indicate that the first frames has been processed, and subsequent
  // ones can be safely discarded.
  bool first_frame_received_;

  DISALLOW_COPY_AND_ASSIGN(SingleShotFrameHandler);
};

void ImageCaptureFrameGrabber::SingleShotFrameHandler::OnVideoFrameOnIOThread(
    SkImageDeliverCB callback,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner,
    scoped_refptr<media::VideoFrame> frame,
    base::TimeTicks /* current_time */) {
  DCHECK(frame->format() == media::PIXEL_FORMAT_I420 ||
         frame->format() == media::PIXEL_FORMAT_I420A);

  if (first_frame_received_)
    return;
  first_frame_received_ = true;

  const SkAlphaType alpha = media::IsOpaque(frame->format())
                                ? kOpaque_SkAlphaType
                                : kPremul_SkAlphaType;
  const SkImageInfo info = SkImageInfo::MakeN32(
      frame->visible_rect().width(), frame->visible_rect().height(), alpha);

  sk_sp<SkSurface> surface = SkSurface::MakeRaster(info);
  DCHECK(surface);

  auto wrapper_callback = media::BindToLoop(
      std::move(task_runner), ConvertToBaseCallback(std::move(callback)));

  SkPixmap pixmap;
  if (!skia::GetWritablePixels(surface->getCanvas(), &pixmap)) {
    DLOG(ERROR) << "Error trying to map SkSurface's pixels";
    std::move(wrapper_callback).Run(sk_sp<SkImage>());
    return;
  }

  const uint32_t destination_pixel_format =
      (kN32_SkColorType == kRGBA_8888_SkColorType) ? libyuv::FOURCC_ABGR
                                                   : libyuv::FOURCC_ARGB;

  libyuv::ConvertFromI420(frame->visible_data(media::VideoFrame::kYPlane),
                          frame->stride(media::VideoFrame::kYPlane),
                          frame->visible_data(media::VideoFrame::kUPlane),
                          frame->stride(media::VideoFrame::kUPlane),
                          frame->visible_data(media::VideoFrame::kVPlane),
                          frame->stride(media::VideoFrame::kVPlane),
                          static_cast<uint8_t*>(pixmap.writable_addr()),
                          pixmap.width() * 4, pixmap.width(), pixmap.height(),
                          destination_pixel_format);

  if (frame->format() == media::PIXEL_FORMAT_I420A) {
    DCHECK(!info.isOpaque());
    // This function copies any plane into the alpha channel of an ARGB image.
    libyuv::ARGBCopyYToAlpha(frame->visible_data(media::VideoFrame::kAPlane),
                             frame->stride(media::VideoFrame::kAPlane),
                             static_cast<uint8_t*>(pixmap.writable_addr()),
                             pixmap.width() * 4, pixmap.width(),
                             pixmap.height());
  }

  std::move(wrapper_callback).Run(surface->makeImageSnapshot());
}

ImageCaptureFrameGrabber::ImageCaptureFrameGrabber()
    : frame_grab_in_progress_(false), weak_factory_(this) {}

ImageCaptureFrameGrabber::~ImageCaptureFrameGrabber() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
}

void ImageCaptureFrameGrabber::GrabFrame(
    WebMediaStreamTrack* track,
    std::unique_ptr<ImageCaptureGrabFrameCallbacks> callbacks,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(!!callbacks);

  DCHECK(track && !track->IsNull() && track->GetPlatformTrack());
  DCHECK_EQ(WebMediaStreamSource::kTypeVideo, track->Source().GetType());

  if (frame_grab_in_progress_) {
    // Reject grabFrame()s too close back to back.
    callbacks->OnError();
    return;
  }

  auto scoped_callbacks =
      MakeScopedWebCallbacks(std::move(callbacks), WTF::Bind(&OnError));

  // A SingleShotFrameHandler is bound and given to the Track to guarantee that
  // only one VideoFrame is converted and delivered to OnSkImage(), otherwise
  // SKImages might be sent to resolved |callbacks| while DisconnectFromTrack()
  // is being processed, which might be further held up if UI is busy, see
  // https://crbug.com/623042.
  frame_grab_in_progress_ = true;
  MediaStreamVideoSink::ConnectToTrack(
      *track,
      ConvertToBaseCallback(CrossThreadBind(
          &SingleShotFrameHandler::OnVideoFrameOnIOThread,
          base::MakeRefCounted<SingleShotFrameHandler>(),
          WTF::Passed(CrossThreadBind(
              &ImageCaptureFrameGrabber::OnSkImage, weak_factory_.GetWeakPtr(),
              WTF::Passed(std::move(scoped_callbacks)))),
          WTF::Passed(std::move(task_runner)))),
      false);
}

void ImageCaptureFrameGrabber::OnSkImage(
    ScopedWebCallbacks<ImageCaptureGrabFrameCallbacks> callbacks,
    sk_sp<SkImage> image) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  MediaStreamVideoSink::DisconnectFromTrack();
  frame_grab_in_progress_ = false;
  if (image)
    callbacks.PassCallbacks()->OnSuccess(image);
  else
    callbacks.PassCallbacks()->OnError();
}

}  // namespace blink
