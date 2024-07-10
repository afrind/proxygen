/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <proxygen/lib/http/webtransport/QuicWebTransport.h>

using FCState = proxygen::WebTransportImpl::TransportProvider::FCState;

namespace {
class WriteCallback : public quic::QuicSocket::WriteCallback {
 public:
  void onStreamWriteReady(quic::StreamId id, uint64_t) noexcept override {
    VLOG(4) << "onStreamWriteReady id=" << id;
    promise_.setValue();
    delete this;
  }

  folly::Future<folly::Unit> getFuture() {
    return promise_.getFuture();
  }

 private:
  folly::Promise<folly::Unit> promise_;
};

} // namespace

namespace proxygen {

void QuicWebTransport::onFlowControlUpdate(quic::StreamId /*id*/) noexcept {
}

void QuicWebTransport::onNewBidirectionalStream(quic::StreamId id) noexcept {
  if (!handler_) {
    resetWebTransportEgress(id, WebTransport::kInternalError);
    stopReadingWebTransportIngress(id, WebTransport::kInternalError);
    return;
  }
  auto handle = WebTransportImpl::onWebTransportBidiStream(id);
  handler_->onWebTransportBidiStream(
      id,
      WebTransport::BidiStreamHandle({handle.readHandle, handle.writeHandle}));
  quicSocket_->setReadCallback(id, handle.readHandle);
}

void QuicWebTransport::onNewUnidirectionalStream(quic::StreamId id) noexcept {
  if (!handler_) {
    LOG(ERROR) << "Handler not set";
    stopReadingWebTransportIngress(id, WebTransport::kInternalError);
    return;
  }
  auto readHandle = WebTransportImpl::onWebTransportUniStream(id);
  handler_->onWebTransportUniStream(id, readHandle);
  quicSocket_->setReadCallback(id, readHandle);
}

void QuicWebTransport::onStopSending(
    quic::StreamId id, quic::ApplicationErrorCode errorCode) noexcept {
  auto appErrorCode = WebTransport::toApplicationErrorCode(errorCode);
  if (!appErrorCode) {
    return;
  }
  onWebTransportStopSending(id, *appErrorCode);
}

void QuicWebTransport::onConnectionEnd() noexcept {
}

void QuicWebTransport::onConnectionError(quic::QuicError code) noexcept {
}
void QuicWebTransport::onConnectionEnd(quic::QuicError /* error */) noexcept {
}

folly::Expected<HTTPCodec::StreamID, WebTransport::ErrorCode>
QuicWebTransport::newWebTransportBidiStream() {
  auto id = quicSocket_->createBidirectionalStream();
  if (id.hasError()) {
    return folly::makeUnexpected(ErrorCode::GENERIC_ERROR);
  }
  return id.value();
}

folly::Expected<HTTPCodec::StreamID, WebTransport::ErrorCode>
QuicWebTransport::newWebTransportUniStream() {
  auto id = quicSocket_->createUnidirectionalStream();
  if (id.hasError()) {
    return folly::makeUnexpected(ErrorCode::GENERIC_ERROR);
  }
  return id.value();
}

folly::Expected<WebTransportImpl::TransportProvider::FCState,
                WebTransport::ErrorCode>
QuicWebTransport::sendWebTransportStreamData(
    HTTPCodec::StreamID id,
    std::unique_ptr<folly::IOBuf> data,
    bool eof,
    quic::QuicSocket::WriteCallback* wcb) {
  auto res = quicSocket_->writeChain(id, std::move(data), eof);
  if (!res) {
    return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
  }
  auto flowControl = quicSocket_->getStreamFlowControl(id);
  if (!flowControl) {
    LOG(ERROR) << "Failed to get flow control";
    return folly::makeUnexpected(WebTransport::ErrorCode::SEND_ERROR);
  }
  if (!eof && flowControl->sendWindowAvailable == 0) {
    quicSocket_->notifyPendingWriteOnStream(id, wcb);
    VLOG(4) << "Closing fc window";
    return FCState::BLOCKED;
  } else {
    return FCState::UNBLOCKED;
  }
}

folly::Expected<folly::Unit, WebTransport::ErrorCode>
QuicWebTransport::resetWebTransportEgress(HTTPCodec::StreamID id,
                                          uint32_t errorCode) {
  auto res = quicSocket_->resetStream(id, errorCode);
  if (!res) {
    return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
  }
  return folly::unit;
}

folly::Expected<folly::Unit, WebTransport::ErrorCode>
QuicWebTransport::pauseWebTransportIngress(HTTPCodec::StreamID id) {
  auto res = quicSocket_->pauseRead(id);
  if (res.hasError()) {
    return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
  }
  return folly::unit;
}

folly::Expected<folly::Unit, WebTransport::ErrorCode>
QuicWebTransport::resumeWebTransportIngress(HTTPCodec::StreamID id) {
  auto res = quicSocket_->resumeRead(id);
  if (res.hasError()) {
    return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
  }
  return folly::unit;
}

folly::Expected<folly::Unit, WebTransport::ErrorCode>
QuicWebTransport::stopReadingWebTransportIngress(HTTPCodec::StreamID id,
                                                 uint32_t errorCode) {
  auto res = quicSocket_->setReadCallback(
      id,
      nullptr,
      quic::ApplicationErrorCode(WebTransport::toHTTPErrorCode(errorCode)));
  if (res.hasError()) {
    return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
  }
  return folly::unit;
}

folly::Expected<folly::Unit, WebTransport::ErrorCode>
QuicWebTransport::sendDatagram(std::unique_ptr<folly::IOBuf> datagram) {
  auto writeRes = quicSocket_->writeDatagram(std::move(datagram));
  if (writeRes.hasError()) {
    LOG(ERROR) << "Failed to send datagram";
    return folly::makeUnexpected(WebTransport::ErrorCode::GENERIC_ERROR);
  }
  return folly::unit;
}

folly::Expected<folly::Unit, WebTransport::ErrorCode>
QuicWebTransport::closeSession(folly::Optional<uint32_t> error) {
  if (error) {
    quicSocket_->close(quic::QuicError(quic::ApplicationErrorCode(*error)));
  } else {
    quicSocket_->close(quic::QuicError(quic::ApplicationErrorCode(0)));
  }
  return folly::unit;
}

void QuicWebTransport::onUnidirectionalStreamsAvailable(
    uint64_t numStreamsAvailable) noexcept {
  if (numStreamsAvailable > 0 && waitingForUniStreams_) {
    waitingForUniStreams_->setValue(folly::unit);
    waitingForUniStreams_.reset();
  }
}

folly::SemiFuture<folly::Unit> QuicWebTransport::awaitUniStreamCredit() {
  auto numOpenable = quicSocket_->getNumOpenableUnidirectionalStreams();
  if (numOpenable > 0) {
    return folly::makeFuture(folly::unit);
  }
  CHECK(!waitingForUniStreams_);
  auto [promise, future] = folly::makePromiseContract<folly::Unit>();
  waitingForUniStreams_ = std::move(promise);
  return std::move(future);
}

void QuicWebTransport::onBidirectionalStreamsAvailable(
    uint64_t numStreamsAvailable) noexcept {
  if (numStreamsAvailable > 0 && waitingForBidiStreams_) {
    waitingForBidiStreams_->setValue(folly::unit);
    waitingForBidiStreams_.reset();
  }
}

folly::SemiFuture<folly::Unit> QuicWebTransport::awaitBidiStreamCredit() {
  auto numOpenable = quicSocket_->getNumOpenableBidirectionalStreams();
  if (numOpenable > 0) {
    return folly::makeFuture(folly::unit);
  }
  CHECK(!waitingForBidiStreams_);
  auto [promise, future] = folly::makePromiseContract<folly::Unit>();
  waitingForBidiStreams_ = std::move(promise);
  return std::move(future);
}

void QuicWebTransport::onDatagramsAvailable() noexcept {
  auto result = quicSocket_->readDatagramBufs();
  if (result.hasError()) {
    LOG(ERROR) << "Got error while reading datagrams: error="
               << toString(result.error());
    closeSession(0);
    return;
  }
  VLOG(4) << "Received " << result.value().size() << " datagrams";
  for (auto& datagram : result.value()) {
    handler_->onDatagram(std::move(datagram));
  }
}

} // namespace proxygen
