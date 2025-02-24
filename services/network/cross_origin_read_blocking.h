// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_CROSS_ORIGIN_READ_BLOCKING_H_
#define SERVICES_NETWORK_CROSS_ORIGIN_READ_BLOCKING_H_

#include <memory>
#include <string>
#include <vector>

#include "base/component_export.h"
#include "base/gtest_prod_util.h"
#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_piece_forward.h"
#include "services/network/initiator_lock_compatibility.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace content {
FORWARD_DECLARE_TEST(CrossSiteDocumentResourceHandlerTest, ResponseBlocking);
}  // namespace content

namespace network {

struct ResourceResponseInfo;

// CrossOriginReadBlocking (CORB) implements response blocking
// policy for Site Isolation.  CORB will monitor network responses to a
// renderer and block illegal responses so that a compromised renderer cannot
// steal private information from other sites.  For more details see
// services/network/cross_origin_read_blocking_explainer.md

class COMPONENT_EXPORT(NETWORK_SERVICE) CrossOriginReadBlocking {
 public:
  // This enum describes how CORB should decide whether to block a given
  // no-cors, cross-origin response.
  //
  // Note that these values are used in histograms, and must not change.
  enum class MimeType {
    // Blocked if served with `X-Content-Type-Options: nosniff` or if this is a
    // 206 range response or if sniffing confirms that the body matches
    // `Content-Type`.
    kHtml = 0,
    kXml = 1,
    kJson = 2,

    // Blocked if served with `X-Content-Type-Options: nosniff` or
    // sniffing detects that this is HTML, JSON or XML.  For example, this
    // behavior is used for `Content-Type: text/plain`.
    kPlain = 3,

    // Blocked if sniffing finds a JSON security prefix.  Used for an otherwise
    // unrecognized type (i.e. type that isn't explicitly recognized as
    // belonging to one of the other categories).
    kOthers = 4,

    // Always blocked.  Used for content types that are unlikely to be
    // incorrectly applied to images, scripts and other legacy no-cors
    // resources.  For example, `Content-Type: application/zip` is blocked
    // without any confirmation sniffing.
    kNeverSniffed = 5,

    kInvalidMimeType,              // For DCHECKs.
    kMaxValue = kInvalidMimeType,  // For UMA histograms.
  };

  // An instance for tracking the state of analyzing a single response
  // and deciding whether CORB should block the response.
  class COMPONENT_EXPORT(NETWORK_SERVICE) ResponseAnalyzer {
   public:
    // Creates a ResponseAnalyzer for the request (|request_url| and
    // |request_initiator|), |response| pair.  The ResponseAnalyzer will decide
    // whether |response| needs to be blocked.
    ResponseAnalyzer(const GURL& request_url,
                     const base::Optional<url::Origin>& request_initiator,
                     const ResourceResponseInfo& response,
                     base::Optional<url::Origin> request_initiator_site_lock,
                     mojom::RequestMode request_mode);

    ~ResponseAnalyzer();

    // true if either 1) ShouldBlockBasedOnHeaders decided to allow the response
    // based on headers alone or 2) ShouldBlockBasedOnHeaders decided to sniff
    // the response body and SniffResponseBody decided to allow the response
    // (e.g. because none of sniffers found blockable content).  false
    // otherwise.
    bool ShouldAllow() const;

    // true if either 1) ShouldBlockBasedOnHeaders decided to block the response
    // based on headers alone or 2) ShouldBlockBasedOnHeaders decided to sniff
    // the response body and SniffResponseBody confirmed that the response
    // contains blockable content.  false otherwise.
    bool ShouldBlock() const;

    // true if the analyzed response should report Cross-Origin Read Blocking in
    // a warning message written to the DevTools console.
    bool ShouldReportBlockedResponse() const;

    // Whether ShouldBlockBasedOnHeaders asked to sniff the body.
    bool needs_sniffing() const {
      return should_block_based_on_headers_ == kNeedToSniffMore;
    }

    // The MIME type determined by ShouldBlockBasedOnHeaders.
    const CrossOriginReadBlocking::MimeType& canonical_mime_type() const {
      return canonical_mime_type_;
    }

    // Value of the content-length response header if available. -1 if not
    // available.
    int64_t content_length() const { return content_length_; }

    // The HTTP response code (e.g. 200 or 404) received in response to this
    // resource request.
    int http_response_code() const { return http_response_code_; }

    // Allows ResponseAnalyzer to sniff the response body.
    void SniffResponseBody(base::StringPiece data, size_t new_data_offset);

    bool found_parser_breaker() const { return found_parser_breaker_; }

    class ConfirmationSniffer;
    class SimpleConfirmationSniffer;
    class FetchOnlyResourceSniffer;

    void LogAllowedResponse();
    void LogBlockedResponse();

   private:
    // Three conclusions are possible from looking at the headers:
    //   - Allow: response doesn't need to be blocked (e.g. if it is same-origin
    //     or has been allowed via CORS headers)
    //   - Block: response needs to be blocked (e.g. text/html + nosniff)
    //   - NeedMoreData: cannot decide yet - need to sniff more body first.
    enum BlockingDecision {
      kAllow,
      kBlock,
      kNeedToSniffMore,
    };
    BlockingDecision ShouldBlockBasedOnHeaders(
        mojom::RequestMode request_mode,
        const GURL& request_url,
        const base::Optional<url::Origin>& request_initiator,
        const ResourceResponseInfo& response);

    // Populates |sniffers_| container based on |canonical_mime_type_|.  Called
    // if ShouldBlockBasedOnHeaders returns kNeedToSniffMore
    void CreateSniffers();

    // Outcome of ShouldBlockBasedOnHeaders recorded inside the Create method.
    BlockingDecision should_block_based_on_headers_;

    // Canonical MIME type detected by ShouldBlockBasedOnHeaders.  Used to
    // determine if blocking the response is needed, as well as which type of
    // sniffing to perform.
    MimeType canonical_mime_type_ = MimeType::kInvalidMimeType;

    // Content length if available. -1 if not available.
    int64_t content_length_ = -1;

    // The HTTP response code (e.g. 200 or 404) received in response to this
    // resource request.
    int http_response_code_ = 0;

    // Propagated from URLLoaderFactoryParams::request_initiator_site_lock;
    base::Optional<url::Origin> request_initiator_site_lock_;

    // The sniffers to be used.
    std::vector<std::unique_ptr<ConfirmationSniffer>> sniffers_;

    // Sniffing results.
    bool found_blockable_content_ = false;
    bool found_parser_breaker_ = false;
    int bytes_read_for_sniffing_ = -1;

    DISALLOW_COPY_AND_ASSIGN(ResponseAnalyzer);
  };

  // Used to strip response headers if a decision to block has been made.
  static void SanitizeBlockedResponse(ResourceResponseInfo* response);

  // This enum backs a histogram, so do not change the order of entries or
  // remove entries. When adding new entries update |kMaxValue| and enums.xml
  // (see the SiteIsolationResponseAction enum).
  enum class Action {
    // Logged at OnResponseStarted.
    kResponseStarted = 0,

    // Logged when a response is blocked without requiring sniffing.
    kBlockedWithoutSniffing = 1,

    // Logged when a response is blocked as a result of sniffing the content.
    kBlockedAfterSniffing = 2,

    // Logged when a response is allowed without requiring sniffing.
    kAllowedWithoutSniffing = 3,

    // Logged when a response is allowed as a result of sniffing the content.
    kAllowedAfterSniffing = 4,

    kMaxValue = kAllowedAfterSniffing
  };
  static void LogAction(Action action);

  // Three conclusions are possible from sniffing a byte sequence:
  //  - No: meaning that the data definitively doesn't match the indicated type.
  //  - Yes: meaning that the data definitive does match the indicated type.
  //  - Maybe: meaning that if more bytes are appended to the stream, it's
  //    possible to get a Yes result. For example, if we are sniffing for a tag
  //    like "<html", a kMaybe result would occur if the data contains just
  //    "<ht".
  enum SniffingResult {
    kNo,
    kMaybe,
    kYes,
  };

  // Notifies CORB that |process_id| is proxying requests on behalf of a
  // universal-access plugin and therefore CORB should stop blocking requests
  // marked as ResourceType::kPluginResource.
  //
  // TODO(lukasza, laforge): https://crbug.com/702995: Remove the static
  // ...ForPlugin methods once Flash support is removed from Chromium (probably
  // around 2020 - see https://www.chromium.org/flash-roadmap).
  static void AddExceptionForPlugin(int process_id);

  // Returns true if CORB should ignore a request initiated by a universal
  // access plugin - i.e. if |process_id| has been previously passed to
  // AddExceptionForPlugin.
  static bool ShouldAllowForPlugin(int process_id);

  // Reverts AddExceptionForPlugin.
  static void RemoveExceptionForPlugin(int process_id);

  // Registers additional MIME types that can be protected by CORB (without any
  // confirmation sniffing).
  //
  // TODO(lukasza): https://crbug.com/944162: Remove the method below once
  // kMimeHandlerViewInCrossProcessFrame feature ships.
  static void AddExtraMimeTypesForCorb(
      const std::vector<std::string>& mime_types);

 private:
  CrossOriginReadBlocking();  // Not instantiable.

  // Returns the representative mime type enum value of the mime type of
  // response. For example, this returns the same value for all text/xml mime
  // type families such as application/xml, application/rss+xml.
  static MimeType GetCanonicalMimeType(base::StringPiece mime_type);
  FRIEND_TEST_ALL_PREFIXES(CrossOriginReadBlockingTest, GetCanonicalMimeType);

  // Returns whether this scheme is a target of the cross-origin read blocking
  // (CORB) policy.  This returns true only for http://* and https://* urls.
  static bool IsBlockableScheme(const GURL& frame_origin);
  FRIEND_TEST_ALL_PREFIXES(CrossOriginReadBlockingTest, IsBlockableScheme);

  // Returns whether there's a valid CORS header for frame_origin.  This is
  // simliar to CrossOriginAccessControl::passesAccessControlCheck(), but we use
  // sites as our security domain, not origins.
  // TODO(dsjang): this must be improved to be more accurate to the actual CORS
  // specification. For now, this works conservatively, allowing XSDs that are
  // not allowed by actual CORS rules by ignoring 1) credentials and 2)
  // methods. Preflight requests don't matter here since they are not used to
  // decide whether to block a response or not on the client side.
  // TODO(crbug.com/736308) Remove this check once the kOutOfBlinkCors feature
  // is shipped.
  static bool IsValidCorsHeaderSet(const url::Origin& frame_origin,
                                   const std::string& access_control_origin);
  FRIEND_TEST_ALL_PREFIXES(CrossOriginReadBlockingTest, IsValidCorsHeaderSet);

  static SniffingResult SniffForHTML(base::StringPiece data);
  static SniffingResult SniffForXML(base::StringPiece data);
  static SniffingResult SniffForJSON(base::StringPiece data);
  FRIEND_TEST_ALL_PREFIXES(CrossOriginReadBlockingTest, SniffForHTML);
  FRIEND_TEST_ALL_PREFIXES(CrossOriginReadBlockingTest, SniffForXML);
  FRIEND_TEST_ALL_PREFIXES(CrossOriginReadBlockingTest, SniffForJSON);
  FRIEND_TEST_ALL_PREFIXES(content::CrossSiteDocumentResourceHandlerTest,
                           ResponseBlocking);

  // Sniff for patterns that indicate |data| only ought to be consumed by XHR()
  // or fetch(). This detects Javascript parser-breaker and particular JS
  // infinite-loop patterns, which are used conventionally as a defense against
  // JSON data exfiltration by means of a <script> tag.
  static SniffingResult SniffForFetchOnlyResource(base::StringPiece data);

  DISALLOW_COPY_AND_ASSIGN(CrossOriginReadBlocking);
};

inline std::ostream& operator<<(
    std::ostream& out,
    const CrossOriginReadBlocking::MimeType& value) {
  out << static_cast<int>(value);
  return out;
}

}  // namespace network

#endif  // SERVICES_NETWORK_CROSS_ORIGIN_READ_BLOCKING_H_
