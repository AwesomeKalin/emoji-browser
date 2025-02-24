// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/xr/xr_input_source.h"

#include "base/time/time.h"
#include "third_party/blink/renderer/modules/xr/xr.h"
#include "third_party/blink/renderer/modules/xr/xr_grip_space.h"
#include "third_party/blink/renderer/modules/xr/xr_session.h"
#include "third_party/blink/renderer/modules/xr/xr_space.h"
#include "third_party/blink/renderer/modules/xr/xr_target_ray_space.h"
#include "third_party/blink/renderer/modules/xr/xr_utils.h"

namespace blink {

namespace {
std::unique_ptr<TransformationMatrix> TryGetTransformationMatrix(
    const gfx::mojom::blink::TransformPtr& transform) {
  if (transform && transform->matrix.has_value()) {
    return std::make_unique<TransformationMatrix>(
        WTFFloatVectorToTransformationMatrix(transform->matrix.value()));
  }

  return nullptr;
}

std::unique_ptr<TransformationMatrix> TryGetTransformationMatrix(
    const TransformationMatrix* other) {
  if (other) {
    return std::make_unique<TransformationMatrix>(*other);
  }

  return nullptr;
}
}  // namespace

XRInputSource::InternalState::InternalState(
    uint32_t source_id,
    device::mojom::XRTargetRayMode target_ray_mode,
    TimeTicks base_timestamp)
    : source_id(source_id),
      target_ray_mode(target_ray_mode),
      base_timestamp(base_timestamp) {}

XRInputSource::InternalState::InternalState(const InternalState& other) =
    default;

XRInputSource::InternalState::~InternalState() = default;

XRInputSource* XRInputSource::CreateOrUpdateFrom(
    XRInputSource* other,
    XRSession* session,
    const device::mojom::blink::XRInputSourceStatePtr& state) {
  if (!state)
    return other;

  XRInputSource* updated_source = other;

  // Check if we have an existing object, and if we do, if it can be re-used.
  if (!other) {
    auto source_id = state->source_id;
    updated_source = MakeGarbageCollected<XRInputSource>(session, source_id);
  } else if (other->InvalidatesSameObject(state)) {
    // Something in the state has changed which requires us to re-create the
    // object.  Create a copy now, and we will blindly update any state later,
    // knowing that we now have a new object if needed.
    updated_source = MakeGarbageCollected<XRInputSource>(*other);
  }

  updated_source->UpdateGamepad(state->gamepad);

  // Update the input source's description if this state update includes them.
  if (state->description) {
    const device::mojom::blink::XRInputSourceDescriptionPtr& desc =
        state->description;

    updated_source->state_.target_ray_mode = desc->target_ray_mode;
    updated_source->state_.handedness = desc->handedness;
    updated_source->state_.emulated_position = desc->emulated_position;

    updated_source->pointer_transform_matrix_ =
        TryGetTransformationMatrix(desc->pointer_offset);
  }

  updated_source->base_pose_matrix_ = TryGetTransformationMatrix(state->grip);

  return updated_source;
}

XRInputSource::XRInputSource(XRSession* session,
                             uint32_t source_id,
                             device::mojom::XRTargetRayMode target_ray_mode)
    : state_(source_id, target_ray_mode, session->xr()->NavigationStart()),
      session_(session),
      target_ray_space_(MakeGarbageCollected<XRTargetRaySpace>(session, this)),
      grip_space_(MakeGarbageCollected<XRGripSpace>(session, this)) {}

// Must make new target_ray_space_ and grip_space_ to ensure that they point to
// the correct XRInputSource object. Otherwise, the controller position gets
// stuck when an XRInputSource gets re-created. Also need to make a deep copy of
// the matrices since they use unique_ptrs.
XRInputSource::XRInputSource(const XRInputSource& other)
    : state_(other.state_),
      session_(other.session_),
      target_ray_space_(
          MakeGarbageCollected<XRTargetRaySpace>(other.session_, this)),
      grip_space_(MakeGarbageCollected<XRGripSpace>(other.session_, this)),
      gamepad_(other.gamepad_),
      base_pose_matrix_(
          TryGetTransformationMatrix(other.base_pose_matrix_.get())),
      pointer_transform_matrix_(
          TryGetTransformationMatrix(other.pointer_transform_matrix_.get())) {}

const String XRInputSource::handedness() const {
  switch (state_.handedness) {
    case device::mojom::XRHandedness::NONE:
      return "none";
    case device::mojom::XRHandedness::LEFT:
      return "left";
    case device::mojom::XRHandedness::RIGHT:
      return "right";
  }

  NOTREACHED() << "Unknown handedness: " << state_.handedness;
}

const String XRInputSource::targetRayMode() const {
  switch (state_.target_ray_mode) {
    case device::mojom::XRTargetRayMode::GAZING:
      return "gaze";
    case device::mojom::XRTargetRayMode::POINTING:
      return "tracked-pointer";
    case device::mojom::XRTargetRayMode::TAPPING:
      return "screen";
  }

  NOTREACHED() << "Unknown target ray mode: " << state_.target_ray_mode;
}

XRSpace* XRInputSource::targetRaySpace() const {
  return target_ray_space_;
}

XRSpace* XRInputSource::gripSpace() const {
  if (state_.target_ray_mode == device::mojom::XRTargetRayMode::POINTING) {
    return grip_space_;
  }

  return nullptr;
}

bool XRInputSource::InvalidatesSameObject(
    const device::mojom::blink::XRInputSourceStatePtr& state) {
  if ((state->gamepad && !gamepad_) || (!state->gamepad && gamepad_)) {
    return true;
  }

  if (state->description) {
    if (state->description->handedness != state_.handedness) {
      return true;
    }

    if (state->description->target_ray_mode != state_.target_ray_mode) {
      return true;
    }
  }

  return false;
}

void XRInputSource::SetPointerTransformMatrix(
    const TransformationMatrix* pointer_transform_matrix) {
  pointer_transform_matrix_ =
      TryGetTransformationMatrix(pointer_transform_matrix);
}

void XRInputSource::SetGamepadConnected(bool state) {
  if (gamepad_)
    gamepad_->SetConnected(state);
}

void XRInputSource::UpdateGamepad(
    const base::Optional<device::Gamepad>& gamepad) {
  if (gamepad) {
    if (!gamepad_) {
      // TODO(https://crbug.com/955104): Is the Gamepad object creation time the
      // correct time floor?
      gamepad_ = MakeGarbageCollected<Gamepad>(this, 0, state_.base_timestamp,
                                               TimeTicks::Now());
    }

    gamepad_->UpdateFromDeviceState(*gamepad);
  } else {
    gamepad_ = nullptr;
  }
}

void XRInputSource::Trace(blink::Visitor* visitor) {
  visitor->Trace(session_);
  visitor->Trace(target_ray_space_);
  visitor->Trace(grip_space_);
  visitor->Trace(gamepad_);
  ScriptWrappable::Trace(visitor);
}

}  // namespace blink
