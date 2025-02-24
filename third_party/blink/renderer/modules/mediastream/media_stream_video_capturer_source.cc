// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/web/modules/mediastream/media_stream_video_capturer_source.h"

#include <utility>

#include "base/bind.h"
#include "media/capture/video_capturer_source.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-blink.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_constraints_util.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_local_frame_client.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {

class MediaStreamVideoCapturerSource::InternalState {
 public:
  InternalState(WebLocalFrame* web_frame)
      : frame_(web_frame ? static_cast<LocalFrame*>(
                               WebLocalFrame::ToCoreFrame(*web_frame))
                         : nullptr) {}

  LocalFrame* frame() { return frame_.Get(); }

  const mojom::blink::MediaStreamDispatcherHostPtr&
  GetMediaStreamDispatcherHost() {
    DCHECK(frame_);
    if (!host_)
      frame_->GetInterfaceProvider().GetInterface(mojo::MakeRequest(&host_));
    return host_;
  }

  void SetMediaStreamDispatcherHostForTesting(
      mojom::blink::MediaStreamDispatcherHostPtr host) {
    host_ = std::move(host);
  }

 private:
  WeakPersistent<LocalFrame> frame_;
  mojom::blink::MediaStreamDispatcherHostPtr host_;
};

MediaStreamVideoCapturerSource::MediaStreamVideoCapturerSource(
    const SourceStoppedCallback& stop_callback,
    std::unique_ptr<media::VideoCapturerSource> source)
    : internal_state_(std::make_unique<InternalState>(
          blink::WebLocalFrame::FrameForCurrentContext())),
      source_(std::move(source)) {
  media::VideoCaptureFormats preferred_formats = source_->GetPreferredFormats();
  if (!preferred_formats.empty())
    capture_params_.requested_format = preferred_formats.front();
  SetStopCallback(stop_callback);
}

MediaStreamVideoCapturerSource::MediaStreamVideoCapturerSource(
    WebLocalFrame* web_frame,
    const SourceStoppedCallback& stop_callback,
    const MediaStreamDevice& device,
    const media::VideoCaptureParams& capture_params,
    DeviceCapturerFactoryCallback device_capturer_factory_callback)
    : internal_state_(std::make_unique<InternalState>(web_frame)),
      source_(device_capturer_factory_callback.Run(device.session_id)),
      capture_params_(capture_params),
      device_capturer_factory_callback_(
          std::move(device_capturer_factory_callback)) {
  SetStopCallback(stop_callback);
  SetDevice(device);
  SetDeviceRotationDetection(true /* enabled */);
}

MediaStreamVideoCapturerSource::~MediaStreamVideoCapturerSource() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void MediaStreamVideoCapturerSource::SetDeviceCapturerFactoryCallbackForTesting(
    DeviceCapturerFactoryCallback testing_factory_callback) {
  device_capturer_factory_callback_ = std::move(testing_factory_callback);
}

void MediaStreamVideoCapturerSource::RequestRefreshFrame() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  source_->RequestRefreshFrame();
}

void MediaStreamVideoCapturerSource::OnFrameDropped(
    media::VideoCaptureFrameDropReason reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  source_->OnFrameDropped(reason);
}

void MediaStreamVideoCapturerSource::OnLog(const std::string& message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  source_->OnLog(message);
}

void MediaStreamVideoCapturerSource::OnHasConsumers(bool has_consumers) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (has_consumers)
    source_->Resume();
  else
    source_->MaybeSuspend();
}

void MediaStreamVideoCapturerSource::OnCapturingLinkSecured(bool is_secure) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!internal_state_->frame())
    return;
  internal_state_->GetMediaStreamDispatcherHost()->SetCapturingLinkSecured(
      device().session_id,
      static_cast<mojom::blink::MediaStreamType>(device().type), is_secure);
}

void MediaStreamVideoCapturerSource::StartSourceImpl(
    const VideoCaptureDeliverFrameCB& frame_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  state_ = STARTING;
  frame_callback_ = frame_callback;
  source_->StartCapture(
      capture_params_, frame_callback_,
      WTF::BindRepeating(&MediaStreamVideoCapturerSource::OnRunStateChanged,
                         WTF::Unretained(this), capture_params_));
}

void MediaStreamVideoCapturerSource::StopSourceImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  source_->StopCapture();
}

void MediaStreamVideoCapturerSource::StopSourceForRestartImpl() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != STARTED) {
    OnStopForRestartDone(false);
    return;
  }
  state_ = STOPPING_FOR_RESTART;
  source_->StopCapture();

  // Force state update for nondevice sources, since they do not
  // automatically update state after StopCapture().
  if (device().type == mojom::blink::MediaStreamType::NO_SERVICE)
    OnRunStateChanged(capture_params_, false);
}

void MediaStreamVideoCapturerSource::RestartSourceImpl(
    const media::VideoCaptureFormat& new_format) {
  DCHECK(new_format.IsValid());
  media::VideoCaptureParams new_capture_params = capture_params_;
  new_capture_params.requested_format = new_format;
  state_ = RESTARTING;
  source_->StartCapture(
      new_capture_params, frame_callback_,
      WTF::BindRepeating(&MediaStreamVideoCapturerSource::OnRunStateChanged,
                         WTF::Unretained(this), new_capture_params));
}

base::Optional<media::VideoCaptureFormat>
MediaStreamVideoCapturerSource::GetCurrentFormat() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return capture_params_.requested_format;
}

base::Optional<media::VideoCaptureParams>
MediaStreamVideoCapturerSource::GetCurrentCaptureParams() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return capture_params_;
}

void MediaStreamVideoCapturerSource::ChangeSourceImpl(
    const MediaStreamDevice& new_device) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(device_capturer_factory_callback_);

  if (state_ != STARTED) {
    return;
  }

  state_ = STOPPING_FOR_CHANGE_SOURCE;
  source_->StopCapture();
  SetDevice(new_device);
  source_ = device_capturer_factory_callback_.Run(new_device.session_id);
  source_->StartCapture(
      capture_params_, frame_callback_,
      WTF::BindRepeating(&MediaStreamVideoCapturerSource::OnRunStateChanged,
                         WTF::Unretained(this), capture_params_));
}

void MediaStreamVideoCapturerSource::OnRunStateChanged(
    const media::VideoCaptureParams& new_capture_params,
    bool is_running) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  switch (state_) {
    case STARTING:
      source_->OnLog("MediaStreamVideoCapturerSource sending OnStartDone");
      if (is_running) {
        state_ = STARTED;
        DCHECK(capture_params_ == new_capture_params);
        OnStartDone(mojom::blink::MediaStreamRequestResult::OK);
      } else {
        state_ = STOPPED;
        OnStartDone(
            mojom::blink::MediaStreamRequestResult::TRACK_START_FAILURE_VIDEO);
      }
      break;
    case STARTED:
      if (!is_running) {
        state_ = STOPPED;
        StopSource();
      }
      break;
    case STOPPING_FOR_RESTART:
      source_->OnLog(
          "MediaStreamVideoCapturerSource sending OnStopForRestartDone");
      state_ = is_running ? STARTED : STOPPED;
      OnStopForRestartDone(!is_running);
      break;
    case STOPPING_FOR_CHANGE_SOURCE:
      state_ = is_running ? STARTED : STOPPED;
      break;
    case RESTARTING:
      if (is_running) {
        state_ = STARTED;
        capture_params_ = new_capture_params;
      } else {
        state_ = STOPPED;
      }
      source_->OnLog("MediaStreamVideoCapturerSource sending OnRestartDone");
      OnRestartDone(is_running);
      break;
    case STOPPED:
      break;
  }
}

void MediaStreamVideoCapturerSource::SetMediaStreamDispatcherHostForTesting(
    void* dispatcher_host) {
  mojom::blink::MediaStreamDispatcherHostPtr* host =
      static_cast<mojom::blink::MediaStreamDispatcherHostPtr*>(dispatcher_host);
  internal_state_->SetMediaStreamDispatcherHostForTesting(std::move(*host));
}

media::VideoCapturerSource*
MediaStreamVideoCapturerSource::GetSourceForTesting() {
  return source_.get();
}

}  // namespace blink
