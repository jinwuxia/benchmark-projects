/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <proxygen/lib/http/codec/compress/HPACKConstants.h>
#include <proxygen/lib/http/codec/HTTPRequestVerifier.h>

namespace proxygen {

class HTTPMessage;

class HeaderDecodeInfo {
 public:
  void init(bool isRequestIn) {
    CHECK(!msg);
    msg.reset(new HTTPMessage());
    isRequest_ = isRequestIn;
    hasStatus_ = false;
    contentLength_ = folly::none;
    regularHeaderSeen_ = false;
    parsingError = "";
    decodeError = HPACK::DecodeError::NONE;
    verifier.reset(msg.get());
  }

  bool onHeader(const folly::fbstring& name, const folly::fbstring& value);

  void onHeadersComplete(HTTPHeaderSize decodedSize);

  // Change this to a map of decoded header blocks when we decide
  // to concurrently decode partial header blocks
  std::unique_ptr<HTTPMessage> msg;
  HTTPRequestVerifier verifier;
  std::string parsingError;
  HPACK::DecodeError decodeError{HPACK::DecodeError::NONE};

 private:
  bool isRequest_{false};
  bool hasStatus_{false};
  bool regularHeaderSeen_{false};
  folly::Optional<uint32_t> contentLength_;
};

}
