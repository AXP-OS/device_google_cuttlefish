/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "host/frontend/webrtc/lib/client_handler.h"

#include <optional>
#include <vector>

#include <json/json.h>
#include <netdb.h>
#include <openssl/rand.h>

#include <android-base/logging.h>

#include "common/libs/utils/base64.h"
#include "host/frontend/webrtc/lib/keyboard.h"
#include "host/libs/config/cuttlefish_config.h"
#include "https/SafeCallbackable.h"

namespace cuttlefish {
namespace webrtc_streaming {

namespace {

static constexpr auto kInputChannelLabel = "input-channel";

class ValidationResult {
 public:
  ValidationResult() = default;
  ValidationResult(const std::string &error) : error_(error) {}

  bool ok() const { return !error_.has_value(); }
  std::string error() const { return error_.value_or(""); }

 private:
  std::optional<std::string> error_;
};

// helper method to ensure a json object has the required fields convertible
// to the appropriate types.
ValidationResult validateJsonObject(
    const Json::Value &obj, const std::string &type,
    const std::map<std::string, Json::ValueType> &fields) {
  for (const auto &field_spec : fields) {
    const auto &field_name = field_spec.first;
    auto field_type = field_spec.second;
    if (!(obj.isMember(field_name) &&
          obj[field_name].isConvertibleTo(field_type))) {
      std::string error_msg = "Expected a field named '";
      error_msg += field_name + "' of type '";
      error_msg += std::to_string(field_type);
      error_msg += "'";
      if (!type.empty()) {
        error_msg += " in message of type '" + type + "'";
      }
      error_msg += ".";
      return {error_msg};
    }
  }
  return {};
}

class MyCreateSessionDescriptionObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  MyCreateSessionDescriptionObserver(
      std::weak_ptr<ClientHandler> client_handler)
      : client_handler_(client_handler) {}

  void OnSuccess(webrtc::SessionDescriptionInterface *desc) override {
    auto client_handler = client_handler_.lock();
    if (client_handler) {
      client_handler->OnCreateSDPSuccess(desc);
    }
  }
  void OnFailure(webrtc::RTCError error) override {
    auto client_handler = client_handler_.lock();
    if (client_handler) {
      client_handler->OnCreateSDPFailure(error);
    }
  }

 private:
  std::weak_ptr<ClientHandler> client_handler_;
};

class MySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  MySetSessionDescriptionObserver(std::weak_ptr<ClientHandler> client_handler)
      : client_handler_(client_handler) {}

  void OnSuccess() override {
    // local description set, nothing else to do
  }
  void OnFailure(webrtc::RTCError error) override {
    auto client_handler = client_handler_.lock();
    if (client_handler) {
      client_handler->OnSetSDPFailure(error);
    }
  }

 private:
  std::weak_ptr<ClientHandler> client_handler_;
};

class MyOnSetRemoteDescription
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  MyOnSetRemoteDescription(std::function<void(webrtc::RTCError error)> on_error)
      : on_error_(on_error) {}

  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    on_error_(error);
  }

 private:
  std::function<void(webrtc::RTCError error)> on_error_;
};

}  // namespace

ClientHandler::InputHandler::InputHandler(
    rtc::scoped_refptr<webrtc::DataChannelInterface> input_channel,
    std::shared_ptr<ConnectionObserver> observer)
    : input_channel_(input_channel), observer_(observer) {
  input_channel->RegisterObserver(this);
}
ClientHandler::InputHandler::~InputHandler() {
  input_channel_->UnregisterObserver();
}
void ClientHandler::InputHandler::OnStateChange() {
  LOG(VERBOSE) << "Input channel state changed to "
               << webrtc::DataChannelInterface::DataStateString(
                      input_channel_->state());
}

void ClientHandler::InputHandler::OnMessage(const webrtc::DataBuffer &msg) {
  if (msg.binary) {
    // TODO (jemoreira) consider binary protocol to avoid JSON parsing overhead
    LOG(ERROR) << "Received invalid (binary) data on input channel";
    return;
  }
  auto size = msg.size();

  Json::Value evt;
  Json::Reader json_reader;
  auto str = reinterpret_cast<const char *>(msg.data.cdata());
  if (!json_reader.parse(str, str + size, evt) < 0) {
    LOG(ERROR) << "Received invalid JSON object over input channel";
    return;
  }
  if (!evt.isMember("type") || !evt["type"].isString()) {
    LOG(ERROR) << "Input event doesn't have a valid 'type' field: "
               << evt.toStyledString();
    return;
  }
  auto event_type = evt["type"].asString();
  if (event_type == "mouse") {
    auto result =
        validateJsonObject(evt, "mouse",
                           {{"down", Json::ValueType::intValue},
                            {"x", Json::ValueType::intValue},
                            {"y", Json::ValueType::intValue},
                            {"display_label", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LOG(ERROR) << result.error();
      return;
    }
    auto label = evt["display_label"].asString();
    int32_t down = evt["down"].asInt();
    int32_t x = evt["x"].asInt();
    int32_t y = evt["y"].asInt();

    observer_->OnTouchEvent(label, x, y, down);
  } else if (event_type == "multi-touch") {
    auto result =
        validateJsonObject(evt, "multi-touch",
                           {{"id", Json::ValueType::intValue},
                            {"initialDown", Json::ValueType::intValue},
                            {"x", Json::ValueType::intValue},
                            {"y", Json::ValueType::intValue},
                            {"slot", Json::ValueType::intValue},
                            {"display_label", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LOG(ERROR) << result.error();
      return;
    }
    auto label = evt["display_label"].asString();
    int32_t id = evt["id"].asInt();
    int32_t initialDown = evt["initialDown"].asInt();
    int32_t x = evt["x"].asInt();
    int32_t y = evt["y"].asInt();
    int32_t slot = evt["slot"].asInt();

    observer_->OnMultiTouchEvent(label, id, slot, x, y, initialDown);
  } else if (event_type == "keyboard") {
    auto result =
        validateJsonObject(evt, "keyboard",
                           {{"event_type", Json::ValueType::stringValue},
                            {"keycode", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LOG(ERROR) << result.error();
      return;
    }
    auto down = evt["event_type"].asString() == std::string("keydown");
    auto code = DomKeyCodeToLinux(evt["keycode"].asString());
    observer_->OnKeyboardEvent(code, down);
  } else {
    LOG(ERROR) << "Unrecognized event type: " << event_type;
    return;
  }
}

std::shared_ptr<ClientHandler> ClientHandler::Create(
    int client_id, std::shared_ptr<ConnectionObserver> observer,
    std::function<void(const Json::Value &)> send_to_client_cb,
    std::function<void()> on_connection_closed_cb) {
  return std::shared_ptr<ClientHandler>(new ClientHandler(
      client_id, observer, send_to_client_cb, on_connection_closed_cb));
}

ClientHandler::ClientHandler(
    int client_id, std::shared_ptr<ConnectionObserver> observer,
    std::function<void(const Json::Value &)> send_to_client_cb,
    std::function<void()> on_connection_closed_cb)
    : client_id_(client_id),
      observer_(observer),
      send_to_client_(send_to_client_cb),
      on_connection_closed_cb_(on_connection_closed_cb) {}

ClientHandler::~ClientHandler() {
  for (auto &data_channel : data_channels_) {
    data_channel->UnregisterObserver();
  }
}

bool ClientHandler::SetPeerConnection(
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection) {
  peer_connection_ = peer_connection;

  // If no channel is created on the peer connection the generated offer won't
  // have an entry for data channels which breaks input and adb.
  // This channel has no use now, but could be used in the future to exchange
  // control data between client and device without going through the signaling
  // server.
  auto control_channel = peer_connection_->CreateDataChannel(
      "device-control", nullptr /* config */);
  if (!control_channel) {
    LOG(ERROR) << "Failed to create control data channel";
    return false;
  }
  return true;
}

bool ClientHandler::AddDisplay(
    rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track,
    const std::string &label) {
  // Send each track as part of a different stream with the label as id
  auto err_or_sender =
      peer_connection_->AddTrack(video_track, {label} /* stream_id */);
  if (!err_or_sender.ok()) {
    LOG(ERROR) << "Failed to add video track to the peer connection";
    return false;
  }
  // TODO (b/154138394): use the returned sender (err_or_sender.value()) to
  // remove the display from the connection.
  return true;
}

void ClientHandler::LogAndReplyError(const std::string &error_msg) const {
  LOG(ERROR) << error_msg;
  Json::Value reply;
  reply["type"] = "error";
  reply["error"] = error_msg;
  send_to_client_(reply);
}

void ClientHandler::OnCreateSDPSuccess(
    webrtc::SessionDescriptionInterface *desc) {
  std::string offer_str;
  desc->ToString(&offer_str);
  peer_connection_->SetLocalDescription(
      // The peer connection wraps this raw pointer with a scoped_refptr, so
      // it's guaranteed to be deleted at some point
      new rtc::RefCountedObject<MySetSessionDescriptionObserver>(
          weak_from_this()),
      desc);
  // The peer connection takes ownership of the description so it should not be
  // used after this
  desc = nullptr;

  Json::Value reply;
  reply["type"] = "offer";
  reply["sdp"] = offer_str;

  send_to_client_(reply);
}

void ClientHandler::OnCreateSDPFailure(webrtc::RTCError error) {
  LogAndReplyError(error.message());
  Close();
}

void ClientHandler::OnSetSDPFailure(webrtc::RTCError error) {
  LogAndReplyError(error.message());
  LOG(ERROR) << "Error setting local description: Either there is a bug in "
                "libwebrtc or the local description was (incorrectly) modified "
                "after creating it";
  Close();
}

void ClientHandler::HandleMessage(const Json::Value &message) {
  {
    auto result = validateJsonObject(message, "",
                                     {{"type", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LogAndReplyError(result.error());
      return;
    }
  }
  auto type = message["type"].asString();
  if (type == "request-offer") {
    peer_connection_->CreateOffer(
        // No memory leak here because this is a ref counted objects and the
        // peer connection immediately wraps it with a scoped_refptr
        new rtc::RefCountedObject<MyCreateSessionDescriptionObserver>(
            weak_from_this()),
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    // The created offer wil be sent to the client on
    // OnSuccess(webrtc::SessionDescriptionInterface* desc)
  } else if (type == "answer") {
    auto result = validateJsonObject(message, type,
                                     {{"sdp", Json::ValueType::stringValue}});
    if (!result.ok()) {
      LogAndReplyError(result.error());
      return;
    }
    auto remote_desc_str = message["sdp"].asString();
    auto remote_desc = webrtc::CreateSessionDescription(
        webrtc::SdpType::kAnswer, remote_desc_str, nullptr /*error*/);
    if (!remote_desc) {
      LogAndReplyError("Failed to parse answer.");
      return;
    }
    rtc::scoped_refptr<webrtc::SetRemoteDescriptionObserverInterface> observer(
        new rtc::RefCountedObject<MyOnSetRemoteDescription>(
            [this](webrtc::RTCError error) {
              if (!error.ok()) {
                LogAndReplyError(error.message());
                // The remote description was rejected, this client can't be
                // trusted anymore.
                Close();
              }
            }));
    peer_connection_->SetRemoteDescription(std::move(remote_desc), observer);

  } else if (type == "ice-candidate") {
    {
      auto result = validateJsonObject(
          message, type, {{"candidate", Json::ValueType::objectValue}});
      if (!result.ok()) {
        LogAndReplyError(result.error());
        return;
      }
    }
    auto candidate_json = message["candidate"];
    {
      auto result =
          validateJsonObject(candidate_json, "ice-candidate/candidate",
                             {
                                 {"sdpMid", Json::ValueType::stringValue},
                                 {"candidate", Json::ValueType::stringValue},
                                 {"sdpMLineIndex", Json::ValueType::intValue},
                             });
      if (!result.ok()) {
        LogAndReplyError(result.error());
        return;
      }
    }
    auto mid = candidate_json["sdpMid"].asString();
    auto candidate_sdp = candidate_json["candidate"].asString();
    auto line_index = candidate_json["sdpMLineIndex"].asInt();

    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(mid, line_index, candidate_sdp,
                                   nullptr /*error*/));
    if (!candidate) {
      LogAndReplyError("Failed to parse ICE candidate");
      return;
    }
    peer_connection_->AddIceCandidate(std::move(candidate),
                                      [this](webrtc::RTCError error) {
                                        if (!error.ok()) {
                                          LogAndReplyError(error.message());
                                        }
                                      });
  } else if (type == "adb-message") {
    {
      auto result = validateJsonObject(
          message, type, {{"payload", Json::ValueType::stringValue}});
      if (!result.ok()) {
        LogAndReplyError(result.error());
        return;
      }
    }
    auto base64_msg = message["payload"].asString();
    std::vector<uint8_t> raw_msg;
    if (!cuttlefish::DecodeBase64(base64_msg, &raw_msg)) {
      LOG(ERROR) << "Invalid base64 string in adb-message";
      return;
    }
    observer_->OnAdbMessage(raw_msg.data(), raw_msg.size());
  } else {
    LogAndReplyError("Unknown client message type: " + type);
    return;
  }
}

void ClientHandler::Close() {
  // We can't simply call peer_connection_->Close() here because this method
  // could be called from one of the PeerConnectionObserver callbacks and that
  // would lead to a deadlock (Close eventually tries to destroy an object that
  // will then wait for the callback to return -> deadlock). Destroying the
  // peer_connection_ has the same effect. The only alternative is to postpone
  // that operation until after the callback returns.
  on_connection_closed_cb_();
}

void ClientHandler::OnConnectionChange(
    webrtc::PeerConnectionInterface::PeerConnectionState new_state) {
  switch (new_state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
      LOG(VERBOSE) << "Client " << client_id_ << ": WebRTC connected";
      observer_->OnConnected();
      observer_->OnAdbChannelOpen([this](const uint8_t *msg, size_t size) {
        std::string base64_msg;
        cuttlefish::EncodeBase64(msg, size, &base64_msg);
        Json::Value reply;
        reply["type"] = "adb-message";
        reply["payload"] = base64_msg;
        send_to_client_(reply);
        return true;
      });
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
      LOG(VERBOSE) << "Client " << client_id_ << ": Connection disconnected";
      Close();
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
      LOG(ERROR) << "Client " << client_id_ << ": Connection failed";
      Close();
      break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
      LOG(VERBOSE) << "Client " << client_id_ << ": Connection closed";
      Close();
      break;
  }
}

void ClientHandler::OnIceCandidate(
    const webrtc::IceCandidateInterface *candidate) {
  std::string candidate_sdp;
  candidate->ToString(&candidate_sdp);
  auto sdp_mid = candidate->sdp_mid();
  auto line_index = candidate->sdp_mline_index();

  Json::Value reply;
  reply["type"] = "ice-candidate";
  reply["mid"] = sdp_mid;
  reply["mLineIndex"] = static_cast<Json::UInt64>(line_index);
  reply["candidate"] = candidate_sdp;

  send_to_client_(reply);
}

void ClientHandler::OnDataChannel(
    rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) {
  auto label = data_channel->label();
  if (label == kInputChannelLabel) {
    input_handler_.reset(new InputHandler(data_channel, observer_));
  } else {
    LOG(VERBOSE) << "Data channel connected: " << label;
    data_channels_.push_back(data_channel);
  }
}

void ClientHandler::OnRenegotiationNeeded() {
  LOG(VERBOSE) << "Client " << client_id_ << " needs renegotiation";
}

void ClientHandler::OnIceGatheringChange(
    webrtc::PeerConnectionInterface::IceGatheringState new_state) {
  std::string state_str;
  switch (new_state) {
    case webrtc::PeerConnectionInterface::IceGatheringState::kIceGatheringNew:
      state_str = "NEW";
      break;
    case webrtc::PeerConnectionInterface::IceGatheringState::
        kIceGatheringGathering:
      state_str = "GATHERING";
      break;
    case webrtc::PeerConnectionInterface::IceGatheringState::
        kIceGatheringComplete:
      state_str = "COMPLETE";
      break;
    default:
      state_str = "UNKNOWN";
  }
  LOG(VERBOSE) << "Client " << client_id_
               << ": ICE Gathering state set to: " << state_str;
}

void ClientHandler::OnIceCandidateError(const std::string &host_candidate,
                                        const std::string &url, int error_code,
                                        const std::string &error_text) {
  LOG(VERBOSE) << "Gathering of an ICE candidate (host candidate: "
               << host_candidate << ", url: " << url
               << ") failed: " << error_text;
}

void ClientHandler::OnIceCandidateError(const std::string &address, int port,
                                        const std::string &url, int error_code,
                                        const std::string &error_text) {
  LOG(VERBOSE) << "Gathering of an ICE candidate (address: " << address
               << ", port: " << port << ", url: " << url
               << ") failed: " << error_text;
}

void ClientHandler::OnSignalingChange(
    webrtc::PeerConnectionInterface::SignalingState new_state) {
  // ignore
}
void ClientHandler::OnStandardizedIceConnectionChange(
    webrtc::PeerConnectionInterface::IceConnectionState new_state) {
  switch (new_state) {
    case webrtc::PeerConnectionInterface::kIceConnectionNew:
      LOG(DEBUG) << "ICE connection state: New";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionChecking:
      LOG(DEBUG) << "ICE connection state: Checking";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionConnected:
      LOG(DEBUG) << "ICE connection state: Connected";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
      LOG(DEBUG) << "ICE connection state: Completed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionFailed:
      LOG(DEBUG) << "ICE connection state: Failed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
      LOG(DEBUG) << "ICE connection state: Disconnected";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionClosed:
      LOG(DEBUG) << "ICE connection state: Closed";
      break;
    case webrtc::PeerConnectionInterface::kIceConnectionMax:
      LOG(DEBUG) << "ICE connection state: Max";
      break;
  }
}
void ClientHandler::OnIceCandidatesRemoved(
    const std::vector<cricket::Candidate> &candidates) {
  // ignore
}
void ClientHandler::OnTrack(
    rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  // ignore
}
void ClientHandler::OnRemoveTrack(
    rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
  // ignore
}

}  // namespace webrtc_streaming
}  // namespace cuttlefish