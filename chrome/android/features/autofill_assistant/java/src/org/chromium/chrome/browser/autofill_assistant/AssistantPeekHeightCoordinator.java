// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill_assistant;

import android.content.Context;
import android.support.annotation.IntDef;
import android.view.View;

import org.chromium.base.Callback;
import org.chromium.chrome.autofill_assistant.R;
import org.chromium.chrome.browser.widget.bottomsheet.BottomSheet;
import org.chromium.chrome.browser.widget.bottomsheet.EmptyBottomSheetObserver;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * Coordinator responsible for setting the peek mode and computing the peek height of the AA bottom
 * sheet content.
 */
class AssistantPeekHeightCoordinator {
    interface Delegate {
        /** Set whether only actions and suggestions should be shown below the progress bar. */
        void setShowOnlyCarousels(boolean showOnlyCarousels);

        /** Called when the peek height changed. */
        void onPeekHeightChanged();
    }

    /**
     * The peek mode allows to set what components are visible when the sheet is in the peek
     * (minimized) state. This is the java version of the ConfigureViewport::PeekMode enum in
     * //components/autofill_assistant/browser/service.proto. DO NOT change this without adapting
     * that proto enum.
     */
    @IntDef({PeekMode.UNDEFINED, PeekMode.HANDLE, PeekMode.HANDLE_HEADER,
            PeekMode.HANDLE_HEADER_CAROUSELS})
    @Retention(RetentionPolicy.SOURCE)
    @interface PeekMode {
        int UNDEFINED = 0;

        /** Only show the swipe handle. */
        int HANDLE = 1;

        /**
         * Show the swipe handle, header (status message, poodle, profile icon) and progress bar.
         */
        int HANDLE_HEADER = 2;

        /** Show swipe handle, header, progress bar, suggestions and actions. */
        int HANDLE_HEADER_CAROUSELS = 3;
    }

    private final View mToolbarView;
    private final Delegate mDelegate;
    private final BottomSheet mBottomSheet;

    private final int mToolbarHeightWithoutPaddingBottom;
    private final int mDefaultToolbarPaddingBottom;
    private final int mProgressBarHeight;
    private final int mChildrenVerticalSpacing;
    private final int mSuggestionsVerticalInset;

    private int mPeekHeight;
    private @PeekMode int mPeekMode = PeekMode.UNDEFINED;
    private int mHeaderHeight;
    private int mSuggestionsHeight;
    private int mActionsHeight;
    private int mToolbarPaddingBottom;

    AssistantPeekHeightCoordinator(Context context, Delegate delegate, BottomSheet bottomSheet,
            View toolbarView, View contentView, View suggestionsView, View actionsView,
            @PeekMode int initialMode) {
        mToolbarView = toolbarView;
        mDelegate = delegate;
        mBottomSheet = bottomSheet;

        mToolbarHeightWithoutPaddingBottom =
                context.getResources().getDimensionPixelSize(
                        R.dimen.autofill_assistant_toolbar_vertical_padding)
                + context.getResources().getDimensionPixelSize(
                        R.dimen.autofill_assistant_toolbar_swipe_handle_height);
        mDefaultToolbarPaddingBottom = context.getResources().getDimensionPixelSize(
                R.dimen.autofill_assistant_toolbar_vertical_padding);
        mProgressBarHeight = context.getResources().getDimensionPixelSize(
                R.dimen.autofill_assistant_progress_bar_height);
        mChildrenVerticalSpacing = context.getResources().getDimensionPixelSize(
                R.dimen.autofill_assistant_bottombar_vertical_spacing);
        mSuggestionsVerticalInset =
                context.getResources().getDimensionPixelSize(R.dimen.chip_bg_vertical_inset);

        // Show only actions if we are in the peek state and peek mode is HANDLE_HEADER_CAROUSELS.
        bottomSheet.addObserver(new EmptyBottomSheetObserver() {
            @Override
            public void onSheetStateChanged(int newState) {
                maybeShowOnlyCarousels();
            }
        });

        // Listen for height changes in the header and carousel to make sure we always have the
        // correct peek height.
        View header = contentView.findViewById(R.id.header);
        mHeaderHeight = header.getHeight();
        mSuggestionsHeight = suggestionsView.getHeight();
        mActionsHeight = actionsView.getHeight();
        listenForHeightChange(header, this::onHeaderHeightChanged);
        listenForHeightChange(suggestionsView, this::onSuggestionsHeightChanged);
        listenForHeightChange(actionsView, this::onActionsHeightChanged);

        setPeekMode(initialMode);
    }

    private void onHeaderHeightChanged(int height) {
        mHeaderHeight = height;
        updateToolbarPadding();
    }

    private void onActionsHeightChanged(int height) {
        mActionsHeight = height;
        updateToolbarPadding();
    }

    private void onSuggestionsHeightChanged(int height) {
        mSuggestionsHeight = height;
        updateToolbarPadding();
    }

    private void maybeShowOnlyCarousels() {
        mDelegate.setShowOnlyCarousels(mBottomSheet.getSheetState() == BottomSheet.SheetState.PEEK
                && mPeekMode == PeekMode.HANDLE_HEADER_CAROUSELS);
    }

    /** Call {@code callback} with new height of {@code view} when it changes. */
    private void listenForHeightChange(View view, Callback<Integer> callback) {
        view.addOnLayoutChangeListener(
                (v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
                    int newHeight = bottom - top;
                    if (newHeight != oldBottom - oldTop) {
                        callback.onResult(newHeight);
                    }
                });
    }

    /**
     * Set the peek mode. If the peek height changed because of this call,
     * Delegate#onPeekHeightChanged() will be called.
     */
    void setPeekMode(@PeekMode int peekMode) {
        if (peekMode == PeekMode.UNDEFINED)
            throw new IllegalArgumentException("Setting UNDEFINED peek mode is not allowed.");
        if (peekMode == mPeekMode) return;

        mPeekMode = peekMode;
        updateToolbarPadding();
        maybeShowOnlyCarousels();
    }

    /** Return the current peek height. */
    int getPeekHeight() {
        return mPeekHeight;
    }

    /** Return the current peek mode. */
    int getPeekMode() {
        return mPeekMode;
    }

    /**
     * Adapt the padding top of the toolbar such that header and carousel are visible if desired.
     */
    private void updateToolbarPadding() {
        switch (mPeekMode) {
            case PeekMode.HANDLE:
                mToolbarPaddingBottom = mDefaultToolbarPaddingBottom;
                break;
            case PeekMode.HANDLE_HEADER:
                mToolbarPaddingBottom = mHeaderHeight + mProgressBarHeight;
                break;
            case PeekMode.HANDLE_HEADER_CAROUSELS:
                mToolbarPaddingBottom = mHeaderHeight + mProgressBarHeight;
                if (mSuggestionsHeight > 0) {
                    mToolbarPaddingBottom += mSuggestionsHeight + mChildrenVerticalSpacing
                            - 2 * mSuggestionsVerticalInset;
                }

                if (mActionsHeight > 0) {
                    mToolbarPaddingBottom += mActionsHeight;
                }

                // We decrease the artificial padding we add to the toolbar by 1 pixel to make sure
                // that toolbarHeight < contentHeight. This way, when the user swipes the sheet from
                // bottom to top, the sheet will enter the SCROLL state and we will show the details
                // and PR, which will allow the user to swipe the whole sheet up with all content
                // shown. An alternative would be to allow toolbarHeight == contentHeight and try to
                // detect swipe/touch events on the sheet, but this alternative is more complex and
                // feels less safe than the current workaround.
                mToolbarPaddingBottom -= 1;
                break;
            default:
                throw new IllegalStateException("Unsupported PeekMode: " + mPeekMode);
        }

        mToolbarView.setPadding(mToolbarView.getPaddingLeft(), mToolbarView.getPaddingTop(),
                mToolbarView.getPaddingRight(), mToolbarPaddingBottom);

        int newHeight = mToolbarHeightWithoutPaddingBottom + mToolbarPaddingBottom;
        if (mPeekHeight != newHeight) {
            mPeekHeight = newHeight;
            mDelegate.onPeekHeightChanged();
        }
    }
}
