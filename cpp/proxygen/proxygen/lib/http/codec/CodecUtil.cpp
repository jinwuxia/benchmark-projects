/*
 *  Copyright (c) 2015-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/CodecUtil.h>

#include <folly/ThreadLocal.h>
#include <proxygen/lib/http/RFC2616.h>
#include <proxygen/lib/http/codec/HeaderConstants.h>

namespace proxygen {

/**
 *  Tokens as defined by rfc 2616. Also lowercases them.
 *        token       = 1*<any CHAR except CTLs or separators>
 *     separators     = "(" | ")" | "<" | ">" | "@"
 *                    | "," | ";" | ":" | "\" | <">
 *                    | "/" | "[" | "]" | "?" | "="
 *                    | "{" | "}" | SP | HT
 */
const char CodecUtil::http_tokens[256] = {
/*   0 nul    1 soh    2 stx    3 etx    4 eot    5 enq    6 ack    7 bel  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*   8 bs     9 ht    10 nl    11 vt    12 np    13 cr    14 so    15 si   */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  16 dle   17 dc1   18 dc2   19 dc3   20 dc4   21 nak   22 syn   23 etb */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  24 can   25 em    26 sub   27 esc   28 fs    29 gs    30 rs    31 us  */
        0,       0,       0,       0,       0,       0,       0,       0,
/*  32 sp    33  !    34  "    35  #    36  $    37  %    38  &    39  '  */
       ' ',      '!',     '"',     '#',     '$',     '%',     '&',    '\'',
/*  40  (    41  )    42  *    43  +    44  ,    45  -    46  .    47  /  */
        0,       0,      '*',     '+',      0,      '-',     '.',     '/',
/*  48  0    49  1    50  2    51  3    52  4    53  5    54  6    55  7  */
       '0',     '1',     '2',     '3',     '4',     '5',     '6',     '7',
/*  56  8    57  9    58  :    59  ;    60  <    61  =    62  >    63  ?  */
       '8',     '9',      0,       0,       0,       0,       0,       0,
/*  64  @    65  A    66  B    67  C    68  D    69  E    70  F    71  G  */
        0,      'a',     'b',     'c',     'd',     'e',     'f',     'g',
/*  72  H    73  I    74  J    75  K    76  L    77  M    78  N    79  O  */
       'h',     'i',     'j',     'k',     'l',     'm',     'n',     'o',
/*  80  P    81  Q    82  R    83  S    84  T    85  U    86  V    87  W  */
       'p',     'q',     'r',     's',     't',     'u',     'v',     'w',
/*  88  X    89  Y    90  Z    91  [    92  \    93  ]    94  ^    95  _  */
       'x',     'y',     'z',      0,       0,       0,      '^',     '_',
/*  96  `    97  a    98  b    99  c   100  d   101  e   102  f   103  g  */
       '`',     'a',     'b',     'c',     'd',     'e',     'f',     'g',
/* 104  h   105  i   106  j   107  k   108  l   109  m   110  n   111  o  */
       'h',     'i',     'j',     'k',     'l',     'm',     'n',     'o',
/* 112  p   113  q   114  r   115  s   116  t   117  u   118  v   119  w  */
       'p',     'q',     'r',     's',     't',     'u',     'v',     'w',
/* 120  x   121  y   122  z   123  {   124  |   125  }   126  ~   127 del */
       'x',     'y',     'z',      0,      '|',     '}',     '~',       0
};

bool CodecUtil::hasGzipAndDeflate(const std::string& value, bool& hasGzip,
                                 bool& hasDeflate) {
  static folly::ThreadLocal<std::vector<RFC2616::TokenQPair>> output;
  output->clear();
  hasGzip = false;
  hasDeflate = false;
  RFC2616::parseQvalues(value, *output);
  for (const auto& encodingQ: *output) {
    std::string lower(encodingQ.first.str());
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    // RFC says 3 sig figs
    if (lower == "gzip" && encodingQ.second >= 0.001) {
      hasGzip = true;
    } else if (lower == "deflate" && encodingQ.second >= 0.001) {
      hasDeflate = true;
    }
  }
  return hasGzip && hasDeflate;
}


std::vector<compress::Header> CodecUtil::prepareMessageForCompression(
    const HTTPMessage& msg,
    std::vector<std::string>& temps) {
  std::vector<compress::Header> allHeaders;
  if (msg.isRequest()) {
    if (msg.isEgressWebsocketUpgrade()) {
      allHeaders.emplace_back(HTTP_HEADER_COLON_METHOD,
          methodToString(HTTPMethod::CONNECT));
      allHeaders.emplace_back(HTTP_HEADER_COLON_PROTOCOL,
                              headers::kWebsocketString);
    } else {
      const std::string& method = msg.getMethodString();
      allHeaders.emplace_back(HTTP_HEADER_COLON_METHOD, method);
    }

    if (msg.getMethod() != HTTPMethod::CONNECT ||
        msg.isEgressWebsocketUpgrade()) {
      const std::string& scheme =
        (msg.isSecure() ? headers::kHttps : headers::kHttp);
      const std::string& path = msg.getURL();
      allHeaders.emplace_back(HTTP_HEADER_COLON_SCHEME, scheme);
      allHeaders.emplace_back(HTTP_HEADER_COLON_PATH, path);
    }
    const HTTPHeaders& headers = msg.getHeaders();
    const std::string& host = headers.getSingleOrEmpty(HTTP_HEADER_HOST);
    if (!host.empty()) {
      allHeaders.emplace_back(HTTP_HEADER_COLON_AUTHORITY, host);
    }
  } else {
    temps.reserve(3); // must be large enough so that emplace does not resize
    if (msg.isEgressWebsocketUpgrade()) {
      temps.emplace_back(headers::kStatus200);
    } else {
      temps.emplace_back(folly::to<std::string>(msg.getStatusCode()));
    }
    allHeaders.emplace_back(HTTP_HEADER_COLON_STATUS, temps.back());
    // HEADERS frames do not include a version or reason string.
  }

  bool hasDateHeader = false;
  // Add the HTTP headers supplied by the caller, but skip
  // any per-hop headers that aren't supported in HTTP/2.
  msg.getHeaders().forEachWithCode(
    [&] (HTTPHeaderCode code,
         const std::string& name,
         const std::string& value) {
      static const std::bitset<256> s_perHopHeaderCodes{
        [] {
          std::bitset<256> bs;
          // HTTP/1.x per-hop headers that have no meaning in HTTP/2
          bs[HTTP_HEADER_CONNECTION] = true;
          bs[HTTP_HEADER_HOST] = true;
          bs[HTTP_HEADER_KEEP_ALIVE] = true;
          bs[HTTP_HEADER_PROXY_CONNECTION] = true;
          bs[HTTP_HEADER_TRANSFER_ENCODING] = true;
          bs[HTTP_HEADER_UPGRADE] = true;
          bs[HTTP_HEADER_SEC_WEBSOCKET_KEY] = true;
          bs[HTTP_HEADER_SEC_WEBSOCKET_ACCEPT] = true;
          return bs;
        }()
      };

      if (s_perHopHeaderCodes[code] || name.size() == 0 || name[0] == ':') {
        DCHECK_GT(name.size(), 0) << "Empty header";
        DCHECK_NE(name[0], ':') << "Invalid header=" << name;
        return;
      }
      // Note this code will not drop headers named by Connection.  That's the
      // caller's job

      // see HTTP/2 spec, 8.1.2
      DCHECK(name != "TE" || value == "trailers");
      if ((name.size() > 0 && name[0] != ':') &&
          code != HTTP_HEADER_HOST) {
        allHeaders.emplace_back(code, name, value);
      }
      if (code == HTTP_HEADER_DATE) {
        hasDateHeader = true;
      }
    });

  if (msg.isResponse() && !hasDateHeader) {
    temps.emplace_back(HTTPMessage::formatDateHeader());
    allHeaders.emplace_back(HTTP_HEADER_DATE, temps.back());
  }
  return allHeaders;
}

}
