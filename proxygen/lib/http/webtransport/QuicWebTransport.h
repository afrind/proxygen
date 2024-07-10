/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <proxygen/lib/http/webtransport/WebTransportImpl.h>
#include <quic/api/QuicSocket.h>

namespace proxygen {

class QuicWebTransport
    : private WebTransportImpl::TransportProvider
    , private WebTransportImpl::SessionProvider
    , private quic::QuicSocket::ConnectionCallback
    , private quic::QuicSocket::DatagramCallback
    , public WebTransportImpl {

 public:
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void onDatagram(std::unique_ptr<folly::IOBuf>) noexcept = 0;
    virtual void onWebTransportBidiStream(
        quic::StreamId id, WebTransport::BidiStreamHandle) noexcept = 0;
    virtual void onWebTransportUniStream(
        quic::StreamId id, WebTransport::StreamReadHandle*) noexcept = 0;
  };

  QuicWebTransport(std::shared_ptr<quic::QuicSocket> quicSocket)
      : WebTransportImpl(
            reinterpret_cast<WebTransportImpl::TransportProvider&>(*this),
            static_cast<WebTransportImpl::SessionProvider&>(*this)),
        quicSocket_(std::move(quicSocket)) {
  }

  ~QuicWebTransport() override = default;

  void setHandler(Handler* handler) {
    handler_ = handler;
  }

 private:
  void onFlowControlUpdate(quic::StreamId /*id*/) noexcept override;

  void onNewBidirectionalStream(quic::StreamId id) noexcept override;

  void onNewUnidirectionalStream(quic::StreamId id) noexcept override;

  void onStopSending(quic::StreamId id,
                     quic::ApplicationErrorCode error) noexcept override;

  void onConnectionEnd() noexcept override;
  void onConnectionError(quic::QuicError code) noexcept override;
  void onConnectionEnd(quic::QuicError /* error */) noexcept override;
  void onBidirectionalStreamsAvailable(
      uint64_t /*numStreamsAvailable*/) noexcept override;

  void onUnidirectionalStreamsAvailable(
      uint64_t /*numStreamsAvailable*/) noexcept override;

  folly::Expected<HTTPCodec::StreamID, WebTransport::ErrorCode>
  newWebTransportBidiStream() override;

  folly::Expected<HTTPCodec::StreamID, WebTransport::ErrorCode>
  newWebTransportUniStream() override;

  folly::SemiFuture<folly::Unit> awaitUniStreamCredit() override;

  folly::SemiFuture<folly::Unit> awaitBidiStreamCredit() override;

  folly::Expected<FCState, WebTransport::ErrorCode> sendWebTransportStreamData(
      HTTPCodec::StreamID /*id*/,
      std::unique_ptr<folly::IOBuf> /*data*/,
      bool /*eof*/,
      quic::QuicSocket::WriteCallback* /*wcb*/) override;

  folly::Expected<folly::Unit, WebTransport::ErrorCode> resetWebTransportEgress(
      HTTPCodec::StreamID /*id*/, uint32_t /*errorCode*/) override;

  folly::Expected<std::pair<std::unique_ptr<folly::IOBuf>, bool>,
                  WebTransport::ErrorCode>
  readWebTransportData(HTTPCodec::StreamID id, size_t max) override {
    auto res = quicSocket_->read(id, max);
    if (res) {
      return std::move(res.value());
    } else {
      return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
    }
  }

  folly::Expected<folly::Unit, WebTransport::ErrorCode>
  initiateReadOnBidiStream(
      HTTPCodec::StreamID id,
      quic::QuicSocket::ReadCallback* readCallback) override {
    auto res = quicSocket_->setReadCallback(id, readCallback);
    if (res) {
      return folly::unit;
    } else {
      return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
    }
  }

  folly::Expected<folly::Unit, WebTransport::ErrorCode>
      pauseWebTransportIngress(HTTPCodec::StreamID /*id*/) override;

  folly::Expected<folly::Unit, WebTransport::ErrorCode>
      resumeWebTransportIngress(HTTPCodec::StreamID /*id*/) override;

  folly::Expected<folly::Unit, WebTransport::ErrorCode>
      stopReadingWebTransportIngress(HTTPCodec::StreamID /*id*/,
                                     uint32_t /*errorCode*/) override;

  folly::Expected<folly::Unit, WebTransport::ErrorCode> sendDatagram(
      std::unique_ptr<folly::IOBuf> /*datagram*/) override;

  folly::Expected<folly::Unit, WebTransport::ErrorCode> closeSession(
      folly::Optional<uint32_t> /*error*/) override;

  void onDatagramsAvailable() noexcept override;

  std::shared_ptr<quic::QuicSocket> quicSocket_;
  Handler* handler_{nullptr};
  folly::Optional<folly::Promise<folly::Unit>> waitingForUniStreams_;
  folly::Optional<folly::Promise<folly::Unit>> waitingForBidiStreams_;
};

} // namespace proxygen
