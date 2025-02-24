// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/services/cups_ipp_parser/ipp_parser.h"

#include <cups/cups.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/optional.h"
#include "chrome/services/cups_ipp_parser/public/cpp/ipp_converter.h"
#include "chrome/services/cups_proxy/public/cpp/type_conversions.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "net/http/http_util.h"

namespace cups_ipp_parser {
namespace {

using ipp_converter::HttpHeader;
using ipp_converter::kCarriage;
using ipp_converter::kIppSentinel;

// Log debugging error and send empty response, signalling error.
void Fail(const std::string& error_log, IppParser::ParseIppCallback cb) {
  DVLOG(1) << "IPP Parser Error: " << error_log;
  std::move(cb).Run(nullptr);
  return;
}

// Returns the starting index of the request-line-delimiter, -1 on failure.
int LocateEndOfRequestLine(base::StringPiece request) {
  auto end_of_request_line = request.find(kCarriage);
  if (end_of_request_line == std::string::npos) {
    return -1;
  }

  return end_of_request_line;
}

// Returns the starting index of the first HTTP header, -1 on failure.
int LocateStartOfHeaders(base::StringPiece request) {
  auto idx = LocateEndOfRequestLine(request);
  if (idx < 0) {
    return -1;
  }

  // Advance to first header and check it exists
  idx += strlen(kCarriage);
  return idx < static_cast<int>(request.size()) ? idx : -1;
}

// Returns the starting index of the end-of-headers-delimiter, -1 on failure.
int LocateEndOfHeaders(base::StringPiece request) {
  auto idx = net::HttpUtil::LocateEndOfHeaders(request.data(), request.size());
  if (idx < 0) {
    return -1;
  }

  // Back up to the start of the delimiter.
  // Note: The end-of-http-headers delimiter is 2 back-to-back carriage returns.
  const int end_of_headers_delimiter_size = 2 * strlen(kCarriage);
  return idx - end_of_headers_delimiter_size;
}

// Return the starting index of the IPP data/payload (pdf).
// Returns |ipp_metadata|.size() on empty IPP data and -1 on failure.
int LocateStartOfIppData(base::span<const uint8_t> ipp_metadata) {
  std::vector<uint8_t> sentinel_wrapper(
      ipp_converter::ConvertToByteBuffer(kIppSentinel));
  auto it = std::search(ipp_metadata.begin(), ipp_metadata.end(),
                        sentinel_wrapper.begin(), sentinel_wrapper.end());
  if (it == ipp_metadata.end()) {
    return -1;
  }

  // Advance to the start of IPP data and check existence or end of request.
  it += strlen(kIppSentinel);
  return it <= ipp_metadata.end() ? std::distance(ipp_metadata.begin(), it)
                                  : -1;
}

// Returns the starting index of the IPP metadata, -1 on failure.
int LocateStartOfIppMetadata(base::span<const uint8_t> request) {
  std::vector<char> char_buffer = ipp_converter::ConvertToCharBuffer(request);
  return net::HttpUtil::LocateEndOfHeaders(char_buffer.data(),
                                           char_buffer.size());
}

bool SplitRequestMetadata(base::span<const uint8_t> request,
                          std::string* http_metadata,
                          base::span<const uint8_t>* ipp_metadata) {
  size_t start_of_ipp_metadata = LocateStartOfIppMetadata(request);
  if (start_of_ipp_metadata < 0) {
    return false;
  }

  *http_metadata =
      ipp_converter::ConvertToString(request.first(start_of_ipp_metadata));
  *ipp_metadata = request.subspan(start_of_ipp_metadata);
  return true;
}

base::Optional<std::vector<std::string>> ExtractHttpRequestLine(
    base::StringPiece request) {
  size_t end_of_request_line = LocateEndOfRequestLine(request);
  if (end_of_request_line < 0) {
    return base::nullopt;
  }

  const base::StringPiece request_line_slice =
      request.substr(0, end_of_request_line);
  return ipp_converter::ParseRequestLine(request_line_slice);
}

base::Optional<std::vector<HttpHeader>> ExtractHttpHeaders(
    base::StringPiece request) {
  size_t start_of_headers = LocateStartOfHeaders(request);
  if (start_of_headers < 0) {
    return base::nullopt;
  }

  size_t end_of_headers = LocateEndOfHeaders(request);
  if (end_of_headers < 0) {
    return base::nullopt;
  }

  const base::StringPiece headers_slice =
      request.substr(start_of_headers, end_of_headers - start_of_headers);
  return ipp_converter::ParseHeaders(headers_slice);
}

mojom::IppMessagePtr ExtractIppMessage(base::span<const uint8_t> ipp_metadata) {
  printing::ScopedIppPtr ipp = ipp_converter::ParseIppMessage(ipp_metadata);
  if (!ipp) {
    return nullptr;
  }

  return ipp_converter::ConvertIppToMojo(ipp.get());
}

base::Optional<std::vector<uint8_t>> ExtractIppData(
    base::span<const uint8_t> ipp_metadata) {
  auto start_of_ipp_data = LocateStartOfIppData(ipp_metadata);
  if (start_of_ipp_data < 0) {
    return base::nullopt;
  }

  ipp_metadata = ipp_metadata.subspan(start_of_ipp_data);
  return std::vector<uint8_t>{ipp_metadata.begin(), ipp_metadata.end()};
}

}  // namespace

IppParser::IppParser(
    std::unique_ptr<service_manager::ServiceContextRef> service_ref)
    : service_ref_(std::move(service_ref)) {}

IppParser::~IppParser() = default;

void IppParser::ParseIpp(const std::vector<uint8_t>& to_parse,
                         ParseIppCallback callback) {
  // Separate |to_parse| into http metadata (interpreted as ASCII chars), and
  // ipp metadata (interpreted as arbitrary bytes).
  std::string http_metadata;
  base::span<const uint8_t> ipp_metadata;
  if (!SplitRequestMetadata(to_parse, &http_metadata, &ipp_metadata)) {
    return Fail("Failed to split HTTP and IPP metadata", std::move(callback));
  }

  // Parse Request line.
  auto request_line = ExtractHttpRequestLine(http_metadata);
  if (!request_line) {
    return Fail("Failed to parse request line", std::move(callback));
  }

  // Parse Headers.
  auto headers = ExtractHttpHeaders(http_metadata);
  if (!headers) {
    return Fail("Failed to parse headers", std::move(callback));
  }

  // Parse IPP message.
  auto ipp_message = ExtractIppMessage(ipp_metadata);
  if (!ipp_message) {
    return Fail("Failed to parse IPP message", std::move(callback));
  }

  // Parse IPP data.
  auto ipp_data = ExtractIppData(ipp_metadata);
  if (!ipp_data) {
    return Fail("Failed to parse IPP data", std::move(callback));
  }

  // Marshall response.
  mojom::IppRequestPtr parsed_request = mojom::IppRequest::New();

  std::vector<std::string> request_line_terms = *request_line;
  parsed_request->method = request_line_terms[0];
  parsed_request->endpoint = request_line_terms[1];
  parsed_request->http_version = request_line_terms[2];

  parsed_request->headers = std::move(*headers);
  parsed_request->ipp = std::move(ipp_message);
  parsed_request->data = std::move(*ipp_data);

  DVLOG(1) << "Finished parsing IPP request.";
  std::move(callback).Run(std::move(parsed_request));
}

}  // namespace cups_ipp_parser
