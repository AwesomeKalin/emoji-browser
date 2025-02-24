// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.omnibox.suggestions.entity;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.support.annotation.VisibleForTesting;
import android.text.TextUtils;

import org.chromium.base.Log;
import org.chromium.base.ThreadUtils;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.ChromeApplication;
import org.chromium.chrome.browser.image_fetcher.ImageFetcher;
import org.chromium.chrome.browser.image_fetcher.ImageFetcherConfig;
import org.chromium.chrome.browser.image_fetcher.ImageFetcherFactory;
import org.chromium.chrome.browser.omnibox.OmniboxSuggestionType;
import org.chromium.chrome.browser.omnibox.suggestions.OmniboxSuggestion;
import org.chromium.chrome.browser.omnibox.suggestions.OmniboxSuggestionUiType;
import org.chromium.chrome.browser.omnibox.suggestions.SuggestionProcessor;
import org.chromium.chrome.browser.omnibox.suggestions.basic.SuggestionHost;
import org.chromium.chrome.browser.omnibox.suggestions.basic.SuggestionViewDelegate;
import org.chromium.ui.modelutil.PropertyModel;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** A class that handles model and view creation for the Entity suggestions. */
public class EntitySuggestionProcessor implements SuggestionProcessor {
    private final static String TAG = "EntitySP";
    private final Context mContext;
    private final SuggestionHost mSuggestionHost;
    private final Map<String, List<PropertyModel>> mPendingImageRequests;
    private final int mEntityImageSizePx;
    private ImageFetcher mImageFetcher;

    /**
     * @param context An Android context.
     * @param suggestionHost A handle to the object using the suggestions.
     */
    public EntitySuggestionProcessor(Context context, SuggestionHost suggestionHost) {
        mContext = context;
        mSuggestionHost = suggestionHost;
        mPendingImageRequests = new HashMap<>();
        mEntityImageSizePx = context.getResources().getDimensionPixelSize(
                R.dimen.omnibox_suggestion_entity_icon_size);
    }

    @Override
    public boolean doesProcessSuggestion(OmniboxSuggestion suggestion) {
        return suggestion.getType() == OmniboxSuggestionType.SEARCH_SUGGEST_ENTITY;
    }

    @Override
    public int getViewTypeId() {
        return OmniboxSuggestionUiType.ENTITY_SUGGESTION;
    }

    @Override
    public PropertyModel createModelForSuggestion(OmniboxSuggestion suggestion) {
        return new PropertyModel(EntitySuggestionViewProperties.ALL_KEYS);
    }

    @Override
    public void onNativeInitialized() {
        mImageFetcher = ImageFetcherFactory.createImageFetcher(
                ImageFetcherConfig.IN_MEMORY_ONLY, ChromeApplication.getReferencePool());
    }

    @Override
    public void onUrlFocusChange(boolean hasFocus) {
        if (mImageFetcher != null && !hasFocus) mImageFetcher.clear();
    }

    @Override
    public void recordSuggestionPresented(OmniboxSuggestion suggestion, PropertyModel model) {
        // Not used.
    }

    @Override
    public void recordSuggestionUsed(OmniboxSuggestion suggestion, PropertyModel model) {
        // Bookkeeping handled in C++:
        // http://cs.chromium.org/Omnibox.SuggestionUsed.RichEntity
    }

    private void fetchEntityImage(OmniboxSuggestion suggestion, PropertyModel model) {
        ThreadUtils.assertOnUiThread();
        final String url = suggestion.getImageUrl();
        if (TextUtils.isEmpty(url)) return;

        // Do not make duplicate answer image requests for the same URL (to avoid generating
        // duplicate bitmaps for the same image).
        if (mPendingImageRequests.containsKey(url)) {
            mPendingImageRequests.get(url).add(model);
            return;
        }

        List<PropertyModel> models = new ArrayList<>();
        models.add(model);
        mPendingImageRequests.put(url, models);

        mImageFetcher.fetchImage(url, "EntitySuggestionProcessor", mEntityImageSizePx,
                mEntityImageSizePx, (Bitmap bitmap) -> {
                    ThreadUtils.assertOnUiThread();

                    final List<PropertyModel> pendingModels = mPendingImageRequests.remove(url);
                    boolean didUpdate = false;
                    for (int i = 0; i < pendingModels.size(); i++) {
                        PropertyModel pendingModel = pendingModels.get(i);
                        if (!mSuggestionHost.isActiveModel(pendingModel)) continue;

                        pendingModel.set(EntitySuggestionViewProperties.IMAGE_BITMAP, bitmap);
                        didUpdate = true;
                    }
                    if (didUpdate) mSuggestionHost.notifyPropertyModelsChanged();
                });
    }

    @VisibleForTesting
    public void applyImageDominantColor(String colorSpec, PropertyModel model) {
        int color = EntitySuggestionViewBinder.NO_DOMINANT_COLOR;
        if (!TextUtils.isEmpty(colorSpec)) {
            try {
                color = Color.parseColor(colorSpec);
            } catch (IllegalArgumentException e) {
                Log.i(TAG, "Failed to parse dominant color: " + colorSpec);
            }
        }
        model.set(EntitySuggestionViewProperties.IMAGE_DOMINANT_COLOR, color);
    }

    @Override
    public void populateModel(OmniboxSuggestion suggestion, PropertyModel model, int position) {
        // Note: to avoid flickering and unnecessary updates, ModelListAdapter re-uses values from
        // previously bound model if the view is being re-used. This means our model may, and likely
        // will not be empty at this point. Make sure we explicitly specify values to avoid using
        // stale ones.
        model.set(EntitySuggestionViewProperties.IMAGE_BITMAP, null);

        SuggestionViewDelegate delegate =
                mSuggestionHost.createSuggestionViewDelegate(suggestion, position);

        applyImageDominantColor(suggestion.getImageDominantColor(), model);
        fetchEntityImage(suggestion, model);

        model.set(EntitySuggestionViewProperties.SUBJECT_TEXT, suggestion.getDisplayText());
        model.set(EntitySuggestionViewProperties.DESCRIPTION_TEXT, suggestion.getDescription());
        model.set(EntitySuggestionViewProperties.DELEGATE, delegate);
    }
}
