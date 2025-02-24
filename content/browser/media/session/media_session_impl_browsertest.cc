// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/media/session/media_session_impl.h"

#include <stddef.h>

#include <list>
#include <vector>

#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_samples.h"
#include "base/strings/strcat.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/simple_test_tick_clock.h"
#include "content/browser/media/session/audio_focus_delegate.h"
#include "content/browser/media/session/mock_media_session_player_observer.h"
#include "content/browser/media/session/mock_media_session_service_impl.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/favicon_url.h"
#include "content/public/test/content_browser_test.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/shell/browser/shell.h"
#include "media/base/media_content_type.h"
#include "net/dns/mock_host_resolver.h"
#include "services/media_session/public/cpp/test/mock_media_session.h"
#include "services/media_session/public/mojom/audio_focus.mojom.h"
#include "testing/gmock/include/gmock/gmock.h"

using content::WebContents;
using content::MediaSession;
using content::MediaSessionImpl;
using content::AudioFocusDelegate;
using content::MediaSessionPlayerObserver;
using content::MediaSessionUmaHelper;
using content::MockMediaSessionPlayerObserver;

using media_session::mojom::AudioFocusType;
using media_session::mojom::MediaPlaybackState;
using media_session::mojom::MediaSessionInfo;

using ::testing::Eq;
using ::testing::Expectation;
using ::testing::NiceMock;
using ::testing::_;

namespace {

const double kDefaultVolumeMultiplier = 1.0;
const double kDuckingVolumeMultiplier = 0.2;
const double kDifferentDuckingVolumeMultiplier = 0.018;

const base::string16 kExpectedSourceTitlePrefix =
    base::ASCIIToUTF16("http://example.com:");

class MockAudioFocusDelegate : public AudioFocusDelegate {
 public:
  MockAudioFocusDelegate(MediaSessionImpl* media_session, bool async_mode)
      : media_session_(media_session), async_mode_(async_mode) {}

  MOCK_METHOD0(AbandonAudioFocus, void());

  AudioFocusDelegate::AudioFocusResult RequestAudioFocus(
      AudioFocusType audio_focus_type) {
    if (async_mode_) {
      requests_.push_back(audio_focus_type);
      return AudioFocusDelegate::AudioFocusResult::kDelayed;
    } else {
      audio_focus_type_ = audio_focus_type;
      return AudioFocusDelegate::AudioFocusResult::kSuccess;
    }
  }

  base::Optional<AudioFocusType> GetCurrentFocusType() const {
    return audio_focus_type_;
  }

  void MediaSessionInfoChanged(
      media_session::mojom::MediaSessionInfoPtr session_info) override {}

  void ResolveRequest(bool result) {
    if (!async_mode_)
      return;

    audio_focus_type_ = requests_.front();
    requests_.pop_front();

    media_session_->FinishSystemAudioFocusRequest(audio_focus_type_.value(),
                                                  result);
  }

  bool HasRequests() const { return !requests_.empty(); }

 private:
  MediaSessionImpl* media_session_;
  const bool async_mode_ = false;

  std::list<AudioFocusType> requests_;
  base::Optional<AudioFocusType> audio_focus_type_;
};

}  // namespace

class MediaSessionImplBrowserTest : public content::ContentBrowserTest {
 protected:
  MediaSessionImplBrowserTest() = default;

  void SetUpOnMainThread() override {
    ContentBrowserTest::SetUpOnMainThread();

    // Navigate to a test page with a a real origin.
    ASSERT_TRUE(embedded_test_server()->Start());
    host_resolver()->AddRule("*", "127.0.0.1");
    NavigateToURL(
        shell(), embedded_test_server()->GetURL("example.com", "/title1.html"));

    media_session_ = MediaSessionImpl::Get(shell()->web_contents());
    mock_audio_focus_delegate_ = new NiceMock<MockAudioFocusDelegate>(
        media_session_, true /* async_mode */);
    media_session_->SetDelegateForTests(
        base::WrapUnique(mock_audio_focus_delegate_));
    ASSERT_TRUE(media_session_);
  }

  void TearDownOnMainThread() override {
    media_session_->RemoveAllPlayersForTest();
    mock_media_session_service_.reset();

    media_session_ = nullptr;

    ContentBrowserTest::TearDownOnMainThread();
  }

  void StartNewPlayer(MockMediaSessionPlayerObserver* player_observer,
                      media::MediaContentType media_content_type) {
    int player_id = player_observer->StartNewPlayer();

    bool result = AddPlayer(player_observer, player_id, media_content_type);

    EXPECT_TRUE(result);
  }

  bool AddPlayer(MockMediaSessionPlayerObserver* player_observer,
                 int player_id,
                 media::MediaContentType type) {
    return media_session_->AddPlayer(player_observer, player_id, type);
  }

  void RemovePlayer(MockMediaSessionPlayerObserver* player_observer,
                    int player_id) {
    media_session_->RemovePlayer(player_observer, player_id);
  }

  void RemovePlayers(MockMediaSessionPlayerObserver* player_observer) {
    media_session_->RemovePlayers(player_observer);
  }

  void OnPlayerPaused(MockMediaSessionPlayerObserver* player_observer,
                      int player_id) {
    media_session_->OnPlayerPaused(player_observer, player_id);
  }

  bool IsActive() { return media_session_->IsActive(); }

  base::Optional<AudioFocusType> GetSessionAudioFocusType() {
    return mock_audio_focus_delegate_->GetCurrentFocusType();
  }

  bool IsControllable() { return media_session_->IsControllable(); }

  void UIResume() { media_session_->Resume(MediaSession::SuspendType::kUI); }

  void SystemResume() {
    media_session_->OnResumeInternal(MediaSession::SuspendType::kSystem);
  }

  void UISuspend() { media_session_->Suspend(MediaSession::SuspendType::kUI); }

  void SystemSuspend(bool temporary) {
    media_session_->OnSuspendInternal(MediaSession::SuspendType::kSystem,
                                      temporary
                                          ? MediaSessionImpl::State::SUSPENDED
                                          : MediaSessionImpl::State::INACTIVE);
  }

  void UISeekForward() {
    media_session_->Seek(base::TimeDelta::FromSeconds(1));
  }

  void UISeekBackward() {
    media_session_->Seek(base::TimeDelta::FromSeconds(-1));
  }

  void SystemStartDucking() { media_session_->StartDucking(); }

  void SystemStopDucking() { media_session_->StopDucking(); }

  void EnsureMediaSessionService() {
    mock_media_session_service_.reset(
        new NiceMock<content::MockMediaSessionServiceImpl>(
            shell()->web_contents()->GetMainFrame()));
  }

  void SetPlaybackState(blink::mojom::MediaSessionPlaybackState state) {
    mock_media_session_service_->SetPlaybackState(state);
  }

  void ResolveAudioFocusSuccess() {
    mock_audio_focus_delegate()->ResolveRequest(true /* result */);
  }

  void ResolveAudioFocusFailure() {
    mock_audio_focus_delegate()->ResolveRequest(false /* result */);
  }

  bool HasUnresolvedAudioFocusRequest() {
    return mock_audio_focus_delegate()->HasRequests();
  }

  MockAudioFocusDelegate* mock_audio_focus_delegate() {
    return mock_audio_focus_delegate_;
  }

  std::unique_ptr<MediaSessionImpl> CreateDummyMediaSession() {
    return base::WrapUnique<MediaSessionImpl>(
        new MediaSessionImpl(CreateBrowser()->web_contents()));
  }

  MediaSessionUmaHelper* GetMediaSessionUMAHelper() {
    return media_session_->uma_helper_for_test();
  }

  void SetAudioFocusDelegateForTests(MockAudioFocusDelegate* delegate) {
    mock_audio_focus_delegate_ = delegate;
    media_session_->SetDelegateForTests(
        base::WrapUnique(mock_audio_focus_delegate_));
  }

  bool IsDucking() const { return media_session_->is_ducking_; }

  base::string16 GetExpectedSourceTitle() {
    return base::StrCat(
        {kExpectedSourceTitlePrefix,
         base::NumberToString16(embedded_test_server()->port())});
  }

 protected:
  MediaSessionImpl* media_session_;
  MockAudioFocusDelegate* mock_audio_focus_delegate_;
  std::unique_ptr<content::MockMediaSessionServiceImpl>
      mock_media_session_service_;

  DISALLOW_COPY_AND_ASSIGN(MediaSessionImplBrowserTest);
};

class MediaSessionImplParamBrowserTest
    : public MediaSessionImplBrowserTest,
      public testing::WithParamInterface<bool> {
 protected:
  MediaSessionImplParamBrowserTest() = default;

  void SetUpOnMainThread() override {
    MediaSessionImplBrowserTest::SetUpOnMainThread();

    SetAudioFocusDelegateForTests(
        new NiceMock<MockAudioFocusDelegate>(media_session_, GetParam()));
  }
};

INSTANTIATE_TEST_SUITE_P(, MediaSessionImplParamBrowserTest, testing::Bool());

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       PlayersFromSameObserverDoNotStopEachOtherInSameSession) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       PlayersFromManyObserverDoNotStopEachOtherInSameSession) {
  auto player_observer_1 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_2 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_3 = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_3.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(player_observer_1->IsPlaying(0));
  EXPECT_TRUE(player_observer_2->IsPlaying(0));
  EXPECT_TRUE(player_observer_3->IsPlaying(0));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SuspendedMediaSessionStopsPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumedMediaSessionRestartsPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemResume();

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StartedPlayerOnSuspendedSessionPlaysAlone) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemSuspend(true);

  EXPECT_FALSE(player_observer->IsPlaying(0));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       InitialVolumeMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));

  ResolveAudioFocusSuccess();

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StartDuckingReducesVolumeMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  SystemStartDucking();

  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(1));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StopDuckingRecoversVolumeMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  SystemStartDucking();
  SystemStopDucking();

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(2));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       DuckingUsesConfiguredMultiplier) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  media_session_->SetDuckingVolumeMultiplier(kDifferentDuckingVolumeMultiplier);
  SystemStartDucking();
  EXPECT_EQ(kDifferentDuckingVolumeMultiplier,
            player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDifferentDuckingVolumeMultiplier,
            player_observer->GetVolumeMultiplier(1));
  SystemStopDucking();
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(1));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AudioFocusInitialState) {
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AddPlayerOnSuspendedFocusUnducks) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  EXPECT_FALSE(IsActive());

  SystemStartDucking();
  EXPECT_EQ(kDuckingVolumeMultiplier, player_observer->GetVolumeMultiplier(0));

  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent));
  ResolveAudioFocusSuccess();
  EXPECT_EQ(kDefaultVolumeMultiplier, player_observer->GetVolumeMultiplier(0));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       CanRequestFocusBeforePlayerCreation) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  media_session_->RequestSystemAudioFocus(AudioFocusType::kGain);
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StartPlayerGivesFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SuspendGivesAwayAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);

  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       StopGivesAwayAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  media_session_->Stop(MediaSession::SuspendType::kUI);

  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SystemResumeGivesBackAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemResume();

  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UIResumeGivesBackAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();

  UIResume();
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingLastPlayerDropsAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer.get(), 1);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer.get(), 2);
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingLastPlayerFromManyObserversDropsAudioFocus) {
  auto player_observer_1 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_2 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_3 = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_3.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer_1.get(), 0);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer_2.get(), 0);
  EXPECT_TRUE(IsActive());
  RemovePlayer(player_observer_3.get(), 0);
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingAllPlayersFromObserversDropsAudioFocus) {
  auto player_observer_1 = std::make_unique<MockMediaSessionPlayerObserver>();
  auto player_observer_2 = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_1.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer_2.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayers(player_observer_1.get());
  EXPECT_TRUE(IsActive());
  RemovePlayers(player_observer_2.get());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumePlayGivesAudioFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
  EXPECT_FALSE(IsActive());

  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent));
  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumeSuspendSeekAreSentOnlyOncePerPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  ResolveAudioFocusSuccess();

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());
  EXPECT_EQ(0, player_observer->received_seek_forward_calls());
  EXPECT_EQ(0, player_observer->received_seek_backward_calls());

  SystemSuspend(true);
  EXPECT_EQ(3, player_observer->received_suspend_calls());

  SystemResume();
  EXPECT_EQ(3, player_observer->received_resume_calls());

  UISeekForward();
  EXPECT_EQ(3, player_observer->received_seek_forward_calls());

  UISeekBackward();
  EXPECT_EQ(3, player_observer->received_seek_backward_calls());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumeSuspendSeekAreSentOnlyOncePerPlayersAddedTwice) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());

  ResolveAudioFocusSuccess();

  // Adding the three players above again.
  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent));
  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 1, media::MediaContentType::Persistent));
  EXPECT_TRUE(
      AddPlayer(player_observer.get(), 2, media::MediaContentType::Persistent));

  EXPECT_EQ(0, player_observer->received_suspend_calls());
  EXPECT_EQ(0, player_observer->received_resume_calls());
  EXPECT_EQ(0, player_observer->received_seek_forward_calls());
  EXPECT_EQ(0, player_observer->received_seek_backward_calls());

  SystemSuspend(true);
  EXPECT_EQ(3, player_observer->received_suspend_calls());

  SystemResume();
  EXPECT_EQ(3, player_observer->received_resume_calls());

  UISeekForward();
  EXPECT_EQ(3, player_observer->received_seek_forward_calls());

  UISeekBackward();
  EXPECT_EQ(3, player_observer->received_seek_backward_calls());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingTheSamePlayerTwiceIsANoop) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
  RemovePlayer(player_observer.get(), 0);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, AudioFocusType) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  // Starting a player with a given type should set the session to that type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  ResolveAudioFocusSuccess();
  EXPECT_EQ(AudioFocusType::kGainTransientMayDuck, GetSessionAudioFocusType());

  // Adding a player of the same type should have no effect on the type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());
  EXPECT_EQ(AudioFocusType::kGainTransientMayDuck, GetSessionAudioFocusType());

  // Adding a player of Content type should override the current type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());

  // Adding a player of the Transient type should have no effect on the type.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());
  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
  EXPECT_TRUE(player_observer->IsPlaying(3));

  SystemSuspend(true);

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(player_observer->IsPlaying(2));
  EXPECT_FALSE(player_observer->IsPlaying(3));

  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());

  SystemResume();

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(player_observer->IsPlaying(2));
  EXPECT_TRUE(player_observer->IsPlaying(3));

  EXPECT_EQ(AudioFocusType::kGain, GetSessionAudioFocusType());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowForContent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a persistent type should show the media controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNoShowForTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should not show the media
    // controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

// This behaviour is specific to desktop.
#if !defined(OS_ANDROID)

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNoShowForTransientAndRoutedService) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should show the media controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNoShowForTransientAndPlaybackStateNone) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should not show the media
    // controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowForTransientAndPlaybackStatePaused) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should show the media controls if
    // we have a playback state from the service.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowForTransientAndPlaybackStatePlaying) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // Starting a player with a transient type should show the media controls if
    // we have a playback state from the service.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

#endif  // !defined(OS_ANDROID)

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenStopped) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  RemovePlayers(player_observer.get());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShownAcceptTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  // Transient player join the session without affecting the controls.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShownAfterContentAdded) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  // The controls are shown when the content player is added.
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsStayIfOnlyOnePlayerHasBeenPaused) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  // Removing only content player doesn't hide the controls since the session
  // is still active.
  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenTheLastPlayerIsRemoved) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());

  RemovePlayer(player_observer.get(), 1);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenAllThePlayersAreRemoved) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  RemovePlayers(player_observer.get());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsNotHideWhenTheLastPlayerIsPaused) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  OnPlayerPaused(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());

  OnPlayerPaused(player_observer.get(), 1);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       SuspendTemporaryUpdatesControls) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedWhenResumed) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);
  SystemResume();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenSessionSuspendedPermanently) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(false);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenSessionStops) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  media_session_->Stop(MediaSession::SuspendType::kUI);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHideWhenSessionChangesFromContentToTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // This should reset the session and change it to a transient, so
    // hide the controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedWhenNewPlayerResetsSession) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // This should reset the session and update the controls.
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsResumedWhenPlayerIsResumed) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SystemSuspend(true);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    // This should resume the session and update the controls.
    AddPlayer(player_observer.get(), 0, media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedDueToResumeSessionAction) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  UISuspend();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsUpdatedDueToSuspendSessionAction) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  UISuspend();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  UIResume();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);

    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsDontShowWhenOneShotIsPresent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_FALSE(IsControllable());
    EXPECT_TRUE(IsActive());
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_FALSE(IsControllable());
    EXPECT_TRUE(IsActive());
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);

    EXPECT_FALSE(IsControllable());
    EXPECT_TRUE(IsActive());
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsHiddenAfterRemoveOneShotWithoutOtherPlayers) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    observer.WaitForControllable(false);
  }

  EXPECT_FALSE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ControlsShowAfterRemoveOneShotWithPersistentPresent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(false);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    observer.WaitForControllable(true);
  }

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       DontSuspendWhenOneShotIsPresent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(false);

  EXPECT_FALSE(IsControllable());
  EXPECT_TRUE(IsActive());

  EXPECT_EQ(0, player_observer->received_suspend_calls());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       DontResumeBySystemUISuspendedSessions) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  SystemResume();
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AllowUIResumeForSystemSuspend) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  UIResume();
  ResolveAudioFocusSuccess();

  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, ResumeSuspendFromUI) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  UIResume();
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ResumeSuspendFromSystem) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  EXPECT_TRUE(IsControllable());
  EXPECT_FALSE(IsActive());

  SystemResume();
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());
  EXPECT_TRUE(IsControllable());
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       OneShotTakesGainFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  ResolveAudioFocusSuccess();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());

  EXPECT_EQ(AudioFocusType::kGain,
            mock_audio_focus_delegate()->GetCurrentFocusType());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingOneShotDropsFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_CALL(*mock_audio_focus_delegate(), AbandonAudioFocus());
  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  ResolveAudioFocusSuccess();

  RemovePlayer(player_observer.get(), 0);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       RemovingOneShotWhileStillHavingOtherPlayersKeepsFocus) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  EXPECT_CALL(*mock_audio_focus_delegate(), AbandonAudioFocus())
      .Times(1);  // Called in TearDown
  StartNewPlayer(player_observer.get(), media::MediaContentType::OneShot);
  ResolveAudioFocusSuccess();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_FALSE(HasUnresolvedAudioFocusRequest());

  RemovePlayer(player_observer.get(), 0);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ActualPlaybackStateWhilePlayerPaused) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  OnPlayerPaused(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kSuspended);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ActualPlaybackStateWhilePlayerPlaying) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       ActualPlaybackStateWhilePlayerRemoved) {
  EnsureMediaSessionService();
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForState(MediaSessionInfo::SessionState::kActive);
    EXPECT_EQ(MediaPlaybackState::kPlaying,
              observer.session_info()->playback_state);
  }

  RemovePlayer(player_observer.get(), 0);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PLAYING);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::PAUSED);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }

  SetPlaybackState(blink::mojom::MediaSessionPlaybackState::NONE);

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForState(MediaSessionInfo::SessionState::kInactive);
    EXPECT_EQ(MediaPlaybackState::kPaused,
              observer.session_info()->playback_state);
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_SystemTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  SystemSuspend(true);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(0, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_SystemPermantent) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  SystemSuspend(false);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(0, samples->GetCount(0));  // System Transient
  EXPECT_EQ(1, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(0, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, UMA_Suspended_UI) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  UISuspend();

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(0, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(1, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_Multiple) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  UIResume();
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemResume();

  UISuspend();
  UIResume();
  ResolveAudioFocusSuccess();

  SystemSuspend(false);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(4, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(0));  // System Transient
  EXPECT_EQ(1, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(2, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_Suspended_Crossing) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  UISuspend();
  SystemSuspend(true);
  SystemSuspend(false);
  UIResume();
  ResolveAudioFocusSuccess();

  SystemSuspend(true);
  SystemSuspend(true);
  SystemSuspend(false);
  SystemResume();

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(2, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(1, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest, UMA_Suspended_Stop) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.Suspended"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(0, samples->GetCount(0));  // System Transient
  EXPECT_EQ(0, samples->GetCount(1));  // System Permanent
  EXPECT_EQ(1, samples->GetCount(2));  // UI
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_NoActivation) {
  base::HistogramTester tester;

  std::unique_ptr<MediaSessionImpl> media_session = CreateDummyMediaSession();
  media_session.reset();

  // A MediaSession that wasn't active doesn't register an active time.
  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(0, samples->TotalCount());
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_SimpleActivation) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(1000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivationWithUISuspension) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  UISuspend();

  clock.Advance(base::TimeDelta::FromMilliseconds(2000));
  UIResume();
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(2000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivationWithSystemSuspension) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  SystemSuspend(true);

  clock.Advance(base::TimeDelta::FromMilliseconds(2000));
  SystemResume();

  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(1, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(2000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivateSuspendedButNotStopped) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(500));
  SystemSuspend(true);

  {
    std::unique_ptr<base::HistogramSamples> samples(
        tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
    EXPECT_EQ(0, samples->TotalCount());
  }

  SystemResume();
  clock.Advance(base::TimeDelta::FromMilliseconds(5000));
  UISuspend();

  {
    std::unique_ptr<base::HistogramSamples> samples(
        tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
    EXPECT_EQ(0, samples->TotalCount());
  }
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_ActivateSuspendStopTwice) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(500));
  SystemSuspend(true);
  media_session_->Stop(MediaSession::SuspendType::kUI);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(5000));
  SystemResume();
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(2, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(500));
  EXPECT_EQ(1, samples->GetCount(5000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       UMA_ActiveTime_MultipleActivations) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();
  base::HistogramTester tester;

  MediaSessionUmaHelper* media_session_uma_helper = GetMediaSessionUMAHelper();
  base::SimpleTestTickClock clock;
  clock.SetNowTicks(base::TimeTicks::Now());
  media_session_uma_helper->SetClockForTest(&clock);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(10000));
  RemovePlayer(player_observer.get(), 0);

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  ResolveAudioFocusSuccess();
  clock.Advance(base::TimeDelta::FromMilliseconds(1000));
  media_session_->Stop(MediaSession::SuspendType::kUI);

  std::unique_ptr<base::HistogramSamples> samples(
      tester.GetHistogramSamplesSinceCreation("Media.Session.ActiveTime"));
  EXPECT_EQ(2, samples->TotalCount());
  EXPECT_EQ(1, samples->GetCount(1000));
  EXPECT_EQ(1, samples->GetCount(10000));
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AddingObserverNotifiesCurrentInformation_EmptyInfo) {
  media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

  media_session::MediaMetadata expected_metadata;
  expected_metadata.title = shell()->web_contents()->GetTitle();
  expected_metadata.artist = GetExpectedSourceTitle();
  observer.WaitForExpectedMetadata(expected_metadata);
}

IN_PROC_BROWSER_TEST_P(MediaSessionImplParamBrowserTest,
                       AddingMojoObserverNotifiesCurrentInformation_WithInfo) {
  // Set up the service and information.
  EnsureMediaSessionService();

  media_session::MediaMetadata expected_metadata;
  expected_metadata.title = base::ASCIIToUTF16("title");
  expected_metadata.artist = base::ASCIIToUTF16("artist");
  expected_metadata.album = base::ASCIIToUTF16("album");
  expected_metadata.source_title = GetExpectedSourceTitle();

  blink::mojom::SpecMediaMetadataPtr spec_metadata(
      blink::mojom::SpecMediaMetadata::New());
  spec_metadata->title = base::ASCIIToUTF16("title");
  spec_metadata->artist = base::ASCIIToUTF16("artist");
  spec_metadata->album = base::ASCIIToUTF16("album");
  mock_media_session_service_->SetMetadata(std::move(spec_metadata));

  // Make sure the service is routed,
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>(
      shell()->web_contents()->GetMainFrame());

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    ResolveAudioFocusSuccess();

    observer.WaitForExpectedMetadata(expected_metadata);
  }
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_RequestFailure_Gain) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());

  // The gain request failed so we should suspend the whole session.
  ResolveAudioFocusFailure();
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_FALSE(IsActive());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       Async_RequestFailure_GainTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());

  // A transient audio focus failure should only affect transient players.
  ResolveAudioFocusFailure();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
  EXPECT_TRUE(IsActive());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_GainThenTransient) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_TransientThenGain) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);

  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));
  EXPECT_TRUE(player_observer->IsPlaying(1));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       Async_SuspendBeforeResolve) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemSuspend(true);
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(IsActive());

  ResolveAudioFocusSuccess();
  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(IsActive());

  SystemResume();
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_ResumeBeforeResolve) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  UISuspend();
  EXPECT_FALSE(IsActive());
  EXPECT_FALSE(player_observer->IsPlaying(0));

  UIResume();
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  ResolveAudioFocusFailure();
  EXPECT_FALSE(IsActive());
  EXPECT_FALSE(player_observer->IsPlaying(0));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_RemoveBeforeResolve) {
  {
    auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

    EXPECT_CALL(*mock_audio_focus_delegate(), AbandonAudioFocus());
    StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
    EXPECT_TRUE(player_observer->IsPlaying(0));

    RemovePlayer(player_observer.get(), 0);
  }

  ResolveAudioFocusSuccess();
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_StopBeforeResolve) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Transient);
  ResolveAudioFocusSuccess();
  EXPECT_TRUE(player_observer->IsPlaying(0));

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(player_observer->IsPlaying(1));

  media_session_->Stop(MediaSession::SuspendType::kUI);
  ResolveAudioFocusSuccess();

  EXPECT_FALSE(player_observer->IsPlaying(0));
  EXPECT_FALSE(player_observer->IsPlaying(1));
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Failure) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusFailure();
  EXPECT_TRUE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Inactive) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  media_session_->Stop(MediaSession::SuspendType::kUI);
  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Success) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusSuccess();
  EXPECT_FALSE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, Async_Unducking_Suspended) {
  auto player_observer = std::make_unique<MockMediaSessionPlayerObserver>();

  StartNewPlayer(player_observer.get(), media::MediaContentType::Persistent);
  EXPECT_TRUE(IsActive());
  EXPECT_TRUE(player_observer->IsPlaying(0));

  UISuspend();
  SystemStartDucking();
  EXPECT_TRUE(IsDucking());

  ResolveAudioFocusSuccess();
  EXPECT_TRUE(IsDucking());
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, MetadataWhenFileUrlScheme) {
  NavigateToURL(shell(), GURL("file:///"));

  media_session::test::MockMediaSessionMojoObserver observer(*media_session_);

  media_session::MediaMetadata expected_metadata;
  expected_metadata.title = shell()->web_contents()->GetTitle();
  expected_metadata.artist = base::ASCIIToUTF16("Local File");
  observer.WaitForExpectedMetadata(expected_metadata);
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest, UpdateFaviconURL) {
  std::vector<gfx::Size> valid_sizes;
  valid_sizes.push_back(gfx::Size(100, 100));
  valid_sizes.push_back(gfx::Size(200, 200));

  std::vector<content::FaviconURL> favicons;
  favicons.push_back(content::FaviconURL(
      GURL("https://www.example.org/favicon1.png"),
      content::FaviconURL::IconType::kInvalid, valid_sizes));
  favicons.push_back(content::FaviconURL(
      GURL(), content::FaviconURL::IconType::kFavicon, valid_sizes));
  favicons.push_back(content::FaviconURL(
      GURL("https://www.example.org/favicon2.png"),
      content::FaviconURL::IconType::kFavicon, std::vector<gfx::Size>()));
  favicons.push_back(content::FaviconURL(
      GURL("https://www.example.org/favicon3.png"),
      content::FaviconURL::IconType::kFavicon, valid_sizes));
  favicons.push_back(content::FaviconURL(
      GURL("https://www.example.org/favicon4.png"),
      content::FaviconURL::IconType::kTouchIcon, valid_sizes));
  favicons.push_back(content::FaviconURL(
      GURL("https://www.example.org/favicon5.png"),
      content::FaviconURL::IconType::kTouchPrecomposedIcon, valid_sizes));

  media_session_->DidUpdateFaviconURL(favicons);

  {
    std::vector<media_session::MediaImage> expected_images;
    media_session::MediaImage test_image_1;
    test_image_1.src = GURL("https://www.example.org/favicon3.png");
    test_image_1.sizes = valid_sizes;
    expected_images.push_back(test_image_1);

    media_session::MediaImage test_image_2;
    test_image_2.src = GURL("https://www.example.org/favicon4.png");
    test_image_2.sizes = valid_sizes;
    expected_images.push_back(test_image_2);

    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        expected_images);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    media_session_->DidUpdateFaviconURL(std::vector<content::FaviconURL>());
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        std::vector<media_session::MediaImage>());
  }
}

IN_PROC_BROWSER_TEST_F(MediaSessionImplBrowserTest,
                       UpdateFaviconURL_ClearOnNavigate) {
  std::vector<gfx::Size> valid_sizes;
  valid_sizes.push_back(gfx::Size(100, 100));

  std::vector<content::FaviconURL> favicons;
  favicons.push_back(content::FaviconURL(
      GURL("https://www.example.org/favicon1.png"),
      content::FaviconURL::IconType::kFavicon, valid_sizes));

  media_session_->DidUpdateFaviconURL(favicons);

  {
    std::vector<media_session::MediaImage> expected_images;
    media_session::MediaImage test_image_1;
    test_image_1.src = GURL("https://www.example.org/favicon1.png");
    test_image_1.sizes = valid_sizes;
    expected_images.push_back(test_image_1);

    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        expected_images);
  }

  {
    media_session::test::MockMediaSessionMojoObserver observer(*media_session_);
    NavigateToURL(
        shell(), embedded_test_server()->GetURL("example.com", "/title1.html"));
    observer.WaitForExpectedImagesOfType(
        media_session::mojom::MediaSessionImageType::kSourceIcon,
        std::vector<media_session::MediaImage>());
  }
}
