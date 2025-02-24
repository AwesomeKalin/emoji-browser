// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/cert/internal/crl.h"

#include "net/cert/internal/cert_errors.h"
#include "net/cert/internal/parsed_certificate.h"
#include "net/cert/internal/test_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/pool.h"

namespace net {

namespace {

constexpr base::TimeDelta kAgeOneWeek = base::TimeDelta::FromDays(7);

std::string GetFilePath(base::StringPiece file_name) {
  return std::string("net/data/crl_unittest/") + file_name.as_string();
}

scoped_refptr<ParsedCertificate> ParseCertificate(base::StringPiece data) {
  CertErrors errors;
  return ParsedCertificate::Create(
      bssl::UniquePtr<CRYPTO_BUFFER>(CRYPTO_BUFFER_new(
          reinterpret_cast<const uint8_t*>(data.data()), data.size(), nullptr)),
      {}, &errors);
}

class CheckCRLTest : public ::testing::TestWithParam<const char*> {};

// Test prefix naming scheme:
//   good = valid CRL, cert affirmatively not revoked
//   revoked = valid CRL, cert affirmatively revoked
//   bad = valid CRL, but cert status is unknown (cases like unhandled features,
//           mismatching issuer or signature, etc)
//   invalid = corrupt or violates some spec requirement
constexpr char const* kTestParams[] = {
    "good.pem",
    "good_issuer_name_normalization.pem",
    "good_issuer_no_keyusage.pem",
    "good_no_nextupdate.pem",
    "good_fake_extension.pem",
    "good_fake_extension_no_nextupdate.pem",
    "good_generalizedtime.pem",
    "good_no_version.pem",
    "good_no_crldp.pem",
    "good_key_rollover.pem",
    "revoked.pem",
    "revoked_no_nextupdate.pem",
    "revoked_fake_crlentryextension.pem",
    "revoked_generalized_revocationdate.pem",
    "revoked_key_rollover.pem",
    "bad_crldp_has_crlissuer.pem",
    "bad_fake_critical_extension.pem",
    "bad_fake_critical_crlentryextension.pem",
    "bad_signature.pem",
    "bad_thisupdate_in_future.pem",
    "bad_thisupdate_too_old.pem",
    "bad_nextupdate_too_old.pem",
    "bad_wrong_issuer.pem",
    "bad_key_rollover_signature.pem",
    "invalid_mismatched_signature_algorithm.pem",
    "invalid_revoked_empty_sequence.pem",
    "invalid_v1_with_extension.pem",
    "invalid_v1_with_crlentryextension.pem",
    "invalid_v1_explicit.pem",
    "invalid_v3.pem",
    "invalid_issuer_keyusage_no_crlsign.pem",
    "invalid_key_rollover_issuer_keyusage_no_crlsign.pem",
    "invalid_garbage_version.pem",
    "invalid_garbage_tbs_signature_algorithm.pem",
    "invalid_garbage_issuer_name.pem",
    "invalid_garbage_thisupdate.pem",
    "invalid_garbage_after_thisupdate.pem",
    "invalid_garbage_after_nextupdate.pem",
    "invalid_garbage_after_revokedcerts.pem",
    "invalid_garbage_after_extensions.pem",
    "invalid_garbage_tbscertlist.pem",
    "invalid_garbage_signaturealgorithm.pem",
    "invalid_garbage_signaturevalue.pem",
    "invalid_garbage_after_signaturevalue.pem",
    "invalid_garbage_revoked_serial_number.pem",
    "invalid_garbage_revocationdate.pem",
    "invalid_garbage_after_revocationdate.pem",
    "invalid_garbage_after_crlentryextensions.pem",
    "invalid_garbage_crlentry.pem",
};

struct PrintTestName {
  std::string operator()(
      const testing::TestParamInfo<const char*>& info) const {
    base::StringPiece name(info.param);
    // Strip ".pem" from the end as GTest names cannot contain period.
    name.remove_suffix(4);
    return name.as_string();
  }
};

INSTANTIATE_TEST_SUITE_P(,
                         CheckCRLTest,
                         ::testing::ValuesIn(kTestParams),
                         PrintTestName());

TEST_P(CheckCRLTest, FromFile) {
  base::StringPiece file_name(GetParam());

  std::string crl_data;
  std::string ca_data_2;
  std::string ca_data;
  std::string cert_data;
  const PemBlockMapping mappings[] = {
      {"CRL", &crl_data},
      {"CA CERTIFICATE 2", &ca_data_2, /*optional=*/true},
      {"CA CERTIFICATE", &ca_data},
      {"CERTIFICATE", &cert_data},
  };

  ASSERT_TRUE(ReadTestDataFromPemFile(GetFilePath(file_name), mappings));

  scoped_refptr<ParsedCertificate> cert = ParseCertificate(cert_data);
  ASSERT_TRUE(cert);
  scoped_refptr<ParsedCertificate> issuer_cert = ParseCertificate(ca_data);
  ASSERT_TRUE(issuer_cert);
  ParsedCertificateList certs = {cert, issuer_cert};
  if (!ca_data_2.empty()) {
    scoped_refptr<ParsedCertificate> issuer_cert_2 =
        ParseCertificate(ca_data_2);
    ASSERT_TRUE(issuer_cert_2);
    certs.push_back(issuer_cert_2);
  }

  // Assumes that all the test data certs have at most 1 CRL distributionPoint.
  // If the cert has a CRL distributionPoint, it is used for verifying the CRL,
  // otherwise the CRL is verified with no distributionPoint.
  // TODO(https://crbug.com/749276): This seems slightly hacky. Maybe the
  // distribution point to use should be specified separately in the test PEM?
  ParsedDistributionPoint* cert_dp = nullptr;
  std::vector<ParsedDistributionPoint> distribution_points;
  ParsedExtension crl_dp_extension;
  if (cert->GetExtension(CrlDistributionPointsOid(), &crl_dp_extension)) {
    ASSERT_TRUE(ParseCrlDistributionPoints(crl_dp_extension.value,
                                           &distribution_points));
    ASSERT_LE(distribution_points.size(), 1U);
    if (!distribution_points.empty())
      cert_dp = &distribution_points[0];
  }

  // Mar 9 00:00:00 2017 GMT
  base::Time kVerifyTime =
      base::Time::UnixEpoch() + base::TimeDelta::FromSeconds(1489017600);

  CRLRevocationStatus expected_revocation_status = CRLRevocationStatus::UNKNOWN;
  if (file_name.starts_with("good"))
    expected_revocation_status = CRLRevocationStatus::GOOD;
  else if (file_name.starts_with("revoked"))
    expected_revocation_status = CRLRevocationStatus::REVOKED;

  CRLRevocationStatus revocation_status =
      CheckCRL(crl_data, certs, /*target_cert_index=*/0, cert_dp, kVerifyTime,
               kAgeOneWeek);
  EXPECT_EQ(expected_revocation_status, revocation_status);

  // Test with a random cert added to the front of the chain and
  // |target_cert_index=1|. This is a hacky way to verify that
  // target_cert_index is actually being honored.
  ParsedCertificateList other_certs;
  ASSERT_TRUE(ReadCertChainFromFile("net/data/ssl/certificates/ok_cert.pem",
                                    &other_certs));
  ASSERT_FALSE(other_certs.empty());
  certs.insert(certs.begin(), other_certs[0]);
  revocation_status = CheckCRL(crl_data, certs, /*target_cert_index=*/1,
                               cert_dp, kVerifyTime, kAgeOneWeek);
  EXPECT_EQ(expected_revocation_status, revocation_status);
}

}  // namespace

}  // namespace net
