// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_MODULES_MEDIASTREAM_MEDIA_STREAM_VIDEO_RENDERER_SINK_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_MODULES_MEDIASTREAM_MEDIA_STREAM_VIDEO_RENDERER_SINK_H_

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/thread_checker.h"
#include "third_party/blink/public/common/media/video_capture.h"
#include "third_party/blink/public/platform/modules/mediastream/web_media_stream_video_renderer.h"
#include "third_party/blink/public/platform/web_common.h"
#include "third_party/blink/public/platform/web_media_stream_track.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_video_sink.h"
#include "ui/gfx/geometry/size.h"

namespace base {
class SingleThreadTaskRunner;
}  // namespace base

namespace blink {

// MediaStreamVideoRendererSink is a blink::WebMediaStreamVideoRenderer designed
// for rendering Video MediaStreamTracks [1], MediaStreamVideoRendererSink
// implements MediaStreamVideoSink in order to render video frames provided from
// a MediaStreamVideoTrack, to which it connects itself when the
// MediaStreamVideoRenderer is Start()ed, and disconnects itself when the latter
// is Stop()ed.
//
// [1] https://dev.w3.org/2011/webrtc/editor/getusermedia.html#mediastreamtrack
//
// TODO(wuchengli): Add unit test. See the link below for reference.
// https://src.chromium.org/viewvc/chrome/trunk/src/content/renderer/media/rtc_
// video_decoder_unittest.cc?revision=180591&view=markup
class BLINK_MODULES_EXPORT MediaStreamVideoRendererSink
    : public blink::WebMediaStreamVideoRenderer,
      public blink::MediaStreamVideoSink {
 public:
  MediaStreamVideoRendererSink(
      const blink::WebMediaStreamTrack& video_track,
      const blink::WebMediaStreamVideoRenderer::RepaintCB& repaint_cb,
      scoped_refptr<base::SingleThreadTaskRunner> io_task_runner,
      scoped_refptr<base::SingleThreadTaskRunner> main_render_task_runner);

  // blink::WebMediaStreamVideoRenderer implementation. Called on the main
  // thread.
  void Start() override;
  void Stop() override;
  void Resume() override;
  void Pause() override;

 protected:
  ~MediaStreamVideoRendererSink() override;

 private:
  friend class MediaStreamVideoRendererSinkTest;
  enum State {
    STARTED,
    PAUSED,
    STOPPED,
  };

  // MediaStreamVideoSink implementation. Called on the main thread.
  void OnReadyStateChanged(
      blink::WebMediaStreamSource::ReadyState state) override;

  // Helper method used for testing.
  State GetStateForTesting();

  const RepaintCB repaint_cb_;
  const blink::WebMediaStreamTrack video_track_;

  // Inner class used for transfering frames on compositor thread and running
  // |repaint_cb_|.
  class FrameDeliverer;
  std::unique_ptr<FrameDeliverer> frame_deliverer_;

  const scoped_refptr<base::SingleThreadTaskRunner> io_task_runner_;
  const scoped_refptr<base::SingleThreadTaskRunner> main_render_task_runner_;

  THREAD_CHECKER(main_thread_checker_);

  base::WeakPtrFactory<MediaStreamVideoRendererSink> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(MediaStreamVideoRendererSink);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_MODULES_MEDIASTREAM_MEDIA_STREAM_VIDEO_RENDERER_SINK_H_
