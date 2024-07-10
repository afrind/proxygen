/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <proxygen/lib/http/codec/test/TestUtils.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <proxygen/lib/http/session/test/HTTPSessionMocks.h>
#include <proxygen/lib/http/session/test/HTTPTransactionMocks.h>

using namespace testing;
using WTFCState = proxygen::WebTransportImpl::TransportProvider::FCState;
namespace {
constexpr uint32_t WT_APP_ERROR_1 = 19;
constexpr uint32_t WT_APP_ERROR_2 = 77;
} // namespace

namespace proxygen::test {
class HTTPTransactionWebTransportTest : public testing::Test {
 public:
  void SetUp() override {
    setup(/*withHandler=*/true);
  }

  void setup(bool withHandler) {
    makeTxn();
    EXPECT_CALL(transport_, describe(_)).WillRepeatedly(Return());
    EXPECT_CALL(transport_, supportsWebTransport())
        .WillRepeatedly(Return(true));
    if (withHandler) {
      handler_.expectTransaction();
      txn_->setHandler(&handler_);
      handler_.expectDetachTransaction();
    }
    EXPECT_CALL(transport_, sendHeaders(txn_.get(), _, _, false));
    EXPECT_CALL(transport_, notifyPendingEgress()).Times(AtLeast(0));
    EXPECT_CALL(transport_, detach(txn_.get())).WillOnce([this] {
      txn_.reset();
    });
    HTTPMessage req;
    req.setHTTPVersion(1, 1);
    req.setUpgradeProtocol("webtransport");
    req.setMethod(HTTPMethod::CONNECT);
    req.setURL("/webtransport");
    req.getHeaders().set(HTTP_HEADER_HOST, "www.facebook.com");
    txn_->sendHeaders(req);
    auto resp = std::make_unique<HTTPMessage>();
    resp->setHTTPVersion(1, 1);
    resp->setStatusCode(200);
    if (withHandler) {
      handler_.expectHeaders();
    }
    txn_->onIngressHeadersComplete(std::move(resp));

    wt_ = txn_->getWebTransport();
    EXPECT_NE(wt_, nullptr);
  }

  void TearDown() override {
    if (txn_) {
      EXPECT_CALL(transport_, sendAbort(txn_.get(), _));
      txn_->sendAbort();
    }
  }

 protected:
  folly::EventBase eventBase_;
  testing::StrictMock<MockHTTPTransactionTransport> transport_;
  testing::StrictMock<MockHTTPHandler> handler_;
  HTTP2PriorityQueue txnEgressQueue_;
  std::unique_ptr<HTTPTransaction> txn_;

  static void readCallback(folly::Try<WebTransport::StreamData> streamData,
                           bool expectException,
                           size_t expectedLength,
                           bool expectFin) {
    VLOG(4) << __func__ << " expectException=" << uint64_t(expectException)
            << " expectedLength=" << expectedLength
            << " expectFin=" << expectFin;
    EXPECT_EQ(streamData.hasException(), expectException);
    if (expectException || streamData.hasException()) {
      return;
    }
    if (streamData->data) {
      EXPECT_EQ(streamData->data->computeChainDataLength(), expectedLength);
    } else {
      EXPECT_EQ(expectedLength, 0);
    }
    EXPECT_EQ(streamData->fin, expectFin);
  }

  HTTPTransaction& makeTxn() {
    txn_ = std::make_unique<HTTPTransaction>(TransportDirection::DOWNSTREAM,
                                             HTTPCodec::StreamID(1),
                                             0,
                                             transport_,
                                             txnEgressQueue_,
                                             &evb_.timer());
    return *txn_;
  }
  WebTransport* wt_{nullptr};
  folly::EventBase evb_;
};

TEST_F(HTTPTransactionWebTransportTest, CreateStreams) {
  EXPECT_CALL(transport_, newWebTransportBidiStream()).WillOnce(Return(0));
  EXPECT_CALL(transport_, initiateReadOnBidiStream(_, _))
      .WillOnce(Return(folly::unit));
  auto res = wt_->createBidiStream();
  EXPECT_TRUE(res.hasValue());
  EXPECT_CALL(transport_, resetWebTransportEgress(0, WT_APP_ERROR_1))
      .WillOnce(Return(folly::unit));
  EXPECT_EQ(res->writeHandle->getID(), 0);
  res->writeHandle->resetStream(WT_APP_ERROR_1);
  EXPECT_CALL(transport_, stopReadingWebTransportIngress(0, WT_APP_ERROR_2))
      .WillOnce(Return(folly::unit));
  res->readHandle->stopSending(WT_APP_ERROR_2);

  EXPECT_CALL(transport_, newWebTransportUniStream()).WillOnce(Return(1));
  auto res2 = wt_->createUniStream();
  EXPECT_TRUE(res2.hasValue());
  EXPECT_CALL(transport_, sendWebTransportStreamData(1, testing::_, true, _))
      .WillOnce(Return(WTFCState::UNBLOCKED));
  res2.value()
      ->writeStreamData(nullptr, true)
      .value()
      .via(&eventBase_)
      .thenTry(
          [](auto writeReady) { EXPECT_FALSE(writeReady.hasException()); });

  // Try creating streams but fail at transport
  EXPECT_CALL(transport_, newWebTransportBidiStream())
      .WillOnce(Return(folly::makeUnexpected(
          WebTransport::ErrorCode::STREAM_CREATION_ERROR)));
  EXPECT_EQ(wt_->createBidiStream().error(),
            WebTransport::ErrorCode::STREAM_CREATION_ERROR);
  EXPECT_CALL(transport_, newWebTransportUniStream())
      .WillOnce(Return(folly::makeUnexpected(
          WebTransport::ErrorCode::STREAM_CREATION_ERROR)));
  EXPECT_EQ(wt_->createUniStream().error(),
            WebTransport::ErrorCode::STREAM_CREATION_ERROR);

  EXPECT_CALL(transport_, sendEOM(txn_.get(), nullptr));
  wt_->closeSession();
}

TEST_F(HTTPTransactionWebTransportTest, ReadStream) {
  WebTransport::StreamReadHandle* readHandle{nullptr};
  EXPECT_CALL(handler_, onWebTransportUniStream(_, _))
      .WillOnce(SaveArg<1>(&readHandle));

  auto implHandle = txn_->onWebTransportUniStream(0);
  EXPECT_NE(readHandle, nullptr);

  // read with no data buffered
  auto fut = readHandle->readStreamData()
                 .via(&eventBase_)
                 .thenTry([](auto streamData) {
                   readCallback(std::move(streamData), false, 10, false);
                 });
  EXPECT_FALSE(fut.isReady());

  implHandle->dataAvailable(makeBuf(10), false);
  EXPECT_FALSE(fut.isReady());
  eventBase_.loopOnce();
  EXPECT_TRUE(fut.isReady());

  // buffer data with no read
  implHandle->dataAvailable(makeBuf(32768), false);

  // full buffer, blocked
  EXPECT_CALL(transport_, pauseWebTransportIngress(0));
  EXPECT_CALL(transport_, readWebTransportData(_, _)).WillOnce(Invoke([] {
    return std::make_pair(makeBuf(32768), false);
  }));
  implHandle->readAvailable(0);
  EXPECT_CALL(transport_, resumeWebTransportIngress(0));
  fut = readHandle->readStreamData()
            .via(&eventBase_)
            .thenTry([](auto streamData) {
              readCallback(std::move(streamData), false, 65536, false);
            });
  EXPECT_FALSE(fut.isReady());
  eventBase_.loopOnce();
  EXPECT_TRUE(fut.isReady());

  // fin
  fut = readHandle->readStreamData()
            .via(&eventBase_)
            .thenTry([](auto streamData) {
              readCallback(std::move(streamData), false, 0, true);
            });
  EXPECT_FALSE(fut.isReady());

  implHandle->dataAvailable(nullptr, true);
  eventBase_.loopOnce();
  EXPECT_TRUE(fut.isReady());
}

TEST_F(HTTPTransactionWebTransportTest, ReadStreamBufferedError) {
  WebTransport::StreamReadHandle* readHandle{nullptr};
  EXPECT_CALL(handler_, onWebTransportUniStream(_, _))
      .WillOnce(SaveArg<1>(&readHandle));

  auto implHandle = txn_->onWebTransportUniStream(0);
  EXPECT_NE(readHandle, nullptr);

  implHandle->deliverReadError(WT_APP_ERROR_2);

  // read with buffered error
  auto fut = readHandle->readStreamData()
                 .via(&eventBase_)
                 .thenTry([](auto streamData) {
                   readCallback(std::move(streamData), true, 0, false);
                 });
  eventBase_.loopOnce();
  EXPECT_TRUE(fut.isReady());
}

TEST_F(HTTPTransactionWebTransportTest, ReadStreamError) {
  WebTransport::StreamReadHandle* readHandle{nullptr};
  EXPECT_CALL(handler_, onWebTransportUniStream(_, _))
      .WillOnce(SaveArg<1>(&readHandle));

  auto implHandle = txn_->onWebTransportUniStream(0);
  EXPECT_NE(readHandle, nullptr);

  // read with nothing queued
  auto fut = readHandle->readStreamData()
                 .via(&eventBase_)
                 .thenTry([](auto streamData) {
                   readCallback(std::move(streamData), true, 0, false);
                 });
  EXPECT_FALSE(fut.isReady());

  implHandle->deliverReadError(WT_APP_ERROR_2);
  eventBase_.loopOnce();
  EXPECT_TRUE(fut.isReady());
}

TEST_F(HTTPTransactionWebTransportTest, WriteFails) {
  EXPECT_CALL(transport_, newWebTransportUniStream()).WillOnce(Return(1));
  auto res = wt_->createUniStream();
  EXPECT_TRUE(res.hasValue());
  EXPECT_CALL(transport_, sendWebTransportStreamData(1, testing::_, false, _))
      .WillOnce(
          Return(folly::makeUnexpected(WebTransport::ErrorCode::SEND_ERROR)));
  EXPECT_EQ(res.value()->writeStreamData(makeBuf(10), false).error(),
            WebTransport::ErrorCode::SEND_ERROR);
}

TEST_F(HTTPTransactionWebTransportTest, WriteStreamPauseStopSending) {
  EXPECT_CALL(transport_, newWebTransportUniStream()).WillOnce(Return(1));
  auto writeHandle = wt_->createUniStream();
  EXPECT_FALSE(writeHandle.hasError());

  // Block write, then resume
  bool ready = false;
  quic::QuicSocket::WriteCallback* wcb{nullptr};
  EXPECT_CALL(transport_, sendWebTransportStreamData(1, testing::_, false, _))
      .WillOnce(DoAll(SaveArg<3>(&wcb), Return(WTFCState::BLOCKED)));
  writeHandle.value()
      ->writeStreamData(makeBuf(10), false)
      .value()
      .via(&eventBase_)
      .thenTry([&ready](auto writeReady) {
        EXPECT_FALSE(writeReady.hasException());
        ready = true;
      });
  EXPECT_FALSE(ready);
  wcb->onStreamWriteReady(0, 65536);
  eventBase_.loopOnce();
  EXPECT_TRUE(ready);

  // Block write/stop sending
  ready = false;
  EXPECT_CALL(transport_, sendWebTransportStreamData(1, testing::_, false, _))
      .WillOnce(DoAll(SaveArg<3>(&wcb), Return(WTFCState::BLOCKED)));
  writeHandle.value()
      ->writeStreamData(makeBuf(10), false)
      .value()
      .via(&eventBase_)
      .thenTry([&ready, &writeHandle, this](auto writeReady) {
        EXPECT_TRUE(
            writeReady.withException([](const WebTransport::Exception& ex) {
              EXPECT_EQ(ex.error, WT_APP_ERROR_2);
            }));
        EXPECT_CALL(transport_, resetWebTransportEgress(1, WT_APP_ERROR_1));
        writeHandle.value()->resetStream(WT_APP_ERROR_1);
        ready = true;
      });
  EXPECT_FALSE(ready);
  txn_->onWebTransportStopSending(1, WT_APP_ERROR_2);
  eventBase_.loopOnce();
  EXPECT_TRUE(ready);
}

TEST_F(HTTPTransactionWebTransportTest, BidiStreamEdgeCases) {
  WebTransport::BidiStreamHandle streamHandle;
  EXPECT_CALL(handler_, onWebTransportBidiStream(_, _))
      .WillOnce(SaveArg<1>(&streamHandle));

  auto bidiHandle = txn_->onWebTransportBidiStream(0);
  EXPECT_NE(streamHandle.readHandle, nullptr);
  EXPECT_NE(streamHandle.writeHandle, nullptr);

  // deliver EOF before read
  bidiHandle.readHandle->dataAvailable(nullptr, true);

  auto fut = streamHandle.readHandle->readStreamData()
                 .via(&eventBase_)
                 .thenTry([](auto streamData) {
                   readCallback(std::move(streamData), false, 0, true);
                 });
  EXPECT_FALSE(fut.isReady());
  eventBase_.loopOnce();
  EXPECT_TRUE(fut.isReady());

  // Cancellation handling
  folly::CancellationCallback writeCancel(
      streamHandle.writeHandle->getCancelToken(), [&streamHandle, this] {
        // Write cancelled:
        // We can retrieve the stop sending code from the handle
        EXPECT_EQ(*streamHandle.writeHandle->stopSendingErrorCode(),
                  WT_APP_ERROR_2);
        // attempt to write, will error, but don't reset the stream
        streamHandle.writeHandle->writeStreamData(makeBuf(10), true)
            .value()
            .via(&eventBase_)
            .thenValue([](auto) {})
            .thenError(folly::tag_t<const WebTransport::Exception&>{},
                       [](auto const& ex) {
                         VLOG(4) << "write error";
                         EXPECT_EQ(ex.error, WT_APP_ERROR_2);
                       });
      });
  // Deliver SS
  txn_->onWebTransportStopSending(0, WT_APP_ERROR_2);
  eventBase_.loopOnce();
  EXPECT_CALL(transport_,
              resetWebTransportEgress(0, WebTransport::kInternalError));
  // Note the egress stream was not reset, will be reset when the txn detaches
}

TEST_F(HTTPTransactionWebTransportTest, StreamDetachWithOpenStreams) {
  EXPECT_CALL(transport_, newWebTransportBidiStream()).WillOnce(Return(0));
  EXPECT_CALL(transport_, initiateReadOnBidiStream(_, _))
      .WillOnce(Return(folly::unit));
  auto res = wt_->createBidiStream();
  EXPECT_FALSE(res.hasError());
  bool readCancelled = false;
  bool writeCancelled = false;
  folly::CancellationCallback readCancel(
      res->readHandle->getCancelToken(), [&readCancelled, &res, this] {
        res->readHandle->readStreamData()
            .via(&eventBase_)
            .thenValue([](auto) {})
            .thenError(folly::tag_t<const WebTransport::Exception&>{},
                       [](auto const& ex) {
                         VLOG(4) << "readCancelled";
                         EXPECT_EQ(ex.error, WebTransport::kInternalError);
                       });
        readCancelled = true;
      });
  folly::CancellationCallback writeCancel(
      res->writeHandle->getCancelToken(), [&writeCancelled, &res, this] {
        VLOG(4) << "writeCancelled";
        EXPECT_CALL(transport_, resetWebTransportEgress(0, WT_APP_ERROR_2));
        res->writeHandle->resetStream(WT_APP_ERROR_2);
        writeCancelled = true;
      });
  HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS, "aborted");
  handler_.expectError();
  EXPECT_CALL(transport_,
              stopReadingWebTransportIngress(0, WebTransport::kInternalError));
  txn_->onError(ex);
  EXPECT_TRUE(readCancelled);
  EXPECT_TRUE(writeCancelled);
}

TEST_F(HTTPTransactionWebTransportTest, NoHandler) {
  TearDown();
  setup(/*withHandler=*/false);
  EXPECT_CALL(transport_,
              stopReadingWebTransportIngress(0, WebTransport::kInternalError))
      .RetiresOnSaturation();
  EXPECT_CALL(transport_,
              resetWebTransportEgress(0, WebTransport::kInternalError));
  txn_->onWebTransportBidiStream(0);
  EXPECT_CALL(transport_,
              stopReadingWebTransportIngress(1, WebTransport::kInternalError));
  txn_->onWebTransportUniStream(1);
}

TEST_F(HTTPTransactionWebTransportTest, StreamIDAPIs) {
  EXPECT_CALL(transport_, newWebTransportBidiStream()).WillOnce(Return(0));
  quic::QuicSocket::ReadCallback* quicReadCallback{nullptr};
  EXPECT_CALL(transport_, initiateReadOnBidiStream(_, _))
      .WillOnce(DoAll(SaveArg<1>(&quicReadCallback), Return(folly::unit)));
  auto res = wt_->createBidiStream();
  auto id = res->readHandle->getID();

  // read by id
  auto fut = wt_->readStreamData(id)
                 .value()
                 .via(&eventBase_)
                 .thenTry([](auto streamData) {
                   readCallback(std::move(streamData), false, 10, 0);
                 });
  EXPECT_FALSE(fut.isReady());
  EXPECT_CALL(transport_, readWebTransportData(_, _)).WillOnce(Invoke([] {
    return std::make_pair(makeBuf(10), false);
  }));
  quicReadCallback->readAvailable(id);
  eventBase_.loopOnce();
  EXPECT_TRUE(fut.isReady());

  // stopSending by ID
  EXPECT_CALL(transport_, stopReadingWebTransportIngress(0, WT_APP_ERROR_1))
      .WillOnce(Return(folly::unit));
  wt_->stopSending(id, WT_APP_ERROR_1);

  // write by ID
  bool ready = false;
  EXPECT_CALL(transport_, sendWebTransportStreamData(id, testing::_, false, _))
      .WillOnce(Return(WTFCState::UNBLOCKED));
  wt_->writeStreamData(id, makeBuf(10), false)
      .value()
      .via(&eventBase_)
      .thenTry([&ready](auto writeReady) {
        EXPECT_FALSE(writeReady.hasException());
        ready = true;
      });
  eventBase_.loopOnce();
  EXPECT_TRUE(ready);

  // resetStream by ID
  EXPECT_CALL(transport_, resetWebTransportEgress(id, WT_APP_ERROR_2));
  wt_->resetStream(id, WT_APP_ERROR_2);
}

TEST_F(HTTPTransactionWebTransportTest, InvalidStreamIDAPIs) {
  uint64_t id = 7;

  EXPECT_EQ(wt_->stopSending(id, WT_APP_ERROR_1).error(),
            WebTransport::ErrorCode::INVALID_STREAM_ID);
  EXPECT_EQ(wt_->resetStream(id, WT_APP_ERROR_2).error(),
            WebTransport::ErrorCode::INVALID_STREAM_ID);
  EXPECT_EQ(wt_->readStreamData(id).error(),
            WebTransport::ErrorCode::INVALID_STREAM_ID);
  EXPECT_EQ(wt_->writeStreamData(id, makeBuf(10), false).error(),
            WebTransport::ErrorCode::INVALID_STREAM_ID);
}

TEST_F(HTTPTransactionWebTransportTest, SendDatagram) {
  EXPECT_CALL(transport_, sendDatagram(_)).WillOnce(Return(folly::unit));
  EXPECT_TRUE(wt_->sendDatagram(makeBuf(100)));
}

TEST_F(HTTPTransactionWebTransportTest, RefreshTimeout) {
  txn_->setIdleTimeout(std::chrono::milliseconds(100));
  evb_.runAfterDelay(
      [this] {
        WebTransport::StreamReadHandle* readHandle{nullptr};
        EXPECT_CALL(handler_, onWebTransportUniStream(_, _))
            .WillOnce(SaveArg<1>(&readHandle));

        txn_->onWebTransportUniStream(0);
        EXPECT_NE(readHandle, nullptr);
      },
      50);
  evb_.runAfterDelay(
      [this] {
        EXPECT_CALL(transport_,
                    stopReadingWebTransportIngress(
                        0, std::numeric_limits<uint32_t>::max()))
            .WillOnce(Return(folly::unit));
        handler_.expectEOM();
        txn_->onIngressEOM();
        EXPECT_CALL(transport_, sendEOM(txn_.get(), nullptr));
        wt_->closeSession();
      },
      150);
  evb_.loop();
}

} // namespace proxygen::test
