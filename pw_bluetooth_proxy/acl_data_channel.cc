// Copyright 2024 The Pigweed Authors
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "pw_bluetooth_proxy/internal/acl_data_channel.h"

#include <cstdint>
#include <mutex>
#include <optional>

#include "lib/stdcompat/utility.h"
#include "pw_assert/check.h"
#include "pw_bluetooth/emboss_util.h"
#include "pw_bluetooth/hci_data.emb.h"
#include "pw_bluetooth_proxy/internal/multibuf.h"
#include "pw_containers/algorithm.h"  // IWYU pragma: keep
#include "pw_log/log.h"
#include "pw_status/status.h"

namespace pw::bluetooth::proxy {

void AclDataChannel::HandleAclFromController(H4PacketWithHci&& h4_packet) {
  if (!HandleAclData(Direction::kFromController, h4_packet.GetHciSpan())) {
    hci_transport_.SendToHost(std::move(h4_packet));
  }
}

void AclDataChannel::HandleAclFromHost(H4PacketWithH4&& h4_packet) {
  if (!HandleAclData(Direction::kFromHost, h4_packet.GetHciSpan())) {
    hci_transport_.SendToController(std::move(h4_packet));
  }
}

AclDataChannel::AclConnection::AclConnection(AclTransportType transport,
                                             uint16_t connection_handle,
                                             uint16_t num_pending_packets)
    : transport_(transport),
      connection_handle_(connection_handle),
      num_pending_packets_(num_pending_packets) {
  PW_LOG_INFO(
      "btproxy: AclConnection ctor. transport_: %u, connection_handle_: %#x",
      cpp23::to_underlying(transport_),
      connection_handle_);
}

AclDataChannel::SendCredit::SendCredit(SendCredit&& other) {
  *this = std::move(other);
}

AclDataChannel::SendCredit& AclDataChannel::SendCredit::operator=(
    SendCredit&& other) {
  if (this != &other) {
    transport_ = other.transport_;
    relinquish_fn_ = std::move(other.relinquish_fn_);
    other.relinquish_fn_ = nullptr;
  }
  return *this;
}

AclDataChannel::SendCredit::~SendCredit() {
  if (relinquish_fn_) {
    relinquish_fn_(transport_);
  }
}

AclDataChannel::SendCredit::SendCredit(
    AclTransportType transport,
    Function<void(AclTransportType transport)>&& relinquish_fn)
    : transport_(transport), relinquish_fn_(std::move(relinquish_fn)) {}

void AclDataChannel::SendCredit::MarkUsed() {
  PW_CHECK(relinquish_fn_);
  relinquish_fn_ = nullptr;
}

void AclDataChannel::Reset() {
  {
    std::lock_guard lock(credit_mutex_);
    // Reset credits first so no packets queued in signaling channels can be
    // sent.
    le_credits_.Reset();
    br_edr_credits_.Reset();
  }
  {
    std::lock_guard lock(connection_mutex_);
    acl_connections_.clear();
  }
  {
    std::lock_guard lock(delegates_mutex_);
    connection_delegates_.clear();
  }
}

Status AclDataChannel::RegisterConnection(ConnectionDelegate& delegate) {
  std::lock_guard lock(delegates_mutex_);
  auto [_, success] = connection_delegates_.insert(delegate);
  if (!success) {
    return Status::AlreadyExists();
  }
  return OkStatus();
}

Status AclDataChannel::UnregisterConnection(ConnectionDelegate& delegate) {
  std::lock_guard lock(delegates_mutex_);
  auto iter = connection_delegates_.find(delegate.key());
  if (iter == connection_delegates_.end()) {
    return Status::NotFound();
  }
  connection_delegates_.erase(iter);
  return OkStatus();
}

void AclDataChannel::Credits::Reset() {
  proxy_max_ = 0;
  proxy_pending_ = 0;
}

uint16_t AclDataChannel::Credits::Reserve(uint16_t controller_max) {
  PW_CHECK(!Initialized(),
           "AclDataChannel is already initialized. Proxy should have been "
           "reset before this.");

  proxy_max_ = std::min(controller_max, to_reserve_);
  const uint16_t host_max = controller_max - proxy_max_;

  PW_LOG_INFO(
      "Bluetooth Proxy reserved %d ACL data credits. Passed %d on to host.",
      proxy_max_,
      host_max);

  if (proxy_max_ < to_reserve_) {
    PW_LOG_ERROR(
        "Only was able to reserve %d acl data credits rather than the "
        "configured %d from the controller provided's data credits of %d. ",
        proxy_max_,
        to_reserve_,
        controller_max);
  }

  return host_max;
}

Status AclDataChannel::Credits::MarkPending(uint16_t num_credits) {
  if (num_credits > Available()) {
    return Status::ResourceExhausted();
  }

  proxy_pending_ += num_credits;

  return OkStatus();
}

void AclDataChannel::Credits::MarkCompleted(uint16_t num_credits) {
  if (num_credits > proxy_pending_) {
    PW_LOG_ERROR("Tried to mark completed more packets than were pending.");
    proxy_pending_ = 0;
  } else {
    proxy_pending_ -= num_credits;
  }
}

AclDataChannel::Credits& AclDataChannel::LookupCredits(
    AclTransportType transport) {
  switch (transport) {
    case AclTransportType::kBrEdr:
      return br_edr_credits_;
    case AclTransportType::kLe:
      return le_credits_;
    default:
      PW_CHECK(false, "Invalid transport type");
  }
}

const AclDataChannel::Credits& AclDataChannel::LookupCredits(
    AclTransportType transport) const {
  switch (transport) {
    case AclTransportType::kBrEdr:
      return br_edr_credits_;
    case AclTransportType::kLe:
      return le_credits_;
    default:
      PW_CHECK(false, "Invalid transport type");
  }
}

void AclDataChannel::ProcessReadBufferSizeCommandCompleteEvent(
    emboss::ReadBufferSizeCommandCompleteEventWriter read_buffer_event) {
  {
    std::lock_guard lock(credit_mutex_);
    const uint16_t controller_max =
        read_buffer_event.total_num_acl_data_packets().Read();
    const uint16_t host_max = br_edr_credits_.Reserve(controller_max);
    read_buffer_event.total_num_acl_data_packets().Write(host_max);
    max_acl_data_packet_length_ =
        read_buffer_event.acl_data_packet_length().Read();
  }

  on_tx_credits_fn_();
}

template <class EventT>
void AclDataChannel::ProcessSpecificLEReadBufferSizeCommandCompleteEvent(
    EventT read_buffer_event) {
  {
    std::lock_guard lock(credit_mutex_);
    const uint16_t controller_max =
        read_buffer_event.total_num_le_acl_data_packets().Read();
    // TODO: https://pwbug.dev/380316252 - Support shared buffers.
    const uint16_t host_max = le_credits_.Reserve(controller_max);
    read_buffer_event.total_num_le_acl_data_packets().Write(host_max);
    max_le_acl_data_packet_length_ =
        read_buffer_event.le_acl_data_packet_length().Read();
  }

  const uint16_t le_acl_data_packet_length =
      read_buffer_event.le_acl_data_packet_length().Read();

  // Core Spec v6.0 Vol 4, Part E, Section 7.8.2:  A value of 0 means "No
  // dedicated LE Buffer exists".
  // TODO: https://pwbug.dev/380316252 - Support shared buffers.
  if (le_acl_data_packet_length == 0) {
    PW_LOG_ERROR(
        "Controller shares data buffers between BR/EDR and LE transport, which "
        "is not yet supported. So channels on LE transport will not be "
        "functional.");
  }

  // Send packets that may have queued before we acquired any LE ACL credits.
  on_tx_credits_fn_();
}

template void
AclDataChannel::ProcessSpecificLEReadBufferSizeCommandCompleteEvent<
    emboss::LEReadBufferSizeV1CommandCompleteEventWriter>(
    emboss::LEReadBufferSizeV1CommandCompleteEventWriter read_buffer_event);

template void
AclDataChannel::ProcessSpecificLEReadBufferSizeCommandCompleteEvent<
    emboss::LEReadBufferSizeV2CommandCompleteEventWriter>(
    emboss::LEReadBufferSizeV2CommandCompleteEventWriter read_buffer_event);

void AclDataChannel::HandleNumberOfCompletedPacketsEvent(
    H4PacketWithHci&& h4_packet) {
  Result<emboss::NumberOfCompletedPacketsEventWriter> nocp_event =
      MakeEmbossWriter<emboss::NumberOfCompletedPacketsEventWriter>(
          h4_packet.GetHciSpan());
  if (!nocp_event.ok()) {
    PW_LOG_ERROR(
        "Buffer is too small for NUMBER_OF_COMPLETED_PACKETS event. So "
        "will not process.");
    hci_transport_.SendToHost(std::move(h4_packet));
    return;
  }

  bool should_send_to_host = false;
  bool did_reclaim_credits = false;
  {
    std::lock_guard lock(connection_mutex_);
    for (uint8_t i = 0; i < nocp_event->num_handles().Read(); ++i) {
      uint16_t handle = nocp_event->nocp_data()[i].connection_handle().Read();
      uint16_t num_completed_packets =
          nocp_event->nocp_data()[i].num_completed_packets().Read();

      if (num_completed_packets == 0) {
        continue;
      }

      AclConnection* connection_ptr = FindAclConnection(handle);
      if (!connection_ptr) {
        // Credits for connection we are not tracking or closed connection, so
        // should pass event on to host.
        should_send_to_host = true;
        continue;
      }

      // Reclaim proxy's credits before event is forwarded to host
      uint16_t num_pending_packets = connection_ptr->num_pending_packets();
      uint16_t num_reclaimed =
          std::min(num_completed_packets, num_pending_packets);

      if (num_reclaimed > 0) {
        did_reclaim_credits = true;
      }

      {
        std::lock_guard credit_lock(credit_mutex_);
        LookupCredits(connection_ptr->transport()).MarkCompleted(num_reclaimed);
      }

      connection_ptr->set_num_pending_packets(num_pending_packets -
                                              num_reclaimed);

      uint16_t credits_remaining = num_completed_packets - num_reclaimed;
      nocp_event->nocp_data()[i].num_completed_packets().Write(
          credits_remaining);
      if (credits_remaining > 0) {
        // Connection has credits remaining, so should past event on to host.
        should_send_to_host = true;
      }
    }
  }

  if (did_reclaim_credits) {
    on_tx_credits_fn_();
  }
  if (should_send_to_host) {
    hci_transport_.SendToHost(std::move(h4_packet));
  }
}

void AclDataChannel::HandleConnectionCompleteEvent(uint16_t connection_handle,
                                                   AclTransportType transport) {
  if (CreateAclConnection(connection_handle, transport) ==
      Status::ResourceExhausted()) {
    PW_LOG_ERROR(
        "Could not track connection like requested. Max connections "
        "reached.");
  }
}

void AclDataChannel::ProcessDisconnectionCompleteEvent(
    uint16_t connection_handle, emboss::StatusCode reason) {
  std::lock_guard lock(connection_mutex_);

  AclConnection* connection_ptr = FindAclConnection(connection_handle);
  if (!connection_ptr) {
    PW_LOG_INFO(
        "btproxy: Viewed disconnect (reason: %#.2hhx) for unacquired "
        "connection %#x.",
        cpp23::to_underlying(reason),
        connection_handle);
    return;
  }
  PW_LOG_INFO("Proxy viewed disconnect (reason: %#.2hhx) for connection %#x.",
              cpp23::to_underlying(reason),
              connection_handle);

  if (connection_ptr->num_pending_packets() > 0) {
    PW_LOG_WARN(
        "Connection %#x is disconnecting with packets in flight. Releasing "
        "associated credits.",
        connection_handle);
    std::lock_guard credit_lock(credit_mutex_);
    LookupCredits(connection_ptr->transport())
        .MarkCompleted(connection_ptr->num_pending_packets());
  }

  acl_connections_.erase(connection_ptr);
}

bool AclDataChannel::HasSendAclCapability(AclTransportType transport) const {
  std::lock_guard lock(credit_mutex_);
  return LookupCredits(transport).HasSendCapability();
}

uint16_t AclDataChannel::GetNumFreeAclPackets(
    AclTransportType transport) const {
  std::lock_guard lock(credit_mutex_);
  return LookupCredits(transport).Remaining();
}

std::optional<AclDataChannel::SendCredit> AclDataChannel::ReserveSendCredit(
    AclTransportType transport) {
  std::lock_guard lock(credit_mutex_);
  if (const auto status = LookupCredits(transport).MarkPending(1);
      !status.ok()) {
    return std::nullopt;
  }
  return SendCredit(transport, [this](AclTransportType t) {
    std::lock_guard fn_lock(credit_mutex_);
    LookupCredits(t).MarkCompleted(1);
  });
}

pw::Status AclDataChannel::SendAcl(H4PacketWithH4&& h4_packet,
                                   SendCredit&& credit) {
  std::lock_guard lock(connection_mutex_);
  Result<emboss::AclDataFrameHeaderView> acl_view =
      MakeEmbossView<emboss::AclDataFrameHeaderView>(h4_packet.GetHciSpan());
  if (!acl_view.ok()) {
    PW_LOG_ERROR("An invalid ACL packet was provided. So will not send.");
    return pw::Status::InvalidArgument();
  }
  uint16_t handle = acl_view->handle().Read();

  AclConnection* connection_ptr = FindAclConnection(handle);
  if (!connection_ptr) {
    PW_LOG_ERROR("Tried to send ACL packet on unregistered connection.");
    return pw::Status::NotFound();
  }

  if (connection_ptr->transport() != credit.transport_) {
    PW_LOG_WARN("Provided credit for wrong transport. So will not send.");
    return pw::Status::InvalidArgument();
  }
  credit.MarkUsed();

  connection_ptr->set_num_pending_packets(
      connection_ptr->num_pending_packets() + 1);

  hci_transport_.SendToController(std::move(h4_packet));
  return pw::OkStatus();
}

Status AclDataChannel::CreateAclConnection(uint16_t connection_handle,
                                           AclTransportType transport) {
  std::lock_guard lock(connection_mutex_);
  AclConnection* connection_it = FindAclConnection(connection_handle);
  if (connection_it) {
    return Status::AlreadyExists();
  }
  if (acl_connections_.full()) {
    PW_LOG_ERROR(
        "btproxy: Attempt to create new AclConnection when acl_connections_ is"
        "already full. connection_handle: %#x",
        connection_handle);
    return Status::ResourceExhausted();
  }
  acl_connections_.emplace_back(transport,
                                /*connection_handle=*/connection_handle,
                                /*num_pending_packets=*/0);
  return OkStatus();
}

AclDataChannel::AclConnection* AclDataChannel::FindAclConnection(
    uint16_t connection_handle) {
  AclConnection* connection_it = containers::FindIf(
      acl_connections_, [connection_handle](const AclConnection& connection) {
        return connection.connection_handle() == connection_handle;
      });
  return connection_it == acl_connections_.end() ? nullptr : connection_it;
}

bool AclDataChannel::HandleAclData(Direction direction,
                                   pw::span<uint8_t> buffer) {
  // This function returns whether or not the frame was handled here.
  // * Return true if the frame was handled by the proxy and should _not_ be
  //   passed on to the other side (Host/Controller).
  // * Return false if the frame was _not_ handled by the proxy and should be
  //   passed on to the other side (Host/Controller).
  static constexpr bool kHandled = true;
  static constexpr bool kUnhandled = false;

  Result<emboss::AclDataFrameWriter> acl =
      MakeEmbossWriter<emboss::AclDataFrameWriter>(buffer);
  if (!acl.ok()) {
    PW_LOG_ERROR("Buffer is too small for ACL header");
    return kUnhandled;
  }

  const uint16_t handle = acl->header().handle().Read();

  ConnectionDelegate::HandleAclDataReturn result;
  {
    std::lock_guard lock(delegates_mutex_);
    auto iter = connection_delegates_.find(handle);
    if (iter == connection_delegates_.end()) {
      return kUnhandled;
    }
    result = iter->HandleAclData(direction, *acl);
  }

  if (result.recombined_buffer.has_value()) {
    pw::span<uint8_t> h4_span =
        MultiBufAdapter::AsSpan(result.recombined_buffer.value());
    // Send onward to its final destination.
    switch (direction) {
      case Direction::kFromController: {
        H4PacketWithHci h4_packet{h4_span};
        hci_transport_.SendToHost(std::move(h4_packet));
        break;
      }
      case Direction::kFromHost: {
        H4PacketWithH4 h4_packet{h4_span};
        hci_transport_.SendToController(std::move(h4_packet));
        break;
      }
    }

    // We still return kHandled here since the last fragment packet was already
    // passed on to the host as part of the recombined H4 packet.
    return kHandled;
  }

  return result.handled;
}

std::optional<uint16_t> AclDataChannel::MaxDataPacketLengthForTransport(
    AclTransportType transport) const {
  std::lock_guard lock(credit_mutex_);
  switch (transport) {
    case AclTransportType::kBrEdr:
      return max_acl_data_packet_length_;
    case AclTransportType::kLe:
      return max_le_acl_data_packet_length_;
    default:
      return std::nullopt;
  }
}

bool AclDataChannel::HasAclConnection(uint16_t connection_handle) {
  std::lock_guard lock(connection_mutex_);
  return FindAclConnection(connection_handle) != nullptr;
}

}  // namespace pw::bluetooth::proxy
