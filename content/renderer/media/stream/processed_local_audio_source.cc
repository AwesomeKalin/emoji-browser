// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/media/stream/processed_local_audio_source.h"

#include <algorithm>
#include <utility>

#include "base/bind.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/stringprintf.h"
#include "build/build_config.h"
#include "content/public/common/content_features.h"
#include "content/renderer/media/audio/audio_device_factory.h"
#include "content/renderer/media/webrtc/peer_connection_dependency_factory.h"
#include "content/renderer/media/webrtc/webrtc_audio_device_impl.h"
#include "content/renderer/render_frame_impl.h"
#include "media/base/channel_layout.h"
#include "media/base/sample_rates.h"
#include "media/webrtc/webrtc_switches.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom-shared.h"
#include "third_party/blink/public/platform/modules/mediastream/media_stream_audio_processor_options.h"
#include "third_party/blink/public/platform/modules/webrtc/webrtc_logging.h"
#include "third_party/blink/public/web/modules/mediastream/media_stream_constraints_util.h"
#include "third_party/webrtc/media/base/media_channel.h"

namespace content {

using EchoCancellationType =
    blink::AudioProcessingProperties::EchoCancellationType;

namespace {
// Used as an identifier for ProcessedLocalAudioSource::From().
void* const kProcessedLocalAudioSourceIdentifier =
    const_cast<void**>(&kProcessedLocalAudioSourceIdentifier);

void LogAudioProcesingProperties(
    const blink::AudioProcessingProperties& properties) {
  auto aec_to_string =
      [](blink::AudioProcessingProperties::EchoCancellationType type) {
        using AEC = blink::AudioProcessingProperties::EchoCancellationType;
        switch (type) {
          case AEC::kEchoCancellationDisabled:
            return "disabled";
          case AEC::kEchoCancellationAec3:
            return "aec3";
          case AEC::kEchoCancellationSystem:
            return "system";
        }
      };
  auto bool_to_string = [](bool value) { return value ? "true" : "false"; };
  auto str = base::StringPrintf(
      "AudioProcessingProperties: "
      "aec=%s, "
      "disable_hw_ns=%s, "
      "goog_audio_mirroring=%s, "
      "goog_auto_gain_control=%s, "
      "goog_experimental_echo_cancellation=%s, "
      "goog_typing_noise_detection=%s, "
      "goog_noise_suppression=%s, "
      "goog_experimental_noise_suppression=%s, "
      "goog_highpass_filter=%s, "
      "goog_experimental_agc=%s, "
      "hybrid_agc=%s",
      aec_to_string(properties.echo_cancellation_type),
      bool_to_string(properties.disable_hw_noise_suppression),
      bool_to_string(properties.goog_audio_mirroring),
      bool_to_string(properties.goog_auto_gain_control),
      bool_to_string(properties.goog_experimental_echo_cancellation),
      bool_to_string(properties.goog_typing_noise_detection),
      bool_to_string(properties.goog_noise_suppression),
      bool_to_string(properties.goog_experimental_noise_suppression),
      bool_to_string(properties.goog_highpass_filter),
      bool_to_string(properties.goog_experimental_auto_gain_control),
      bool_to_string(base::FeatureList::IsEnabled(features::kWebRtcHybridAgc)));

  blink::WebRtcLogMessage(str);
}
}  // namespace

bool IsApmInAudioServiceEnabled() {
#if defined(OS_WIN) || defined(OS_MACOSX) || defined(OS_LINUX)
  return base::FeatureList::IsEnabled(features::kWebRtcApmInAudioService);
#else
  return false;
#endif
}

ProcessedLocalAudioSource::ProcessedLocalAudioSource(
    int consumer_render_frame_id,
    const blink::MediaStreamDevice& device,
    bool disable_local_echo,
    const blink::AudioProcessingProperties& audio_processing_properties,
    ConstraintsOnceCallback started_callback,
    PeerConnectionDependencyFactory* factory,
    scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : blink::MediaStreamAudioSource(std::move(task_runner),
                                    true /* is_local_source */,
                                    disable_local_echo),
      consumer_render_frame_id_(consumer_render_frame_id),
      pc_factory_(factory),
      audio_processing_properties_(audio_processing_properties),
      started_callback_(std::move(started_callback)),
      volume_(0),
      allow_invalid_render_frame_id_for_testing_(false),
      weak_factory_(this) {
  DCHECK(pc_factory_);
  DVLOG(1) << "ProcessedLocalAudioSource::ProcessedLocalAudioSource()";
  SetDevice(device);
}

ProcessedLocalAudioSource::~ProcessedLocalAudioSource() {
  DVLOG(1) << "ProcessedLocalAudioSource::~ProcessedLocalAudioSource()";
  EnsureSourceIsStopped();
}

// static
ProcessedLocalAudioSource* ProcessedLocalAudioSource::From(
    blink::MediaStreamAudioSource* source) {
  if (source &&
      source->GetClassIdentifier() == kProcessedLocalAudioSourceIdentifier)
    return static_cast<ProcessedLocalAudioSource*>(source);
  return nullptr;
}

void* ProcessedLocalAudioSource::GetClassIdentifier() const {
  return kProcessedLocalAudioSourceIdentifier;
}

bool ProcessedLocalAudioSource::EnsureSourceIsStarted() {
  DCHECK(GetTaskRunner()->BelongsToCurrentThread());

  if (source_)
    return true;

  // Sanity-check that the consuming RenderFrame still exists. This is required
  // to initialize the audio source.
  if (!allow_invalid_render_frame_id_for_testing_ &&
      !RenderFrameImpl::FromRoutingID(consumer_render_frame_id_)) {
    blink::WebRtcLogMessage(
        "ProcessedLocalAudioSource::EnsureSourceIsStarted() fails "
        " because the render frame does not exist.");
    return false;
  }

  std::string str = base::StringPrintf(
      "ProcessedLocalAudioSource::EnsureSourceIsStarted. render_frame_id=%d"
      ", channel_layout=%d, sample_rate=%d, buffer_size=%d"
      ", session_id=%d, effects=%d. ",
      consumer_render_frame_id_, device().input.channel_layout(),
      device().input.sample_rate(), device().input.frames_per_buffer(),
      device().session_id, device().input.effects());
  blink::WebRtcLogMessage(str);
  DVLOG(1) << str;

  LogAudioProcesingProperties(audio_processing_properties_);

  blink::MediaStreamDevice modified_device(device());
  bool device_is_modified = false;

  // Disable system echo cancellation if specified by
  // |audio_processing_properties_|.
  if (audio_processing_properties_.echo_cancellation_type !=
          EchoCancellationType::kEchoCancellationSystem &&
      device().input.effects() & media::AudioParameters::ECHO_CANCELLER) {
    modified_device.input.set_effects(modified_device.input.effects() &
                                      ~media::AudioParameters::ECHO_CANCELLER);
    device_is_modified = true;
  } else if (audio_processing_properties_.echo_cancellation_type ==
                 EchoCancellationType::kEchoCancellationSystem &&
             (device().input.effects() &
              media::AudioParameters::EXPERIMENTAL_ECHO_CANCELLER)) {
    // Set the ECHO_CANCELLER effect, since that is what controls what's
    // actually being used. The EXPERIMENTAL_ flag only indicates availability.
    // TODO(grunell): AND with
    // ~media::AudioParameters::EXPERIMENTAL_ECHO_CANCELLER.
    modified_device.input.set_effects(modified_device.input.effects() |
                                      media::AudioParameters::ECHO_CANCELLER);
    device_is_modified = true;
  }

  // Disable noise suppression on the device if the properties explicitly
  // specify to do so.
  if (audio_processing_properties_.disable_hw_noise_suppression &&
      (device().input.effects() & media::AudioParameters::NOISE_SUPPRESSION)) {
    modified_device.input.set_effects(
        modified_device.input.effects() &
        ~media::AudioParameters::NOISE_SUPPRESSION);
    device_is_modified = true;
  }

  if (device_is_modified)
    SetDevice(modified_device);

  // Create the MediaStreamAudioProcessor, bound to the WebRTC audio device
  // module.
  WebRtcAudioDeviceImpl* const rtc_audio_device =
      pc_factory_->GetWebRtcAudioDevice();
  if (!rtc_audio_device) {
    blink::WebRtcLogMessage(
        "ProcessedLocalAudioSource::EnsureSourceIsStarted() fails"
        " because there is no WebRtcAudioDeviceImpl instance.");
    return false;
  }

  // If KEYBOARD_MIC effect is set, change the layout to the corresponding
  // layout that includes the keyboard mic.
  media::ChannelLayout channel_layout = device().input.channel_layout();
  if ((device().input.effects() & media::AudioParameters::KEYBOARD_MIC) &&
      audio_processing_properties_.goog_experimental_noise_suppression) {
    if (channel_layout == media::CHANNEL_LAYOUT_STEREO) {
      channel_layout = media::CHANNEL_LAYOUT_STEREO_AND_KEYBOARD_MIC;
      DVLOG(1) << "Changed stereo layout to stereo + keyboard mic layout due "
               << "to KEYBOARD_MIC effect.";
    } else {
      DVLOG(1) << "KEYBOARD_MIC effect ignored, not compatible with layout "
               << channel_layout;
    }
  }

  DVLOG(1) << "Audio input hardware channel layout: " << channel_layout;
  UMA_HISTOGRAM_ENUMERATION("WebRTC.AudioInputChannelLayout",
                            channel_layout, media::CHANNEL_LAYOUT_MAX + 1);

  // Verify that the reported input channel configuration is supported.
  if (channel_layout != media::CHANNEL_LAYOUT_MONO &&
      channel_layout != media::CHANNEL_LAYOUT_STEREO &&
      channel_layout != media::CHANNEL_LAYOUT_STEREO_AND_KEYBOARD_MIC &&
      channel_layout != media::CHANNEL_LAYOUT_DISCRETE) {
    blink::WebRtcLogMessage(base::StringPrintf(
        "ProcessedLocalAudioSource::EnsureSourceIsStarted() fails "
        " because the input channel layout (%d) is not supported.",
        static_cast<int>(channel_layout)));
    return false;
  }

  DVLOG(1) << "Audio input hardware sample rate: "
           << device().input.sample_rate();
  media::AudioSampleRate asr;
  if (media::ToAudioSampleRate(device().input.sample_rate(), &asr)) {
    UMA_HISTOGRAM_ENUMERATION(
        "WebRTC.AudioInputSampleRate", asr, media::kAudioSampleRateMax + 1);
  } else {
    UMA_HISTOGRAM_COUNTS_1M("WebRTC.AudioInputSampleRateUnexpected",
                            device().input.sample_rate());
  }

  // Determine the audio format required of the AudioCapturerSource. Then, pass
  // that to the |audio_processor_| and set the output format of this
  // ProcessedLocalAudioSource to the processor's output format.
  media::AudioParameters params(media::AudioParameters::AUDIO_PCM_LOW_LATENCY,
                                channel_layout, device().input.sample_rate(),
                                device().input.sample_rate() / 100);
  params.set_effects(device().input.effects());
  if (channel_layout == media::CHANNEL_LAYOUT_DISCRETE) {
    DCHECK_LE(device().input.channels(), 2);
    params.set_channels_for_discrete(device().input.channels());
  }
  DVLOG(1) << params.AsHumanReadableString();
  DCHECK(params.IsValid());
  media::AudioSourceParameters source_params(device().session_id);
  const bool use_remote_apm =
      IsApmInAudioServiceEnabled() &&
      MediaStreamAudioProcessor::WouldModifyAudio(audio_processing_properties_);
  if (use_remote_apm) {
    audio_processor_proxy_ =
        new rtc::RefCountedObject<AudioServiceAudioProcessorProxy>(
            GetTaskRunner());
    SetFormat(params);
    // Add processing to the source.
    source_params.processing = media::AudioSourceParameters::ProcessingConfig(
        rtc_audio_device->GetAudioProcessingId(),
        audio_processing_properties_.ToAudioProcessingSettings());
    if (source_params.processing->settings.automatic_gain_control !=
            media::AutomaticGainControlType::kDisabled &&
        base::FeatureList::IsEnabled(features::kWebRtcHybridAgc)) {
      source_params.processing->settings.automatic_gain_control =
          media::AutomaticGainControlType::kHybridExperimental;
    }
    blink::WebRtcLogMessage(base::StringPrintf(
        "Using APM in audio process; settings: %s",
        source_params.processing->settings.ToString().c_str()));

  } else {
    blink::WebRtcLogMessage("Using APM in renderer process.");
    audio_processor_ = new rtc::RefCountedObject<MediaStreamAudioProcessor>(
        audio_processing_properties_, rtc_audio_device);
    params.set_frames_per_buffer(GetBufferSize(device().input.sample_rate()));
    audio_processor_->OnCaptureFormatChanged(params);
    SetFormat(audio_processor_->OutputFormat());
  }

  // Start the source.
  DVLOG(1) << "Starting WebRTC audio source for consumption by render frame "
           << consumer_render_frame_id_ << " with input parameters={"
           << params.AsHumanReadableString() << "} and output parameters={"
           << GetAudioParameters().AsHumanReadableString() << '}';
  scoped_refptr<media::AudioCapturerSource> new_source =
      AudioDeviceFactory::NewAudioCapturerSource(consumer_render_frame_id_,
                                                 source_params);
  new_source->Initialize(params, this);
  // We need to set the AGC control before starting the stream.
  new_source->SetAutomaticGainControl(true);
  source_ = std::move(new_source);
  source_->Start();

  // Register this source with the WebRtcAudioDeviceImpl.
  rtc_audio_device->AddAudioCapturer(this);

  return true;
}

void ProcessedLocalAudioSource::EnsureSourceIsStopped() {
  DCHECK(GetTaskRunner()->BelongsToCurrentThread());

  if (!source_)
    return;

  scoped_refptr<media::AudioCapturerSource> source_to_stop(std::move(source_));

  if (WebRtcAudioDeviceImpl* rtc_audio_device =
      pc_factory_->GetWebRtcAudioDevice()) {
    rtc_audio_device->RemoveAudioCapturer(this);
  }

  source_to_stop->Stop();

  // Stop the audio processor to avoid feeding render data into the processor.
  if (audio_processor_)
    audio_processor_->Stop();

  // Stop the proxy, if we have one, so as to detach from the processor
  // controls.
  if (audio_processor_proxy_)
    audio_processor_proxy_->Stop();

  DVLOG(1) << "Stopped WebRTC audio pipeline for consumption by render frame "
           << consumer_render_frame_id_ << '.';
}

void ProcessedLocalAudioSource::SetVolume(int volume) {
  DVLOG(1) << "ProcessedLocalAudioSource::SetVolume()";
  DCHECK_LE(volume, MaxVolume());
  const double normalized_volume = static_cast<double>(volume) / MaxVolume();
  if (source_)
    source_->SetVolume(normalized_volume);
}

int ProcessedLocalAudioSource::Volume() const {
  // Note: Using NoBarrier_Load() because the timing of visibility of the
  // updated volume information on other threads can be relaxed.
  return base::subtle::NoBarrier_Load(&volume_);
}

int ProcessedLocalAudioSource::MaxVolume() const {
  return WebRtcAudioDeviceImpl::kMaxVolumeLevel;
}

void ProcessedLocalAudioSource::OnCaptureStarted() {
  std::move(started_callback_)
      .Run(this, blink::mojom::MediaStreamRequestResult::OK, "");
}

void ProcessedLocalAudioSource::Capture(const media::AudioBus* audio_bus,
                                        int audio_delay_milliseconds,
                                        double volume,
                                        bool key_pressed) {
  if (audio_processor_) {
    // The data must be processed here.
    CaptureUsingProcessor(audio_bus, audio_delay_milliseconds, volume,
                          key_pressed);
  } else {
    // The audio is already processed in the audio service, just send it along.
    level_calculator_.Calculate(*audio_bus, false);
    DeliverDataToTracks(
        *audio_bus, base::TimeTicks::Now() - base::TimeDelta::FromMilliseconds(
                                                 audio_delay_milliseconds));
  }
}

void ProcessedLocalAudioSource::OnCaptureError(const std::string& message) {
  blink::WebRtcLogMessage("ProcessedLocalAudioSource::OnCaptureError: " +
                          message);
  StopSourceOnError(message);
}

void ProcessedLocalAudioSource::OnCaptureMuted(bool is_muted) {
  SetMutedState(is_muted);
}

void ProcessedLocalAudioSource::OnCaptureProcessorCreated(
    media::AudioProcessorControls* controls) {
  DCHECK(audio_processor_proxy_);
  audio_processor_proxy_->SetControls(controls);
}

void ProcessedLocalAudioSource::SetOutputDeviceForAec(
    const std::string& output_device_id) {
  DVLOG(1) << "ProcessedLocalAudioSource::SetOutputDeviceForAec()";
  if (source_)
    source_->SetOutputDeviceForAec(output_device_id);
}

void ProcessedLocalAudioSource::CaptureUsingProcessor(
    const media::AudioBus* audio_bus,
    int audio_delay_milliseconds,
    double volume,
    bool key_pressed) {
#if defined(OS_WIN) || defined(OS_MACOSX)
  DCHECK_LE(volume, 1.0);
#elif (defined(OS_LINUX) && !defined(OS_CHROMEOS)) || defined(OS_OPENBSD)
  // We have a special situation on Linux where the microphone volume can be
  // "higher than maximum". The input volume slider in the sound preference
  // allows the user to set a scaling that is higher than 100%. It means that
  // even if the reported maximum levels is N, the actual microphone level can
  // go up to 1.5x*N and that corresponds to a normalized |volume| of 1.5x.
  DCHECK_LE(volume, 1.6);
#endif

  // TODO(miu): Plumbing is needed to determine the actual capture timestamp
  // of the audio, instead of just snapshotting TimeTicks::Now(), for proper
  // audio/video sync.  https://crbug.com/335335
  const base::TimeTicks reference_clock_snapshot = base::TimeTicks::Now();
  TRACE_EVENT2("audio", "ProcessedLocalAudioSource::Capture", "now (ms)",
               (reference_clock_snapshot - base::TimeTicks()).InMillisecondsF(),
               "delay (ms)", audio_delay_milliseconds);

  // Map internal volume range of [0.0, 1.0] into [0, 255] used by AGC.
  // The volume can be higher than 255 on Linux, and it will be cropped to
  // 255 since AGC does not allow values out of range.
  int current_volume = static_cast<int>((volume * MaxVolume()) + 0.5);
  // Note: Using NoBarrier_Store() because the timing of visibility of the
  // updated volume information on other threads can be relaxed.
  base::subtle::NoBarrier_Store(&volume_, current_volume);
  current_volume = std::min(current_volume, MaxVolume());

  // Sanity-check the input audio format in debug builds.  Then, notify the
  // tracks if the format has changed.
  //
  // Locking is not needed here to read the audio input/output parameters
  // because the audio processor format changes only occur while audio capture
  // is stopped.
  DCHECK(audio_processor_->InputFormat().IsValid());
  DCHECK_EQ(audio_bus->channels(), audio_processor_->InputFormat().channels());
  DCHECK_EQ(audio_bus->frames(),
            audio_processor_->InputFormat().frames_per_buffer());

  // Figure out if the pre-processed data has any energy or not. This
  // information will be passed to the level calculator to force it to report
  // energy in case the post-processed data is zeroed by the audio processing.
  const bool force_report_nonzero_energy = !audio_bus->AreFramesZero();

  // Push the data to the processor for processing.
  audio_processor_->PushCaptureData(
      *audio_bus,
      base::TimeDelta::FromMilliseconds(audio_delay_milliseconds));

  // Process and consume the data in the processor until there is not enough
  // data in the processor.
  media::AudioBus* processed_data = nullptr;
  base::TimeDelta processed_data_audio_delay;
  int new_volume = 0;
  while (audio_processor_->ProcessAndConsumeData(
             current_volume, key_pressed,
             &processed_data, &processed_data_audio_delay, &new_volume)) {
    DCHECK(processed_data);

    level_calculator_.Calculate(*processed_data, force_report_nonzero_energy);

    DeliverDataToTracks(*processed_data,
                        reference_clock_snapshot - processed_data_audio_delay);

    if (new_volume) {
      GetTaskRunner()->PostTask(
          FROM_HERE, base::BindOnce(&ProcessedLocalAudioSource::SetVolume,
                                    weak_factory_.GetWeakPtr(), new_volume));
      // Update the |current_volume| to avoid passing the old volume to AGC.
      current_volume = new_volume;
    }
  }
}

int ProcessedLocalAudioSource::GetBufferSize(int sample_rate) const {
  DCHECK(GetTaskRunner()->BelongsToCurrentThread());
#if defined(OS_ANDROID)
  // TODO(henrika): Re-evaluate whether to use same logic as other platforms.
  // https://crbug.com/638081
  return (2 * sample_rate / 100);
#endif

  // If audio processing is turned on, require 10ms buffers.
  if (audio_processor_->has_audio_processing() || audio_processor_proxy_)
    return (sample_rate / 100);

  // If audio processing is off and the native hardware buffer size was
  // provided, use it. It can be harmful, in terms of CPU/power consumption, to
  // use smaller buffer sizes than the native size (https://crbug.com/362261).
  if (int hardware_buffer_size = device().input.frames_per_buffer())
    return hardware_buffer_size;

  // If the buffer size is missing from the MediaStreamDevice, provide 10ms as a
  // fall-back.
  //
  // TODO(miu): Identify where/why the buffer size might be missing, fix the
  // code, and then require it here. https://crbug.com/638081
  return (sample_rate / 100);
}

}  // namespace content
