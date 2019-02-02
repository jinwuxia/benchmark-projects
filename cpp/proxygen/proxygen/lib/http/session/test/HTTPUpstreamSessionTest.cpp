/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <folly/io/Cursor.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/TimeoutManager.h>
#include <wangle/acceptor/ConnectionManager.h>
#include <folly/portability/GTest.h>
#include <proxygen/lib/http/codec/test/MockHTTPCodec.h>
#include <proxygen/lib/http/codec/HTTPCodecFactory.h>
#include <proxygen/lib/http/codec/test/TestUtils.h>
#include <proxygen/lib/http/session/HTTPUpstreamSession.h>
#include <proxygen/lib/http/session/test/MockByteEventTracker.h>
#include <proxygen/lib/http/session/test/HTTPSessionMocks.h>
#include <proxygen/lib/http/session/test/HTTPSessionTest.h>
#include <proxygen/lib/http/session/test/TestUtils.h>
#include <proxygen/lib/test/TestAsyncTransport.h>
#include <string>
#include <folly/io/async/test/MockAsyncTransport.h>
#include <vector>

using folly::test::MockAsyncTransport;

using namespace folly;
using namespace proxygen;
using namespace testing;

using std::string;
using std::unique_ptr;
using std::vector;

class TestPriorityMapBuilder : public HTTPUpstreamSession::PriorityMapFactory {
public:
  virtual std::unique_ptr<HTTPUpstreamSession::PriorityAdapter>
     createVirtualStreams(HTTPPriorityMapFactoryProvider* session) const
        override;
  virtual ~TestPriorityMapBuilder() = default;

  uint8_t hiPriWeight_{18};
  uint8_t hiPriLevel_{0};
  uint8_t loPriWeight_{2};
  uint8_t loPriLevel_{2};
};

class TestPriorityAdapter : public HTTPUpstreamSession::PriorityAdapter {
public:
  virtual folly::Optional<const HTTPMessage::HTTPPriority> getHTTPPriority(
      uint8_t level) override {
    if (priorityMap_.empty()) {
      return folly::none;
    }
    auto it = priorityMap_.find(level);
    if (it == priorityMap_.end()) {
      return minPriority_;
    }
    return it->second;
  }

  virtual ~TestPriorityAdapter() = default;

  std::map<uint8_t, HTTPMessage::HTTPPriority> priorityMap_;
  HTTPMessage::HTTPPriority minPriority_{std::make_tuple(0, false, 0)};
  HTTPCodec::StreamID parentId_{0};
  HTTPCodec::StreamID hiPriId_{0};
  HTTPCodec::StreamID loPriId_{0};
  HTTPMessage::HTTPPriority hiPri_{std::make_tuple(0, false, 0)};
  HTTPMessage::HTTPPriority loPri_{std::make_tuple(0, false, 0)};
};

std::unique_ptr<HTTPUpstreamSession::PriorityAdapter>
    TestPriorityMapBuilder::createVirtualStreams(
        HTTPPriorityMapFactoryProvider* session) const {
  std::unique_ptr<TestPriorityAdapter>  a =
      std::make_unique<TestPriorityAdapter>();
  a->parentId_ = session->sendPriority({0, false, 1});

  std::get<0>(a->hiPri_) = a->parentId_;
  std::get<2>(a->hiPri_) = hiPriWeight_;
  a->hiPriId_ = session->sendPriority({(uint32_t)a->parentId_, false, hiPriWeight_});
  a->priorityMap_[hiPriLevel_] = a->hiPri_;

  std::get<0>(a->loPri_) = a->parentId_;
  std::get<2>(a->loPri_) = loPriWeight_;
  a->loPriId_ = session->sendPriority({(uint32_t)a->parentId_, false, loPriWeight_});
  a->priorityMap_[loPriLevel_] = a->loPri_;

  a->minPriority_ = a->loPri_;

  return std::move(a);
}

namespace {
HTTPMessage getUpgradePostRequest(uint32_t bodyLen,
                                  const std::string& upgradeHeader,
                                  bool expect100 = false) {
  HTTPMessage req = getPostRequest(bodyLen);
  req.getHeaders().set(HTTP_HEADER_UPGRADE, upgradeHeader);
  if (expect100) {
    req.getHeaders().add(HTTP_HEADER_EXPECT, "100-continue");
  }
  return req;
}

std::unique_ptr<folly::IOBuf>
getResponseBuf(CodecProtocol protocol, HTTPCodec::StreamID id,
               uint32_t code, uint32_t bodyLen, bool include100 = false) {
  auto egressCodec =
    HTTPCodecFactory::getCodec(protocol, TransportDirection::DOWNSTREAM);
  folly::IOBufQueue respBufQ{folly::IOBufQueue::cacheChainLength()};
  egressCodec->generateSettings(respBufQ);
  if (include100) {
    HTTPMessage msg;
    msg.setStatusCode(100);
    msg.setStatusMessage("continue");
    egressCodec->generateHeader(respBufQ, id, msg);
  }
  HTTPMessage resp = getResponse(code, bodyLen);
  egressCodec->generateHeader(respBufQ, id, resp);
  if (bodyLen > 0) {
    auto buf = makeBuf(bodyLen);
    egressCodec->generateBody(respBufQ, id, std::move(buf),
                              HTTPCodec::NoPadding, true /* eom */);
  }
  return respBufQ.move();
}

}

template <class C>
class HTTPUpstreamTest: public testing::Test,
                        public HTTPSessionBase::InfoCallback {
 public:
  explicit HTTPUpstreamTest(std::vector<int64_t> flowControl = {-1, -1, -1})
      : eventBase_(),
        transport_(new NiceMock<MockAsyncTransport>()),
        transactionTimeouts_(folly::HHWheelTimer::newTimer(
            &eventBase_,
            std::chrono::milliseconds(
                folly::HHWheelTimer::DEFAULT_TICK_INTERVAL),
            TimeoutManager::InternalEnum::INTERNAL,
            std::chrono::milliseconds(500))),
        flowControl_(flowControl) {}

  void resumeWrites() {
    pauseWrites_ = false;
    for (auto cb: cbs_) {
      handleWrite(cb);
    }
    cbs_.clear();
  }

  virtual void onWriteChain(
      folly::AsyncTransportWrapper::WriteCallback* callback,
      std::shared_ptr<IOBuf> iob,
      WriteFlags) {
    if (pauseWrites_) {
      cbs_.push_back(callback);
      return;  // let write requests timeout
    }
    auto mybuf = iob->clone();
    mybuf->unshare();
    writes_.append(std::move(mybuf));
    handleWrite(callback);
  }

  void handleWrite(folly::AsyncTransportWrapper::WriteCallback* callback) {
    if (failWrites_) {
      AsyncSocketException ex(AsyncSocketException::UNKNOWN, "");
      callback->writeErr(0, ex);
    } else {
      if (writeInLoop_) {
        eventBase_.runInLoop([&] {
            callback->writeSuccess();
          });
      } else {
        callback->writeSuccess();
      }
    }
  }

  void SetUp() override {
    commonSetUp(makeClientCodec<typename C::Codec>(C::version));
  }

  void commonSetUp(unique_ptr<HTTPCodec> codec) {
    HTTPSession::setDefaultReadBufferLimit(65536);
    HTTPSession::setDefaultWriteBufferLimit(65536);
    EXPECT_CALL(*transport_, writeChain(_, _, _))
      .WillRepeatedly(Invoke(this, &HTTPUpstreamTest<C>::onWriteChain));
    EXPECT_CALL(*transport_, setReadCB(_))
      .WillRepeatedly(SaveArg<0>(&readCallback_));
    EXPECT_CALL(*transport_, getReadCB())
      .WillRepeatedly(Return(readCallback_));
    EXPECT_CALL(*transport_, getEventBase())
      .WillRepeatedly(ReturnPointee(&eventBasePtr_));
    EXPECT_CALL(*transport_, good())
      .WillRepeatedly(ReturnPointee(&transportGood_));
    EXPECT_CALL(*transport_, closeNow())
      .WillRepeatedly(Assign(&transportGood_, false));
    EXPECT_CALL(*transport_, isReplaySafe())
      .WillOnce(Return(false));
    EXPECT_CALL(*transport_, setReplaySafetyCallback(_))
      .WillRepeatedly(SaveArg<0>(&replaySafetyCallback_));
    EXPECT_CALL(*transport_, attachEventBase(_))
      .WillRepeatedly(SaveArg<0>(&eventBasePtr_));

    for (auto& param: flowControl_) {
      if (param < 0) {
        param = codec->getDefaultWindowSize();
      }
    }
    httpSession_ = new HTTPUpstreamSession(
      transactionTimeouts_.get(),
      std::move(AsyncTransportWrapper::UniquePtr(transport_)),
      localAddr_, peerAddr_,
      std::move(codec),
      mockTransportInfo_, this);
    httpSession_->setFlowControl(flowControl_[0], flowControl_[1],
                                 flowControl_[2]);
    httpSession_->setMaxConcurrentOutgoingStreams(10);
    httpSession_->setEgressSettings({{SettingsId::ENABLE_EX_HEADERS, 1}});
    httpSession_->startNow();
    eventBase_.loop();
    ASSERT_EQ(this->sessionDestroyed_, false);
  }

  unique_ptr<typename C::Codec> makeServerCodec() {
    return ::makeServerCodec<typename C::Codec>(C::version);
  }

  void enableExHeader(typename C::Codec* serverCodec){
    if (!serverCodec || serverCodec->getProtocol() != CodecProtocol::HTTP_2) {
      return;
    }

    auto clientCodec = makeClientCodec<HTTP2Codec>(2);
    folly::IOBufQueue c2s{IOBufQueue::cacheChainLength()};
    clientCodec->getEgressSettings()->setSetting(
      SettingsId::ENABLE_EX_HEADERS, 1);
    clientCodec->generateConnectionPreface(c2s);
    clientCodec->generateSettings(c2s);

    // set ENABLE_EX_HEADERS to 1 in egressSettings
    serverCodec->getEgressSettings()->setSetting(
      SettingsId::ENABLE_EX_HEADERS, 1);
    // set ENABLE_EX_HEADERS to 1 in ingressSettings
    auto setup = c2s.move();
    serverCodec->onIngress(*setup);
  }

  void parseOutput(HTTPCodec& serverCodec) {
    uint32_t consumed = std::numeric_limits<uint32_t>::max();
    while (!writes_.empty() && consumed > 0) {
      consumed = serverCodec.onIngress(*writes_.front());
      writes_.split(consumed);
    }
    EXPECT_TRUE(writes_.empty());
  }

  void readAndLoop(const std::string& input) {
    readAndLoop((const uint8_t *)input.data(), input.length());
  }

  void readAndLoop(IOBuf* buf) {
    buf->coalesce();
    readAndLoop(buf->data(), buf->length());
  }

  void readAndLoop(const uint8_t* input, size_t length) {
    CHECK_NOTNULL(readCallback_);
    void* buf;
    size_t bufSize;
    while (length > 0) {
      readCallback_->getReadBuffer(&buf, &bufSize);
      // This is somewhat specific to our implementation, but currently we
      // always return at least some space from getReadBuffer
      CHECK_GT(bufSize, 0);
      bufSize = std::min(bufSize, length);
      memcpy(buf, input, bufSize);
      readCallback_->readDataAvailable(bufSize);
      eventBasePtr_->loop();
      length -= bufSize;
      input += bufSize;
    }
  }

  void testBasicRequest();
  void testBasicRequestHttp10(bool keepalive);

  // HTTPSession::InfoCallback interface
  void onCreate(const HTTPSessionBase&) override { sessionCreated_ = true; }
  void onDestroy(const HTTPSessionBase&) override { sessionDestroyed_ = true; }
  void onSettingsOutgoingStreamsFull(const HTTPSessionBase&) override {
    transactionsFull_ = true;
  }
  void onSettingsOutgoingStreamsNotFull(const HTTPSessionBase&) override {
    transactionsFull_ = false;
  }

  void TearDown() override {
    AsyncSocketException ex(AsyncSocketException::UNKNOWN, "");
    for (auto& cb : cbs_) {
      cb->writeErr(0, ex);
    }
  }

  std::unique_ptr<StrictMock<MockHTTPHandler>> openTransaction(
    bool expectStartPaused = false) {
    // Returns a mock handler with txn_ field set in it
    auto handler = std::make_unique<StrictMock<MockHTTPHandler>>();
    handler->expectTransaction();
    if (expectStartPaused) {
      handler->expectEgressPaused();
    }
    auto txn = httpSession_->newTransaction(handler.get());
    EXPECT_EQ(txn, handler->txn_);
    return handler;
  }

  std::unique_ptr<NiceMock<MockHTTPHandler>> openNiceTransaction(
    bool expectStartPaused = false) {
    // Returns a mock handler with txn_ field set in it
    auto handler = std::make_unique<NiceMock<MockHTTPHandler>>();
    handler->expectTransaction();
    if (expectStartPaused) {
      handler->expectEgressPaused();
    }
    auto txn = httpSession_->newTransaction(handler.get());
    EXPECT_EQ(txn, handler->txn_);
    return handler;
  }

  void testSimpleUpgrade(const std::string& upgradeReqHeader,
                         const std::string& upgradeRespHeader,
                         CodecProtocol respCodecVersion);

  MockByteEventTracker* setMockByteEventTracker() {
    auto byteEventTracker = new MockByteEventTracker(nullptr);
    httpSession_->setByteEventTracker(
      std::unique_ptr<ByteEventTracker>(byteEventTracker));
    EXPECT_CALL(*byteEventTracker, preSend(_, _, _))
      .WillRepeatedly(Return(0));
    EXPECT_CALL(*byteEventTracker, drainByteEvents())
      .WillRepeatedly(Return(0));
    EXPECT_CALL(*byteEventTracker, processByteEvents(_, _))
      .WillRepeatedly(Invoke([]
                            (std::shared_ptr<ByteEventTracker> self,
                             uint64_t bytesWritten) {
                              return self->ByteEventTracker::processByteEvents(
                                self,
                                bytesWritten);
                            }));

    return byteEventTracker;
  }

 protected:
  bool sessionCreated_{false};
  bool sessionDestroyed_{false};

  bool transactionsFull_{false};
  bool transportGood_{true};

  EventBase eventBase_;
  EventBase* eventBasePtr_{&eventBase_};
  MockAsyncTransport* transport_;  // invalid once httpSession_ is destroyed
  folly::AsyncTransportWrapper::ReadCallback* readCallback_{nullptr};
  folly::AsyncTransport::ReplaySafetyCallback* replaySafetyCallback_{nullptr};
  folly::HHWheelTimer::UniquePtr transactionTimeouts_;
  std::vector<int64_t> flowControl_;
  wangle::TransportInfo mockTransportInfo_;
  SocketAddress localAddr_{"127.0.0.1", 80};
  SocketAddress peerAddr_{"127.0.0.1", 12345};
  HTTPUpstreamSession* httpSession_{nullptr};
  IOBufQueue writes_{IOBufQueue::cacheChainLength()};
  std::vector<folly::AsyncTransportWrapper::WriteCallback*> cbs_;
  bool failWrites_{false};
  bool pauseWrites_{false};
  bool writeInLoop_{false};
};
TYPED_TEST_CASE_P(HTTPUpstreamTest);

template <class C>
class TimeoutableHTTPUpstreamTest: public HTTPUpstreamTest<C> {
 public:
  TimeoutableHTTPUpstreamTest(): HTTPUpstreamTest<C>() {
    // make it non-internal for this test class
    HTTPUpstreamTest<C>::transactionTimeouts_ =
      folly::HHWheelTimer::newTimer(
        &this->HTTPUpstreamTest<C>::eventBase_,
        std::chrono::milliseconds(folly::HHWheelTimer::DEFAULT_TICK_INTERVAL),
        folly::AsyncTimeout::InternalEnum::NORMAL,
        std::chrono::milliseconds(500));
  }
};

using HTTPUpstreamSessionTest = HTTPUpstreamTest<HTTP1xCodecPair>;
typedef HTTPUpstreamTest<SPDY3CodecPair> SPDY3UpstreamSessionTest;
typedef HTTPUpstreamTest<HTTP2CodecPair> HTTP2UpstreamSessionTest;

TEST_F(SPDY3UpstreamSessionTest, ServerPush) {
  SPDYCodec egressCodec(TransportDirection::DOWNSTREAM,
                        SPDYVersion::SPDY3);
  folly::IOBufQueue output(folly::IOBufQueue::cacheChainLength());

  HTTPMessage push;
  push.getHeaders().set("HOST", "www.foo.com");
  push.setURL("https://www.foo.com/");
  egressCodec.generatePushPromise(output, 2, push, 1, false, nullptr);
  auto buf = makeBuf(100);
  egressCodec.generateBody(output, 2, std::move(buf), HTTPCodec::NoPadding,
                           true /* eom */);

  HTTPMessage resp;
  resp.setStatusCode(200);
  resp.setStatusMessage("Ohai");
  egressCodec.generateHeader(output, 1, resp, false, nullptr);
  buf = makeBuf(100);
  egressCodec.generateBody(output, 1, std::move(buf), HTTPCodec::NoPadding,
                           true /* eom */);

  std::unique_ptr<folly::IOBuf> input = output.move();
  input->coalesce();

  MockHTTPHandler pushHandler;

  InSequence enforceOrder;

  auto handler = openTransaction();
  EXPECT_CALL(*handler, onPushedTransaction(_))
    .WillOnce(Invoke([&pushHandler] (HTTPTransaction* pushTxn) {
          pushTxn->setHandler(&pushHandler);
        }));
  EXPECT_CALL(pushHandler, setTransaction(_));
  EXPECT_CALL(pushHandler, onHeadersComplete(_))
    .WillOnce(Invoke([&] (std::shared_ptr<HTTPMessage> msg) {
          EXPECT_EQ(httpSession_->getNumIncomingStreams(), 1);
          EXPECT_TRUE(msg->getIsChunked());
          EXPECT_FALSE(msg->getIsUpgraded());
          EXPECT_EQ(msg->getPath(), "/");
          EXPECT_EQ(msg->getHeaders().getSingleOrEmpty(HTTP_HEADER_HOST),
                    "www.foo.com");
        }));
  EXPECT_CALL(pushHandler, onBody(_));
  EXPECT_CALL(pushHandler, onEOM());
  EXPECT_CALL(pushHandler, detachTransaction());

  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  EXPECT_CALL(*handler, onBody(_));
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest();
  readAndLoop(input->data(), input->length());

  EXPECT_EQ(httpSession_->getNumIncomingStreams(), 0);
  httpSession_->destroy();
}

TEST_F(SPDY3UpstreamSessionTest, IngressGoawayAbortUncreatedStreams) {
  // Tests whether the session aborts the streams which are not created
  // at the remote end.

  // Create SPDY buf for GOAWAY with last good stream as 0 (no streams created)
  SPDYCodec egressCodec(TransportDirection::DOWNSTREAM,
                        SPDYVersion::SPDY3);
  folly::IOBufQueue respBuf{IOBufQueue::cacheChainLength()};
  egressCodec.generateGoaway(respBuf, 0, ErrorCode::NO_ERROR);
  std::unique_ptr<folly::IOBuf> goawayFrame = respBuf.move();
  goawayFrame->coalesce();

  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectGoaway();
  handler->expectError([&] (const HTTPException& err) {
      EXPECT_TRUE(err.hasProxygenError());
      EXPECT_EQ(err.getProxygenError(), kErrorStreamUnacknowledged);
      ASSERT_EQ(
        folly::to<std::string>("StreamUnacknowledged on transaction id: ",
                               handler->txn_->getID()),
        std::string(err.what()));
    });
  handler->expectDetachTransaction([this] {
      // Make sure the session can't create any more transactions.
      MockHTTPHandler handler2;
      EXPECT_EQ(httpSession_->newTransaction(&handler2), nullptr);
    });

  // Send the GET request
  handler->sendRequest();

  // Receive GOAWAY frame while waiting for SYN_REPLY
  readAndLoop(goawayFrame->data(), goawayFrame->length());

  // Session will delete itself after the abort
}

TEST_F(SPDY3UpstreamSessionTest, IngressGoawaySessionError) {
  // Tests whether the session aborts the streams which are not created
  // at the remote end which have error codes.

  // Create SPDY buf for GOAWAY with last good stream as 0 (no streams created)
  SPDYCodec egressCodec(TransportDirection::DOWNSTREAM,
                        SPDYVersion::SPDY3);
  folly::IOBufQueue respBuf{IOBufQueue::cacheChainLength()};
  egressCodec.generateGoaway(respBuf, 0, ErrorCode::PROTOCOL_ERROR);
  std::unique_ptr<folly::IOBuf> goawayFrame = respBuf.move();
  goawayFrame->coalesce();

  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectGoaway();
  handler->expectError([&] (const HTTPException& err) {
      EXPECT_TRUE(err.hasProxygenError());
      EXPECT_EQ(err.getProxygenError(), kErrorStreamUnacknowledged);
      ASSERT_EQ(
        folly::to<std::string>("StreamUnacknowledged on transaction id: ",
                               handler->txn_->getID(),
                               " with codec error: PROTOCOL_ERROR"),
        std::string(err.what()));
    });
  handler->expectDetachTransaction([this] {
      // Make sure the session can't create any more transactions.
      MockHTTPHandler handler2;
      EXPECT_EQ(httpSession_->newTransaction(&handler2), nullptr);
    });

  // Send the GET request
  handler->sendRequest();

  // Receive GOAWAY frame while waiting for SYN_REPLY
  readAndLoop(goawayFrame->data(), goawayFrame->length());

  // Session will delete itself after the abort
}

TEST_F(SPDY3UpstreamSessionTest, TestUnderLimitOnWriteError) {
  InSequence enforceOrder;
  auto handler = openTransaction();

  auto req = getPostRequest();
  handler->txn_->sendHeaders(req);
  // pause writes
  pauseWrites_ = true;
  handler->expectEgressPaused();

  // send body
  handler->txn_->sendBody(makeBuf(70000));
  eventBase_.loopOnce();

  // but no expectEgressResumed
  handler->expectError();
  handler->expectDetachTransaction();
  failWrites_ = true;
  resumeWrites();

  this->eventBase_.loop();
  EXPECT_EQ(this->sessionDestroyed_, true);
}

TEST_F(SPDY3UpstreamSessionTest, TestOverlimitResume) {
  InSequence enforceOrder;
  auto handler1 = openTransaction();
  auto handler2 = openTransaction();

  // Disable stream flow control for this test
  handler1->txn_->onIngressWindowUpdate(80000);
  handler2->txn_->onIngressWindowUpdate(80000);

  auto req = getPostRequest();
  handler1->txn_->sendHeaders(req);
  handler2->txn_->sendHeaders(req);
  // pause writes
  pauseWrites_ = true;
  handler1->expectEgressPaused();
  handler2->expectEgressPaused();

  // send body
  handler1->txn_->sendBody(makeBuf(70000));
  handler2->txn_->sendBody(makeBuf(70000));
  eventBase_.loopOnce();

  // when this handler is resumed, re-pause the pipe
  handler1->expectEgressResumed([&] {
      handler1->txn_->sendBody(makeBuf(70000));
    });
  // handler2 will get a shot
  handler2->expectEgressResumed();

  // both handlers will be paused
  handler1->expectEgressPaused();
  handler2->expectEgressPaused();
  resumeWrites();

  // They both get resumed
  handler1->expectEgressResumed([&] { handler1->txn_->sendEOM(); });
  handler2->expectEgressResumed([&] { handler2->txn_->sendEOM(); });

  this->eventBase_.loop();

  // less than graceful shutdown
  handler1->expectError();
  handler1->expectDetachTransaction();
  handler2->expectError();
  handler2->expectDetachTransaction();

  httpSession_->dropConnection();
  EXPECT_EQ(this->sessionDestroyed_, true);
}

TEST_F(HTTP2UpstreamSessionTest, TestPriority) {
  InSequence enforceOrder;
  // virtual priority node with pri=8
  auto priGroupID = httpSession_->sendPriority({0, false, 7});
  auto handler1 = openTransaction();
  auto handler2 = openTransaction();

  auto req = getGetRequest();
  // send request with maximal weight
  req.setHTTP2Priority(HTTPMessage::HTTPPriority(0, false, 255));
  handler1->sendRequest(req);
  handler2->sendRequest(req);

  auto id = handler1->txn_->getID();
  auto id2 = handler2->txn_->getID();

  // Insert depth is 1, only root node has depth 0
  EXPECT_EQ(std::get<0>(handler1->txn_->getPrioritySummary()), 1);
  EXPECT_EQ(handler1->txn_->getPriorityFallback(), false);

  // update handler to be in the pri-group
  handler1->txn_->updateAndSendPriority(
    http2::PriorityUpdate{(uint32_t)priGroupID, false, 15});
  handler2->txn_->updateAndSendPriority(
    http2::PriorityUpdate{(uint32_t)priGroupID + 254, false, 15});

  // Change pri-group weight to max
  httpSession_->sendPriority(priGroupID, http2::PriorityUpdate{0, false, 255});
  eventBase_.loop();

  auto serverCodec = makeServerCodec();
  NiceMock<MockHTTPCodecCallback> callbacks;
  serverCodec->setCallback(&callbacks);
  EXPECT_CALL(callbacks, onPriority(priGroupID,
                                    HTTPMessage::HTTPPriority(0, false, 7)));
  EXPECT_CALL(callbacks, onHeadersComplete(id, _))
    .WillOnce(Invoke([&] (HTTPCodec::StreamID,
                          std::shared_ptr<HTTPMessage> msg) {
          EXPECT_EQ(*(msg->getHTTP2Priority()),
                    HTTPMessage::HTTPPriority(0, false, 255));
        }));
  EXPECT_CALL(callbacks, onHeadersComplete(id2, _))
    .WillOnce(Invoke([&] (HTTPCodec::StreamID,
                          std::shared_ptr<HTTPMessage> msg) {
          EXPECT_EQ(*(msg->getHTTP2Priority()),
                    HTTPMessage::HTTPPriority(0, false, 255));
          }));
  EXPECT_CALL(callbacks,
              onPriority(
                id, HTTPMessage::HTTPPriority(priGroupID, false, 15)));
  EXPECT_CALL(callbacks,
              onPriority(
                id2, HTTPMessage::HTTPPriority(priGroupID + 254, false, 15)));
  EXPECT_EQ(handler1->txn_->getPriorityFallback(), false);
  EXPECT_EQ(handler2->txn_->getPriorityFallback(), false);

  EXPECT_EQ(std::get<1>(handler1->txn_->getPrioritySummary()), 2);
  // created virtual parent node
  EXPECT_EQ(std::get<1>(handler2->txn_->getPrioritySummary()), 2);
  EXPECT_CALL(callbacks,
              onPriority(priGroupID, HTTPMessage::HTTPPriority(0, false, 255)));
  parseOutput(*serverCodec);
  eventBase_.loop();

  handler1->expectError();
  handler1->expectDetachTransaction();
  handler2->expectError();
  handler2->expectDetachTransaction();
  httpSession_->dropConnection();
  eventBase_.loop();
  EXPECT_EQ(sessionDestroyed_, true);
}

TEST_F(HTTP2UpstreamSessionTest, TestSettingsAck) {
  auto serverCodec = makeServerCodec();
  folly::IOBufQueue buf{IOBufQueue::cacheChainLength()};
  serverCodec->generateSettings(buf);
  auto settingsFrame = buf.move();
  settingsFrame->coalesce();

  InSequence enforceOrder;

  NiceMock<MockHTTPCodecCallback> callbacks;
  serverCodec->setCallback(&callbacks);
  EXPECT_CALL(callbacks, onSettings(_));
  EXPECT_CALL(callbacks, onSettingsAck());

  readAndLoop(settingsFrame.get());
  parseOutput(*serverCodec);
  httpSession_->dropConnection();
  EXPECT_EQ(sessionDestroyed_, true);
}

TEST_F(HTTP2UpstreamSessionTest, TestSettingsInfoCallbacks) {
  auto serverCodec = makeServerCodec();

  folly::IOBufQueue settingsBuf{IOBufQueue::cacheChainLength()};
  serverCodec->generateSettings(settingsBuf);
  auto settingsFrame = settingsBuf.move();

  folly::IOBufQueue settingsAckBuf{IOBufQueue::cacheChainLength()};
  serverCodec->generateSettingsAck(settingsAckBuf);
  auto settingsAckFrame = settingsAckBuf.move();

  MockHTTPSessionInfoCallback infoCb;
  httpSession_->setInfoCallback(&infoCb);

  EXPECT_CALL(infoCb, onRead(_, _)).Times(2);
  EXPECT_CALL(infoCb, onWrite(_, _)).Times(1);
  EXPECT_CALL(infoCb, onDestroy(_)).Times(1);

  EXPECT_CALL(infoCb, onSettings(_, _)).Times(1);
  EXPECT_CALL(infoCb, onSettingsAck(_)).Times(1);

  InSequence enforceOrder;

  readAndLoop(settingsFrame.get());
  readAndLoop(settingsAckFrame.get());

  httpSession_->destroy();
}

TEST_F(HTTP2UpstreamSessionTest, TestSetControllerInitHeaderIndexingStrat) {
  StrictMock<MockUpstreamController> mockController;
  HeaderIndexingStrategy testH2IndexingStrat;
  EXPECT_CALL(mockController, getHeaderIndexingStrategy())
    .WillOnce(
      Return(&testH2IndexingStrat)
  );

  httpSession_->setController(&mockController);

  auto handler = openTransaction();
  handler->expectDetachTransaction();

  const HTTP2Codec* h2Codec = static_cast<const HTTP2Codec*>(
    &handler->txn_->getTransport().getCodec());
  EXPECT_EQ(h2Codec->getHeaderIndexingStrategy(), &testH2IndexingStrat);

  handler->txn_->sendAbort();
  eventBase_.loop();

  EXPECT_CALL(mockController, detachSession(_));
  httpSession_->destroy();
}

/*
 * The sequence of streams are generated in the following order:
 * - [client --> server] setup the control stream (getGetRequest())
 * - [server --> client] respond to 1st stream (OK, without EOM)
 * - [server --> client] request 2nd stream (pub, EOM)
 * - [client --> server] abort the 2nd stream
 * - [server --> client] respond to 1st stream (EOM)
 */

TEST_F(HTTP2UpstreamSessionTest, ExheaderFromServer) {
  folly::IOBufQueue queue{IOBufQueue::cacheChainLength()};

  // generate enable_ex_headers setting
  auto serverCodec = makeServerCodec();
  enableExHeader(serverCodec.get());
  serverCodec->generateSettings(queue);
  // generate the response for control stream, but EOM
  auto cStreamId = HTTPCodec::StreamID(1);
  serverCodec->generateHeader(queue, cStreamId, getResponse(200, 0),
                              false, nullptr);
  // generate a request from server, encapsulated in EX_HEADERS frame
  serverCodec->generateExHeader(queue, 2, getGetRequest("/messaging"),
                                cStreamId, true, nullptr);
  serverCodec->generateEOM(queue, 1);

  auto cHandler = openTransaction();
  cHandler->sendRequest(getGetRequest("/cc"));

  NiceMock<MockHTTPHandler> pubHandler;
  InSequence handlerSequence;
  cHandler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(200, msg->getStatusCode());
    });

  EXPECT_CALL(*cHandler, onExTransaction(_))
    .WillOnce(Invoke([&pubHandler] (HTTPTransaction* pubTxn) {
          pubTxn->setHandler(&pubHandler);
          pubHandler.txn_ = pubTxn;
        }));
  pubHandler.expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(msg->getPath(), "/messaging");
    });
  pubHandler.expectEOM([&] () {
      pubHandler.txn_->sendAbort();
    });
  pubHandler.expectDetachTransaction();

  cHandler->expectEOM();
  cHandler->expectDetachTransaction();

  auto buf = queue.move();
  buf->coalesce();
  readAndLoop(buf.get());

  httpSession_->destroy();
}

TEST_F(HTTP2UpstreamSessionTest, InvalidControlStream) {
  folly::IOBufQueue queue{IOBufQueue::cacheChainLength()};

  // generate enable_ex_headers setting
  auto serverCodec = makeServerCodec();
  enableExHeader(serverCodec.get());
  serverCodec->generateSettings(queue);
  // generate the response for control stream, but EOM
  auto cStreamId = HTTPCodec::StreamID(1);
  serverCodec->generateHeader(queue, cStreamId, getResponse(200, 0),
                              false, nullptr);
  // generate a EX_HEADERS frame with non-existing control stream
  serverCodec->generateExHeader(queue, 2, getGetRequest("/messaging"),
                                cStreamId + 2, true, nullptr);
  serverCodec->generateEOM(queue, 1);

  auto cHandler = openTransaction();
  cHandler->sendRequest(getGetRequest("/cc"));

  InSequence handlerSequence;
  cHandler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(200, msg->getStatusCode());
    });
  EXPECT_CALL(*cHandler, onExTransaction(_)).Times(0);
  cHandler->expectEOM();
  cHandler->expectDetachTransaction();

  auto buf = queue.move();
  buf->coalesce();
  readAndLoop(buf.get());

  httpSession_->destroy();
}

class HTTP2UpstreamSessionWithVirtualNodesTest:
  public HTTPUpstreamTest<MockHTTPCodecPair> {
 public:
  void SetUp() override {
    auto codec = std::make_unique<NiceMock<MockHTTPCodec>>();
    codecPtr_ = codec.get();
    EXPECT_CALL(*codec, supportsParallelRequests())
      .WillRepeatedly(Return(true));
    EXPECT_CALL(*codec, getTransportDirection())
      .WillRepeatedly(Return(TransportDirection::UPSTREAM));
    EXPECT_CALL(*codec, getProtocol())
      .WillRepeatedly(Return(CodecProtocol::HTTP_2));
    EXPECT_CALL(*codec, setCallback(_))
      .WillRepeatedly(SaveArg<0>(&codecCb_));
    EXPECT_CALL(*codec, createStream())
      .WillRepeatedly(Invoke([&] {
            auto ret = nextOutgoingTxn_;
            nextOutgoingTxn_ += 2;
            return ret;
          }));
    commonSetUp(std::move(codec));
  }

  void commonSetUp(unique_ptr<HTTPCodec> codec) {
    HTTPSession::setDefaultReadBufferLimit(65536);
    HTTPSession::setDefaultWriteBufferLimit(65536);
    EXPECT_CALL(*transport_, writeChain(_, _, _))
      .WillRepeatedly(Invoke(
            this,
            &HTTPUpstreamTest<MockHTTPCodecPair>::onWriteChain));
    EXPECT_CALL(*transport_, setReadCB(_))
      .WillRepeatedly(SaveArg<0>(&readCallback_));
    EXPECT_CALL(*transport_, getReadCB())
      .WillRepeatedly(Return(readCallback_));
    EXPECT_CALL(*transport_, getEventBase())
      .WillRepeatedly(Return(&eventBase_));
    EXPECT_CALL(*transport_, good())
      .WillRepeatedly(ReturnPointee(&transportGood_));
    EXPECT_CALL(*transport_, closeNow())
      .WillRepeatedly(Assign(&transportGood_, false));
    AsyncTransportWrapper::UniquePtr transportPtr(transport_);
    httpSession_ = new HTTPUpstreamSession(
      transactionTimeouts_.get(),
      std::move(transportPtr),
      localAddr_, peerAddr_,
      std::move(codec),
      mockTransportInfo_,
      this,
      level_,
      builder_);
    eventBase_.loop();
    ASSERT_EQ(this->sessionDestroyed_, false);
  }

  void TearDown() override {
    EXPECT_TRUE(sessionDestroyed_);
  }

 protected:
  MockHTTPCodec* codecPtr_{nullptr};
  HTTPCodec::Callback* codecCb_{nullptr};
  uint32_t nextOutgoingTxn_{1};
  std::vector<HTTPCodec::StreamID> dependencies;
  uint8_t level_{3};
  std::shared_ptr<TestPriorityMapBuilder> builder_;
};

TEST_F(HTTP2UpstreamSessionWithVirtualNodesTest, VirtualNodes) {
  InSequence enforceOrder;

  HTTPCodec::StreamID deps[] = {11, 13, 15};
  EXPECT_CALL(*codecPtr_, addPriorityNodes(_, _, _))
    .Times(1)
    .WillOnce(Invoke([&] (
            HTTPCodec::PriorityQueue&,
            folly::IOBufQueue&,
            uint8_t maxLevel) {
          for (size_t i = 0; i < maxLevel; i++) {
            dependencies.push_back(deps[i]);
          }
          return 123;
        }));
  httpSession_->startNow();

  EXPECT_EQ(level_, dependencies.size());
  StrictMock<MockHTTPHandler> handler;
  handler.expectTransaction();
  auto txn = httpSession_->newTransaction(&handler);

  EXPECT_CALL(*codecPtr_, mapPriorityToDependency(_))
    .Times(1)
    .WillOnce(Invoke([&] (uint8_t priority) {
            return dependencies[priority];
          }));
  txn->updateAndSendPriority(0);

  handler.expectError();
  handler.expectDetachTransaction();
  httpSession_->dropConnection();

  eventBase_.loop();
}

class HTTP2UpstreamSessionWithPriorityTree:
    public HTTP2UpstreamSessionWithVirtualNodesTest {
public:
  HTTP2UpstreamSessionWithPriorityTree() {
    builder_ = std::make_shared<TestPriorityMapBuilder>();
  }
};

TEST_F(HTTP2UpstreamSessionWithPriorityTree, PriorityTree) {
  InSequence enforceOrder;

  std::array<HTTPCodec::StreamID, 3> deps = { {11, 13, 15} };
  EXPECT_CALL(*codecPtr_, addPriorityNodes(_, _, _))
    .Times(0)
    .WillOnce(Invoke([&] (
            HTTPCodec::PriorityQueue&,
            folly::IOBufQueue&,
            uint8_t maxLevel) {
          for (size_t i = 0; i < maxLevel; i++) {
            dependencies.push_back(deps[i]);
          }
          return 123;
        }));
  httpSession_->startNow();

  // It should have built the virtual streams from the tree but not the old
  // priority levels.
  EXPECT_EQ(dependencies.size(), 0);
  HTTPMessage::HTTPPriority hiPri =
      *httpSession_->getHTTPPriority(builder_->hiPriLevel_);
  EXPECT_EQ(std::get<2>(hiPri), builder_->hiPriWeight_);
  HTTPMessage::HTTPPriority loPri =
      *httpSession_->getHTTPPriority(builder_->loPriLevel_);
  EXPECT_EQ(std::get<2>(loPri), builder_->loPriWeight_);

  for (size_t level = 0; level < 256; ++level) {
    if (level == builder_->hiPriLevel_) {
      continue;
    }
    HTTPMessage::HTTPPriority pri = *httpSession_->getHTTPPriority(level);
    EXPECT_EQ(pri, loPri);
  }

  StrictMock<MockHTTPHandler> handler;
  handler.expectTransaction();
  auto txn = httpSession_->newTransaction(&handler);

  txn->updateAndSendPriority(0);

  handler.expectError();
  handler.expectDetachTransaction();
  httpSession_->dropConnection();

  eventBase_.loop();
}

TYPED_TEST_P(HTTPUpstreamTest, ImmediateEof) {
  // Receive an EOF without any request data
  this->readCallback_->readEOF();
  this->eventBase_.loop();
  EXPECT_EQ(this->sessionDestroyed_, true);
}

template <class CodecPair>
void HTTPUpstreamTest<CodecPair>::testBasicRequest() {
  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_TRUE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest();
  readAndLoop("HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n\r\n"
              "0\r\n\r\n");

  CHECK(httpSession_->supportsMoreTransactions());
  CHECK_EQ(httpSession_->getNumOutgoingStreams(), 0);
}

TEST_F(HTTPUpstreamSessionTest, BasicRequest) {
  testBasicRequest();
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, TwoRequests) {
  testBasicRequest();
  testBasicRequest();
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, 10Requests) {
  for (uint16_t i = 0; i < 10; i++) {
    testBasicRequest();
  }
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, TestFirstHeaderByteEventTracker) {
  auto byteEventTracker = setMockByteEventTracker();

  EXPECT_CALL(*byteEventTracker, addFirstHeaderByteEvent(_, _))
      .WillOnce(Invoke([] (uint64_t /*byteNo*/,
                           HTTPTransaction* txn) {
                         txn->incrementPendingByteEvents();
                       }));

  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_TRUE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest();
  readAndLoop("HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n\r\n"
              "0\r\n\r\n");

  CHECK(httpSession_->supportsMoreTransactions());
  CHECK_EQ(httpSession_->getNumOutgoingStreams(), 0);
  handler->txn_->decrementPendingByteEvents();
  httpSession_->destroy();
}

template <class CodecPair>
void HTTPUpstreamTest<CodecPair>::testBasicRequestHttp10(bool keepalive) {
  HTTPMessage req = getGetRequest();
  req.setHTTPVersion(1, 0);
  if (keepalive) {
    req.getHeaders().set(HTTP_HEADER_CONNECTION, "Keep-Alive");
  }

  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(200, msg->getStatusCode());
      EXPECT_EQ(keepalive ? "keep-alive" : "close",
                msg->getHeaders().getSingleOrEmpty(HTTP_HEADER_CONNECTION));
    });
  EXPECT_CALL(*handler, onBody(_));
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest();
  if (keepalive) {
    readAndLoop("HTTP/1.0 200 OK\r\n"
                "Connection: keep-alive\r\n"
                "Content-length: 7\r\n\r\n"
                "content");
  } else {
    readAndLoop("HTTP/1.0 200 OK\r\n"
                "Connection: close\r\n"
                "Content-length: 7\r\n\r\n"
                "content");
  }
}

TEST_F(HTTPUpstreamSessionTest, Http10Keepalive) {
  testBasicRequestHttp10(true);
  testBasicRequestHttp10(false);
}

TEST_F(HTTPUpstreamSessionTest, BasicTrailers) {
  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_TRUE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  EXPECT_CALL(*handler, onTrailers(_));
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest();
  readAndLoop("HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n\r\n"
              "0\r\n"
              "X-Trailer1: foo\r\n"
              "\r\n");

  CHECK(httpSession_->supportsMoreTransactions());
  CHECK_EQ(httpSession_->getNumOutgoingStreams(), 0);
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, TwoRequestsWithPause) {
  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_TRUE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });

  handler->expectEOM([&] () { handler->txn_->pauseIngress(); });
  handler->expectDetachTransaction();

  handler->sendRequest();
  readAndLoop("HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n\r\n"
              "0\r\n\r\n");

  // Even though the previous transaction paused ingress just before it
  // finished up, reads resume automatically when the number of
  // transactions goes to zero. This way, the second request can read
  // without having to call resumeIngress()
  testBasicRequest();
  httpSession_->destroy();
}

using HTTPUpstreamTimeoutTest = TimeoutableHTTPUpstreamTest<HTTP1xCodecPair>;
TEST_F(HTTPUpstreamTimeoutTest, WriteTimeoutAfterResponse) {
  // Test where the upstream session times out while writing the request
  // to the server, but after the server has already sent back a full
  // response.
  pauseWrites_ = true;
  HTTPMessage req = getPostRequest();

  InSequence enforceOrder;
  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_TRUE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();
  handler->expectError([&] (const HTTPException& err) {
      EXPECT_TRUE(err.hasProxygenError());
      ASSERT_EQ(err.getDirection(),
                HTTPException::Direction::INGRESS_AND_EGRESS);
      EXPECT_EQ(err.getProxygenError(), kErrorWriteTimeout);
      ASSERT_EQ(
        folly::to<std::string>("WriteTimeout on transaction id: ",
                               handler->txn_->getID()),
        std::string(err.what()));
    });
  handler->expectDetachTransaction();

  handler->txn_->sendHeaders(req);
  // Don't send the body, but get a response immediately
  readAndLoop("HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n\r\n"
              "0\r\n\r\n");
}

TEST_F(HTTPUpstreamSessionTest, SetTransactionTimeout) {
  // Test that setting a new timeout on the transaction will cancel
  // the old one.
  auto handler = openTransaction();
  handler->expectDetachTransaction();

  EXPECT_TRUE(handler->txn_->hasIdleTimeout());
  handler->txn_->setIdleTimeout(std::chrono::milliseconds(747));
  EXPECT_TRUE(handler->txn_->hasIdleTimeout());
  EXPECT_TRUE(handler->txn_->isScheduled());
  EXPECT_EQ(transactionTimeouts_->count(), 1);
  handler->txn_->sendAbort();
  eventBase_.loop();
}

TEST_F(HTTPUpstreamSessionTest, ReadTimeout) {
  NiceMock<MockUpstreamController> controller;
  httpSession_->setController(&controller);
  auto cm = wangle::ConnectionManager::makeUnique(
    &eventBase_, std::chrono::milliseconds(50));
  cm->addConnection(httpSession_, true);
  eventBase_.loop();
}

TEST_F(HTTPUpstreamSessionTest, 100ContinueKeepalive) {
  // Test a request with 100 continue on a keepalive connection. Then make
  // another request.
  HTTPMessage req = getGetRequest();
  req.getHeaders().set(HTTP_HEADER_EXPECT, "100-continue");

  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(100, msg->getStatusCode());
    });
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_TRUE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest(req);
  readAndLoop("HTTP/1.1 100 Continue\r\n\r\n"
              "HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n\r\n"
              "0\r\n\r\n");

  // Now make sure everything still works
  testBasicRequest();
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, 417Keepalive) {
  // Test a request with 100 continue on a keepalive connection. Then make
  // another request after the expectation fails.
  HTTPMessage req = getGetRequest();
  req.getHeaders().set(HTTP_HEADER_EXPECT, "100-continue");

  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(417, msg->getStatusCode());
    });
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest(req);
  readAndLoop("HTTP/1.1 417 Expectation Failed\r\n"
              "Content-Length: 0\r\n\r\n");

  // Now make sure everything still works
  testBasicRequest();
  EXPECT_FALSE(sessionDestroyed_);
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, 101Upgrade) {
  // Test an upgrade request with sending 101 response. Then send
  // some data and check the onBody callback contents
  HTTPMessage req = getGetRequest();
  req.getHeaders().set(HTTP_HEADER_UPGRADE, "http/2.0");

  InSequence enforceOrder;

  auto handler = openTransaction();
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsChunked());
      EXPECT_EQ(101, msg->getStatusCode());
    });
  EXPECT_CALL(*handler, onUpgrade(_));
  EXPECT_CALL(*handler, onBody(_))
    .WillOnce(ExpectString("Test Body\r\n"));
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest(req);
  eventBase_.loop();
  readAndLoop("HTTP/1.1 101 Switching Protocols\r\n"
              "Upgrade: http/2.0\r\n\r\n"
              "Test Body\r\n");
  readCallback_->readEOF();
  eventBase_.loop();

  CHECK_EQ(httpSession_->getNumOutgoingStreams(), 0);
  httpSession_->destroy();
}

// ===== Upgrade Tests ====

template <class CodecPair>
void HTTPUpstreamTest<CodecPair>::testSimpleUpgrade(
  const std::string& upgradeReqHeader,
  const std::string& upgradeRespHeader,
  CodecProtocol respCodecVersion) {
  InSequence dummy;
  auto handler = openTransaction();
  NiceMock<MockUpstreamController> controller;

  httpSession_->setController(&controller);
  EXPECT_CALL(controller, onSessionCodecChange(httpSession_));

  EXPECT_EQ(httpSession_->getMaxConcurrentOutgoingStreams(), 1);

  HeaderIndexingStrategy testH2IndexingStrat;
  if (respCodecVersion == CodecProtocol::HTTP_2) {
    EXPECT_CALL(controller, getHeaderIndexingStrategy())
      .WillOnce(
        Return(&testH2IndexingStrat)
    );
  }

  handler->expectHeaders([] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();

  auto txn = handler->txn_;
  HTTPMessage req = getUpgradeRequest(upgradeReqHeader);
  txn->sendHeaders(req);
  txn->sendEOM();
  eventBase_.loopOnce(); // force HTTP/1.1 writes
  writes_.move(); // clear them out
  readAndLoop(folly::to<string>("HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: ", upgradeRespHeader, "\r\n"
                                "\r\n"));

  if (respCodecVersion == CodecProtocol::HTTP_2) {
    const HTTP2Codec* codec = dynamic_cast<const HTTP2Codec*>(
      &txn->getTransport().getCodec());
    ASSERT_NE(codec, nullptr);
    EXPECT_EQ(codec->getHeaderIndexingStrategy(), &testH2IndexingStrat);
  }

  readAndLoop(getResponseBuf(respCodecVersion, txn->getID(), 200, 100).get());

  EXPECT_EQ(httpSession_->getMaxConcurrentOutgoingStreams(), 10);
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, HttpUpgradeNativeH2) {
  testSimpleUpgrade("h2c", "h2c", CodecProtocol::HTTP_2);
}

// Upgrade to SPDY/3.1 with a non-native proto in the list
TEST_F(HTTPUpstreamSessionTest, HttpUpgradeNativeUnknown) {
  testSimpleUpgrade("blarf, h2c", "h2c", CodecProtocol::HTTP_2);
}

// Upgrade header with extra whitespace
TEST_F(HTTPUpstreamSessionTest, HttpUpgradeNativeWhitespace) {
  testSimpleUpgrade("blarf, \th2c\t, xyz", "h2c",
                    CodecProtocol::HTTP_2);
}

// Upgrade header with random junk
TEST_F(HTTPUpstreamSessionTest, HttpUpgradeNativeJunk) {
  testSimpleUpgrade(",,,,   ,,\t~^%$(*&@(@$^^*(,h2c", "h2c",
                    CodecProtocol::HTTP_2);
}

TEST_F(HTTPUpstreamSessionTest, HttpUpgrade101Unexpected) {
  InSequence dummy;
  auto handler = openTransaction();

  EXPECT_CALL(*handler, onError(_));
  handler->expectDetachTransaction();

  handler->sendRequest();
  eventBase_.loop();
  readAndLoop(folly::to<string>("HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: spdy/3\r\n"
                                "\r\n"));
  EXPECT_EQ(readCallback_, nullptr);
  EXPECT_TRUE(sessionDestroyed_);
}

TEST_F(HTTPUpstreamSessionTest, HttpUpgrade101MissingUpgrade) {
  InSequence dummy;
  auto handler = openTransaction();

  EXPECT_CALL(*handler, onError(_));
  handler->expectDetachTransaction();

  handler->sendRequest(getUpgradeRequest("spdy/3"));
  readAndLoop(folly::to<string>("HTTP/1.1 101 Switching Protocols\r\n"
                                "\r\n"));
  EXPECT_EQ(readCallback_, nullptr);
  EXPECT_TRUE(sessionDestroyed_);
}

TEST_F(HTTPUpstreamSessionTest, HttpUpgrade101BogusHeader) {
  InSequence dummy;
  auto handler = openTransaction();

  EXPECT_CALL(*handler, onError(_));
  handler->expectDetachTransaction();

  handler->sendRequest(getUpgradeRequest("spdy/3"));
  eventBase_.loop();
  readAndLoop(folly::to<string>("HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: blarf\r\n"
                                "\r\n"));
  EXPECT_EQ(readCallback_, nullptr);
  EXPECT_TRUE(sessionDestroyed_);
}

TEST_F(HTTPUpstreamSessionTest, HttpUpgradePost100) {
  InSequence dummy;
  auto handler = openTransaction();

  handler->expectHeaders([] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(100, msg->getStatusCode());
    });
  handler->expectHeaders([] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();

  auto txn = handler->txn_;
  HTTPMessage req = getUpgradePostRequest(100, "h2c", true /* 100 */);
  txn->sendHeaders(req);
  auto buf = makeBuf(100);
  txn->sendBody(std::move(buf));
  txn->sendEOM();
  eventBase_.loop();
  readAndLoop(folly::to<string>("HTTP/1.1 100 Continue\r\n"
                                "\r\n"
                                "HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: h2c\r\n"
                                "\r\n"));
  readAndLoop(
    getResponseBuf(CodecProtocol::HTTP_2, txn->getID(), 200, 100).get());
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, HttpUpgradePost100Http2) {
  InSequence dummy;
  auto handler = openTransaction();

  handler->expectHeaders([] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(100, msg->getStatusCode());
    });
  handler->expectHeaders([] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();

  auto txn = handler->txn_;
  HTTPMessage req = getUpgradePostRequest(100, "h2c");
  txn->sendHeaders(req);
  auto buf = makeBuf(100);
  txn->sendBody(std::move(buf));
  txn->sendEOM();
  eventBase_.loop();
  readAndLoop(folly::to<string>("HTTP/1.1 101 Switching Protocols\r\n"
                                "Upgrade: h2c\r\n"
                                "\r\n"));
  readAndLoop(getResponseBuf(CodecProtocol::HTTP_2,
                             txn->getID(), 200, 100, true).get());
  httpSession_->destroy();
}

TEST_F(HTTPUpstreamSessionTest, HttpUpgradeOnTxn2) {
  InSequence dummy;
  auto handler1 = openTransaction();

  handler1->expectHeaders([] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler1->expectBody();
  handler1->expectEOM();
  handler1->expectDetachTransaction();

  auto txn = handler1->txn_;
  HTTPMessage req = getUpgradeRequest("spdy/3");
  txn->sendHeaders(req);
  txn->sendEOM();
  readAndLoop("HTTP/1.1 200 Ok\r\n"
              "Content-Length: 10\r\n"
              "\r\n"
              "abcdefghij");
  eventBase_.loop();

  auto handler2 = openTransaction();

  txn = handler2->txn_;
  txn->sendHeaders(req);
  txn->sendEOM();

  handler2->expectHeaders();
  handler2->expectEOM();
  handler2->expectDetachTransaction();
  readAndLoop("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
  httpSession_->destroy();
}


class HTTPUpstreamRecvStreamTest : public HTTPUpstreamSessionTest {
 public:
  HTTPUpstreamRecvStreamTest()
      : HTTPUpstreamTest({100000, 105000, 110000}) {}
};

TEST_F(HTTPUpstreamRecvStreamTest, UpgradeFlowControl) {
  InSequence dummy;
  testSimpleUpgrade("h2c", "h2c", CodecProtocol::HTTP_2);

  HTTP2Codec serverCodec(TransportDirection::DOWNSTREAM);
  NiceMock<MockHTTPCodecCallback> callbacks;
  serverCodec.setCallback(&callbacks);
  EXPECT_CALL(callbacks, onSettings(_))
    .WillOnce(Invoke([this] (const SettingsList& settings) {
          if (flowControl_[0] > 0) {
            for (const auto& setting: settings) {
              if (setting.id == SettingsId::INITIAL_WINDOW_SIZE) {
                EXPECT_EQ(flowControl_[0], setting.value);
              }
            }
          }
        }));
  EXPECT_CALL(callbacks, onWindowUpdate(0, flowControl_[2] -
                                        serverCodec.getDefaultWindowSize()));
  size_t initWindow = flowControl_[0] > 0 ?
    flowControl_[0] : serverCodec.getDefaultWindowSize();
  EXPECT_CALL(callbacks, onWindowUpdate(1, flowControl_[1] - initWindow));
  parseOutput(serverCodec);
}

class NoFlushUpstreamSessionTest: public HTTPUpstreamTest<SPDY3CodecPair> {
 public:
  void onWriteChain(folly::AsyncTransportWrapper::WriteCallback* callback,
                    std::shared_ptr<IOBuf>,
                    WriteFlags) override {
    if (!timesCalled_++) {
      callback->writeSuccess();
    } else {
      cbs_.push_back(callback);
    }
    // do nothing -- let unacked egress build up
  }

  ~NoFlushUpstreamSessionTest() override {
    AsyncSocketException ex(AsyncSocketException::UNKNOWN, "");
    for (auto& cb : cbs_) {
      cb->writeErr(0, ex);
    }
  }
 private:
  uint32_t timesCalled_{0};
  std::vector<folly::AsyncTransportWrapper::WriteCallback*> cbs_;
};

TEST_F(NoFlushUpstreamSessionTest, SessionPausedStartPaused) {
  // If the session is paused, new upstream transactions should start
  // paused too.
  HTTPMessage req = getGetRequest();

  InSequence enforceOrder;

  auto handler1 = openNiceTransaction();
  handler1->txn_->sendHeaders(req);
  Mock::VerifyAndClearExpectations(handler1.get());
  // The session pauses all txns since no writeSuccess for too many bytes
  handler1->expectEgressPaused();
  // Send a body big enough to pause egress
  handler1->txn_->sendBody(makeBuf(httpSession_->getWriteBufferLimit()));
  eventBase_.loop();
  Mock::VerifyAndClearExpectations(handler1.get());

  auto handler2 = openNiceTransaction(true /* expect start paused */);
  eventBase_.loop();
  Mock::VerifyAndClearExpectations(handler2.get());

  httpSession_->dropConnection();
}

TEST_F(NoFlushUpstreamSessionTest, DeleteTxnOnUnpause) {
  // Test where the handler gets onEgressPaused() and triggers another
  // HTTPSession call to iterate over all transactions (to ensure nested
  // iteration works).

  HTTPMessage req = getGetRequest();

  InSequence enforceOrder;

  auto handler1 = openNiceTransaction();
  auto handler2 = openNiceTransaction();
  auto handler3 = openNiceTransaction();
  handler2->expectEgressPaused([this] {
      // This time it is invoked by the session on all transactions
      httpSession_->dropConnection();
    });
  handler2->txn_->sendHeaders(req);
  // This happens when the body write fills the txn egress queue
  // Send a body big enough to pause egress
  handler2->txn_->onIngressWindowUpdate(100);
  handler2->txn_->sendBody(makeBuf(httpSession_->getWriteBufferLimit() + 1));
  eventBase_.loop();
}

class MockHTTPUpstreamTest: public HTTPUpstreamTest<MockHTTPCodecPair> {
 public:
  void SetUp() override {
    auto codec = std::make_unique<NiceMock<MockHTTPCodec>>();
    codecPtr_ = codec.get();
    EXPECT_CALL(*codec, supportsParallelRequests())
      .WillRepeatedly(Return(true));
    EXPECT_CALL(*codec, getTransportDirection())
      .WillRepeatedly(Return(TransportDirection::UPSTREAM));
    EXPECT_CALL(*codec, setCallback(_))
      .WillRepeatedly(SaveArg<0>(&codecCb_));
    EXPECT_CALL(*codec, isReusable())
      .WillRepeatedly(ReturnPointee(&reusable_));
    EXPECT_CALL(*codec, isWaitingToDrain())
      .WillRepeatedly(ReturnPointee(&reusable_));
    EXPECT_CALL(*codec, getDefaultWindowSize())
      .WillRepeatedly(Return(65536));
    EXPECT_CALL(*codec, getProtocol())
      .WillRepeatedly(Return(CodecProtocol::SPDY_3_1));
    EXPECT_CALL(*codec, generateGoaway(_, _, _, _))
      .WillRepeatedly(Invoke([&] (IOBufQueue& writeBuf,
                                  HTTPCodec::StreamID lastStream,
                                  ErrorCode,
                                  std::shared_ptr<folly::IOBuf>) {
            EXPECT_LT(lastStream, std::numeric_limits<int32_t>::max());
            if (reusable_) {
              writeBuf.append("GOAWAY", 6);
              reusable_ = false;
            }
            return 6;
          }));
    EXPECT_CALL(*codec, createStream())
      .WillRepeatedly(Invoke([&] {
            auto ret = nextOutgoingTxn_;
            nextOutgoingTxn_ += 2;
            return ret;
          }));

    commonSetUp(std::move(codec));
  }

  void TearDown() override {
    EXPECT_TRUE(sessionDestroyed_);
  }

  std::unique_ptr<StrictMock<MockHTTPHandler>> openTransaction() {
    // Returns a mock handler with txn_ field set in it
    auto handler = std::make_unique<StrictMock<MockHTTPHandler>>();
    handler->expectTransaction();
    auto txn = httpSession_->newTransaction(handler.get());
    EXPECT_EQ(txn, handler->txn_);
    return handler;
  }

  MockHTTPCodec* codecPtr_{nullptr};
  HTTPCodec::Callback* codecCb_{nullptr};
  bool reusable_{true};
  uint32_t nextOutgoingTxn_{1};
};

TEST_F(HTTP2UpstreamSessionTest, ServerPush) {
  httpSession_->setEgressSettings({{SettingsId::ENABLE_PUSH, 1}});

  auto egressCodec = makeServerCodec();
  folly::IOBufQueue output(folly::IOBufQueue::cacheChainLength());

  HTTPMessage push;
  push.getHeaders().set("HOST", "www.foo.com");
  push.setURL("https://www.foo.com/");
  egressCodec->generateSettings(output);
  // PUSH_PROMISE
  egressCodec->generatePushPromise(output, 2, push, 1);

  // Pushed resource
  HTTPMessage resp;
  resp.setStatusCode(200);
  resp.getHeaders().set("ohai", "push");
  egressCodec->generateHeader(output, 2, resp);
  auto buf = makeBuf(100);
  egressCodec->generateBody(output, 2, std::move(buf), HTTPCodec::NoPadding,
                            true /* eom */);

  // Original resource
  resp.getHeaders().set("ohai", "orig");
  egressCodec->generateHeader(output, 1, resp);
  buf = makeBuf(100);
  egressCodec->generateBody(output, 1, std::move(buf), HTTPCodec::NoPadding,
                           true /* eom */);

  std::unique_ptr<folly::IOBuf> input = output.move();
  input->coalesce();

  MockHTTPHandler pushHandler;

  InSequence enforceOrder;

  auto handler = openTransaction();
  EXPECT_CALL(*handler, onPushedTransaction(_))
    .WillOnce(Invoke([&pushHandler] (HTTPTransaction* pushTxn) {
          pushTxn->setHandler(&pushHandler);
        }));
  EXPECT_CALL(pushHandler, setTransaction(_));
  pushHandler.expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(httpSession_->getNumIncomingStreams(), 1);
      EXPECT_TRUE(msg->getIsChunked());
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(msg->getPath(), "/");
      EXPECT_EQ(msg->getHeaders().getSingleOrEmpty(HTTP_HEADER_HOST),
                "www.foo.com");
    });
  pushHandler.expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_EQ(msg->getStatusCode(), 200);
      EXPECT_EQ(msg->getHeaders().getSingleOrEmpty("ohai"), "push");
    });
  pushHandler.expectBody();
  pushHandler.expectEOM();
  pushHandler.expectDetachTransaction();

  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
      EXPECT_EQ(msg->getHeaders().getSingleOrEmpty("ohai"), "orig");
    });
  handler->expectBody();
  handler->expectEOM();
  handler->expectDetachTransaction();

  handler->sendRequest();
  readAndLoop(input->data(), input->length());

  EXPECT_EQ(httpSession_->getNumIncomingStreams(), 0);
  httpSession_->destroy();
}

class MockHTTP2UpstreamTest: public MockHTTPUpstreamTest {
 public:
  void SetUp() override {
    MockHTTPUpstreamTest::SetUp();

    // This class assumes we are doing a test for SPDY or HTTP/2+ where
    // this function is *not* a no-op. Indicate this via a positive number
    // of bytes being generated for writing RST_STREAM.

    ON_CALL(*codecPtr_, generateRstStream(_, _, _))
      .WillByDefault(Return(1));
  }
};


TEST_F(MockHTTP2UpstreamTest, ParseErrorNoTxn) {
  // 1) Create streamID == 1
  // 2) Send request
  // 3) Detach handler
  // 4) Get an ingress parse error on reply
  // Expect that the codec should be asked to generate an abort on streamID==1

  // Setup the codec expectations.
  EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, _, _))
    .WillOnce(Invoke([] (folly::IOBufQueue& writeBuf, HTTPCodec::StreamID,
                         const HTTPMessage&, bool, HTTPHeaderSize*) {
                       writeBuf.append("1", 1);
                     }));
  EXPECT_CALL(*codecPtr_, generateEOM(_, _))
    .WillOnce(Return(20));
  EXPECT_CALL(*codecPtr_, generateRstStream(_, 1, _));

  // 1)
  auto handler = openTransaction();

  // 2)
  handler->sendRequest(getPostRequest());

  // 3) Note this sendAbort() doesn't destroy the txn since byte events are
  // enqueued
  handler->txn_->sendAbort();

  // 4)
  HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS, "foo");
  ex.setProxygenError(kErrorParseHeader);
  ex.setCodecStatusCode(ErrorCode::REFUSED_STREAM);
  codecCb_->onError(1, ex, true);

  // cleanup
  handler->expectDetachTransaction();
  httpSession_->dropConnection();
  eventBase_.loop();
}

TEST_F(MockHTTPUpstreamTest, 0MaxOutgoingTxns) {
  // Test where an upstream session gets a SETTINGS frame with 0 max
  // outgoing transactions. In our implementation, we jsut send a GOAWAY
  // and close the connection.

  codecCb_->onSettings({{SettingsId::MAX_CONCURRENT_STREAMS, 0}});
  EXPECT_TRUE(transactionsFull_);
  httpSession_->dropConnection();
}

TEST_F(MockHTTPUpstreamTest, OutgoingTxnSettings) {
  // Create 2 transactions, then receive a settings frame from
  // the server indicating 1 parallel transaction at a time is allowed.
  // Then get another SETTINGS frame indicating 100 max parallel
  // transactions. Expect that HTTPSession invokes both info callbacks.

  NiceMock<MockHTTPHandler> handler1;
  NiceMock<MockHTTPHandler> handler2;
  httpSession_->newTransaction(&handler1);
  httpSession_->newTransaction(&handler2);

  codecCb_->onSettings({{SettingsId::MAX_CONCURRENT_STREAMS, 1}});
  EXPECT_TRUE(transactionsFull_);
  codecCb_->onSettings({{SettingsId::MAX_CONCURRENT_STREAMS, 100}});
  EXPECT_FALSE(transactionsFull_);
  httpSession_->dropConnection();
}

TEST_F(MockHTTPUpstreamTest, IngressGoawayDrain) {
  // Tests whether the session drains existing transactions and
  // deletes itself after receiving a GOAWAY.

  InSequence enforceOrder;

  auto handler = openTransaction();
  EXPECT_CALL(*handler, onGoaway(ErrorCode::NO_ERROR));
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();
  handler->expectDetachTransaction();

  // Send the GET request
  handler->sendRequest();

  // Receive GOAWAY frame with last good stream as 1
  codecCb_->onGoaway(1, ErrorCode::NO_ERROR);

  // New transactions cannot be created afrer goaway
  EXPECT_FALSE(httpSession_->isReusable());
  EXPECT_EQ(httpSession_->newTransaction(handler.get()), nullptr);

  // Receive 200 OK
  auto resp = makeResponse(200);
  codecCb_->onMessageBegin(1, resp.get());
  codecCb_->onHeadersComplete(1, std::move(resp));
  codecCb_->onMessageComplete(1, false);
  eventBase_.loop();

  // Session will delete itself after getting the response
}

TEST_F(MockHTTPUpstreamTest, Goaway) {
  // Make sure existing txns complete successfully even if we drain the
  // upstream session
  const unsigned numTxns = 10;
  MockHTTPHandler handler[numTxns];

  InSequence enforceOrder;

  for (unsigned i = 0; i < numTxns; ++i) {
    handler[i].expectTransaction();
    handler[i].expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
        EXPECT_FALSE(msg->getIsUpgraded());
        EXPECT_EQ(200, msg->getStatusCode());
      });
    httpSession_->newTransaction(&handler[i]);

    // Send the GET request
    handler[i].sendRequest();

    // Receive 200 OK
    auto resp = makeResponse(200);
    codecCb_->onMessageBegin(handler[i].txn_->getID(), resp.get());
    codecCb_->onHeadersComplete(handler[i].txn_->getID(), std::move(resp));
  }

  codecCb_->onGoaway(numTxns * 2 + 1, ErrorCode::NO_ERROR);
  for (unsigned i = 0; i < numTxns; ++i) {
    handler[i].expectEOM();
    handler[i].expectDetachTransaction();
    codecCb_->onMessageComplete(i*2 + 1, false);
  }
  eventBase_.loop();

  // Session will delete itself after drain completes
}

TEST_F(MockHTTPUpstreamTest, GoawayPreHeaders) {
  // Make sure existing txns complete successfully even if we drain the
  // upstream session
  MockHTTPHandler handler;

  InSequence enforceOrder;

  handler.expectTransaction();
  EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, _, _))
      .WillOnce(Invoke(
          [&](IOBufQueue& writeBuf,
              HTTPCodec::StreamID /*stream*/,
              const HTTPMessage& /*msg*/,
              bool /*eom*/,
              HTTPHeaderSize* /*size*/) { writeBuf.append("HEADERS", 7); }));
  handler.expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  httpSession_->newTransaction(&handler);
  httpSession_->drain();

  // Send the GET request
  handler.sendRequest();

  // Receive 200 OK
  auto resp = makeResponse(200);
  codecCb_->onMessageBegin(handler.txn_->getID(), resp.get());
  codecCb_->onHeadersComplete(handler.txn_->getID(), std::move(resp));

  codecCb_->onGoaway(1, ErrorCode::NO_ERROR);
  handler.expectEOM();
  handler.expectDetachTransaction();
  codecCb_->onMessageComplete(1, false);
  eventBase_.loop();

  auto buf = writes_.move();
  ASSERT_TRUE(buf != nullptr);
  EXPECT_EQ(buf->moveToFbString().data(), string("HEADERSGOAWAY"));
  // Session will delete itself after drain completes
}

TEST_F(MockHTTPUpstreamTest, NoWindowUpdateOnDrain) {
  EXPECT_CALL(*codecPtr_, supportsStreamFlowControl())
    .WillRepeatedly(Return(true));

  auto handler = openTransaction();

  handler->sendRequest();
  httpSession_->drain();
  auto streamID = handler->txn_->getID();

  EXPECT_CALL(*handler, onGoaway(ErrorCode::NO_ERROR));
  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  EXPECT_CALL(*handler, onBody(_))
    .Times(3);
  handler->expectEOM();

  handler->expectDetachTransaction();

  uint32_t outstanding = 0;
  uint32_t sendWindow = 65536;
  uint32_t toSend = sendWindow * 1.55;

  // We'll get exactly one window update because we are draining
  EXPECT_CALL(*codecPtr_, generateWindowUpdate(_, _, _))
      .WillOnce(Invoke([&](folly::IOBufQueue& writeBuf,
                           HTTPCodec::StreamID /*stream*/,
                           uint32_t delta) {
        EXPECT_EQ(delta, sendWindow);
        outstanding -= delta;
        uint32_t len = std::min(toSend, sendWindow - outstanding);
        EXPECT_LT(len, sendWindow);
        toSend -= len;
        EXPECT_EQ(toSend, 0);
        eventBase_.tryRunAfterDelay(
            [this, streamID, len] {
              failWrites_ = true;
              auto respBody = makeBuf(len);
              codecCb_->onBody(streamID, std::move(respBody), 0);
              codecCb_->onMessageComplete(streamID, false);
            },
            50);

        const std::string dummy("window");
        writeBuf.append(dummy);
        return 6;
      }));

  codecCb_->onGoaway(streamID, ErrorCode::NO_ERROR);
  auto resp = makeResponse(200);
  codecCb_->onMessageBegin(streamID, resp.get());
  codecCb_->onHeadersComplete(streamID, std::move(resp));

  // While there is room and the window and body to send
  while (sendWindow - outstanding > 0 && toSend > 0) {
    // Send up to a 36k chunk
    uint32_t len = std::min(toSend, uint32_t(36000));
    // limited by the available window
    len = std::min(len, sendWindow - outstanding);
    auto respBody = makeBuf(len);
    toSend -= len;
    outstanding += len;
    codecCb_->onBody(streamID, std::move(respBody), 0);
  }

  eventBase_.loop();
}

TEST_F(MockHTTPUpstreamTest, GetWithBody) {
  // Should be allowed to send a GET request with body.
  NiceMock<MockHTTPHandler> handler;
  HTTPMessage req = getGetRequest();
  req.getHeaders().set(HTTP_HEADER_CONTENT_LENGTH, "10");

  InSequence enforceOrder;

  EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, _, _));
  EXPECT_CALL(*codecPtr_, generateBody(_, _, _, _, true));

  auto txn = httpSession_->newTransaction(&handler);
  txn->sendHeaders(req);
  txn->sendBody(makeBuf(10));
  txn->sendEOM();

  eventBase_.loop();
  httpSession_->dropConnection();
}

TEST_F(MockHTTPUpstreamTest, HeaderWithEom) {
  NiceMock<MockHTTPHandler> handler;
  HTTPMessage req = getGetRequest();
  EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, true, _));

  auto txn = httpSession_->newTransaction(&handler);
  txn->sendHeadersWithEOM(req);
  eventBase_.loop();
  EXPECT_TRUE(txn->isEgressComplete());
  httpSession_->dropConnection();
}

template <int stage>
class TestAbortPost : public MockHTTPUpstreamTest {
 public:
  void doAbortTest() {
    // Send an abort at various points while receiving the response to a GET
    // The test is broken into "stages"
    // Stage 0) headers received
    // Stage 1) chunk header received
    // Stage 2) body received
    // Stage 3) chunk complete received
    // Stage 4) trailers received
    // Stage 5) eom received
    // This test makes sure expected callbacks are received if an abort is
    // sent before any of these stages.
    InSequence enforceOrder;
    StrictMock<MockHTTPHandler> handler;
    HTTPMessage req = getPostRequest(10);

    std::unique_ptr<HTTPMessage> resp;
    std::unique_ptr<folly::IOBuf> respBody;
    std::tie(resp, respBody) = makeResponse(200, 50);

    handler.expectTransaction();
    EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, _, _));

    if (stage > 0) {
      handler.expectHeaders();
    }
    if (stage > 1) {
      EXPECT_CALL(handler, onChunkHeader(_));
    }
    if (stage > 2) {
      EXPECT_CALL(handler, onBody(_));
    }
    if (stage > 3) {
      EXPECT_CALL(handler, onChunkComplete());
    }
    if (stage > 4) {
      EXPECT_CALL(handler, onTrailers(_));
    }
    if (stage > 5) {
      handler.expectEOM();
    }

    auto txn = httpSession_->newTransaction(&handler);
    auto streamID = txn->getID();
    txn->sendHeaders(req);
    txn->sendBody(makeBuf(5)); // only send half the body

    auto doAbort = [&] {
      EXPECT_CALL(*codecPtr_, generateRstStream(_, txn->getID(), _));
      handler.expectDetachTransaction();
      const auto id = txn->getID();
      txn->sendAbort();
      EXPECT_CALL(*codecPtr_,
                  generateRstStream(_, id, ErrorCode::_SPDY_INVALID_STREAM))
        .Times(AtLeast(0));
    };

    if (stage == 0) {
      doAbort();
    }
    codecCb_->onHeadersComplete(streamID, std::move(resp));
    if (stage == 1) {
      doAbort();
    }
    codecCb_->onChunkHeader(streamID, respBody->computeChainDataLength());
    if (stage == 2) {
      doAbort();
    }
    codecCb_->onBody(streamID, std::move(respBody), 0);
    if (stage == 3) {
      doAbort();
    }
    codecCb_->onChunkComplete(streamID);
    if (stage == 4) {
      doAbort();
    }
    codecCb_->onTrailersComplete(streamID,
                                 std::make_unique<HTTPHeaders>());
    if (stage == 5) {
      doAbort();
    }
    codecCb_->onMessageComplete(streamID, false);

    eventBase_.loop();
  }
};

typedef TestAbortPost<0> TestAbortPost0;
typedef TestAbortPost<1> TestAbortPost1;
typedef TestAbortPost<2> TestAbortPost2;
typedef TestAbortPost<3> TestAbortPost3;
typedef TestAbortPost<4> TestAbortPost4;
typedef TestAbortPost<5> TestAbortPost5;

TEST_F(TestAbortPost1, Test) { doAbortTest(); }
TEST_F(TestAbortPost2, Test) { doAbortTest(); }
TEST_F(TestAbortPost3, Test) { doAbortTest(); }
TEST_F(TestAbortPost4, Test) { doAbortTest(); }
TEST_F(TestAbortPost5, Test) { doAbortTest(); }

TEST_F(MockHTTPUpstreamTest, AbortUpgrade) {
  // This is basically the same test as above, just for the upgrade path
  InSequence enforceOrder;
  StrictMock<MockHTTPHandler> handler;
  HTTPMessage req = getPostRequest(10);

  std::unique_ptr<HTTPMessage> resp = makeResponse(200);

  handler.expectTransaction();
  EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, _, _));

  auto txn = httpSession_->newTransaction(&handler);
  const auto streamID = txn->getID();
  txn->sendHeaders(req);
  txn->sendBody(makeBuf(5)); // only send half the body

  handler.expectHeaders();
  codecCb_->onHeadersComplete(streamID, std::move(resp));

  EXPECT_CALL(*codecPtr_, generateRstStream(_, streamID, _));
  handler.expectDetachTransaction();
  EXPECT_CALL(*codecPtr_, generateRstStream(_, streamID,
                                            ErrorCode::_SPDY_INVALID_STREAM));
  txn->sendAbort();
  codecCb_->onMessageComplete(streamID, true); // upgrade
  EXPECT_CALL(*codecPtr_, generateRstStream(_, streamID,
                                            ErrorCode::_SPDY_INVALID_STREAM));
  codecCb_->onMessageComplete(streamID, false); // eom

  eventBase_.loop();
}

TEST_F(MockHTTPUpstreamTest, DrainBeforeSendHeaders) {
  // Test that drain on session before sendHeaders() is called on open txn

  InSequence enforceOrder;
  MockHTTPHandler pushHandler;

  auto handler = openTransaction();
  EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, _, _));

  handler->expectHeaders();
  handler->expectEOM();
  handler->expectDetachTransaction();

  httpSession_->drain();
  handler->sendRequest();
  codecCb_->onHeadersComplete(handler->txn_->getID(), makeResponse(200));
  codecCb_->onMessageComplete(handler->txn_->getID(), false); // eom

  eventBase_.loop();
}

TEST_F(MockHTTP2UpstreamTest, ReceiveDoubleGoaway) {
  // Test that we handle receiving two goaways correctly

  InSequence enforceOrder;
  auto req = getGetRequest();

  // Open 2 txns but doesn't send headers yet
  auto handler1 = openTransaction();
  auto handler2 = openTransaction();

  // Get first goaway acking many un-started txns
  handler1->expectGoaway();
  handler2->expectGoaway();
  codecCb_->onGoaway(101, ErrorCode::NO_ERROR);

  // This txn should be alive since it was ack'd by the above goaway
  handler1->txn_->sendHeaders(req);

  // Second goaway acks the only the current outstanding transaction
  handler1->expectGoaway();
  handler2->expectGoaway();
  handler2->expectError([&] (const HTTPException& err) {
      EXPECT_TRUE(err.hasProxygenError());
      EXPECT_EQ(err.getProxygenError(), kErrorStreamUnacknowledged);
      ASSERT_EQ(
        folly::to<std::string>("StreamUnacknowledged on transaction id: ",
                               handler2->txn_->getID()),
        std::string(err.what()));
    });
  handler2->expectDetachTransaction();
  codecCb_->onGoaway(handler1->txn_->getID(), ErrorCode::NO_ERROR);

  // Clean up
  httpSession_->drain();
  EXPECT_CALL(*codecPtr_, generateRstStream(_, handler1->txn_->getID(), _));
  handler1->expectDetachTransaction();
  handler1->txn_->sendAbort();
  eventBase_.loop();
}

TEST_F(MockHTTP2UpstreamTest, ServerPushInvalidAssoc) {
  // Test that protocol error is generated on server push
  // with invalid assoc stream id
  InSequence enforceOrder;
  auto req = getGetRequest();
  auto handler = openTransaction();

  int streamID = handler->txn_->getID();
  int pushID = streamID + 1;
  int badAssocID = streamID + 2;

  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::PROTOCOL_ERROR));
  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::_SPDY_INVALID_STREAM))
    .Times(2);

  auto resp = makeResponse(200);
  codecCb_->onPushMessageBegin(pushID, badAssocID, resp.get());
  codecCb_->onHeadersComplete(pushID, std::move(resp));
  codecCb_->onMessageComplete(pushID, false);

  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();

  resp = makeResponse(200);
  codecCb_->onMessageBegin(streamID, resp.get());
  codecCb_->onHeadersComplete(streamID, std::move(resp));
  codecCb_->onMessageComplete(streamID, false);

  // Cleanup
  EXPECT_CALL(*codecPtr_, generateRstStream(_, streamID, _));
  EXPECT_CALL(*handler, detachTransaction());
  handler->terminate();

  EXPECT_TRUE(!httpSession_->hasActiveTransactions());
  httpSession_->destroy();
}

TEST_F(MockHTTP2UpstreamTest, ServerPushAfterFin) {
  // Test that protocol error is generated on server push
  // after FIN is received on regular response on the stream
  InSequence enforceOrder;
  auto req = getGetRequest();
  auto handler = openTransaction();

  int streamID = handler->txn_->getID();
  int pushID = streamID + 1;

  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();

  auto resp = makeResponse(200);
  codecCb_->onMessageBegin(streamID, resp.get());
  codecCb_->onHeadersComplete(streamID, std::move(resp));
  codecCb_->onMessageComplete(streamID, false);

  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::PROTOCOL_ERROR))
    .WillOnce(InvokeWithoutArgs([this] {
            // Verify that the assoc txn is still present
            EXPECT_TRUE(httpSession_->hasActiveTransactions());
            return 1;
          }));
  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::_SPDY_INVALID_STREAM))
    .Times(2);

  resp = makeResponse(200);
  codecCb_->onPushMessageBegin(pushID, streamID, resp.get());
  codecCb_->onHeadersComplete(pushID, std::move(resp));
  codecCb_->onMessageComplete(pushID, false);

  // Cleanup
  EXPECT_CALL(*codecPtr_, generateRstStream(_, streamID, _));
  EXPECT_CALL(*handler, detachTransaction());
  handler->terminate();

  EXPECT_TRUE(!httpSession_->hasActiveTransactions());
  httpSession_->destroy();
}

TEST_F(MockHTTP2UpstreamTest, ServerPushHandlerInstallFail) {
  // Test that REFUSED_STREAM error is generated when the session
  // fails to install the server push handler
  InSequence enforceOrder;
  auto req = getGetRequest();
  auto handler = openTransaction();

  int streamID = handler->txn_->getID();
  int pushID = streamID + 1;

  EXPECT_CALL(*handler, onPushedTransaction(_))
    .WillOnce(Invoke([] (HTTPTransaction* txn) {
            // Intentionally unset the handler on the upstream push txn
            txn->setHandler(nullptr);
          }));
  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::REFUSED_STREAM));
  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::_SPDY_INVALID_STREAM))
    .Times(2);

  auto resp = std::make_unique<HTTPMessage>();
  resp->setStatusCode(200);
  resp->setStatusMessage("OK");
  codecCb_->onPushMessageBegin(pushID, streamID, resp.get());
  codecCb_->onHeadersComplete(pushID, std::move(resp));
  codecCb_->onMessageComplete(pushID, false);

  handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
      EXPECT_FALSE(msg->getIsUpgraded());
      EXPECT_EQ(200, msg->getStatusCode());
    });
  handler->expectEOM();

  resp = std::make_unique<HTTPMessage>();
  resp->setStatusCode(200);
  resp->setStatusMessage("OK");
  codecCb_->onMessageBegin(streamID, resp.get());
  codecCb_->onHeadersComplete(streamID, std::move(resp));
  codecCb_->onMessageComplete(streamID, false);

  // Cleanup
  EXPECT_CALL(*codecPtr_, generateRstStream(_, streamID, _));
  EXPECT_CALL(*handler, detachTransaction());
  handler->terminate();

  EXPECT_TRUE(!httpSession_->hasActiveTransactions());
  httpSession_->destroy();
}

TEST_F(MockHTTP2UpstreamTest, ServerPushUnhandledAssoc) {
  // Test that REFUSED_STREAM error is generated when the assoc txn
  // is unhandled
  InSequence enforceOrder;
  auto req = getGetRequest();
  auto handler = openTransaction();

  int streamID = handler->txn_->getID();
  int pushID = streamID + 1;

  // Forcefully unset the handler on the assoc txn
  handler->txn_->setHandler(nullptr);

  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::REFUSED_STREAM));
  EXPECT_CALL(*codecPtr_,
              generateRstStream(_, pushID, ErrorCode::_SPDY_INVALID_STREAM))
    .Times(2);

  auto resp = std::make_unique<HTTPMessage>();
  resp->setStatusCode(200);
  resp->setStatusMessage("OK");
  codecCb_->onPushMessageBegin(pushID, streamID, resp.get());
  codecCb_->onHeadersComplete(pushID, std::move(resp));
  codecCb_->onMessageComplete(pushID, false);

  // Cleanup
  EXPECT_CALL(*codecPtr_, generateRstStream(_, streamID, _));
  handler->terminate();

  EXPECT_TRUE(!httpSession_->hasActiveTransactions());
  httpSession_->destroy();
}

TEST_F(MockHTTPUpstreamTest, HeadersThenBodyThenHeaders) {
  HTTPMessage req = getGetRequest();
  auto handler = openTransaction();
  handler->txn_->sendHeaders(req);

  handler->expectHeaders();
  EXPECT_CALL(*handler, onBody(_));
  // After getting the second headers, transaction will detach the handler
  handler->expectError([&] (const HTTPException& err) {
      EXPECT_TRUE(err.hasProxygenError());
      EXPECT_EQ(err.getProxygenError(), kErrorIngressStateTransition);
      ASSERT_EQ("Invalid ingress state transition, state=RegularBodyReceived, "
                "event=onHeaders, streamID=1",
                std::string(err.what()));
    });
  handler->expectDetachTransaction();
  auto resp = makeResponse(200);
  codecCb_->onMessageBegin(1, resp.get());
  codecCb_->onHeadersComplete(1, std::move(resp));
  codecCb_->onBody(1, makeBuf(20), 0);
  // Now receive headers again, on the same stream (illegal!)
  codecCb_->onHeadersComplete(1, makeResponse(200));
  eventBase_.loop();
}

TEST_F(MockHTTP2UpstreamTest, DelayUpstreamWindowUpdate) {
  EXPECT_CALL(*codecPtr_, supportsStreamFlowControl())
    .WillRepeatedly(Return(true));

  auto handler = openTransaction();
  handler->txn_->setReceiveWindow(1000000); // One miiiillion

  InSequence enforceOrder;
  EXPECT_CALL(*codecPtr_, generateHeader(_, _, _, _, _));
  EXPECT_CALL(*codecPtr_, generateWindowUpdate(_, _, _));

  HTTPMessage req = getGetRequest();
  handler->txn_->sendHeaders(req);
  handler->expectDetachTransaction();
  handler->txn_->sendAbort();
  httpSession_->destroy();
}

TEST_F(MockHTTPUpstreamTest, ForceShutdownInSetTransaction) {
  StrictMock<MockHTTPHandler> handler;
  handler.expectTransaction([&] (HTTPTransaction* txn) {
      handler.txn_ = txn;
      httpSession_->dropConnection();
    });
  handler.expectError([&] (const HTTPException& err) {
      EXPECT_TRUE(err.hasProxygenError());
      EXPECT_EQ(err.getProxygenError(), kErrorDropped);
      ASSERT_EQ(folly::to<std::string>("Dropped on transaction id: ",
                                       handler.txn_->getID()),
        std::string(err.what()));
    });
  handler.expectDetachTransaction();
  (void)httpSession_->newTransaction(&handler);
}

TEST_F(HTTP2UpstreamSessionTest, TestReplaySafetyCallback) {
  auto sock = dynamic_cast<HTTPTransaction::Transport*>(httpSession_);

  StrictMock<folly::test::MockReplaySafetyCallback> cb1;
  StrictMock<folly::test::MockReplaySafetyCallback> cb2;
  StrictMock<folly::test::MockReplaySafetyCallback> cb3;

  EXPECT_CALL(*transport_, isReplaySafe())
    .WillRepeatedly(Return(false));
  sock->addWaitingForReplaySafety(&cb1);
  sock->addWaitingForReplaySafety(&cb2);
  sock->addWaitingForReplaySafety(&cb3);
  sock->removeWaitingForReplaySafety(&cb2);

  ON_CALL(*transport_, isReplaySafe())
    .WillByDefault(Return(true));
  EXPECT_CALL(cb1, onReplaySafe_());
  EXPECT_CALL(cb3, onReplaySafe_());
  replaySafetyCallback_->onReplaySafe();

  httpSession_->destroy();
}

TEST_F(HTTP2UpstreamSessionTest, TestAlreadyReplaySafe) {
  auto sock = dynamic_cast<HTTPTransaction::Transport*>(httpSession_);

  StrictMock<folly::test::MockReplaySafetyCallback> cb;

  EXPECT_CALL(*transport_, isReplaySafe())
    .WillRepeatedly(Return(true));
  EXPECT_CALL(cb, onReplaySafe_());
  sock->addWaitingForReplaySafety(&cb);

  httpSession_->destroy();
}

TEST_F(HTTP2UpstreamSessionTest ,TestChainedBufIngress) {
  auto buf = folly::IOBuf::copyBuffer("hi");
  buf->prependChain(folly::IOBuf::copyBuffer("hello"));

  MockHTTPSessionInfoCallback infoCb;
  this->httpSession_->setInfoCallback(&infoCb);

  EXPECT_CALL(infoCb, onRead(_, 7));
  readCallback_->readBufferAvailable(std::move(buf));

  httpSession_->destroy();
}

TEST_F(HTTP2UpstreamSessionTest, AttachDetach) {
  folly::EventBase base;
  auto timer =
    folly::HHWheelTimer::newTimer(
      &base,
      std::chrono::milliseconds(folly::HHWheelTimer::DEFAULT_TICK_INTERVAL),
      TimeoutManager::InternalEnum::INTERNAL, std::chrono::milliseconds(500));
  WheelTimerInstance timerInstance(timer.get());
  uint64_t filterCount = 0;
  auto fn = [&filterCount](HTTPCodecFilter* /*filter*/) { filterCount++; };

  InSequence enforceOrder;
  auto egressCodec = makeServerCodec();
  folly::IOBufQueue output(folly::IOBufQueue::cacheChainLength());
  egressCodec->generateConnectionPreface(output);
  egressCodec->generateSettings(output);

  for (auto i = 0; i < 2; i++) {
    auto handler = openTransaction();
    handler->expectHeaders([&] (std::shared_ptr<HTTPMessage> msg) {
        EXPECT_EQ(200, msg->getStatusCode());
      });
    handler->expectBody();
    handler->expectEOM();
    handler->expectDetachTransaction();

    HTTPMessage resp;
    resp.setStatusCode(200);
    egressCodec->generateHeader(output, handler->txn_->getID(), resp);
    egressCodec->generateBody(output, handler->txn_->getID(), makeBuf(20),
                              HTTPCodec::NoPadding, true /* eom */);

    handler->sendRequest();
    auto buf = output.move();
    buf->coalesce();
    readAndLoop(buf.get());

    httpSession_->detachThreadLocals();
    httpSession_->attachThreadLocals(&base, nullptr, timerInstance, nullptr, fn,
                                     nullptr, nullptr);
    EXPECT_EQ(filterCount, 2);
    filterCount = 0;
    base.loopOnce();
  }
  httpSession_->destroy();
}

// Register and instantiate all our type-paramterized tests
REGISTER_TYPED_TEST_CASE_P(HTTPUpstreamTest,
                           ImmediateEof);

using AllTypes = ::testing::Types<HTTP1xCodecPair, SPDY3CodecPair>;
INSTANTIATE_TYPED_TEST_CASE_P(AllTypesPrefix, HTTPUpstreamTest, AllTypes);
