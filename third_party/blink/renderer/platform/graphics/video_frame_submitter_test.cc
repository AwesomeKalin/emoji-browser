// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/graphics/video_frame_submitter.h"

#include <memory>
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/memory/ptr_util.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/test/scoped_task_environment.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/time/time.h"
#include "cc/layers/video_frame_provider.h"
#include "cc/test/layer_test_common.h"
#include "cc/trees/layer_tree_settings.h"
#include "cc/trees/task_runner_provider.h"
#include "components/viz/test/fake_external_begin_frame_source.h"
#include "components/viz/test/test_context_provider.h"
#include "media/base/video_frame.h"
#include "mojo/public/cpp/bindings/interface_request.h"
#include "services/viz/public/interfaces/compositing/compositor_frame_sink.mojom-blink.h"
#include "services/viz/public/interfaces/hit_test/hit_test_region_list.mojom-blink.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/platform/graphics/test/mock_compositor_frame_sink.h"
#include "third_party/blink/renderer/platform/graphics/test/mock_embedded_frame_sink_provider.h"
#include "third_party/blink/renderer/platform/graphics/video_frame_resource_provider.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

using testing::_;
using testing::AnyNumber;
using testing::Return;
using testing::StrictMock;

namespace blink {

namespace {

class MockVideoFrameProvider : public cc::VideoFrameProvider {
 public:
  MockVideoFrameProvider() = default;
  ~MockVideoFrameProvider() override = default;

  MOCK_METHOD1(SetVideoFrameProviderClient, void(Client*));
  MOCK_METHOD2(UpdateCurrentFrame, bool(base::TimeTicks, base::TimeTicks));
  MOCK_METHOD0(HasCurrentFrame, bool());
  MOCK_METHOD0(GetCurrentFrame, scoped_refptr<media::VideoFrame>());
  MOCK_METHOD0(PutCurrentFrame, void());

  base::TimeDelta GetPreferredRenderInterval() override {
    return preferred_interval;
  }

  base::TimeDelta preferred_interval;

 private:
  DISALLOW_COPY_AND_ASSIGN(MockVideoFrameProvider);
};

class VideoMockCompositorFrameSink
    : public viz::mojom::blink::CompositorFrameSink {
 public:
  VideoMockCompositorFrameSink(
      viz::mojom::blink::CompositorFrameSinkRequest* request)
      : binding_(this, std::move(*request)) {}
  ~VideoMockCompositorFrameSink() override = default;

  const viz::CompositorFrame& last_submitted_compositor_frame() const {
    return last_submitted_compositor_frame_;
  }

  MOCK_METHOD1(SetNeedsBeginFrame, void(bool));
  MOCK_METHOD0(SetWantsAnimateOnlyBeginFrames, void());

  MOCK_METHOD2(DoSubmitCompositorFrame,
               void(const viz::LocalSurfaceId&, viz::CompositorFrame*));
  void SubmitCompositorFrame(
      const viz::LocalSurfaceId& id,
      viz::CompositorFrame frame,
      viz::mojom::blink::HitTestRegionListPtr hit_test_region_list,
      uint64_t submit_time) override {
    last_submitted_compositor_frame_ = std::move(frame);
    DoSubmitCompositorFrame(id, &last_submitted_compositor_frame_);
  }
  void SubmitCompositorFrameSync(
      const viz::LocalSurfaceId& id,
      viz::CompositorFrame frame,
      viz::mojom::blink::HitTestRegionListPtr hit_test_region_list,
      uint64_t submit_time,
      const SubmitCompositorFrameSyncCallback callback) override {
    last_submitted_compositor_frame_ = std::move(frame);
    DoSubmitCompositorFrame(id, &last_submitted_compositor_frame_);
  }

  MOCK_METHOD1(DidNotProduceFrame, void(const viz::BeginFrameAck&));

  MOCK_METHOD2(DidAllocateSharedBitmap_,
               void(base::ReadOnlySharedMemoryRegion* region,
                    gpu::mojom::blink::MailboxPtr* id));
  void DidAllocateSharedBitmap(base::ReadOnlySharedMemoryRegion region,
                               gpu::mojom::blink::MailboxPtr id) override {
    DidAllocateSharedBitmap_(&region, &id);
  }

  MOCK_METHOD1(DidDeleteSharedBitmap_, void(gpu::mojom::blink::MailboxPtr* id));
  void DidDeleteSharedBitmap(gpu::mojom::blink::MailboxPtr id) override {
    DidDeleteSharedBitmap_(&id);
  }

 private:
  mojo::Binding<viz::mojom::blink::CompositorFrameSink> binding_;

  viz::CompositorFrame last_submitted_compositor_frame_;

  DISALLOW_COPY_AND_ASSIGN(VideoMockCompositorFrameSink);
};

class MockVideoFrameResourceProvider
    : public blink::VideoFrameResourceProvider {
 public:
  MockVideoFrameResourceProvider(
      viz::RasterContextProvider* context_provider,
      viz::SharedBitmapReporter* shared_bitmap_reporter)
      : blink::VideoFrameResourceProvider(cc::LayerTreeSettings(), false) {
    blink::VideoFrameResourceProvider::Initialize(context_provider,
                                                  shared_bitmap_reporter);
  }
  ~MockVideoFrameResourceProvider() override = default;

  MOCK_METHOD2(Initialize,
               void(viz::RasterContextProvider*, viz::SharedBitmapReporter*));
  MOCK_METHOD4(AppendQuads,
               void(viz::RenderPass*,
                    scoped_refptr<media::VideoFrame>,
                    media::VideoRotation,
                    bool));
  MOCK_METHOD0(ReleaseFrameResources, void());
  MOCK_METHOD2(PrepareSendToParent,
               void(const std::vector<viz::ResourceId>&,
                    std::vector<viz::TransferableResource>*));
  MOCK_METHOD1(
      ReceiveReturnsFromParent,
      void(const std::vector<viz::ReturnedResource>& transferable_resources));
  MOCK_METHOD0(ObtainContextProvider, void());

 private:
  DISALLOW_COPY_AND_ASSIGN(MockVideoFrameResourceProvider);
};
}  // namespace

class VideoFrameSubmitterTest : public testing::Test {
 public:
  VideoFrameSubmitterTest()
      : now_src_(new base::SimpleTestTickClock()),
        begin_frame_source_(new viz::FakeExternalBeginFrameSource(0.f, false)),
        video_frame_provider_(new StrictMock<MockVideoFrameProvider>()),
        context_provider_(viz::TestContextProvider::Create()) {
    context_provider_->BindToCurrentThread();
    MakeSubmitter();
    scoped_task_environment_.RunUntilIdle();
  }

  void MakeSubmitter() {
    resource_provider_ = new StrictMock<MockVideoFrameResourceProvider>(
        context_provider_.get(), nullptr);
    submitter_ = std::make_unique<VideoFrameSubmitter>(
        base::DoNothing(),
        base::WrapUnique<MockVideoFrameResourceProvider>(resource_provider_));

    submitter_->Initialize(video_frame_provider_.get());
    viz::mojom::blink::CompositorFrameSinkPtr submitter_sink;
    viz::mojom::blink::CompositorFrameSinkRequest request =
        mojo::MakeRequest(&submitter_sink);
    sink_ =
        std::make_unique<StrictMock<VideoMockCompositorFrameSink>>(&request);

    // By setting the submission state before we set the sink, we can make
    // testing easier without having to worry about the first sent frame.
    submitter_->SetIsSurfaceVisible(true);
    submitter_->compositor_frame_sink_ = std::move(submitter_sink);
    mojom::blink::SurfaceEmbedderPtr embedder;
    mojo::MakeRequest(&embedder);
    submitter_->surface_embedder_ = std::move(embedder);
    auto surface_id = viz::SurfaceId(
        viz::FrameSinkId(1, 1),
        viz::LocalSurfaceId(11,
                            base::UnguessableToken::Deserialize(0x111111, 0)));
    submitter_->frame_sink_id_ = surface_id.frame_sink_id();
    submitter_->child_local_surface_id_allocator_.UpdateFromParent(
        viz::LocalSurfaceIdAllocation(surface_id.local_surface_id(),
                                      base::TimeTicks::Now()));
  }

  bool IsRendering() const { return submitter_->is_rendering_; }

  cc::VideoFrameProvider* GetProvider() const {
    return submitter_->video_frame_provider_;
  }

  bool ShouldSubmit() const { return submitter_->ShouldSubmit(); }

  void SubmitSingleFrame() { submitter_->SubmitSingleFrame(); }

  const viz::ChildLocalSurfaceIdAllocator& child_local_surface_id_allocator()
      const {
    return submitter_->child_local_surface_id_allocator_;
  }

  gfx::Size frame_size() const { return submitter_->frame_size_; }

  void OnReceivedContextProvider(
      bool use_gpu_compositing,
      scoped_refptr<viz::RasterContextProvider> context_provider) {
    submitter_->OnReceivedContextProvider(use_gpu_compositing,
                                          std::move(context_provider));
  }

 protected:
  base::test::ScopedTaskEnvironment scoped_task_environment_;
  std::unique_ptr<base::SimpleTestTickClock> now_src_;
  std::unique_ptr<viz::FakeExternalBeginFrameSource> begin_frame_source_;
  std::unique_ptr<StrictMock<VideoMockCompositorFrameSink>> sink_;
  std::unique_ptr<StrictMock<MockVideoFrameProvider>> video_frame_provider_;
  StrictMock<MockVideoFrameResourceProvider>* resource_provider_;
  scoped_refptr<viz::TestContextProvider> context_provider_;
  std::unique_ptr<VideoFrameSubmitter> submitter_;
};

TEST_F(VideoFrameSubmitterTest, StatRenderingFlipsBits) {
  EXPECT_FALSE(IsRendering());
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();

  scoped_task_environment_.RunUntilIdle();

  EXPECT_TRUE(IsRendering());
}

TEST_F(VideoFrameSubmitterTest, StopRenderingSkipsUpdateCurrentFrame) {
  EXPECT_FALSE(IsRendering());
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();

  scoped_task_environment_.RunUntilIdle();

  EXPECT_TRUE(IsRendering());

  // OnBeginFrame() submits one frame.
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .WillOnce(Return(true));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();

  // StopRendering submits one more frame.
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());
  submitter_->StopRendering();
  scoped_task_environment_.RunUntilIdle();

  // No frames should be produced after StopRendering().
  EXPECT_CALL(*sink_, DidNotProduceFrame(_));
  begin_frame_source_->CreateBeginFrameArgs(BEGINFRAME_FROM_HERE,
                                            now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, StopUsingProviderNullsProvider) {
  EXPECT_FALSE(IsRendering());
  EXPECT_EQ(video_frame_provider_.get(), GetProvider());

  submitter_->StopUsingProvider();

  EXPECT_EQ(nullptr, GetProvider());
}

TEST_F(VideoFrameSubmitterTest,
       StopUsingProviderSubmitsFrameAndStopsRendering) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_TRUE(IsRendering());

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  submitter_->StopUsingProvider();

  scoped_task_environment_.RunUntilIdle();

  EXPECT_FALSE(IsRendering());
}

TEST_F(VideoFrameSubmitterTest, DidReceiveFrameDoesNothingIfRendering) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_TRUE(IsRendering());

  submitter_->DidReceiveFrame();
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, DidReceiveFrameSubmitsFrame) {
  EXPECT_FALSE(IsRendering());

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  submitter_->DidReceiveFrame();
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, ShouldSubmitPreventsSubmission) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  submitter_->SetIsSurfaceVisible(false);
  scoped_task_environment_.RunUntilIdle();

  EXPECT_FALSE(ShouldSubmit());

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->SetIsSurfaceVisible(true);
  scoped_task_environment_.RunUntilIdle();

  EXPECT_TRUE(ShouldSubmit());

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame()).Times(0);
  submitter_->SetIsSurfaceVisible(false);
  scoped_task_environment_.RunUntilIdle();

  EXPECT_FALSE(ShouldSubmit());

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());

  SubmitSingleFrame();
}

// Tests that when set to true SetForceSubmit forces frame submissions.
// regardless of the internal submit state.
TEST_F(VideoFrameSubmitterTest, SetForceSubmitForcesSubmission) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  submitter_->SetIsSurfaceVisible(false);
  scoped_task_environment_.RunUntilIdle();

  EXPECT_FALSE(ShouldSubmit());

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  submitter_->SetForceSubmit(true);
  EXPECT_TRUE(ShouldSubmit());

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _)).Times(1);
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->SetIsSurfaceVisible(true);
  scoped_task_environment_.RunUntilIdle();
  EXPECT_TRUE(ShouldSubmit());

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->SetIsSurfaceVisible(false);
  scoped_task_environment_.RunUntilIdle();
  EXPECT_TRUE(ShouldSubmit());

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());

  SubmitSingleFrame();
}

TEST_F(VideoFrameSubmitterTest, RotationInformationPassedToResourceProvider) {
  // Check to see if rotation is communicated pre-rendering.
  EXPECT_FALSE(IsRendering());

  submitter_->SetRotation(media::VideoRotation::VIDEO_ROTATION_90);

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_,
              AppendQuads(_, _, media::VideoRotation::VIDEO_ROTATION_90, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  submitter_->DidReceiveFrame();
  scoped_task_environment_.RunUntilIdle();

  {
    WTF::Vector<viz::ReturnedResource> resources;
    EXPECT_CALL(*resource_provider_, ReceiveReturnsFromParent(_));
    submitter_->DidReceiveCompositorFrameAck(resources);
  }

  // Check to see if an update to rotation just before rendering is
  // communicated.
  submitter_->SetRotation(media::VideoRotation::VIDEO_ROTATION_180);

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  {
    WTF::Vector<viz::ReturnedResource> resources;
    EXPECT_CALL(*resource_provider_, ReceiveReturnsFromParent(_));
    submitter_->DidReceiveCompositorFrameAck(resources);
  }

  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .WillOnce(Return(true));
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_,
              AppendQuads(_, _, media::VideoRotation::VIDEO_ROTATION_180, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();

  {
    WTF::Vector<viz::ReturnedResource> resources;
    EXPECT_CALL(*resource_provider_, ReceiveReturnsFromParent(_));
    submitter_->DidReceiveCompositorFrameAck(resources);
  }

  // Check to see if changing rotation while rendering is handled.
  submitter_->SetRotation(media::VideoRotation::VIDEO_ROTATION_270);

  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .WillOnce(Return(true));
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_,
              AppendQuads(_, _, media::VideoRotation::VIDEO_ROTATION_270, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, OnBeginFrameSubmitsFrame) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .WillOnce(Return(true));
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, MissedFrameArgDoesNotProduceFrame) {
  EXPECT_CALL(*sink_, DidNotProduceFrame(_));

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  args.type = viz::BeginFrameArgs::MISSED;
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, MissingProviderDoesNotProduceFrame) {
  submitter_->StopUsingProvider();

  EXPECT_CALL(*sink_, DidNotProduceFrame(_));

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, NoUpdateOnFrameDoesNotProduceFrame) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->StartRendering();

  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .WillOnce(Return(false));
  EXPECT_CALL(*sink_, DidNotProduceFrame(_));

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, NotRenderingDoesNotProduceFrame) {
  // We don't care if UpdateCurrentFrame is called or not; it doesn't matter
  // if we're not rendering.
  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .Times(AnyNumber());
  EXPECT_CALL(*sink_, DidNotProduceFrame(_));

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, ReturnsResourceOnCompositorAck) {
  WTF::Vector<viz::ReturnedResource> resources;
  EXPECT_CALL(*resource_provider_, ReceiveReturnsFromParent(_));
  submitter_->DidReceiveCompositorFrameAck(resources);
  scoped_task_environment_.RunUntilIdle();
}

// Tests that after submitting a frame, no frame will be submitted until an ACK
// was received. This is tested by simulating another BeginFrame message.
TEST_F(VideoFrameSubmitterTest, WaitingForAckPreventsNewFrame) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .Times(1)
      .WillOnce(Return(true));

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .Times(1)
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));

  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();

  // DidNotProduceFrame should be called because no frame will be submitted
  // given that the ACK is still pending.
  EXPECT_CALL(*sink_, DidNotProduceFrame(_)).Times(1);

  // UpdateCurrentFrame should still be called, however, so that the compositor
  // knows that we missed a frame.
  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _)).Times(1);

  std::unique_ptr<base::SimpleTestTickClock> new_time =
      std::make_unique<base::SimpleTestTickClock>();
  args = begin_frame_source_->CreateBeginFrameArgs(BEGINFRAME_FROM_HERE,
                                                   new_time.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();
}

// Test that after context is lost, the CompositorFrameSink is recreated but the
// SurfaceEmbedder isn't.
TEST_F(VideoFrameSubmitterTest, RecreateCompositorFrameSinkAfterContextLost) {
  MockEmbeddedFrameSinkProvider mock_embedded_frame_sink_provider;
  mojo::Binding<mojom::blink::EmbeddedFrameSinkProvider>
      embedded_frame_sink_provider_binding(&mock_embedded_frame_sink_provider);
  auto override =
      mock_embedded_frame_sink_provider.CreateScopedOverrideMojoInterface(
          &embedded_frame_sink_provider_binding);

  EXPECT_CALL(*resource_provider_, Initialize(_, _));
  EXPECT_CALL(mock_embedded_frame_sink_provider, ConnectToEmbedder(_, _))
      .Times(0);
  EXPECT_CALL(mock_embedded_frame_sink_provider, CreateCompositorFrameSink_(_))
      .Times(1);
  submitter_->OnContextLost();
  OnReceivedContextProvider(true, context_provider_);
  scoped_task_environment_.RunUntilIdle();
}

// Test that no crash happens if the context is lost during a frame submission.
TEST_F(VideoFrameSubmitterTest, ContextLostDuringSubmit) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());

  // This will post a task that will later call SubmitFrame(). The call will
  // happen after OnContextLost().
  SubmitSingleFrame();

  submitter_->OnContextLost();

  scoped_task_environment_.RunUntilIdle();
}

// This test simulates a race condition in which the |video_frame_provider_| is
// destroyed before OnReceivedContextProvider returns.
TEST_F(VideoFrameSubmitterTest, StopUsingProviderDuringContextLost) {
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());

  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  submitter_->StopUsingProvider();

  scoped_task_environment_.RunUntilIdle();

  // OnReceivedContextProvider returns. We don't run the actual function
  // because it would overwrite our fake |sink_| with a real one.
  SubmitSingleFrame();

  scoped_task_environment_.RunUntilIdle();
}

// Test the behaviour of the ChildLocalSurfaceIdAllocator instance. It checks
// that the LocalSurfaceId is propoerly set at creation and updated when the
// video frames change.
TEST_F(VideoFrameSubmitterTest, FrameSizeChangeUpdatesLocalSurfaceId) {
  {
    viz::LocalSurfaceId local_surface_id =
        child_local_surface_id_allocator()
            .GetCurrentLocalSurfaceIdAllocation()
            .local_surface_id();
    EXPECT_TRUE(local_surface_id.is_valid());
    EXPECT_EQ(11u, local_surface_id.parent_sequence_number());
    EXPECT_EQ(viz::kInitialChildSequenceNumber,
              local_surface_id.child_sequence_number());
    EXPECT_TRUE(frame_size().IsEmpty());
  }

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  SubmitSingleFrame();
  scoped_task_environment_.RunUntilIdle();

  {
    viz::LocalSurfaceId local_surface_id =
        child_local_surface_id_allocator()
            .GetCurrentLocalSurfaceIdAllocation()
            .local_surface_id();
    EXPECT_TRUE(local_surface_id.is_valid());
    EXPECT_EQ(11u, local_surface_id.parent_sequence_number());
    EXPECT_EQ(viz::kInitialChildSequenceNumber,
              local_surface_id.child_sequence_number());
    EXPECT_EQ(gfx::Size(8, 8), frame_size());
  }

  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(2, 2), gfx::Rect(gfx::Size(2, 2)),
          gfx::Size(2, 2), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  SubmitSingleFrame();
  scoped_task_environment_.RunUntilIdle();

  {
    viz::LocalSurfaceId local_surface_id =
        child_local_surface_id_allocator()
            .GetCurrentLocalSurfaceIdAllocation()
            .local_surface_id();
    EXPECT_TRUE(local_surface_id.is_valid());
    EXPECT_EQ(11u, local_surface_id.parent_sequence_number());
    EXPECT_EQ(viz::kInitialChildSequenceNumber + 1,
              local_surface_id.child_sequence_number());
    EXPECT_EQ(gfx::Size(2, 2), frame_size());
  }
}

TEST_F(VideoFrameSubmitterTest, VideoRotationOutputRect) {
  MakeSubmitter();
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->StartRendering();
  EXPECT_TRUE(IsRendering());

  gfx::Size coded_size(1280, 720);
  gfx::Size natural_size(1280, 1024);
  gfx::Size rotated_size(1024, 1280);

  {
    submitter_->SetRotation(media::VideoRotation::VIDEO_ROTATION_90);

    EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
        .WillOnce(Return(true));
    EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
        .WillOnce(Return(media::VideoFrame::CreateFrame(
            media::PIXEL_FORMAT_YV12, coded_size, gfx::Rect(coded_size),
            natural_size, base::TimeDelta())));
    EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
    EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
    EXPECT_CALL(*resource_provider_,
                AppendQuads(_, _, media::VideoRotation::VIDEO_ROTATION_90, _));
    EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
    EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

    viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
        BEGINFRAME_FROM_HERE, now_src_.get());
    submitter_->OnBeginFrame(args, {});
    scoped_task_environment_.RunUntilIdle();

    EXPECT_EQ(sink_->last_submitted_compositor_frame().size_in_pixels(),
              rotated_size);

    submitter_->DidReceiveFrame();

    WTF::Vector<viz::ReturnedResource> resources;
    EXPECT_CALL(*resource_provider_, ReceiveReturnsFromParent(_));
    submitter_->DidReceiveCompositorFrameAck(resources);
  }

  {
    submitter_->SetRotation(media::VideoRotation::VIDEO_ROTATION_180);

    EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
        .WillOnce(Return(true));
    EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
        .WillOnce(Return(media::VideoFrame::CreateFrame(
            media::PIXEL_FORMAT_YV12, coded_size, gfx::Rect(coded_size),
            natural_size, base::TimeDelta())));
    EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
    EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
    EXPECT_CALL(*resource_provider_,
                AppendQuads(_, _, media::VideoRotation::VIDEO_ROTATION_180, _));
    EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
    EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

    viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
        BEGINFRAME_FROM_HERE, now_src_.get());
    submitter_->OnBeginFrame(args, {});
    scoped_task_environment_.RunUntilIdle();

    // 180 deg rotation has same size.
    EXPECT_EQ(sink_->last_submitted_compositor_frame().size_in_pixels(),
              natural_size);

    submitter_->DidReceiveFrame();

    WTF::Vector<viz::ReturnedResource> resources;
    EXPECT_CALL(*resource_provider_, ReceiveReturnsFromParent(_));
    submitter_->DidReceiveCompositorFrameAck(resources);
  }

  {
    submitter_->SetRotation(media::VideoRotation::VIDEO_ROTATION_270);

    EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
        .WillOnce(Return(true));
    EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
        .WillOnce(Return(media::VideoFrame::CreateFrame(
            media::PIXEL_FORMAT_YV12, coded_size, gfx::Rect(coded_size),
            natural_size, base::TimeDelta())));
    EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
    EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
    EXPECT_CALL(*resource_provider_,
                AppendQuads(_, _, media::VideoRotation::VIDEO_ROTATION_270, _));
    EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
    EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

    viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
        BEGINFRAME_FROM_HERE, now_src_.get());
    submitter_->OnBeginFrame(args, {});
    scoped_task_environment_.RunUntilIdle();

    EXPECT_EQ(sink_->last_submitted_compositor_frame().size_in_pixels(),
              rotated_size);

    submitter_->DidReceiveFrame();

    WTF::Vector<viz::ReturnedResource> resources;
    EXPECT_CALL(*resource_provider_, ReceiveReturnsFromParent(_));
    submitter_->DidReceiveCompositorFrameAck(resources);
  }
}

TEST_F(VideoFrameSubmitterTest, PageVisibilityControlsSubmission) {
  // Hide the page and ensure no begin frames are issued.
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  submitter_->SetIsPageVisible(false);
  scoped_task_environment_.RunUntilIdle();
  EXPECT_FALSE(ShouldSubmit());

  // Start rendering, but since page is hidden nothing should start yet.
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  // Mark the page as visible and confirm frame submission. This should not
  // submit since we're already rendering.
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));
  submitter_->SetIsPageVisible(true);
  scoped_task_environment_.RunUntilIdle();

  // Transition back to the page being hidden and ensure begin frames stop.
  EXPECT_TRUE(ShouldSubmit());
  EXPECT_CALL(*sink_, SetNeedsBeginFrame(false));
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame()).Times(0);
  submitter_->SetIsPageVisible(false);
  scoped_task_environment_.RunUntilIdle();
}

TEST_F(VideoFrameSubmitterTest, PreferredInterval) {
  video_frame_provider_->preferred_interval = base::TimeDelta::FromSeconds(1);

  EXPECT_CALL(*sink_, SetNeedsBeginFrame(true));

  submitter_->StartRendering();
  scoped_task_environment_.RunUntilIdle();

  EXPECT_CALL(*video_frame_provider_, UpdateCurrentFrame(_, _))
      .WillOnce(Return(true));
  EXPECT_CALL(*video_frame_provider_, GetCurrentFrame())
      .WillOnce(Return(media::VideoFrame::CreateFrame(
          media::PIXEL_FORMAT_YV12, gfx::Size(8, 8), gfx::Rect(gfx::Size(8, 8)),
          gfx::Size(8, 8), base::TimeDelta())));
  EXPECT_CALL(*sink_, DoSubmitCompositorFrame(_, _));
  EXPECT_CALL(*video_frame_provider_, PutCurrentFrame());
  EXPECT_CALL(*resource_provider_, AppendQuads(_, _, _, _));
  EXPECT_CALL(*resource_provider_, PrepareSendToParent(_, _));
  EXPECT_CALL(*resource_provider_, ReleaseFrameResources());

  viz::BeginFrameArgs args = begin_frame_source_->CreateBeginFrameArgs(
      BEGINFRAME_FROM_HERE, now_src_.get());
  submitter_->OnBeginFrame(args, {});
  scoped_task_environment_.RunUntilIdle();

  EXPECT_EQ(sink_->last_submitted_compositor_frame()
                .metadata.preferred_frame_interval,
            video_frame_provider_->preferred_interval);
}

}  // namespace blink
