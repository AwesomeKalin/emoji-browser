// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/test/hit_test_region_observer.h"

#include <algorithm>

#include "base/test/test_timeouts.h"
#include "components/viz/host/hit_test/hit_test_query.h"
#include "components/viz/host/host_frame_sink_manager.h"
#include "content/browser/compositor/surface_utils.h"
#include "content/browser/frame_host/cross_process_frame_connector.h"
#include "content/browser/frame_host/render_frame_host_impl.h"
#include "content/browser/renderer_host/frame_connector_delegate.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_view_base.h"
#include "content/browser/renderer_host/render_widget_host_view_child_frame.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test_utils.h"

namespace content {

namespace {

// Returns a transform from root to |frame_sink_id|. If no HitTestQuery contains
// |frame_sink_id| then this will return an empty optional.
base::Optional<gfx::Transform> GetRootToTargetTransform(
    const viz::FrameSinkId& frame_sink_id) {
  for (auto& it : GetHostFrameSinkManager()->display_hit_test_query()) {
    if (it.second->ContainsActiveFrameSinkId(frame_sink_id)) {
      base::Optional<gfx::Transform> transform(base::in_place);
      it.second->GetTransformToTarget(frame_sink_id, &transform.value());
      return transform;
    }
  }

  return base::nullopt;
}

}  // namespace

void WaitForHitTestDataOrChildSurfaceReady(RenderFrameHost* child_frame) {
  RenderWidgetHostViewBase* child_view =
      static_cast<RenderFrameHostImpl*>(child_frame)
          ->GetRenderWidgetHost()
          ->GetView();

  HitTestRegionObserver observer(child_view->GetFrameSinkId());
  observer.WaitForHitTestData();
}

void WaitForHitTestDataOrGuestSurfaceReady(WebContents* guest_web_contents) {
  DCHECK(static_cast<RenderWidgetHostViewBase*>(
             guest_web_contents->GetRenderWidgetHostView())
             ->IsRenderWidgetHostViewChildFrame());
  RenderWidgetHostViewChildFrame* child_view =
      static_cast<RenderWidgetHostViewChildFrame*>(
          guest_web_contents->GetRenderWidgetHostView());

  HitTestRegionObserver observer(child_view->GetFrameSinkId());
  observer.WaitForHitTestData();
}

HitTestRegionObserver::HitTestRegionObserver(
    const viz::FrameSinkId& frame_sink_id)
    : frame_sink_id_(frame_sink_id) {
  CHECK(frame_sink_id.is_valid());
  GetHostFrameSinkManager()->AddHitTestRegionObserver(this);
}

HitTestRegionObserver::~HitTestRegionObserver() {
  GetHostFrameSinkManager()->RemoveHitTestRegionObserver(this);
}

void HitTestRegionObserver::WaitForHitTestData() {
  for (auto& it : GetHostFrameSinkManager()->display_hit_test_query()) {
    if (it.second->ContainsActiveFrameSinkId(frame_sink_id_)) {
      return;
    }
  }

  run_loop_ = std::make_unique<base::RunLoop>();
  run_loop_->Run();
  run_loop_.reset();
}

void HitTestRegionObserver::OnAggregatedHitTestRegionListUpdated(
    const viz::FrameSinkId& frame_sink_id,
    const std::vector<viz::AggregatedHitTestRegion>& hit_test_data) {

  if (!run_loop_)
    return;

  for (auto& it : hit_test_data) {
    if (it.frame_sink_id == frame_sink_id_ &&
        !(it.flags & viz::HitTestRegionFlags::kHitTestNotActive)) {
      run_loop_->Quit();
      return;
    }
  }
}

const std::vector<viz::AggregatedHitTestRegion>&
HitTestRegionObserver::GetHitTestData() {
  const auto& hit_test_query_map =
      GetHostFrameSinkManager()->display_hit_test_query();
  const auto iter = hit_test_query_map.find(frame_sink_id_);
  DCHECK(iter != hit_test_query_map.end());
  return iter->second.get()->hit_test_data_;
}

HitTestTransformChangeObserver::HitTestTransformChangeObserver(
    const viz::FrameSinkId& frame_sink_id)
    : target_frame_sink_id_(frame_sink_id),
      cached_transform_(GetRootToTargetTransform(frame_sink_id)) {
  DCHECK(frame_sink_id.is_valid());
}

HitTestTransformChangeObserver::~HitTestTransformChangeObserver() = default;

void HitTestTransformChangeObserver::WaitForHitTestDataChange() {
  DCHECK(!run_loop_);

  // If the transform has already changed then don't run RunLoop.
  base::Optional<gfx::Transform> transform =
      GetRootToTargetTransform(target_frame_sink_id_);
  if (transform != cached_transform_) {
    cached_transform_ = transform;
    return;
  }

  GetHostFrameSinkManager()->AddHitTestRegionObserver(this);
  run_loop_ = std::make_unique<base::RunLoop>();
  run_loop_->Run();
  run_loop_.reset();
  GetHostFrameSinkManager()->RemoveHitTestRegionObserver(this);
}

void HitTestTransformChangeObserver::OnAggregatedHitTestRegionListUpdated(
    const viz::FrameSinkId& frame_sink_id,
    const std::vector<viz::AggregatedHitTestRegion>& hit_test_data) {
  // Check if the transform has changed since it was cached.
  base::Optional<gfx::Transform> transform =
      GetRootToTargetTransform(target_frame_sink_id_);
  if (transform != cached_transform_) {
    cached_transform_ = transform;
    run_loop_->Quit();
  }
}

}  // namespace content
