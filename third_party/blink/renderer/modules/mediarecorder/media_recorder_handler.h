// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIARECORDER_MEDIA_RECORDER_HANDLER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIARECORDER_MEDIA_RECORDER_HANDLER_H_

#include <memory>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_piece.h"
#include "base/threading/thread_checker.h"
#include "third_party/blink/public/platform/web_media_stream.h"
#include "third_party/blink/public/platform/web_vector.h"
#include "third_party/blink/renderer/modules/mediarecorder/audio_track_recorder.h"
#include "third_party/blink/renderer/modules/mediarecorder/video_track_recorder.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace media {
class AudioBus;
class AudioParameters;
class VideoFrame;
class WebmMuxer;
}  // namespace media

namespace blink {

struct WebMediaCapabilitiesInfo;
struct WebMediaConfiguration;
class WebMediaRecorderHandlerClient;

// MediaRecorderHandler orchestrates the creation, lifetime management and
// mapping between:
// - MediaStreamTrack(s) providing data,
// - {Audio,Video}TrackRecorders encoding that data,
// - a WebmMuxer class multiplexing encoded data into a WebM container, and
// - a single recorder client receiving this contained data.
// All methods are called on the same thread as construction and destruction,
// i.e. the Main Render thread. (Note that a BindToCurrentLoop is used to
// guarantee this, since VideoTrackRecorder sends back frames on IO thread.)
class MODULES_EXPORT MediaRecorderHandler {
 public:
  static std::unique_ptr<MediaRecorderHandler> Create(
      scoped_refptr<base::SingleThreadTaskRunner> task_runner);

  explicit MediaRecorderHandler(
      scoped_refptr<base::SingleThreadTaskRunner> task_runner);
  ~MediaRecorderHandler();

  // MediaRecorder API isTypeSupported(), which boils down to
  // CanSupportMimeType() [1] "If true is returned from this method, it only
  // indicates that the MediaRecorder implementation is capable of recording
  // Blob objects for the specified MIME type. Recording may still fail if
  // sufficient resources are not available to support the concrete media
  // encoding."
  // [1] https://w3c.github.io/mediacapture-record/MediaRecorder.html#methods
  bool CanSupportMimeType(const String& type, const String& web_codecs);
  bool Initialize(WebMediaRecorderHandlerClient* client,
                  const WebMediaStream& media_stream,
                  const String& type,
                  const String& codecs,
                  int32_t audio_bits_per_second,
                  int32_t video_bits_per_second);
  bool Start(int timeslice);
  void Stop();
  void Pause();
  void Resume();

  // Implements WICG Media Capabilities encodingInfo() call for local encoding.
  // https://wicg.github.io/media-capabilities/#media-capabilities-interface
  using OnMediaCapabilitiesEncodingInfoCallback =
      base::OnceCallback<void(std::unique_ptr<WebMediaCapabilitiesInfo>)>;
  void EncodingInfo(const WebMediaConfiguration& configuration,
                    OnMediaCapabilitiesEncodingInfoCallback cb);
  String ActualMimeType();

 private:
  friend class MediaRecorderHandlerTest;

  // Called to indicate there is encoded video data available. |encoded_alpha|
  // represents the encode output of alpha channel when available, can be
  // nullptr otherwise.
  // TODO(crbug.com/960665): Replace std::string with WTF::String
  void OnEncodedVideo(const media::WebmMuxer::VideoParameters& params,
                      std::unique_ptr<std::string> encoded_data,
                      std::unique_ptr<std::string> encoded_alpha,
                      base::TimeTicks timestamp,
                      bool is_key_frame);
  // TODO(crbug.com/960665): Replace std::string with WTF::String
  void OnEncodedAudio(const media::AudioParameters& params,
                      std::unique_ptr<std::string> encoded_data,
                      base::TimeTicks timestamp);
  void WriteData(base::StringPiece data);

  // Updates |video_tracks_|,|audio_tracks_| and returns true if any changed.
  bool UpdateTracksAndCheckIfChanged();

  void OnVideoFrameForTesting(scoped_refptr<media::VideoFrame> frame,
                              const base::TimeTicks& timestamp);
  void OnAudioBusForTesting(const media::AudioBus& audio_bus,
                            const base::TimeTicks& timestamp);
  void SetAudioFormatForTesting(const media::AudioParameters& params);

  // Sanitized video and audio bitrate settings passed on initialize().
  int32_t video_bits_per_second_;
  int32_t audio_bits_per_second_;

  // Video Codec, VP8 is used by default.
  VideoTrackRecorder::CodecId video_codec_id_;

  // Audio Codec, OPUS is used by default.
  AudioTrackRecorder::CodecId audio_codec_id_;

  // |client_| has no notion of time, thus may configure us via start(timeslice)
  // to notify it after a certain |timeslice_| has passed. We use a moving
  // |slice_origin_timestamp_| to track those time chunks.
  base::TimeDelta timeslice_;
  base::TimeTicks slice_origin_timestamp_;

  bool recording_;
  WebMediaStream media_stream_;  // The MediaStream being recorded.
  WebVector<WebMediaStreamTrack> video_tracks_;
  WebVector<WebMediaStreamTrack> audio_tracks_;

  // |client_| is a weak pointer, and is valid for the lifetime of this object.
  WebMediaRecorderHandlerClient* client_;

  Vector<std::unique_ptr<VideoTrackRecorder>> video_recorders_;
  Vector<std::unique_ptr<AudioTrackRecorder>> audio_recorders_;

  // Worker class doing the actual Webm Muxing work.
  std::unique_ptr<media::WebmMuxer> webm_muxer_;

  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;

  base::WeakPtrFactory<MediaRecorderHandler> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(MediaRecorderHandler);
};

}  // namespace blink
#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIARECORDER_MEDIA_RECORDER_HANDLER_H_
