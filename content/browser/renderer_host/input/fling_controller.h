// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_RENDERER_HOST_INPUT_FLING_CONTROLLER_H_
#define CONTENT_BROWSER_RENDERER_HOST_INPUT_FLING_CONTROLLER_H_

#include "content/browser/renderer_host/input/touchpad_tap_suppression_controller.h"
#include "content/browser/renderer_host/input/touchscreen_tap_suppression_controller.h"
#include "content/public/common/input_event_ack_state.h"
#include "ui/events/blink/fling_booster.h"

namespace blink {
class WebGestureCurve;
}

namespace content {

class FlingController;

// Interface with which the FlingController can forward generated fling progress
// events.
class CONTENT_EXPORT FlingControllerEventSenderClient {
 public:
  virtual ~FlingControllerEventSenderClient() {}

  virtual void SendGeneratedWheelEvent(
      const MouseWheelEventWithLatencyInfo& wheel_event) = 0;

  virtual void SendGeneratedGestureScrollEvents(
      const GestureEventWithLatencyInfo& gesture_event) = 0;
};

// Interface with which the fling progress gets scheduled.
class CONTENT_EXPORT FlingControllerSchedulerClient {
 public:
  virtual ~FlingControllerSchedulerClient() {}

  virtual void ScheduleFlingProgress(
      base::WeakPtr<FlingController> fling_controller) = 0;

  virtual void DidStopFlingingOnBrowser(
      base::WeakPtr<FlingController> fling_controller) = 0;

  virtual bool NeedsBeginFrameForFlingProgress() = 0;
};

class CONTENT_EXPORT FlingController {
 public:
  struct CONTENT_EXPORT Config {
    Config();

    // Controls touchpad-related tap suppression, disabled by default.
    TapSuppressionController::Config touchpad_tap_suppression_config;

    // Controls touchscreen-related tap suppression, disabled by default.
    TapSuppressionController::Config touchscreen_tap_suppression_config;
  };

  struct ActiveFlingParameters {
    gfx::Vector2dF velocity;
    gfx::PointF point;
    gfx::PointF global_point;
    int modifiers;
    blink::WebGestureDevice source_device;
    base::TimeTicks start_time;

    ActiveFlingParameters() : modifiers(0) {}
  };

  FlingController(FlingControllerEventSenderClient* event_sender_client,
                  FlingControllerSchedulerClient* scheduler_client,
                  const Config& config);

  ~FlingController();

  // Used to progress an active fling on every begin frame.
  void ProgressFling(base::TimeTicks current_time);

  // Used to halt an active fling progress whenever needed.
  void StopFling();

  // The fling controller needs to observe all gesture events. It may consume
  // or filter some events.  It will return true if the event was consumed or
  // filtered and should not be propagated further.
  bool ObserveAndMaybeConsumeGestureEvent(
      const GestureEventWithLatencyInfo& gesture_event);

  void ProcessGestureFlingStart(
      const GestureEventWithLatencyInfo& gesture_event);

  void ProcessGestureFlingCancel(
      const GestureEventWithLatencyInfo& gesture_event);

  bool fling_in_progress() const { return fling_curve_.get(); }

  gfx::Vector2dF CurrentFlingVelocity() const;

  // Returns the |TouchpadTapSuppressionController| instance.
  TouchpadTapSuppressionController* GetTouchpadTapSuppressionController();

  void set_clock_for_testing(const base::TickClock* clock) { clock_ = clock; }

 protected:
  ui::FlingBooster fling_booster_;

 private:
  // Sub-filter for suppressing taps immediately after a GestureFlingCancel.
  bool ObserveAndFilterForTapSuppression(
      const GestureEventWithLatencyInfo& gesture_event);

  void ScheduleFlingProgress();

  // Used to generate synthetic wheel events from touchpad fling and send them.
  void GenerateAndSendWheelEvents(const gfx::Vector2dF& delta,
                                  blink::WebMouseWheelEvent::Phase phase);

  // Used to generate synthetic gesture scroll events from touchscreen fling and
  // send them.
  void GenerateAndSendGestureScrollEvents(
      blink::WebInputEvent::Type type,
      const gfx::Vector2dF& delta = gfx::Vector2dF());

  // Calls one of the GenerateAndSendWheelEvents or
  // GenerateAndSendGestureScrollEvents functions depending on the source
  // device of the current_fling_parameters_. We send GSU and wheel events
  // to progress flings with touchscreen and touchpad source respectively.
  // The reason for this difference is that during the touchpad fling we still
  // send wheel events to JS and generating GSU events directly is not enough.
  void GenerateAndSendFlingProgressEvents(const gfx::Vector2dF& delta);

  void GenerateAndSendFlingEndEvents();

  void EndCurrentFling();

  // Used to update the fling_curve_ state based on the parameters of the fling
  // start event. Returns true if the fling curve was updated for a valid
  // fling. Returns false if the parameters should not cause a fling and the
  // fling_curve_ is not updated.
  bool UpdateCurrentFlingState(const blink::WebGestureEvent& fling_start_event);

  bool first_fling_update_sent() const {
    return !last_progress_time_.is_null();
  }

  FlingControllerEventSenderClient* event_sender_client_;

  FlingControllerSchedulerClient* scheduler_client_;

  // An object tracking the state of touchpad on the delivery of mouse events to
  // the renderer to filter mouse immediately after a touchpad fling canceling
  // tap.
  TouchpadTapSuppressionController touchpad_tap_suppression_controller_;

  // An object tracking the state of touchscreen on the delivery of gesture tap
  // events to the renderer to filter taps immediately after a touchscreen fling
  // canceling tap.
  TouchscreenTapSuppressionController touchscreen_tap_suppression_controller_;

  // Gesture curve of the current active fling. nullptr while a fling is not
  // active.
  std::unique_ptr<blink::WebGestureCurve> fling_curve_;

  ActiveFlingParameters current_fling_parameters_;

  // The last time fling progress events were sent.
  base::TimeTicks last_progress_time_;

  // The clock used; overridable for tests.
  const base::TickClock* clock_;

  // Time of the last seen scroll update that wasn't filtered. Used to know the
  // starting time for a possible fling gesture curve.
  base::TimeTicks last_seen_scroll_update_;

  base::WeakPtrFactory<FlingController> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(FlingController);
};

}  // namespace content

#endif  // CONTENT_BROWSER_RENDERER_HOST_INPUT_FLING_CONTROLLER_H_
