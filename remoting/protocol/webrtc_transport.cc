// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "remoting/protocol/webrtc_transport.h"

#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/bind.h"
#include "base/command_line.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task_runner_util.h"
#include "base/threading/thread_restrictions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "jingle/glue/thread_wrapper.h"
#include "remoting/protocol/authenticator.h"
#include "remoting/protocol/port_allocator_factory.h"
#include "remoting/protocol/sdp_message.h"
#include "remoting/protocol/stream_message_pipe_adapter.h"
#include "remoting/protocol/transport_context.h"
#include "remoting/protocol/webrtc_audio_module.h"
#include "remoting/protocol/webrtc_dummy_video_encoder.h"
#include "third_party/libjingle_xmpp/xmllite/xmlelement.h"
#include "third_party/webrtc/api/audio_codecs/audio_decoder_factory_template.h"
#include "third_party/webrtc/api/audio_codecs/audio_encoder_factory_template.h"
#include "third_party/webrtc/api/audio_codecs/opus/audio_decoder_opus.h"
#include "third_party/webrtc/api/audio_codecs/opus/audio_encoder_opus.h"
#include "third_party/webrtc/api/call/call_factory_interface.h"
#include "third_party/webrtc/api/peer_connection_interface.h"
#include "third_party/webrtc/api/rtc_event_log/rtc_event_log_factory.h"
#include "third_party/webrtc/api/stats/rtcstats_objects.h"
#include "third_party/webrtc/api/video_codecs/builtin_video_decoder_factory.h"
#include "third_party/webrtc/media/engine/webrtc_media_engine.h"
#include "third_party/webrtc/modules/audio_processing/include/audio_processing.h"
#include "third_party/webrtc_overrides/task_queue_factory.h"

using jingle_xmpp::QName;
using jingle_xmpp::XmlElement;

namespace remoting {
namespace protocol {

class ScopedAllowThreadJoinForWebRtcTransport
    : public base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope {};

namespace {

// Delay after candidate creation before sending transport-info message to
// accumulate multiple candidates. This is an optimization to reduce number of
// transport-info messages.
const int kTransportInfoSendDelayMs = 20;

// XML namespace for the transport elements.
const char kTransportNamespace[] = "google:remoting:webrtc";

// Global minimum/maximum bitrates set for the PeerConnection.
const int kMinBitrateBps = 1e6;  // 1 Mbps.
const int kMaxBitrateBps = 1e8;  // 100 Mbps.

// Frequency of polling for RTCStats. Polling is needed because WebRTC native
// API does not provide a route-change notification for the connection type
// (direct/STUN/relay).
// TODO(lambroslambrou): Remove polling when a native API is provided.
constexpr base::TimeDelta kRtcStatsPollingInterval =
    base::TimeDelta::FromSeconds(2);

#if !defined(NDEBUG)
// Command line switch used to disable signature verification.
// TODO(sergeyu): Remove this flag.
const char kDisableAuthenticationSwitchName[] = "disable-authentication";
#endif

bool IsValidSessionDescriptionType(const std::string& type) {
  return type == webrtc::SessionDescriptionInterface::kOffer ||
         type == webrtc::SessionDescriptionInterface::kAnswer;
}

void UpdateCodecParameters(SdpMessage* sdp_message, bool incoming) {
  // Update SDP format to use 160kbps stereo for opus codec.
  if (sdp_message->has_audio() &&
      !sdp_message->AddCodecParameter("opus",
                                      "stereo=1; maxaveragebitrate=163840")) {
    if (incoming) {
      LOG(WARNING) << "Opus not found in an incoming SDP.";
    } else {
      LOG(FATAL) << "Opus not found in SDP generated by WebRTC.";
    }
  }
}

// Returns true if the RTC stats report indicates a relay connection. If the
// connection type cannot be determined (which should never happen with a valid
// RTCStatsReport), nullopt is returned.
base::Optional<bool> IsConnectionRelayed(
    const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  auto transport_stats_list =
      report->GetStatsOfType<webrtc::RTCTransportStats>();
  if (transport_stats_list.size() != 1) {
    LOG(ERROR) << "Unexpected number of transport stats: "
               << transport_stats_list.size();
    return base::nullopt;
  }
  std::string selected_candidate_pair_id =
      *(transport_stats_list[0]->selected_candidate_pair_id);
  const webrtc::RTCStats* selected_candidate_pair =
      report->Get(selected_candidate_pair_id);
  if (!selected_candidate_pair) {
    LOG(ERROR) << "Expected to find RTC stats for id: "
               << selected_candidate_pair;
    return base::nullopt;
  }
  std::string local_candidate_id =
      *(selected_candidate_pair->cast_to<webrtc::RTCIceCandidatePairStats>()
            .local_candidate_id);
  const webrtc::RTCStats* local_candidate = report->Get(local_candidate_id);
  if (!local_candidate) {
    LOG(ERROR) << "Expected to find RTC stats for id: " << local_candidate_id;
    return base::nullopt;
  }
  std::string local_candidate_type =
      *(local_candidate->cast_to<webrtc::RTCLocalIceCandidateStats>()
            .candidate_type);
  std::string remote_candidate_id =
      *(selected_candidate_pair->cast_to<webrtc::RTCIceCandidatePairStats>()
            .remote_candidate_id);
  const webrtc::RTCStats* remote_candidate = report->Get(remote_candidate_id);
  if (!remote_candidate) {
    LOG(ERROR) << "Expected to find RTC stats for id: " << remote_candidate_id;
    return base::nullopt;
  }
  std::string remote_candidate_type =
      *(remote_candidate->cast_to<webrtc::RTCRemoteIceCandidateStats>()
            .candidate_type);

  return local_candidate_type == "relay" || remote_candidate_type == "relay";
}

// A webrtc::CreateSessionDescriptionObserver implementation used to receive the
// results of creating descriptions for this end of the PeerConnection.
class CreateSessionDescriptionObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  typedef base::Callback<void(
      std::unique_ptr<webrtc::SessionDescriptionInterface> description,
      const std::string& error)>
      ResultCallback;

  static CreateSessionDescriptionObserver* Create(
      const ResultCallback& result_callback) {
    return new rtc::RefCountedObject<CreateSessionDescriptionObserver>(
        result_callback);
  }
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    std::move(result_callback_).Run(base::WrapUnique(desc), std::string());
  }
  void OnFailure(const std::string& error) override {
    std::move(result_callback_).Run(nullptr, error);
  }

 protected:
  explicit CreateSessionDescriptionObserver(
      const ResultCallback& result_callback)
      : result_callback_(result_callback) {}
  ~CreateSessionDescriptionObserver() override = default;

 private:
  ResultCallback result_callback_;

  DISALLOW_COPY_AND_ASSIGN(CreateSessionDescriptionObserver);
};

// A webrtc::SetSessionDescriptionObserver implementation used to receive the
// results of setting local and remote descriptions of the PeerConnection.
class SetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  typedef base::Callback<void(bool success, const std::string& error)>
      ResultCallback;

  static SetSessionDescriptionObserver* Create(
      const ResultCallback& result_callback) {
    return new rtc::RefCountedObject<SetSessionDescriptionObserver>(
        result_callback);
  }

  void OnSuccess() override {
    std::move(result_callback_).Run(true, std::string());
  }

  void OnFailure(const std::string& error) override {
    std::move(result_callback_).Run(false, error);
  }

 protected:
  explicit SetSessionDescriptionObserver(const ResultCallback& result_callback)
      : result_callback_(result_callback) {}
  ~SetSessionDescriptionObserver() override = default;

 private:
  ResultCallback result_callback_;

  DISALLOW_COPY_AND_ASSIGN(SetSessionDescriptionObserver);
};

class RTCStatsCollectorCallback : public webrtc::RTCStatsCollectorCallback {
 public:
  typedef base::RepeatingCallback<void(
      const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report)>
      ResultCallback;

  static RTCStatsCollectorCallback* Create(
      const ResultCallback& result_callback) {
    return new rtc::RefCountedObject<RTCStatsCollectorCallback>(
        result_callback);
  }

  void OnStatsDelivered(
      const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    std::move(result_callback_).Run(report);
  }

 protected:
  explicit RTCStatsCollectorCallback(const ResultCallback& result_callback)
      : result_callback_(result_callback) {}
  ~RTCStatsCollectorCallback() override = default;

 private:
  ResultCallback result_callback_;

  DISALLOW_COPY_AND_ASSIGN(RTCStatsCollectorCallback);
};

}  // namespace

class WebrtcTransport::PeerConnectionWrapper
    : public webrtc::PeerConnectionObserver {
 public:
  PeerConnectionWrapper(
      rtc::Thread* worker_thread,
      std::unique_ptr<webrtc::VideoEncoderFactory> encoder_factory,
      std::unique_ptr<cricket::PortAllocator> port_allocator,
      base::WeakPtr<WebrtcTransport> transport)
      : transport_(transport) {
    audio_module_ = new rtc::RefCountedObject<WebrtcAudioModule>();

    webrtc::PeerConnectionFactoryDependencies pcf_deps;
    pcf_deps.network_thread = worker_thread;
    pcf_deps.worker_thread = worker_thread;
    pcf_deps.signaling_thread = rtc::Thread::Current();
    pcf_deps.task_queue_factory = CreateWebRtcTaskQueueFactory();
    pcf_deps.call_factory = webrtc::CreateCallFactory();
    pcf_deps.event_log_factory = std::make_unique<webrtc::RtcEventLogFactory>(
        pcf_deps.task_queue_factory.get());
    cricket::MediaEngineDependencies media_deps;
    media_deps.task_queue_factory = pcf_deps.task_queue_factory.get();
    media_deps.adm = audio_module_;
    media_deps.audio_encoder_factory =
        webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
    media_deps.audio_decoder_factory =
        webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
    media_deps.video_encoder_factory = std::move(encoder_factory);
    media_deps.video_decoder_factory =
        webrtc::CreateBuiltinVideoDecoderFactory();
    media_deps.audio_processing = webrtc::AudioProcessingBuilder().Create();
    pcf_deps.media_engine = cricket::CreateMediaEngine(std::move(media_deps));
    peer_connection_factory_ =
        webrtc::CreateModularPeerConnectionFactory(std::move(pcf_deps));

    webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
    rtc_config.enable_dtls_srtp = true;

    // Set bundle_policy and rtcp_mux_policy to ensure that all channels are
    // multiplexed over a single channel.
    rtc_config.bundle_policy =
        webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    rtc_config.rtcp_mux_policy =
        webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;

    rtc_config.media_config.video.periodic_alr_bandwidth_probing = true;

    rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

    peer_connection_ = peer_connection_factory_->CreatePeerConnection(
        rtc_config, std::move(port_allocator), nullptr, this);
  }

  ~PeerConnectionWrapper() override {
    // PeerConnection creates threads internally, which are joined when the
    // connection is closed. See crbug.com/660081.
    ScopedAllowThreadJoinForWebRtcTransport allow_thread_join;
    peer_connection_->Close();
    peer_connection_ = nullptr;
    peer_connection_factory_ = nullptr;
    audio_module_ = nullptr;
  }

  WebrtcAudioModule* audio_module() {
    return audio_module_.get();
  }

  webrtc::PeerConnectionInterface* peer_connection() {
    return peer_connection_.get();
  }

  webrtc::PeerConnectionFactoryInterface* peer_connection_factory() {
    return peer_connection_factory_.get();
  }

  // webrtc::PeerConnectionObserver interface.
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {
    if (transport_)
      transport_->OnSignalingChange(new_state);
  }
  void OnAddStream(
      rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
    if (transport_)
      transport_->OnAddStream(stream);
  }
  void OnRemoveStream(
      rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
    if (transport_)
      transport_->OnRemoveStream(stream);
  }
  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
    if (transport_)
      transport_->OnDataChannel(data_channel);
  }
  void OnRenegotiationNeeded() override {
    if (transport_)
      transport_->OnRenegotiationNeeded();
  }
  void OnIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
    if (transport_)
      transport_->OnIceConnectionChange(new_state);
  }
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
    if (transport_)
      transport_->OnIceGatheringChange(new_state);
  }
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
    if (transport_)
      transport_->OnIceCandidate(candidate);
  }

 private:
  rtc::scoped_refptr<WebrtcAudioModule> audio_module_;
  scoped_refptr<webrtc::PeerConnectionFactoryInterface>
      peer_connection_factory_;
  scoped_refptr<webrtc::PeerConnectionInterface> peer_connection_;

  base::WeakPtr<WebrtcTransport> transport_;

  DISALLOW_COPY_AND_ASSIGN(PeerConnectionWrapper);
};

WebrtcTransport::WebrtcTransport(
    rtc::Thread* worker_thread,
    scoped_refptr<TransportContext> transport_context,
    EventHandler* event_handler)
    : transport_context_(transport_context),
      event_handler_(event_handler),
      handshake_hmac_(crypto::HMAC::SHA256),
      weak_factory_(this) {
  transport_context_->set_relay_mode(TransportContext::RelayMode::TURN);

  video_encoder_factory_ = new WebrtcDummyVideoEncoderFactory();
  std::unique_ptr<cricket::PortAllocator> port_allocator =
      transport_context_->port_allocator_factory()->CreatePortAllocator(
          transport_context_);

  // Takes ownership of video_encoder_factory_.
  peer_connection_wrapper_.reset(new PeerConnectionWrapper(
      worker_thread, base::WrapUnique(video_encoder_factory_),
      std::move(port_allocator), weak_factory_.GetWeakPtr()));
}

WebrtcTransport::~WebrtcTransport() {
  DCHECK(thread_checker_.CalledOnValidThread());
  Close(OK);
}

webrtc::PeerConnectionInterface* WebrtcTransport::peer_connection() {
  return peer_connection_wrapper_ ? peer_connection_wrapper_->peer_connection()
                                  : nullptr;
}

webrtc::PeerConnectionFactoryInterface*
WebrtcTransport::peer_connection_factory() {
  return peer_connection_wrapper_
             ? peer_connection_wrapper_->peer_connection_factory()
             : nullptr;
}

WebrtcAudioModule* WebrtcTransport::audio_module() {
  return peer_connection_wrapper_
             ? peer_connection_wrapper_->audio_module()
             : nullptr;
}

std::unique_ptr<MessagePipe> WebrtcTransport::CreateOutgoingChannel(
    const std::string& name) {
  webrtc::DataChannelInit config;
  config.reliable = true;
  return std::make_unique<WebrtcDataStreamAdapter>(
      peer_connection()->CreateDataChannel(name, &config));
}

void WebrtcTransport::Start(
    Authenticator* authenticator,
    SendTransportInfoCallback send_transport_info_callback) {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(send_transport_info_callback_.is_null());

  jingle_glue::JingleThreadWrapper::EnsureForCurrentMessageLoop();

  // TODO(sergeyu): Investigate if it's possible to avoid Send().
  jingle_glue::JingleThreadWrapper::current()->set_send_allowed(true);

  send_transport_info_callback_ = std::move(send_transport_info_callback);

  if (!handshake_hmac_.Init(authenticator->GetAuthKey())) {
    LOG(FATAL) << "HMAC::Init() failed.";
  }

  event_handler_->OnWebrtcTransportConnecting();

  if (transport_context_->role() == TransportRole::SERVER)
    RequestNegotiation();
}

bool WebrtcTransport::ProcessTransportInfo(XmlElement* transport_info) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (transport_info->Name() != QName(kTransportNamespace, "transport"))
    return false;

  if (!peer_connection())
    return false;

  XmlElement* session_description = transport_info->FirstNamed(
      QName(kTransportNamespace, "session-description"));
  if (session_description) {
    webrtc::PeerConnectionInterface::SignalingState expected_state =
        transport_context_->role() == TransportRole::CLIENT
            ? webrtc::PeerConnectionInterface::kStable
            : webrtc::PeerConnectionInterface::kHaveLocalOffer;
    if (peer_connection()->signaling_state() != expected_state) {
      LOG(ERROR) << "Received unexpected WebRTC session_description.";
      return false;
    }

    std::string type = session_description->Attr(QName(std::string(), "type"));
    std::string raw_sdp = session_description->BodyText();
    if (!IsValidSessionDescriptionType(type) || raw_sdp.empty()) {
      LOG(ERROR) << "Incorrect session description format.";
      return false;
    }

    SdpMessage sdp_message(raw_sdp);

    std::string signature_base64 =
        session_description->Attr(QName(std::string(), "signature"));
    std::string signature;
    if (!base::Base64Decode(signature_base64, &signature) ||
        !handshake_hmac_.Verify(
            type + " " + sdp_message.NormalizedForSignature(), signature)) {
      LOG(WARNING) << "Received session-description with invalid signature.";
      bool ignore_error = false;
#if !defined(NDEBUG)
      ignore_error = base::CommandLine::ForCurrentProcess()->HasSwitch(
          kDisableAuthenticationSwitchName);
#endif
      if (!ignore_error) {
        Close(AUTHENTICATION_FAILED);
        return true;
      }
    }

    UpdateCodecParameters(&sdp_message, /*incoming=*/true);

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description(
        webrtc::CreateSessionDescription(type, sdp_message.ToString(), &error));
    if (!session_description) {
      LOG(ERROR) << "Failed to parse the session description: "
                 << error.description << " line: " << error.line;
      return false;
    }

    peer_connection()->SetRemoteDescription(
        SetSessionDescriptionObserver::Create(
            base::Bind(&WebrtcTransport::OnRemoteDescriptionSet,
                       weak_factory_.GetWeakPtr(),
                       type == webrtc::SessionDescriptionInterface::kOffer)),
        session_description.release());

    // SetRemoteDescription() might overwrite any bitrate caps previously set,
    // so (re)apply them here. This might happen if ICE state were already
    // connected and OnStatsDelivered() had already set the caps.
    SetPeerConnectionBitrates(MaxBitrateForConnection());
  }

  XmlElement* candidate_element;
  QName candidate_qname(kTransportNamespace, "candidate");
  for (candidate_element = transport_info->FirstNamed(candidate_qname);
       candidate_element;
       candidate_element = candidate_element->NextNamed(candidate_qname)) {
    std::string candidate_str = candidate_element->BodyText();
    std::string sdp_mid =
        candidate_element->Attr(QName(std::string(), "sdpMid"));
    std::string sdp_mlineindex_str =
        candidate_element->Attr(QName(std::string(), "sdpMLineIndex"));
    int sdp_mlineindex;
    if (candidate_str.empty() || sdp_mid.empty() ||
        !base::StringToInt(sdp_mlineindex_str, &sdp_mlineindex)) {
      LOG(ERROR) << "Failed to parse incoming candidates.";
      return false;
    }

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, candidate_str,
                                   &error));
    if (!candidate) {
      LOG(ERROR) << "Failed to parse incoming candidate: " << error.description
                 << " line: " << error.line;
      return false;
    }

    if (peer_connection()->signaling_state() ==
        webrtc::PeerConnectionInterface::kStable) {
      if (!peer_connection()->AddIceCandidate(candidate.get())) {
        LOG(ERROR) << "Failed to add incoming ICE candidate.";
        return false;
      }
    } else {
      pending_incoming_candidates_.push_back(std::move(candidate));
    }
  }

  return true;
}

void WebrtcTransport::Close(ErrorCode error) {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!peer_connection_wrapper_)
    return;

  weak_factory_.InvalidateWeakPtrs();

  // Close and delete PeerConnection asynchronously. PeerConnection may be on
  // the stack and so it must be destroyed later.
  base::ThreadTaskRunnerHandle::Get()->DeleteSoon(
      FROM_HERE, peer_connection_wrapper_.release());

  if (error != OK)
    event_handler_->OnWebrtcTransportError(error);
}

void WebrtcTransport::ApplySessionOptions(const SessionOptions& options) {
  base::Optional<std::string> video_codec = options.Get("Video-Codec");
  if (video_codec) {
    preferred_video_codec_ = *video_codec;
  }
}

void WebrtcTransport::OnAudioSenderCreated(
    rtc::scoped_refptr<webrtc::RtpSenderInterface> sender) {}

void WebrtcTransport::OnVideoSenderCreated(
    rtc::scoped_refptr<webrtc::RtpSenderInterface> sender) {
  // TODO(lambroslambrou): Store the VideoSender here, instead of looping over
  // all Senders in GetVideoSender().
  DCHECK_EQ(GetVideoSender(), sender);
  SetSenderBitrates(MaxBitrateForConnection());
}

void WebrtcTransport::OnLocalSessionDescriptionCreated(
    std::unique_ptr<webrtc::SessionDescriptionInterface> description,
    const std::string& error) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (!peer_connection())
    return;

  if (!description) {
    LOG(ERROR) << "PeerConnection offer creation failed: " << error;
    Close(CHANNEL_CONNECTION_ERROR);
    return;
  }

  std::string description_sdp;
  if (!description->ToString(&description_sdp)) {
    LOG(ERROR) << "Failed to serialize description.";
    Close(CHANNEL_CONNECTION_ERROR);
    return;
  }

  SdpMessage sdp_message(description_sdp);
  UpdateCodecParameters(&sdp_message, /*incoming=*/false);
  if (preferred_video_codec_.empty()) {
    sdp_message.PreferVideoCodec("VP8");
  } else {
    sdp_message.PreferVideoCodec(preferred_video_codec_);
  }
  description_sdp = sdp_message.ToString();
  webrtc::SdpParseError parse_error;
  description.reset(webrtc::CreateSessionDescription(
      description->type(), description_sdp, &parse_error));
  if (!description) {
    LOG(ERROR) << "Failed to parse the session description: "
               << parse_error.description << " line: " << parse_error.line;
    Close(CHANNEL_CONNECTION_ERROR);
    return;
  }

  // Format and send the session description to the peer.
  std::unique_ptr<XmlElement> transport_info(
      new XmlElement(QName(kTransportNamespace, "transport"), true));
  XmlElement* offer_tag =
      new XmlElement(QName(kTransportNamespace, "session-description"));
  transport_info->AddElement(offer_tag);
  offer_tag->SetAttr(QName(std::string(), "type"), description->type());
  offer_tag->SetBodyText(description_sdp);

  std::string digest;
  digest.resize(handshake_hmac_.DigestLength());
  CHECK(handshake_hmac_.Sign(
      description->type() + " " + sdp_message.NormalizedForSignature(),
      reinterpret_cast<uint8_t*>(&(digest[0])), digest.size()));
  std::string digest_base64;
  base::Base64Encode(digest, &digest_base64);
  offer_tag->SetAttr(QName(std::string(), "signature"), digest_base64);

  send_transport_info_callback_.Run(std::move(transport_info));

  peer_connection()->SetLocalDescription(
      SetSessionDescriptionObserver::Create(base::Bind(
          &WebrtcTransport::OnLocalDescriptionSet, weak_factory_.GetWeakPtr())),
      description.release());
}

void WebrtcTransport::OnLocalDescriptionSet(bool success,
                                            const std::string& error) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (!peer_connection())
    return;

  if (!success) {
    LOG(ERROR) << "Failed to set local description: " << error;
    Close(CHANNEL_CONNECTION_ERROR);
    return;
  }

  AddPendingCandidatesIfPossible();
}

void WebrtcTransport::OnRemoteDescriptionSet(bool send_answer,
                                             bool success,
                                             const std::string& error) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (!peer_connection())
    return;

  if (!success) {
    LOG(ERROR) << "Failed to set local description: " << error;
    Close(CHANNEL_CONNECTION_ERROR);
    return;
  }

  // Create and send answer on the server.
  if (send_answer) {
    const webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    peer_connection()->CreateAnswer(
        CreateSessionDescriptionObserver::Create(
            base::Bind(&WebrtcTransport::OnLocalSessionDescriptionCreated,
                       weak_factory_.GetWeakPtr())),
        options);
  }

  AddPendingCandidatesIfPossible();
}

void WebrtcTransport::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  DCHECK(thread_checker_.CalledOnValidThread());
}

void WebrtcTransport::OnAddStream(
    rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
  DCHECK(thread_checker_.CalledOnValidThread());
  event_handler_->OnWebrtcTransportMediaStreamAdded(stream.get());
}

void WebrtcTransport::OnRemoveStream(
    rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
  DCHECK(thread_checker_.CalledOnValidThread());
  event_handler_->OnWebrtcTransportMediaStreamRemoved(stream.get());
}

void WebrtcTransport::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  DCHECK(thread_checker_.CalledOnValidThread());
  event_handler_->OnWebrtcTransportIncomingDataChannel(
      data_channel->label(),
      std::make_unique<WebrtcDataStreamAdapter>(data_channel));
}

void WebrtcTransport::OnRenegotiationNeeded() {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (transport_context_->role() == TransportRole::SERVER) {
    RequestNegotiation();
  } else {
    // TODO(sergeyu): Is it necessary to support renegotiation initiated by the
    // client?
    NOTIMPLEMENTED();
  }
}

void WebrtcTransport::OnIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (!connected_ &&
      new_state == webrtc::PeerConnectionInterface::kIceConnectionConnected) {
    connected_ = true;
    connection_relayed_.reset();
    event_handler_->OnWebrtcTransportConnected();

    // Request RTC statistics, to determine if the connection is direct or
    // relayed.
    RequestRtcStats();
  } else if (connected_ &&
             new_state ==
                 webrtc::PeerConnectionInterface::kIceConnectionDisconnected &&
             transport_context_->role() == TransportRole::SERVER) {
    connected_ = false;
    want_ice_restart_ = true;
    RequestNegotiation();
  }
}

void WebrtcTransport::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  DCHECK(thread_checker_.CalledOnValidThread());
}

void WebrtcTransport::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  DCHECK(thread_checker_.CalledOnValidThread());

  std::unique_ptr<XmlElement> candidate_element(
      new XmlElement(QName(kTransportNamespace, "candidate")));
  std::string candidate_str;
  if (!candidate->ToString(&candidate_str)) {
    LOG(ERROR) << "Failed to serialize local candidate.";
    return;
  }
  candidate_element->SetBodyText(candidate_str);
  candidate_element->SetAttr(QName(std::string(), "sdpMid"),
                             candidate->sdp_mid());
  candidate_element->SetAttr(
      QName(std::string(), "sdpMLineIndex"),
      base::NumberToString(candidate->sdp_mline_index()));

  EnsurePendingTransportInfoMessage();
  pending_transport_info_message_->AddElement(candidate_element.release());
}

void WebrtcTransport::OnStatsDelivered(
    const rtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
  if (!connected_)
    return;

  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&WebrtcTransport::RequestRtcStats,
                     weak_factory_.GetWeakPtr()),
      kRtcStatsPollingInterval);

  base::Optional<bool> connection_relayed = IsConnectionRelayed(report);
  if (connection_relayed == connection_relayed_) {
    // No change in connection type. Unknown -> direct/relayed is treated as a
    // change, so the correct initial bitrate caps are set.
    return;
  }

  connection_relayed_ = connection_relayed;
  if (connection_relayed_.has_value()) {
    VLOG(0) << "Relay connection: "
            << (connection_relayed_.value() ? "true" : "false");
  } else {
    LOG(ERROR) << "Connection type unknown, treating as direct.";
  }

  // The max-bitrate needs to be applied even for direct (non-TURN) connections.
  // Otherwise the video-sender b/w estimate is capped to a low default value
  // (~600kbps).
  // Set the global bitrate caps in addition to the VideoSender bitrates. The
  // global caps affect the probing configuration used by b/w estimator.
  // Setting min bitrate here enables padding.
  //
  // TODO(sergeyu): Padding needs to be enabled to workaround b/w estimator not
  // handling spiky traffic patterns well. This won't be necessary with a
  // better bandwidth estimator.
  int max_bitrate_bps = MaxBitrateForConnection();
  SetPeerConnectionBitrates(max_bitrate_bps);
  SetSenderBitrates(max_bitrate_bps);
}

int WebrtcTransport::MaxBitrateForConnection() {
  int max_bitrate_bps = kMaxBitrateBps;
  if (connection_relayed_.value_or(false)) {
    int turn_max_rate_kbps = transport_context_->GetTurnMaxRateKbps();
    if (turn_max_rate_kbps <= 0) {
      VLOG(0) << "No TURN bitrate cap set.";
    } else {
      // Apply the TURN bitrate cap to prevent large amounts of packet loss.
      // The Google TURN/relay server limits the connection speed by dropping
      // packets, which may interact badly with WebRTC's bandwidth-estimation.
      VLOG(0) << "Capping bitrate to " << turn_max_rate_kbps << "kbps.";
      max_bitrate_bps = turn_max_rate_kbps * 1000;
    }
  }
  return max_bitrate_bps;
}

void WebrtcTransport::SetPeerConnectionBitrates(int max_bitrate_bps) {
  webrtc::BitrateSettings bitrate;
  bitrate.min_bitrate_bps = kMinBitrateBps;
  bitrate.max_bitrate_bps = max_bitrate_bps;
  peer_connection()->SetBitrate(bitrate);
}

void WebrtcTransport::SetSenderBitrates(int max_bitrate_bps) {
  // Only set the cap on the VideoSender, because the AudioSender (via the
  // Opus codec) is already configured with a lower bitrate.
  rtc::scoped_refptr<webrtc::RtpSenderInterface> sender = GetVideoSender();
  if (!sender) {
    LOG(ERROR) << "Video sender not found.";
    return;
  }

  webrtc::RtpParameters parameters = sender->GetParameters();
  if (parameters.encodings.empty()) {
    LOG(ERROR) << "No encodings found for sender " << sender->id();
    return;
  }

  if (parameters.encodings.size() != 1) {
    LOG(ERROR) << "Unexpected number of encodings ("
               << parameters.encodings.size() << ") for sender "
               << sender->id();
  }

  parameters.encodings[0].min_bitrate_bps = kMinBitrateBps;
  parameters.encodings[0].max_bitrate_bps = max_bitrate_bps;
  webrtc::RTCError result = sender->SetParameters(parameters);
  DCHECK(result.ok()) << "SetParameters() failed: " << result.message();
}

void WebrtcTransport::RequestRtcStats() {
  if (!connected_)
    return;

  peer_connection()->GetStats(
      RTCStatsCollectorCallback::Create(base::BindRepeating(
          &WebrtcTransport::OnStatsDelivered, weak_factory_.GetWeakPtr())));
}

void WebrtcTransport::RequestNegotiation() {
  DCHECK(transport_context_->role() == TransportRole::SERVER);

  if (!negotiation_pending_) {
    negotiation_pending_ = true;
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(&WebrtcTransport::SendOffer,
                                  weak_factory_.GetWeakPtr()));
  }
}

void WebrtcTransport::SendOffer() {
  DCHECK(transport_context_->role() == TransportRole::SERVER);

  DCHECK(negotiation_pending_);
  negotiation_pending_ = false;

  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
  options.offer_to_receive_video = true;
  options.offer_to_receive_audio = false;
  options.ice_restart = want_ice_restart_;
  peer_connection()->CreateOffer(
      CreateSessionDescriptionObserver::Create(base::BindRepeating(
          &WebrtcTransport::OnLocalSessionDescriptionCreated,
          weak_factory_.GetWeakPtr())),
      options);
}

void WebrtcTransport::EnsurePendingTransportInfoMessage() {
  DCHECK(thread_checker_.CalledOnValidThread());

  // |transport_info_timer_| must be running iff
  // |pending_transport_info_message_| exists.
  DCHECK_EQ(pending_transport_info_message_ != nullptr,
            transport_info_timer_.IsRunning());

  if (!pending_transport_info_message_) {
    pending_transport_info_message_.reset(
        new XmlElement(QName(kTransportNamespace, "transport"), true));

    // Delay sending the new candidates in case we get more candidates
    // that we can send in one message.
    transport_info_timer_.Start(
        FROM_HERE, base::TimeDelta::FromMilliseconds(kTransportInfoSendDelayMs),
        this, &WebrtcTransport::SendTransportInfo);
  }
}

void WebrtcTransport::SendTransportInfo() {
  DCHECK(thread_checker_.CalledOnValidThread());
  DCHECK(pending_transport_info_message_);

  send_transport_info_callback_.Run(std::move(pending_transport_info_message_));
}

void WebrtcTransport::AddPendingCandidatesIfPossible() {
  DCHECK(thread_checker_.CalledOnValidThread());

  if (peer_connection()->signaling_state() ==
      webrtc::PeerConnectionInterface::kStable) {
    for (const auto& candidate : pending_incoming_candidates_) {
      if (!peer_connection()->AddIceCandidate(candidate.get())) {
        LOG(ERROR) << "Failed to add incoming candidate";
        Close(INCOMPATIBLE_PROTOCOL);
        return;
      }
    }
    pending_incoming_candidates_.clear();
  }
}

rtc::scoped_refptr<webrtc::RtpSenderInterface>
WebrtcTransport::GetVideoSender() {
  auto senders = peer_connection()->GetSenders();
  for (rtc::scoped_refptr<webrtc::RtpSenderInterface> sender : senders) {
    if (sender->media_type() == cricket::MediaType::MEDIA_TYPE_VIDEO) {
      return sender;
    }
  }
  return nullptr;
}

}  // namespace protocol
}  // namespace remoting
