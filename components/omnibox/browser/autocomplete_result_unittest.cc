// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/omnibox/browser/autocomplete_result.h"

#include <stddef.h>

#include <memory>
#include <string>
#include <vector>

#include "base/metrics/field_trial.h"
#include "base/metrics/field_trial_params.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_task_environment.h"
#include "build/build_config.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/autocomplete_match_type.h"
#include "components/omnibox/browser/autocomplete_provider.h"
#include "components/omnibox/browser/autocomplete_provider_client.h"
#include "components/omnibox/browser/fake_autocomplete_provider_client.h"
#include "components/omnibox/browser/omnibox_field_trial.h"
#include "components/omnibox/browser/test_scheme_classifier.h"
#include "components/omnibox/common/omnibox_features.h"
#include "components/search_engines/template_url_service.h"
#include "components/variations/entropy_provider.h"
#include "components/variations/variations_associated_data.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/metrics_proto/omnibox_event.pb.h"

using metrics::OmniboxEventProto;

namespace {

struct AutocompleteMatchTestData {
  std::string destination_url;
  AutocompleteMatch::Type type;
};

const AutocompleteMatchTestData kVerbatimMatches[] = {
  { "http://search-what-you-typed/",
    AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED },
  { "http://url-what-you-typed/", AutocompleteMatchType::URL_WHAT_YOU_TYPED },
};

const AutocompleteMatchTestData kNonVerbatimMatches[] = {
  { "http://search-history/", AutocompleteMatchType::SEARCH_HISTORY },
  { "http://history-title/", AutocompleteMatchType::HISTORY_TITLE },
};

// Adds |count| AutocompleteMatches to |matches|.
void PopulateAutocompleteMatchesFromTestData(
    const AutocompleteMatchTestData* data,
    size_t count,
    ACMatches* matches) {
  ASSERT_TRUE(matches != nullptr);
  for (size_t i = 0; i < count; ++i) {
    AutocompleteMatch match;
    match.destination_url = GURL(data[i].destination_url);
    match.relevance =
        matches->empty() ? 1300 : (matches->back().relevance - 100);
    match.allowed_to_be_default_match = true;
    match.type = data[i].type;
    matches->push_back(match);
  }
}

// A simple AutocompleteProvider that does nothing.
class MockAutocompleteProvider : public AutocompleteProvider {
 public:
  explicit MockAutocompleteProvider(Type type): AutocompleteProvider(type) {}

  void Start(const AutocompleteInput& input, bool minimal_changes) override {}

 private:
  ~MockAutocompleteProvider() override {}
};

}  // namespace

class AutocompleteResultTest : public testing::Test {
 public:
  struct TestData {
    // Used to build a url for the AutocompleteMatch. The URL becomes
    // "http://" + ('a' + |url_id|) (e.g. an ID of 2 yields "http://c").
    int url_id;

    // ID of the provider.
    int provider_id;

    // Relevance score.
    int relevance;

    // Allowed to be default match status.
    bool allowed_to_be_default_match;

    // Duplicate matches.
    std::vector<AutocompleteMatch> duplicate_matches;
  };

  AutocompleteResultTest() {
    // Destroy the existing FieldTrialList before creating a new one to avoid
    // a DCHECK.
    field_trial_list_.reset();
    field_trial_list_.reset(new base::FieldTrialList(
        std::make_unique<variations::SHA1EntropyProvider>("foo")));
    variations::testing::ClearAllVariationParams();

    // Create the list of mock providers.  5 is enough.
    for (size_t i = 0; i < 5; ++i) {
      mock_provider_list_.push_back(new MockAutocompleteProvider(
              static_cast<AutocompleteProvider::Type>(i)));
    }
  }

  void SetUp() override {
    template_url_service_.reset(new TemplateURLService(nullptr, 0));
    template_url_service_->Load();
  }

  void TearDown() override { scoped_task_environment_.RunUntilIdle(); }

  // Configures |match| from |data|.
  void PopulateAutocompleteMatch(const TestData& data,
                                 AutocompleteMatch* match);

  // Adds |count| AutocompleteMatches to |matches|.
  void PopulateAutocompleteMatches(const TestData* data,
                                   size_t count,
                                   ACMatches* matches);

  // Asserts that |result| has |expected_count| matches matching |expected|.
  void AssertResultMatches(const AutocompleteResult& result,
                           const TestData* expected,
                           size_t expected_count);

  // Creates an AutocompleteResult from |last| and |current|. The two are
  // merged by |CopyOldMatches| and compared by |AssertResultMatches|.
  void RunCopyOldMatchesTest(const TestData* last, size_t last_size,
                             const TestData* current, size_t current_size,
                             const TestData* expected, size_t expected_size);

  // Returns a (mock) AutocompleteProvider of given |provider_id|.
  MockAutocompleteProvider* GetProvider(int provider_id) {
    EXPECT_LT(provider_id, static_cast<int>(mock_provider_list_.size()));
    return mock_provider_list_[provider_id].get();
  }

 protected:
  std::unique_ptr<TemplateURLService> template_url_service_;

 private:
  base::test::ScopedTaskEnvironment scoped_task_environment_;
  std::unique_ptr<base::FieldTrialList> field_trial_list_;

  // For every provider mentioned in TestData, we need a mock provider.
  std::vector<scoped_refptr<MockAutocompleteProvider> > mock_provider_list_;

  DISALLOW_COPY_AND_ASSIGN(AutocompleteResultTest);
};

void AutocompleteResultTest::PopulateAutocompleteMatch(
    const TestData& data,
    AutocompleteMatch* match) {
  match->provider = GetProvider(data.provider_id);
  match->fill_into_edit = base::NumberToString16(data.url_id);
  std::string url_id(1, data.url_id + 'a');
  match->destination_url = GURL("http://" + url_id);
  match->relevance = data.relevance;
  match->allowed_to_be_default_match = data.allowed_to_be_default_match;
  match->duplicate_matches = data.duplicate_matches;
}

void AutocompleteResultTest::PopulateAutocompleteMatches(
    const TestData* data,
    size_t count,
    ACMatches* matches) {
  for (size_t i = 0; i < count; ++i) {
    AutocompleteMatch match;
    PopulateAutocompleteMatch(data[i], &match);
    matches->push_back(match);
  }
}

void AutocompleteResultTest::AssertResultMatches(
    const AutocompleteResult& result,
    const TestData* expected,
    size_t expected_count) {
  ASSERT_EQ(expected_count, result.size());
  for (size_t i = 0; i < expected_count; ++i) {
    AutocompleteMatch expected_match;
    PopulateAutocompleteMatch(expected[i], &expected_match);
    const AutocompleteMatch& match = *(result.begin() + i);
    EXPECT_EQ(expected_match.provider, match.provider) << i;
    EXPECT_EQ(expected_match.relevance, match.relevance) << i;
    EXPECT_EQ(expected_match.allowed_to_be_default_match,
              match.allowed_to_be_default_match) << i;
    EXPECT_EQ(expected_match.destination_url.spec(),
              match.destination_url.spec()) << i;
  }
}

void AutocompleteResultTest::RunCopyOldMatchesTest(
    const TestData* last, size_t last_size,
    const TestData* current, size_t current_size,
    const TestData* expected, size_t expected_size) {
  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());

  ACMatches last_matches;
  PopulateAutocompleteMatches(last, last_size, &last_matches);
  AutocompleteResult last_result;
  last_result.AppendMatches(input, last_matches);
  last_result.SortAndCull(input, template_url_service_.get());

  ACMatches current_matches;
  PopulateAutocompleteMatches(current, current_size, &current_matches);
  AutocompleteResult current_result;
  current_result.AppendMatches(input, current_matches);
  current_result.SortAndCull(input, template_url_service_.get());
  current_result.CopyOldMatches(input, &last_result,
                                template_url_service_.get());

  AssertResultMatches(current_result, expected, expected_size);
}

// Assertion testing for AutocompleteResult::Swap.
TEST_F(AutocompleteResultTest, Swap) {
  AutocompleteResult r1;
  AutocompleteResult r2;

  // Swap with empty shouldn't do anything interesting.
  r1.Swap(&r2);
  EXPECT_EQ(r1.end(), r1.default_match());
  EXPECT_EQ(r2.end(), r2.default_match());

  // Swap with a single match.
  ACMatches matches;
  AutocompleteMatch match;
  match.relevance = 1;
  match.allowed_to_be_default_match = true;
  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  matches.push_back(match);
  r1.AppendMatches(input, matches);
  r1.SortAndCull(input, template_url_service_.get());
  EXPECT_EQ(r1.begin(), r1.default_match());
  EXPECT_EQ("http://a/", r1.alternate_nav_url().spec());
  r1.Swap(&r2);
  EXPECT_TRUE(r1.empty());
  EXPECT_EQ(r1.end(), r1.default_match());
  EXPECT_TRUE(r1.alternate_nav_url().is_empty());
  ASSERT_FALSE(r2.empty());
  EXPECT_EQ(r2.begin(), r2.default_match());
  EXPECT_EQ("http://a/", r2.alternate_nav_url().spec());
}

// Tests that if the new results have a lower max relevance score than last,
// any copied results have their relevance shifted down.
TEST_F(AutocompleteResultTest, CopyOldMatches) {
  TestData last[] = {
    { 0, 1, 1000, true },
    { 1, 1, 500,  true },
  };
  TestData current[] = {
    { 2, 1, 400,  true },
  };
  TestData result[] = {
    { 2, 1, 400,  true },
    { 1, 1, 399,  true },
  };

  ASSERT_NO_FATAL_FAILURE(RunCopyOldMatchesTest(last, base::size(last), current,
                                                base::size(current), result,
                                                base::size(result)));
}

// Tests that if the new results have a lower max relevance score than last,
// any copied results have their relevance shifted down when the allowed to
// be default constraint comes into play.
TEST_F(AutocompleteResultTest, CopyOldMatchesAllowedToBeDefault) {
  TestData last[] = {
    { 0, 1, 1300,  true },
    { 1, 1, 1200,  true },
    { 2, 1, 1100,  true },
  };
  TestData current[] = {
    { 3, 1, 1000, false },
    { 4, 1, 900,  true  },
  };
  // The expected results are out of relevance order because the top-scoring
  // allowed to be default match is always pulled to the top.
  TestData result[] = {
    { 4, 1, 900,  true  },
    { 3, 1, 1000, false },
    { 2, 1, 899,  true },
  };

  ASSERT_NO_FATAL_FAILURE(RunCopyOldMatchesTest(last, base::size(last), current,
                                                base::size(current), result,
                                                base::size(result)));
}

// Tests that matches are copied correctly from two distinct providers.
TEST_F(AutocompleteResultTest, CopyOldMatchesMultipleProviders) {
  TestData last[] = {
    { 0, 1, 1300, false },
    { 1, 2, 1250, true  },
    { 2, 1, 1200, false },
    { 3, 2, 1150, true  },
    { 4, 1, 1100, false },
  };
  TestData current[] = {
    { 5, 1, 1000, false },
    { 6, 2, 800,  true  },
    { 7, 1, 500,  true  },
  };
  // The expected results are out of relevance order because the top-scoring
  // allowed to be default match is always pulled to the top.
  TestData result[] = {
    { 6, 2, 800,  true  },
    { 5, 1, 1000, false },
    { 3, 2, 799,  true  },
    { 7, 1, 500,  true  },
    { 4, 1, 499,  false  },
  };

  ASSERT_NO_FATAL_FAILURE(RunCopyOldMatchesTest(last, base::size(last), current,
                                                base::size(current), result,
                                                base::size(result)));
}

// Tests that matches are copied correctly from two distinct providers when
// one provider doesn't have a current legal default match.
TEST_F(AutocompleteResultTest, CopyOldMatchesWithOneProviderWithoutDefault) {
  TestData last[] = {
    { 0, 2, 1250, true  },
    { 1, 2, 1150, true  },
    { 2, 1, 900,  false },
    { 3, 1, 800,  false },
    { 4, 1, 700,  false },
  };
  TestData current[] = {
    { 5, 1, 1000, true },
    { 6, 2, 800,  false },
    { 7, 1, 500,  true  },
  };
  TestData result[] = {
    { 5, 1, 1000, true  },
    { 1, 2, 999,  true  },
    { 6, 2, 800,  false },
    { 4, 1, 700,  false },
    { 7, 1, 500,  true  },
  };

  ASSERT_NO_FATAL_FAILURE(RunCopyOldMatchesTest(last, base::size(last), current,
                                                base::size(current), result,
                                                base::size(result)));
}

// Tests that matches with empty destination URLs aren't treated as duplicates
// and culled.
TEST_F(AutocompleteResultTest, SortAndCullEmptyDestinationURLs) {
  TestData data[] = {
    { 1, 1, 500,  true },
    { 0, 1, 1100, true },
    { 1, 1, 1000, true },
    { 0, 1, 1300, true },
    { 0, 1, 1200, true },
  };

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  matches[1].destination_url = GURL();
  matches[3].destination_url = GURL();
  matches[4].destination_url = GURL();

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // Of the two results with the same non-empty destination URL, the
  // lower-relevance one should be dropped.  All of the results with empty URLs
  // should be kept.
  ASSERT_EQ(4U, result.size());
  EXPECT_TRUE(result.match_at(0)->destination_url.is_empty());
  EXPECT_EQ(1300, result.match_at(0)->relevance);
  EXPECT_TRUE(result.match_at(1)->destination_url.is_empty());
  EXPECT_EQ(1200, result.match_at(1)->relevance);
  EXPECT_TRUE(result.match_at(2)->destination_url.is_empty());
  EXPECT_EQ(1100, result.match_at(2)->relevance);
  EXPECT_EQ("http://b/", result.match_at(3)->destination_url.spec());
  EXPECT_EQ(1000, result.match_at(3)->relevance);
}

#if !(defined(OS_ANDROID) || defined(OS_IOS))
// Tests which remove results only work on desktop.

TEST_F(AutocompleteResultTest, SortAndCullTailSuggestions) {
  // clang-format off
  TestData data[] = {
      {1, 1, 500,  true},
      {2, 1, 1100, false},
      {3, 1, 1000, false},
      {4, 1, 1300, false},
      {5, 1, 1200, false},
  };
  // clang-format on

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  // These will get sorted up, but still removed.
  matches[3].type = AutocompleteMatchType::SEARCH_SUGGEST_TAIL;
  matches[4].type = AutocompleteMatchType::SEARCH_SUGGEST_TAIL;

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  EXPECT_EQ(3UL, result.size());
  EXPECT_NE(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(0)->type);
  EXPECT_NE(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(1)->type);
  EXPECT_NE(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(2)->type);
}

TEST_F(AutocompleteResultTest, SortAndCullKeepDefaultTailSuggestions) {
  // clang-format off
  TestData data[] = {
      {1, 1, 500,  true},
      {2, 1, 1100, false},
      {3, 1, 1000, false},
      {4, 1, 1300, false},
      {5, 1, 1200, false},
  };
  // clang-format on

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  // Make sure that even bad tail suggestions, if the only default match,
  // are kept.
  matches[0].type = AutocompleteMatchType::SEARCH_SUGGEST_TAIL;
  matches[1].type = AutocompleteMatchType::SEARCH_SUGGEST_TAIL;
  matches[2].type = AutocompleteMatchType::SEARCH_SUGGEST_TAIL;

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  EXPECT_EQ(3UL, result.size());
  EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(0)->type);
  EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(1)->type);
  EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(2)->type);
}

TEST_F(AutocompleteResultTest, SortAndCullKeepMoreDefaultTailSuggestions) {
  // clang-format off
  TestData data[] = {
      {1, 1, 500,  true},   // Low score non-tail default
      {2, 1, 1100, false},  // Tail
      {3, 1, 1000, true},   // Allow a tail suggestion to be the default.
      {4, 1, 1300, false},  // Tail
      {5, 1, 1200, false},  // Tail
  };
  // clang-format on

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  // Make sure that even a bad non-tail default suggestion is kept.
  for (size_t i = 1; i < 5; ++i)
    matches[i].type = AutocompleteMatchType::SEARCH_SUGGEST_TAIL;

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  EXPECT_EQ(5UL, result.size());
  // Non-tail default must be first, regardless of score
  EXPECT_NE(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(0)->type);
  for (size_t i = 1; i < 5; ++i) {
    EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
              result.match_at(i)->type);
    EXPECT_FALSE(result.match_at(i)->allowed_to_be_default_match);
  }
}

#endif

TEST_F(AutocompleteResultTest, SortAndCullOnlyTailSuggestions) {
  // clang-format off
  TestData data[] = {
      {1, 1, 500,  true},   // Allow a bad non-tail default.
      {2, 1, 1100, false},  // Tail
      {3, 1, 1000, false},  // Tail
      {4, 1, 1300, false},  // Tail
      {5, 1, 1200, false},  // Tail
  };
  // clang-format on

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  // These will not be removed.
  for (size_t i = 1; i < 5; ++i)
    matches[i].type = AutocompleteMatchType::SEARCH_SUGGEST_TAIL;

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  EXPECT_EQ(5UL, result.size());
  EXPECT_NE(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
            result.match_at(0)->type);
  for (size_t i = 1; i < 5; ++i)
    EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
              result.match_at(i)->type);
}

TEST_F(AutocompleteResultTest, SortAndCullDuplicateSearchURLs) {
  // Register a template URL that corresponds to 'foo' search engine.
  TemplateURLData url_data;
  url_data.SetShortName(base::ASCIIToUTF16("unittest"));
  url_data.SetKeyword(base::ASCIIToUTF16("foo"));
  url_data.SetURL("http://www.foo.com/s?q={searchTerms}");
  template_url_service_->Add(std::make_unique<TemplateURL>(url_data));

  TestData data[] = {
    { 0, 1, 1300, true },
    { 1, 1, 1200, true },
    { 2, 1, 1100, true },
    { 3, 1, 1000, true },
    { 4, 2, 900,  true },
  };

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  matches[0].destination_url = GURL("http://www.foo.com/s?q=foo");
  matches[1].destination_url = GURL("http://www.foo.com/s?q=foo2");
  matches[2].destination_url = GURL("http://www.foo.com/s?q=foo&oq=f");
  matches[3].destination_url = GURL("http://www.foo.com/s?q=foo&aqs=0");
  matches[4].destination_url = GURL("http://www.foo.com/");

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // We expect the 3rd and 4th results to be removed.
  ASSERT_EQ(3U, result.size());
  EXPECT_EQ("http://www.foo.com/s?q=foo",
            result.match_at(0)->destination_url.spec());
  EXPECT_EQ(1300, result.match_at(0)->relevance);
  EXPECT_EQ("http://www.foo.com/s?q=foo2",
            result.match_at(1)->destination_url.spec());
  EXPECT_EQ(1200, result.match_at(1)->relevance);
  EXPECT_EQ("http://www.foo.com/",
            result.match_at(2)->destination_url.spec());
  EXPECT_EQ(900, result.match_at(2)->relevance);
}

TEST_F(AutocompleteResultTest, SortAndCullWithMatchDups) {
  // Register a template URL that corresponds to 'foo' search engine.
  TemplateURLData url_data;
  url_data.SetShortName(base::ASCIIToUTF16("unittest"));
  url_data.SetKeyword(base::ASCIIToUTF16("foo"));
  url_data.SetURL("http://www.foo.com/s?q={searchTerms}");
  template_url_service_->Add(std::make_unique<TemplateURL>(url_data));

  AutocompleteMatch dup_match;
  dup_match.destination_url = GURL("http://www.foo.com/s?q=foo&oq=dup");
  std::vector<AutocompleteMatch> dups;
  dups.push_back(dup_match);

  TestData data[] = {
    { 0, 1, 1300, true, dups },
    { 1, 1, 1200, true  },
    { 2, 1, 1100, true  },
    { 3, 1, 1000, true, dups },
    { 4, 2, 900,  true  },
    { 5, 1, 800,  true  },
  };

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  matches[0].destination_url = GURL("http://www.foo.com/s?q=foo");
  matches[1].destination_url = GURL("http://www.foo.com/s?q=foo2");
  matches[2].destination_url = GURL("http://www.foo.com/s?q=foo&oq=f");
  matches[3].destination_url = GURL("http://www.foo.com/s?q=foo&aqs=0");
  matches[4].destination_url = GURL("http://www.foo.com/");
  matches[5].destination_url = GURL("http://www.foo.com/s?q=foo2&oq=f");

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // Expect 3 unique results after SortAndCull().
  ASSERT_EQ(3U, result.size());

  // Check that 3rd and 4th result got added to the first result as dups
  // and also duplicates of the 4th match got copied.
  ASSERT_EQ(4U, result.match_at(0)->duplicate_matches.size());
  const AutocompleteMatch* first_match = result.match_at(0);
  EXPECT_EQ(matches[2].destination_url,
            first_match->duplicate_matches.at(1).destination_url);
  EXPECT_EQ(dup_match.destination_url,
            first_match->duplicate_matches.at(2).destination_url);
  EXPECT_EQ(matches[3].destination_url,
            first_match->duplicate_matches.at(3).destination_url);

  // Check that 6th result started a new list of dups for the second result.
  ASSERT_EQ(1U, result.match_at(1)->duplicate_matches.size());
  EXPECT_EQ(matches[5].destination_url,
            result.match_at(1)->duplicate_matches.at(0).destination_url);
}

TEST_F(AutocompleteResultTest, SortAndCullWithDemotionsByType) {
  // Add some matches.
  ACMatches matches;
  const AutocompleteMatchTestData data[] = {
    { "http://history-url/", AutocompleteMatchType::HISTORY_URL },
    { "http://search-what-you-typed/",
      AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED },
    { "http://history-title/", AutocompleteMatchType::HISTORY_TITLE },
    { "http://search-history/", AutocompleteMatchType::SEARCH_HISTORY },
  };
  PopulateAutocompleteMatchesFromTestData(data, base::size(data), &matches);

  // Demote the search history match relevance score.
  matches.back().relevance = 500;

  // Add a rule demoting history-url and killing history-title.
  {
    std::map<std::string, std::string> params;
    params[std::string(OmniboxFieldTrial::kDemoteByTypeRule) + ":3:*"] =
        "1:50,7:100,2:0";  // 3 == HOME_PAGE
    ASSERT_TRUE(variations::AssociateVariationParams(
        OmniboxFieldTrial::kBundledExperimentFieldTrialName, "A", params));
  }
  base::FieldTrialList::CreateFieldTrial(
      OmniboxFieldTrial::kBundledExperimentFieldTrialName, "A");

  AutocompleteInput input(base::ASCIIToUTF16("a"), OmniboxEventProto::HOME_PAGE,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // Check the new ordering.  The history-title results should be omitted.
  // We cannot check relevance scores because the matches are sorted by
  // demoted relevance but the actual relevance scores are not modified.
  ASSERT_EQ(3u, result.size());
  EXPECT_EQ("http://search-what-you-typed/",
            result.match_at(0)->destination_url.spec());
  EXPECT_EQ("http://history-url/",
            result.match_at(1)->destination_url.spec());
  EXPECT_EQ("http://search-history/",
            result.match_at(2)->destination_url.spec());
}

TEST_F(AutocompleteResultTest, DemoteByTypeButPreserveDefaultMatchScore) {
  // Add some matches.
  ACMatches matches;
  const AutocompleteMatchTestData data[] = {
      {"http://history-url/", AutocompleteMatchType::HISTORY_URL},
      {"http://history-title/", AutocompleteMatchType::HISTORY_TITLE},
      {"http://search-what-you-typed/",
       AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED},
      {"http://search-history/", AutocompleteMatchType::SEARCH_HISTORY},
  };
  PopulateAutocompleteMatchesFromTestData(data, base::size(data), &matches);

  // Make history-title and search-history the only default matches, so that
  // they compete.
  matches[0].allowed_to_be_default_match = false;
  matches[2].allowed_to_be_default_match = false;

  // Add a rule demoting history-title.
  {
    std::map<std::string, std::string> params;
    params[std::string(OmniboxFieldTrial::kDemoteByTypeRule) + ":*:*"] = "2:50";
    ASSERT_TRUE(variations::AssociateVariationParams(
        OmniboxFieldTrial::kBundledExperimentFieldTrialName, "A", params));
  }
  base::FieldTrialList::CreateFieldTrial(
      OmniboxFieldTrial::kBundledExperimentFieldTrialName, "A");

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(omnibox::kOmniboxPreserveDefaultMatchScore);

  {
    AutocompleteInput input(base::ASCIIToUTF16("a"),
                            OmniboxEventProto::HOME_PAGE,
                            TestSchemeClassifier());
    AutocompleteResult result;
    result.AppendMatches(input, matches);
    result.SortAndCull(input, template_url_service_.get());

    // Make sure history-title is the default match, despite demotion.
    ASSERT_EQ(4u, result.size());
    EXPECT_EQ("http://history-title/",
              result.match_at(0)->destination_url.spec());
    EXPECT_EQ("http://history-url/",
              result.match_at(1)->destination_url.spec());
    EXPECT_EQ("http://search-what-you-typed/",
              result.match_at(2)->destination_url.spec());
    EXPECT_EQ("http://search-history/",
              result.match_at(3)->destination_url.spec());
  }

  {
    // Re-sort with a page classification of fake-box, and make sure
    // history-title is now demoted.
    AutocompleteInput input(
        base::ASCIIToUTF16("a"),
        OmniboxEventProto::INSTANT_NTP_WITH_FAKEBOX_AS_STARTING_FOCUS,
        TestSchemeClassifier());
    AutocompleteResult result;
    result.AppendMatches(input, matches);
    result.SortAndCull(input, template_url_service_.get());

    ASSERT_EQ(4u, result.size());
    EXPECT_EQ("http://search-history/",
              result.match_at(0)->destination_url.spec());
    EXPECT_EQ("http://history-url/",
              result.match_at(1)->destination_url.spec());
    EXPECT_EQ("http://search-what-you-typed/",
              result.match_at(2)->destination_url.spec());
    EXPECT_EQ("http://history-title/",
              result.match_at(3)->destination_url.spec());
  }
}

TEST_F(AutocompleteResultTest, SortAndCullWithMatchDupsAndDemotionsByType) {
  // Add some matches.
  ACMatches matches;
  const AutocompleteMatchTestData data[] = {
    { "http://search-what-you-typed/",
      AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED },
    { "http://dup-url/", AutocompleteMatchType::HISTORY_URL },
    { "http://dup-url/", AutocompleteMatchType::NAVSUGGEST },
    { "http://search-url/", AutocompleteMatchType::SEARCH_SUGGEST },
    { "http://history-url/", AutocompleteMatchType::HISTORY_URL },
  };
  PopulateAutocompleteMatchesFromTestData(data, base::size(data), &matches);

  // Add a rule demoting HISTORY_URL.
  {
    std::map<std::string, std::string> params;
    params[std::string(OmniboxFieldTrial::kDemoteByTypeRule) + ":8:*"] =
        "1:50";  // 8 == INSTANT_NTP_WITH_FAKEBOX_AS_STARTING_FOCUS
    ASSERT_TRUE(variations::AssociateVariationParams(
        OmniboxFieldTrial::kBundledExperimentFieldTrialName, "C", params));
  }
  base::FieldTrialList::CreateFieldTrial(
      OmniboxFieldTrial::kBundledExperimentFieldTrialName, "C");

  {
    AutocompleteInput input(
        base::ASCIIToUTF16("a"),
        OmniboxEventProto::INSTANT_NTP_WITH_FAKEBOX_AS_STARTING_FOCUS,
        TestSchemeClassifier());
    AutocompleteResult result;
    result.AppendMatches(input, matches);
    result.SortAndCull(input, template_url_service_.get());

    // The NAVSUGGEST dup-url stay above search-url since the navsuggest
    // variant should not be demoted.
    ASSERT_EQ(4u, result.size());
    EXPECT_EQ("http://search-what-you-typed/",
              result.match_at(0)->destination_url.spec());
    EXPECT_EQ("http://dup-url/",
              result.match_at(1)->destination_url.spec());
    EXPECT_EQ(AutocompleteMatchType::NAVSUGGEST,
              result.match_at(1)->type);
    EXPECT_EQ("http://search-url/",
              result.match_at(2)->destination_url.spec());
    EXPECT_EQ("http://history-url/",
              result.match_at(3)->destination_url.spec());
  }
}

TEST_F(AutocompleteResultTest, SortAndCullReorderForDefaultMatch) {
  TestData data[] = {
    { 0, 1, 1300, true },
    { 1, 1, 1200, true },
    { 2, 1, 1100, true },
    { 3, 1, 1000, true }
  };
  TestSchemeClassifier test_scheme_classifier;

  {
    // Check that reorder doesn't do anything if the top result
    // is already a legal default match (which is the default from
    // PopulateAutocompleteMatches()).
    ACMatches matches;
    PopulateAutocompleteMatches(data, base::size(data), &matches);
    AutocompleteInput input(base::ASCIIToUTF16("a"),
                            metrics::OmniboxEventProto::HOME_PAGE,
                            test_scheme_classifier);
    AutocompleteResult result;
    result.AppendMatches(input, matches);
    result.SortAndCull(input, template_url_service_.get());
    AssertResultMatches(result, data, 4);
  }

  {
    // Check that reorder swaps up a result appropriately.
    ACMatches matches;
    PopulateAutocompleteMatches(data, base::size(data), &matches);
    matches[0].allowed_to_be_default_match = false;
    matches[1].allowed_to_be_default_match = false;
    AutocompleteInput input(base::ASCIIToUTF16("a"),
                            metrics::OmniboxEventProto::HOME_PAGE,
                            test_scheme_classifier);
    AutocompleteResult result;
    result.AppendMatches(input, matches);
    result.SortAndCull(input, template_url_service_.get());
    ASSERT_EQ(4U, result.size());
    EXPECT_EQ("http://c/", result.match_at(0)->destination_url.spec());
    EXPECT_EQ("http://a/", result.match_at(1)->destination_url.spec());
    EXPECT_EQ("http://b/", result.match_at(2)->destination_url.spec());
    EXPECT_EQ("http://d/", result.match_at(3)->destination_url.spec());
  }
}

TEST_F(AutocompleteResultTest, SortAndCullPromoteDefaultMatch) {
  TestData data[] = {
    { 0, 1, 1300, false },
    { 1, 1, 1200, false },
    { 2, 2, 1100, false },
    { 2, 3, 1000, false },
    { 2, 4, 900, true }
  };
  TestSchemeClassifier test_scheme_classifier;

  // Check that reorder swaps up a result, and promotes relevance,
  // appropriately.
  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::HOME_PAGE,
                          test_scheme_classifier);
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());
  ASSERT_EQ(3U, result.size());
  EXPECT_EQ("http://c/", result.match_at(0)->destination_url.spec());
  EXPECT_EQ(1100, result.match_at(0)->relevance);
  EXPECT_TRUE(result.match_at(0)->allowed_to_be_default_match);
  EXPECT_EQ(GetProvider(4), result.match_at(0)->provider);
  EXPECT_EQ("http://a/", result.match_at(1)->destination_url.spec());
  EXPECT_FALSE(result.match_at(1)->allowed_to_be_default_match);
  EXPECT_EQ("http://b/", result.match_at(2)->destination_url.spec());
  EXPECT_FALSE(result.match_at(2)->allowed_to_be_default_match);
}

TEST_F(AutocompleteResultTest, SortAndCullPromoteUnconsecutiveMatches) {
  TestData data[] = {
    { 0, 1, 1300, false },
    { 1, 1, 1200, true },
    { 3, 2, 1100, false },
    { 2, 1, 1000, false },
    { 3, 3, 900, true },
    { 4, 1, 800, false },
    { 3, 4, 700, false },
  };
  TestSchemeClassifier test_scheme_classifier;

  // Check that reorder swaps up a result, and promotes relevance,
  // even for a default match that isn't the best.
  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::HOME_PAGE,
                          test_scheme_classifier);
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());
  ASSERT_EQ(5U, result.size());
  EXPECT_EQ("http://b/", result.match_at(0)->destination_url.spec());
  EXPECT_EQ(1200, result.match_at(0)->relevance);
  EXPECT_EQ("http://a/", result.match_at(1)->destination_url.spec());
  EXPECT_EQ("http://d/", result.match_at(2)->destination_url.spec());
  EXPECT_EQ(1100, result.match_at(2)->relevance);
  EXPECT_EQ(GetProvider(3), result.match_at(2)->provider);
  EXPECT_EQ("http://c/", result.match_at(3)->destination_url.spec());
  EXPECT_EQ("http://e/", result.match_at(4)->destination_url.spec());
}

struct EntityTestData {
  AutocompleteMatchType::Type type;
  std::string destination_url;
  int relevance;
  bool allowed_to_be_default_match;
  std::string fill_into_edit;
  std::string inline_autocompletion;
};

void PopulateEntityTestCases(std::vector<EntityTestData>& test_cases,
                             ACMatches* matches) {
  for (const auto& test_case : test_cases) {
    AutocompleteMatch match;
    match.type = test_case.type;
    match.destination_url = GURL(test_case.destination_url);
    match.relevance = test_case.relevance;
    match.allowed_to_be_default_match = test_case.allowed_to_be_default_match;
    match.fill_into_edit = base::UTF8ToUTF16(test_case.fill_into_edit);
    match.inline_autocompletion =
        base::UTF8ToUTF16(test_case.inline_autocompletion);
    matches->push_back(match);
  }
}

TEST_F(AutocompleteResultTest, SortAndCullPreferEntities) {
  // clang-format off
  std::vector<EntityTestData> test_cases = {
    {
      AutocompleteMatchType::SEARCH_SUGGEST,
      "http://search/?q=foo", 1100, false, "foo", ""
    },
    {
      AutocompleteMatchType::SEARCH_SUGGEST_ENTITY,
      "http://search/?q=foo", 1000, false, "foo", ""
    },
    {
      AutocompleteMatchType::SEARCH_SUGGEST,
      "http://search/?q=foo", 900, true, "foo", "oo"
    },
    // This match will be the first result but it won't affect the entity
    // deduping because it has a different URL.
    {
      AutocompleteMatchType::SEARCH_SUGGEST_PERSONALIZED,
      "http://search/?q=bar", 1200, true, "foo", "oo"
    },
  };
  // clang-format on
  ACMatches matches;
  PopulateEntityTestCases(test_cases, &matches);

  AutocompleteInput input(base::ASCIIToUTF16("f"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // The first result will be the personalized suggestion.
  EXPECT_EQ(2UL, result.size());
  EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST_PERSONALIZED,
            result.match_at(0)->type);
  EXPECT_EQ(1200, result.match_at(0)->relevance);

  // The second result will be the result of deduping the other three.
  // The chosen match should be the entity suggestion and it should have been
  // promoted to receive the first match's relevance and the last match's
  // allowed_to_be_default_match and inline_autocompletion values.
  EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST_ENTITY,
            result.match_at(1)->type);
  EXPECT_EQ(1100, result.match_at(1)->relevance);
  EXPECT_TRUE(result.match_at(1)->allowed_to_be_default_match);
  EXPECT_EQ(base::ASCIIToUTF16("oo"),
            result.match_at(1)->inline_autocompletion);
}

TEST_F(AutocompleteResultTest, SortAndCullPreferEntitiesFillIntoEditMustMatch) {
  // clang-format off
  std::vector<EntityTestData> test_cases = {
    {
      AutocompleteMatchType::SEARCH_SUGGEST_PERSONALIZED,
      "http://search/?q=foo", 1100, false, "foo", ""
    },
    {
      AutocompleteMatchType::SEARCH_SUGGEST_ENTITY,
      "http://search/?q=foo", 1000, false, "foobar", ""
    },
    {
      AutocompleteMatchType::SEARCH_SUGGEST,
      "http://search/?q=foo", 900, true, "foo", "oo"
    },
  };
  // clang-format on
  ACMatches matches;
  PopulateEntityTestCases(test_cases, &matches);

  AutocompleteInput input(base::ASCIIToUTF16("f"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // The entity suggestion won't be chosen in this case because it has a non-
  // matching value for fill_into_edit.
  EXPECT_EQ(1UL, result.size());
  EXPECT_EQ(AutocompleteMatchType::SEARCH_SUGGEST, result.match_at(0)->type);
  EXPECT_EQ(1100, result.match_at(0)->relevance);
  EXPECT_TRUE(result.match_at(0)->allowed_to_be_default_match);
  EXPECT_EQ(base::ASCIIToUTF16("oo"),
            result.match_at(0)->inline_autocompletion);
}

TEST_F(AutocompleteResultTest, SortAndCullPromoteDuplicateSearchURLs) {
  // Register a template URL that corresponds to 'foo' search engine.
  TemplateURLData url_data;
  url_data.SetShortName(base::ASCIIToUTF16("unittest"));
  url_data.SetKeyword(base::ASCIIToUTF16("foo"));
  url_data.SetURL("http://www.foo.com/s?q={searchTerms}");
  template_url_service_->Add(std::make_unique<TemplateURL>(url_data));

  TestData data[] = {
    { 0, 1, 1300, false },
    { 1, 1, 1200, true },
    { 2, 1, 1100, true },
    { 3, 1, 1000, true },
    { 4, 2, 900,  true },
  };

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  // Note that 0, 2 and 3 will compare equal after stripping.
  matches[0].destination_url = GURL("http://www.foo.com/s?q=foo");
  matches[1].destination_url = GURL("http://www.foo.com/s?q=foo2");
  matches[2].destination_url = GURL("http://www.foo.com/s?q=foo&oq=f");
  matches[3].destination_url = GURL("http://www.foo.com/s?q=foo&aqs=0");
  matches[4].destination_url = GURL("http://www.foo.com/");

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // We expect the 3rd and 4th results to be removed.
  ASSERT_EQ(3U, result.size());
  EXPECT_EQ("http://www.foo.com/s?q=foo&oq=f",
            result.match_at(0)->destination_url.spec());
  EXPECT_EQ(1300, result.match_at(0)->relevance);
  EXPECT_EQ("http://www.foo.com/s?q=foo2",
            result.match_at(1)->destination_url.spec());
  EXPECT_EQ(1200, result.match_at(1)->relevance);
  EXPECT_EQ("http://www.foo.com/",
            result.match_at(2)->destination_url.spec());
  EXPECT_EQ(900, result.match_at(2)->relevance);
}

TEST_F(AutocompleteResultTest, SortAndCullGroupSuggestionsByType) {
  TestData data[] = {
    { 0, 1,  500, false },
    { 1, 2,  600, false },
    { 2, 1,  700, false },
    { 3, 2,  800, true  },
    { 4, 1,  900, false },
    { 5, 2, 1000, false },
    { 6, 3, 1100, false },
  };
  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  AutocompleteMatchType::Type match_types[] = {
      AutocompleteMatchType::SEARCH_SUGGEST,
      AutocompleteMatchType::HISTORY_URL,
      AutocompleteMatchType::SEARCH_HISTORY,
      AutocompleteMatchType::HISTORY_TITLE,
      AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED,
      AutocompleteMatchType::HISTORY_BODY,
      AutocompleteMatchType::BOOKMARK_TITLE};
  for (size_t i = 0; i < base::size(data); ++i)
    matches[i].type = match_types[i];

  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      omnibox::kOmniboxGroupSuggestionsBySearchVsUrl);

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  TestData expected_data[] = {
    { 3, 2,  800, true  },  // default match unmoved
    { 4, 1,  900, false },  // search types
    { 2, 1,  700, false },
    { 6, 3, 1100, false },  // other types
    { 5, 2, 1000, false },
    { 1, 2,  600, false },
  };
  AssertResultMatches(result, expected_data, base::size(expected_data));
}

TEST_F(AutocompleteResultTest, SortAndCullMaxURLMatches) {
  base::test::ScopedFeatureList feature_list;
  std::map<std::string, std::string> parameters = {
      {OmniboxFieldTrial::kOmniboxMaxURLMatchesParam, "3"}};
  feature_list.InitAndEnableFeatureWithParameters(
      omnibox::kOmniboxMaxURLMatches, parameters);
  EXPECT_TRUE(OmniboxFieldTrial::IsMaxURLMatchesFeatureEnabled());
  EXPECT_EQ(OmniboxFieldTrial::GetMaxURLMatches(), 3u);

  // Case 1: Eject URL match for a search.
  {
    ACMatches matches;
    const AutocompleteMatchTestData data[] = {
        {"http://search-what-you-typed/",
         AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED},
        {"http://search-history/", AutocompleteMatchType::SEARCH_HISTORY},
        {"http://history-url/", AutocompleteMatchType::HISTORY_URL},
        {"http://history-title/", AutocompleteMatchType::HISTORY_TITLE},
        {"http://url-what-you-typed/",
         AutocompleteMatchType::URL_WHAT_YOU_TYPED},
        {"http://clipboard-url/", AutocompleteMatchType::CLIPBOARD_URL},
        {"http://search-suggest/", AutocompleteMatchType::SEARCH_SUGGEST},
    };
    PopulateAutocompleteMatchesFromTestData(data, base::size(data), &matches);

    AutocompleteInput input(base::ASCIIToUTF16("a"),
                            metrics::OmniboxEventProto::OTHER,
                            TestSchemeClassifier());
    AutocompleteResult result;
    result.AppendMatches(input, matches);
    result.SortAndCull(input, template_url_service_.get());

    EXPECT_EQ(result.size(), 6u);
    AutocompleteMatchType::Type expected_types[] = {
        AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED,
        AutocompleteMatchType::SEARCH_HISTORY,
        AutocompleteMatchType::HISTORY_URL,
        AutocompleteMatchType::HISTORY_TITLE,
        AutocompleteMatchType::URL_WHAT_YOU_TYPED,
        AutocompleteMatchType::SEARCH_SUGGEST,
    };
    for (size_t i = 0; i < result.size(); ++i)
      EXPECT_EQ(result.match_at(i)->type, expected_types[i]);
  }

  // Case 2: Do not eject URL match because there's no replacement.
  {
    ACMatches matches;
    const AutocompleteMatchTestData data[] = {
        {"http://search-what-you-typed/",
         AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED},
        {"http://search-history/", AutocompleteMatchType::SEARCH_HISTORY},
        {"http://history-url/", AutocompleteMatchType::HISTORY_URL},
        {"http://history-title/", AutocompleteMatchType::HISTORY_TITLE},
        {"http://url-what-you-typed/",
         AutocompleteMatchType::URL_WHAT_YOU_TYPED},
        {"http://clipboard-url/", AutocompleteMatchType::CLIPBOARD_URL},
        {"http://bookmark-title/", AutocompleteMatchType::BOOKMARK_TITLE},
    };
    PopulateAutocompleteMatchesFromTestData(data, base::size(data), &matches);

    AutocompleteInput input(base::ASCIIToUTF16("a"),
                            metrics::OmniboxEventProto::OTHER,
                            TestSchemeClassifier());
    AutocompleteResult result;
    result.AppendMatches(input, matches);
    result.SortAndCull(input, template_url_service_.get());

    EXPECT_EQ(result.size(), 6u);
    AutocompleteMatchType::Type expected_types[] = {
        AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED,
        AutocompleteMatchType::SEARCH_HISTORY,
        AutocompleteMatchType::HISTORY_URL,
        AutocompleteMatchType::HISTORY_TITLE,
        AutocompleteMatchType::URL_WHAT_YOU_TYPED,
        AutocompleteMatchType::CLIPBOARD_URL,
    };
    for (size_t i = 0; i < result.size(); ++i)
      EXPECT_EQ(result.match_at(i)->type, expected_types[i]);
  }
}

TEST_F(AutocompleteResultTest, TopMatchIsStandaloneVerbatimMatch) {
  ACMatches matches;
  AutocompleteResult result;
  result.AppendMatches(AutocompleteInput(), matches);

  // Case 1: Result set is empty.
  EXPECT_FALSE(result.TopMatchIsStandaloneVerbatimMatch());

  // Case 2: Top match is not a verbatim match.
  PopulateAutocompleteMatchesFromTestData(kNonVerbatimMatches, 1, &matches);
  result.AppendMatches(AutocompleteInput(), matches);
  EXPECT_FALSE(result.TopMatchIsStandaloneVerbatimMatch());
  result.Reset();
  matches.clear();

  // Case 3: Top match is a verbatim match.
  PopulateAutocompleteMatchesFromTestData(kVerbatimMatches, 1, &matches);
  result.AppendMatches(AutocompleteInput(), matches);
  EXPECT_TRUE(result.TopMatchIsStandaloneVerbatimMatch());
  result.Reset();
  matches.clear();

  // Case 4: Standalone verbatim match found in AutocompleteResult.
  PopulateAutocompleteMatchesFromTestData(kVerbatimMatches, 1, &matches);
  PopulateAutocompleteMatchesFromTestData(kNonVerbatimMatches, 1, &matches);
  result.AppendMatches(AutocompleteInput(), matches);
  EXPECT_TRUE(result.TopMatchIsStandaloneVerbatimMatch());
  result.Reset();
  matches.clear();
}

namespace {

bool EqualClassifications(const std::vector<ACMatchClassification>& lhs,
                          const std::vector<ACMatchClassification>& rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t n = 0; n < lhs.size(); ++n)
    if (lhs[n].style != rhs[n].style || lhs[n].offset != rhs[n].offset)
      return false;
  return true;
}

}  // namespace

TEST_F(AutocompleteResultTest, InlineTailPrefixes) {
  struct TestData {
    AutocompleteMatchType::Type type;
    std::string before_contents, after_contents;
    std::vector<ACMatchClassification> before_contents_class;
    std::vector<ACMatchClassification> after_contents_class;
  } cases[] = {
      // It should not touch this, since it's not a tail suggestion.
      {
          AutocompleteMatchType::SEARCH_WHAT_YOU_TYPED,
          "this is a test",
          "this is a test",
          {{0, ACMatchClassification::NONE}, {9, ACMatchClassification::MATCH}},
          {{0, ACMatchClassification::NONE}, {9, ACMatchClassification::MATCH}},
      },
      // Make sure it finds this tail suggestion, and prepends appropriately.
      {
          AutocompleteMatchType::SEARCH_SUGGEST_TAIL,
          "a recording",
          "... a recording",
          {{0, ACMatchClassification::MATCH}},
          {{0, ACMatchClassification::NONE}, {4, ACMatchClassification::MATCH}},
      },
  };
  ACMatches matches;
  for (const auto& test_case : cases) {
    AutocompleteMatch match;
    match.type = test_case.type;
    match.contents = base::UTF8ToUTF16(test_case.before_contents);
    for (const auto& classification : test_case.before_contents_class)
      match.contents_class.push_back(classification);
    matches.push_back(match);
  }
  // Tail suggestion needs one-off initialization.
  matches[1].RecordAdditionalInfo(kACMatchPropertyContentsStartIndex, "9");
  matches[1].RecordAdditionalInfo(kACMatchPropertySuggestionText,
                                  "this is a test");
  AutocompleteResult result;
  result.AppendMatches(AutocompleteInput(), matches);
  result.InlineTailPrefixes();
  for (size_t i = 0; i < base::size(cases); ++i) {
    EXPECT_EQ(result.match_at(i)->contents,
              base::UTF8ToUTF16(cases[i].after_contents));
    EXPECT_TRUE(EqualClassifications(result.match_at(i)->contents_class,
                                     cases[i].after_contents_class));
  }
  // Run twice and make sure that it doesn't re-prepend ellipsis.
  result.InlineTailPrefixes();
  for (size_t i = 0; i < base::size(cases); ++i) {
    EXPECT_EQ(result.match_at(i)->contents,
              base::UTF8ToUTF16(cases[i].after_contents));
    EXPECT_TRUE(EqualClassifications(result.match_at(i)->contents_class,
                                     cases[i].after_contents_class));
  }
}

TEST_F(AutocompleteResultTest, ConvertsOpenTabsCorrectly) {
  AutocompleteResult result;
  AutocompleteMatch match;
  match.destination_url = GURL("http://this-site-matches.com");
  result.matches_.push_back(match);
  match.destination_url = GURL("http://other-site-matches.com");
  match.description = base::UTF8ToUTF16("Some Other Site");
  result.matches_.push_back(match);
  match.destination_url = GURL("http://doesnt-match.com");
  match.description = base::string16();
  result.matches_.push_back(match);

  // Have IsTabOpenWithURL() return true for some URLs.
  FakeAutocompleteProviderClient client;
  client.set_url_substring_match("matches");

  result.ConvertOpenTabMatches(&client, nullptr);

  EXPECT_TRUE(result.match_at(0)->has_tab_match);
  EXPECT_TRUE(result.match_at(1)->has_tab_match);
  EXPECT_FALSE(result.match_at(2)->has_tab_match);
}

TEST_F(AutocompleteResultTest, DocumentSuggestionsCanMergeButNotToDefault) {
  // Types are populated below to avoid introducing a new test data creation
  // process.
  TestData data[] = {
      {1, 4, 500, false},   // DOCUMENT result for url [1].
      {1, 1, 1100, false},  // HISTORY result for url [1], higher priority.
      {2, 4, 600, false},   // DOCUMENT result for [2].
      {2, 1, 1200, true},   // HISTORY result for url [2], higher priority,
                            // Can be default.
      {3, 4, 1000, false},  // DOCUMENT result for [3], higher priority
      {3, 1, 400, false},   // HISTORY result for url [3].
  };
  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  matches[0].type = AutocompleteMatchType::DOCUMENT_SUGGESTION;
  matches[1].type = AutocompleteMatchType::HISTORY_URL;
  matches[2].type = AutocompleteMatchType::DOCUMENT_SUGGESTION;
  matches[3].type = AutocompleteMatchType::HISTORY_URL;
  matches[4].type = AutocompleteMatchType::DOCUMENT_SUGGESTION;
  matches[5].type = AutocompleteMatchType::HISTORY_URL;

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);
  result.SortAndCull(input, template_url_service_.get());

  // We expect three results:
  // The document result for [1] may override the history result.
  // The document result for [2] cannot override a potentially-default result.
  // The document result for [3] is already higher-priority.
  EXPECT_EQ(result.size(), 3u);

  // First result should be the default with its original top-ranking score.
  EXPECT_EQ(result.match_at(0)->relevance, 1200);
  EXPECT_EQ(AutocompleteMatchType::HISTORY_URL, result.match_at(0)->type);
  EXPECT_TRUE(result.match_at(0)->allowed_to_be_default_match);

  // Second result should be a document result with elevated score.
  // The second DOCUMENT result is deduped and effectively dropped.
  EXPECT_EQ(result.match_at(1)->relevance, 1100);
  EXPECT_EQ(AutocompleteMatchType::DOCUMENT_SUGGESTION,
            result.match_at(1)->type);
  EXPECT_FALSE(result.match_at(1)->allowed_to_be_default_match);

  // Third result should be a document with original score. The history result
  // it duped against is lower-priority.
  EXPECT_EQ(result.match_at(2)->relevance, 1000);
  EXPECT_EQ(AutocompleteMatchType::DOCUMENT_SUGGESTION,
            result.match_at(2)->type);
  EXPECT_FALSE(result.match_at(2)->allowed_to_be_default_match);
}

TEST_F(AutocompleteResultTest, PedalSuggestionsCantBeDefaultMatch) {
  TestData data[] = {
      {1, 1, 500, true},
      {0, 1, 1100, true},
  };

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  matches[0].contents = base::UTF8ToUTF16("clear chrome history");
  matches[1].contents = base::UTF8ToUTF16("open incognito tab");

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);

  FakeAutocompleteProviderClient client;
  result.AppendDedicatedPedalMatches(&client, input);

  // Two distinct Pedals should be appended.
  EXPECT_EQ(result.size(), 4u);
  EXPECT_NE(result.match_at(2)->pedal, nullptr);
  EXPECT_NE(result.match_at(3)->pedal, nullptr);

  // Neither should be allowed to be default match, even though they were both
  // derived from suggestions where the field is set true.
  EXPECT_TRUE(result.match_at(0)->allowed_to_be_default_match);
  EXPECT_TRUE(result.match_at(1)->allowed_to_be_default_match);
  EXPECT_FALSE(result.match_at(2)->allowed_to_be_default_match);
  EXPECT_FALSE(result.match_at(3)->allowed_to_be_default_match);
}

TEST_F(AutocompleteResultTest, PedalSuggestionsRemainUnique) {
  TestData data[] = {
      {1, 1, 500, true},
      {0, 1, 1100, true},
      {2, 1, 1000, true},
      {0, 1, 1200, true},
  };

  ACMatches matches;
  PopulateAutocompleteMatches(data, base::size(data), &matches);
  matches[0].contents = base::UTF8ToUTF16("clear chrome history");
  matches[1].contents = base::UTF8ToUTF16("open incognito tab");
  matches[2].contents = base::UTF8ToUTF16("clear chrome history");

  AutocompleteInput input(base::ASCIIToUTF16("a"),
                          metrics::OmniboxEventProto::OTHER,
                          TestSchemeClassifier());
  AutocompleteResult result;
  result.AppendMatches(input, matches);

  FakeAutocompleteProviderClient client;
  result.AppendDedicatedPedalMatches(&client, input);

  // Exactly 2 (not 3) unique Pedals should be added with relevance close to max
  // of the triggering suggestions.
  EXPECT_EQ(result.size(), 6u);
  EXPECT_NE(result.match_at(4)->pedal, nullptr);
  EXPECT_NE(result.match_at(5)->pedal, nullptr);
  EXPECT_NE(result.match_at(4)->pedal, result.match_at(5)->pedal);
  EXPECT_EQ(result.match_at(4)->relevance, 999);
  EXPECT_EQ(result.match_at(5)->relevance, 1099);

  // Now artificially modify existing suggestions and run again to ensure that
  // no duplicates are added, but the existing Pedal suggestion is updated.
  result.match_at(3)->contents = base::UTF8ToUTF16("open incognito tab");
  result.AppendDedicatedPedalMatches(&client, input);
  EXPECT_EQ(result.size(), 6u);
  EXPECT_NE(result.match_at(4)->pedal, nullptr);
  EXPECT_NE(result.match_at(5)->pedal, nullptr);
  EXPECT_NE(result.match_at(4)->pedal, result.match_at(5)->pedal);
  EXPECT_EQ(result.match_at(5)->relevance, 1199);
}
