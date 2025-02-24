// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill_assistant;

import static android.support.test.espresso.Espresso.onView;
import static android.support.test.espresso.action.ViewActions.swipeRight;
import static android.support.test.espresso.assertion.PositionAssertions.isRightOf;
import static android.support.test.espresso.assertion.ViewAssertions.matches;
import static android.support.test.espresso.matcher.ViewMatchers.hasDescendant;
import static android.support.test.espresso.matcher.ViewMatchers.isDisplayed;
import static android.support.test.espresso.matcher.ViewMatchers.withText;

import static org.hamcrest.CoreMatchers.is;
import static org.hamcrest.MatcherAssert.assertThat;
import static org.hamcrest.Matchers.allOf;

import android.support.design.widget.CoordinatorLayout;
import android.support.test.InstrumentationRegistry;
import android.support.test.filters.MediumTest;
import android.support.v7.widget.DefaultItemAnimator;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.LinearLayout;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.ThreadUtils;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.browser.autofill_assistant.carousel.AssistantActionsCarouselCoordinator;
import org.chromium.chrome.browser.autofill_assistant.carousel.AssistantCarouselModel;
import org.chromium.chrome.browser.autofill_assistant.carousel.AssistantChip;
import org.chromium.chrome.browser.customtabs.CustomTabActivity;
import org.chromium.chrome.browser.customtabs.CustomTabActivityTestRule;
import org.chromium.chrome.browser.customtabs.CustomTabsTestUtils;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.content_public.browser.test.util.TestThreadUtils;

import java.util.concurrent.ExecutionException;

/**
 * Tests for the autofill assistant actions carousel.
 */
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE})
public class AutofillAssistantActionsCarouselUiTest {
    @Rule
    public CustomTabActivityTestRule mCustomTabActivityTestRule = new CustomTabActivityTestRule();

    @Before
    public void setUp() throws Exception {
        mCustomTabActivityTestRule.startCustomTabActivityWithIntent(
                CustomTabsTestUtils.createMinimalCustomTabIntent(
                        InstrumentationRegistry.getTargetContext(), "about:blank"));
    }

    private CustomTabActivity getActivity() {
        return mCustomTabActivityTestRule.getActivity();
    }

    /** Creates a coordinator for use in UI tests, and adds it to the global view hierarchy. */
    private AssistantActionsCarouselCoordinator createCoordinator(AssistantCarouselModel model) {
        ThreadUtils.assertOnUiThread();
        AssistantActionsCarouselCoordinator coordinator = new AssistantActionsCarouselCoordinator(
                InstrumentationRegistry.getTargetContext(), model);

        // Note: apparently, we need an intermediate container for this coordinator's view,
        // otherwise the view will be invisible.
        // @TODO(crbug.com/806868) figure out why this is the case.
        LinearLayout container = new LinearLayout(InstrumentationRegistry.getTargetContext());
        container.addView(coordinator.getView());

        CoordinatorLayout.LayoutParams lp = new CoordinatorLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.gravity = Gravity.BOTTOM;

        ViewGroup chromeCoordinatorView =
                getActivity().findViewById(org.chromium.chrome.autofill_assistant.R.id.coordinator);
        chromeCoordinatorView.addView(container, lp);

        return coordinator;
    }

    /** Tests assumptions about the initial state of the carousel. */
    @Test
    @MediumTest
    public void testInitialState() {
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            AssistantCarouselModel model = new AssistantCarouselModel();
            AssistantActionsCarouselCoordinator coordinator = createCoordinator(model);

            assertThat(((DefaultItemAnimator) coordinator.getView().getItemAnimator())
                               .getSupportsChangeAnimations(),
                    is(false));
            assertThat(model.getChipsModel().size(), is(0));
            assertThat(coordinator.getView().getAdapter().getItemCount(), is(0));
        });
    }

    /** Adds a single chip and tests assumptions about the view state after the change. */
    @Test
    @MediumTest
    public void testAddSingleChip() throws ExecutionException {
        AssistantCarouselModel model = new AssistantCarouselModel();
        AssistantActionsCarouselCoordinator coordinator =
                TestThreadUtils.runOnUiThreadBlocking(() -> createCoordinator(model));

        TestThreadUtils.runOnUiThreadBlocking(
                ()
                        -> model.getChipsModel().add(
                                new AssistantChip(AssistantChip.Type.BUTTON_HAIRLINE,
                                        AssistantChip.Icon.NONE, "Test", false, true, null)));

        // Chip was created and is displayed on the screen.
        onView(is(coordinator.getView()))
                .check(matches(hasDescendant(allOf(withText("Test"), isDisplayed()))));

        // TODO(crbug.com/806868): test that single chip is center aligned.
    }

    /** Adds multiple chips and tests assumptions about the view state after the change. */
    @Test
    @MediumTest
    public void testAddMultipleChips() throws ExecutionException {
        AssistantCarouselModel model = new AssistantCarouselModel();
        AssistantActionsCarouselCoordinator coordinator =
                TestThreadUtils.runOnUiThreadBlocking(() -> createCoordinator(model));

        // Note: this should be a small number that fits on screen without scrolling.
        int numChips = 3;
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            for (int i = 0; i < numChips; i++) {
                model.getChipsModel().add(new AssistantChip(AssistantChip.Type.BUTTON_HAIRLINE,
                        AssistantChip.Icon.NONE, "T" + i, false, false, null));
            }
            model.getChipsModel().add(new AssistantChip(AssistantChip.Type.BUTTON_HAIRLINE,
                    AssistantChip.Icon.NONE, "X", false, true, null));
        });

        // Cancel chip is displayed to the user.
        onView(withText("X")).check(matches(isDisplayed()));

        // All chips are to the right of the cancel chip.
        for (int i = 0; i < numChips; i++) {
            onView(withText("T" + i)).check(isRightOf(withText("X")));
        }
    }

    /** Adds many chips and tests that the cancel chip is always visible. */
    @Test
    @MediumTest
    public void testCancelChipAlwaysVisible() throws ExecutionException {
        AssistantCarouselModel model = new AssistantCarouselModel();
        AssistantActionsCarouselCoordinator coordinator =
                TestThreadUtils.runOnUiThreadBlocking(() -> createCoordinator(model));

        // Note: this should be a large number that does not fit on screen without scrolling.
        int numChips = 30;
        TestThreadUtils.runOnUiThreadBlocking(() -> {
            for (int i = 0; i < numChips; i++) {
                model.getChipsModel().add(new AssistantChip(AssistantChip.Type.BUTTON_HAIRLINE,
                        AssistantChip.Icon.NONE, "Test" + i, false, false, null));
            }
            model.getChipsModel().add(new AssistantChip(AssistantChip.Type.BUTTON_HAIRLINE,
                    AssistantChip.Icon.NONE, "Cancel", false, true, null));
        });

        // Cancel chip is initially displayed to the user.
        onView(withText("Cancel")).check(matches(isDisplayed()));

        // Scroll right, check that cancel is still visible.
        onView(is(coordinator.getView())).perform(swipeRight());
        onView(withText("Cancel")).check(matches(isDisplayed()));
    }
}
