// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tab;

import android.graphics.Bitmap;
import android.view.ContextMenu;

import org.chromium.chrome.browser.TabLoadStatus;
import org.chromium.chrome.browser.findinpage.FindMatchRectsDetails;
import org.chromium.chrome.browser.findinpage.FindNotificationDetails;
import org.chromium.chrome.browser.fullscreen.FullscreenOptions;
import org.chromium.chrome.browser.tab.Tab.TabHidingType;
import org.chromium.chrome.browser.tabmodel.TabSelectionType;
import org.chromium.content_public.browser.LoadUrlParams;
import org.chromium.content_public.browser.NavigationHandle;
import org.chromium.content_public.browser.WebContents;
import org.chromium.content_public.common.BrowserControlsState;

/**
 * An observer that is notified of changes to a {@link Tab} object.
 */
public interface TabObserver {
    /**
     * Called when a {@link Tab} finished initialization. The {@link TabState} contains,
     * if not {@code null}, various states that a Tab should restore itself from.
     * @param tab The notifying {@link Tab}.
     * @param tabState {@link TabState} to restore tab's state from if not {@code null}.
     */
    void onInitialized(Tab tab, TabState tabState);

    /**
     * Called when a {@link Tab} is shown.
     * @param tab The notifying {@link Tab}.
     * @param type Specifies how the tab was selected.
     */
    void onShown(Tab tab, @TabSelectionType int type);

    /**
     * Called when a {@link Tab} is hidden.
     * @param tab The notifying {@link Tab}.
     * @param type Specifies how the tab was hidden.
     */
    void onHidden(Tab tab, @TabHidingType int type);

    /**
     * Called when a {@link Tab}'s closing state has changed.
     *
     * @param tab The notifying {@link Tab}.
     * @param closing Whether the {@link Tab} is currently marked for closure.
     */
    void onClosingStateChanged(Tab tab, boolean closing);

    /**
     * Called when a {@link Tab} is being destroyed.
     * @param tab The notifying {@link Tab}.
     */
    void onDestroyed(Tab tab);

    /**
     * Called when the tab content changes (to/from native pages or swapping native WebContents).
     * @param tab The notifying {@link Tab}.
     */
    void onContentChanged(Tab tab);

    /**
     * Called when loadUrl is triggered on a a {@link Tab}.
     * @param tab      The notifying {@link Tab}.
     * @param params   The params describe the page being loaded.
     * @param loadType The type of load that was performed.
     *
     * @see TabLoadStatus#PAGE_LOAD_FAILED
     * @see TabLoadStatus#DEFAULT_PAGE_LOAD
     * @see TabLoadStatus#PARTIAL_PRERENDERED_PAGE_LOAD
     * @see TabLoadStatus#FULL_PRERENDERED_PAGE_LOAD
     */
    void onLoadUrl(Tab tab, LoadUrlParams params, int loadType);

    /**
     * Called when a tab has started to load a page.
     * <p>
     * This will occur when the main frame starts the navigation, and will also occur in instances
     * where we need to simulate load progress (i.e. swapping in a not fully loaded pre-rendered
     * page).
     * <p>
     * For visual loading indicators/throbbers, {@link #onLoadStarted(Tab)} and
     * {@link #onLoadStopped(Tab)} should be used to drive updates.
     *
     * @param tab The notifying {@link Tab}.
     * @param url The committed URL being navigated to.
     */
    void onPageLoadStarted(Tab tab, String url);

    /**
     * Called when a tab has finished loading a page.
     *
     * @param tab The notifying {@link Tab}.
     * @param url The committed URL that was navigated to.
     */
    void onPageLoadFinished(Tab tab, String url);

    /**
     * Called when a tab has failed loading a page.
     *
     * @param tab The notifying {@link Tab}.
     * @param errorCode The error code that causes the page to fail loading.
     */
    void onPageLoadFailed(Tab tab, int errorCode);

    /**
     * Called when the favicon of a {@link Tab} has been updated.
     * @param tab The notifying {@link Tab}.
     * @param icon The favicon that was received.
     */
    void onFaviconUpdated(Tab tab, Bitmap icon);

    /**
     * Called when the title of a {@link Tab} changes.
     * @param tab The notifying {@link Tab}.
     */
    void onTitleUpdated(Tab tab);

    /**
     * Called when the URL of a {@link Tab} changes.
     * @param tab The notifying {@link Tab}.
     */
    void onUrlUpdated(Tab tab);

    /**
     * Called when the SSL state of a {@link Tab} changes.
     * @param tab The notifying {@link Tab}.
     */
    void onSSLStateUpdated(Tab tab);

    /**
     * Called when the ContentView of a {@link Tab} crashes.
     * @param tab The notifying {@link Tab}.
     */
    void onCrash(Tab tab);

    /**
     * Called when restore of the corresponding tab is triggered.
     * @param tab The notifying {@link Tab}.
     */
    void onRestoreStarted(Tab tab);

    /**
     * Called when the WebContents of a {@link Tab} have been swapped.
     * @param tab The notifying {@link Tab}.
     * @param didStartLoad Whether WebContentsObserver::DidStartProvisionalLoadForFrame() has
     *     already been called.
     * @param didFinishLoad Whether WebContentsObserver::DidFinishLoad() has already been called.
     */
    void onWebContentsSwapped(Tab tab, boolean didStartLoad, boolean didFinishLoad);

    /**
     * Called when a context menu is shown for a {@link WebContents} owned by a {@link Tab}.
     * @param tab  The notifying {@link Tab}.
     * @param menu The {@link ContextMenu} that is being shown.
     */
    void onContextMenuShown(Tab tab, ContextMenu menu);

    /**
     * Called when the contextual action bar's visibility has changed (i.e. the widget shown
     * when you can copy/paste text after long press).
     * @param tab The notifying {@link Tab}.
     * @param visible Whether the contextual action bar is now visible.
     */
    void onContextualActionBarVisibilityChanged(Tab tab, boolean visible);

    // WebContentsDelegateAndroid methods ---------------------------------------------------------

    /**
     * Called when the WebContents is closed.
     * @param tab The notifying {@link Tab}.
     */
    void onCloseContents(Tab tab);

    /**
     * Called when the WebContents starts loading. Different from
     * {@link #onPageLoadStarted(Tab, String)}, if the user is navigated to a different url while
     * staying in the same html document, {@link #onLoadStarted(Tab)} will be called, while
     * {@link #onPageLoadStarted(Tab, String)} will not.
     * @param tab The notifying {@link Tab}.
     * @param toDifferentDocument Whether this navigation will transition between
     * documents (i.e., not a fragment navigation or JS History API call).
     */
    void onLoadStarted(Tab tab, boolean toDifferentDocument);

    /**
     * Called when the contents loading stops.
     * @param tab The notifying {@link Tab}.
     */
    void onLoadStopped(Tab tab, boolean toDifferentDocument);

    /**
     * Called when the load progress of a {@link Tab} changes.
     * @param tab      The notifying {@link Tab}.
     * @param progress The new progress from [0,100].
     */
    void onLoadProgressChanged(Tab tab, int progress);

    /**
     * Called when the URL of a {@link Tab} changes.
     * @param tab The notifying {@link Tab}.
     * @param url The new URL.
     */
    void onUpdateUrl(Tab tab, String url);

    /**
     * Called when the {@link Tab} should enter fullscreen mode.
     * @param tab    The notifying {@link Tab}.
     * @param options Options to adjust fullscreen mode.
     */
    void onEnterFullscreenMode(Tab tab, FullscreenOptions options);

    /**
     * Called when the {@link Tab} should exit fullscreen mode.
     * @param tab    The notifying {@link Tab}.
     */
    void onExitFullscreenMode(Tab tab);

    // WebContentsObserver methods ---------------------------------------------------------

    /**
     * Called when an error occurs while loading a page and/or the page fails to load.
     * @param tab               The notifying {@link Tab}.
     * @param isProvisionalLoad Whether the failed load occurred during the provisional load.
     * @param isMainFrame       Whether failed load happened for the main frame.
     * @param errorCode         Code for the occurring error.
     * @param description       The description for the error.
     * @param failingUrl        The url that was loading when the error occurred.
     */
    void onDidFailLoad(
            Tab tab, boolean isMainFrame, int errorCode, String description, String failingUrl);

    /**
     * Called when a navigation is started in the WebContents.
     * @param tab The notifying {@link Tab}.
     * @param navigationHandle Pointer to a NavigationHandle representing the navigation.
     *                         Its lifetime end at the end of onDidFinishNavigation().
     */
    void onDidStartNavigation(Tab tab, NavigationHandle navigationHandle);

    /**
     * Called when a navigation is redirected in the WebContents.
     * @param tab The notifying {@link Tab}.
     * @param navigationHandle Pointer to a NavigationHandle representing the navigation.
     *                         Its lifetime end at the end of onDidFinishNavigation().
     */
    void onDidRedirectNavigation(Tab tab, NavigationHandle navigationHandle);

    /**
     * Called when a navigation is finished i.e. committed, aborted or replaced by a new one.
     * @param tab The notifying {@link Tab}.
     * @param navigationHandle Pointer to a NavigationHandle representing the navigation.
     *                         Its lifetime end at the end of this function.
     */
    void onDidFinishNavigation(Tab tab, NavigationHandle navigation);

    /**
     * Called when the page has painted something non-empty.
     * @param tab The notifying {@link Tab}.
     */
    void didFirstVisuallyNonEmptyPaint(Tab tab);

    /**
     * Called when the theme color is changed
     * @param tab   The notifying {@link Tab}.
     * @param color the new color in ARGB format.
     */
    void onDidChangeThemeColor(Tab tab, int color);

    /**
     * Called when an interstitial page gets attached to the tab content.
     * @param tab The notifying {@link Tab}.
     */
    void onDidAttachInterstitialPage(Tab tab);

    /**
     * Called when an interstitial page gets detached from the tab content.
     * @param tab The notifying {@link Tab}.
     */
    void onDidDetachInterstitialPage(Tab tab);

    /**
     * Called when the background color for the tab has changed.
     * @param tab The notifying {@link Tab}.
     * @param color The current background color.
     */
    void onBackgroundColorChanged(Tab tab, int color);

    /**
     * Called when a {@link WebContents} object has been created.
     * @param tab                    The notifying {@link Tab}.
     * @param sourceWebContents      The {@link WebContents} that triggered the creation.
     * @param openerRenderProcessId  The opener render process id.
     * @param openerRenderFrameId    The opener render frame id.
     * @param frameName              The name of the frame.
     * @param targetUrl              The target url.
     * @param newWebContents         The newly created {@link WebContents}.
     */
    void webContentsCreated(Tab tab, WebContents sourceWebContents, long openerRenderProcessId,
            long openerRenderFrameId, String frameName, String targetUrl,
            WebContents newWebContents);

    /**
     * Called when the Tab is attached or detached from an {@code Activity}.
     * @param tab The notifying {@link Tab}.
     * @param isAttached Whether the Tab is being attached or detached.
     */
    void onActivityAttachmentChanged(Tab tab, boolean isAttached);

    /**
     * A notification when tab changes whether or not it is interactable and is accepting input.
     * @param isInteractable Whether or not the tab is interactable.
     */
    void onInteractabilityChanged(boolean isInteractable);

    /**
     * Called when renderer changes its state about being responsive to requests.
     * @param tab The notifying {@link Tab}.
     * @param {@code true} if the renderer becomes responsive, otherwise {@code false}.
     */
    void onRendererResponsiveStateChanged(Tab tab, boolean isResponsive);

    /**
     * Called when navigation entries of a tab have been deleted.
     * @param tab The notifying {@link Tab}.
     */
    void onNavigationEntriesDeleted(Tab tab);

    /**
     * Called when the tab's browser controls constraints has been updated.
     * @param tab The notifying {@link Tab}.
     * @param constraints The updated browser controls constraints.
     */
    void onBrowserControlsConstraintsUpdated(Tab tab, @BrowserControlsState int constraints);

    /**
     * This method is invoked when the WebContents reloads the LoFi images on the page.
     */
    void didReloadLoFiImages(Tab tab);

    /**
     * Called when a find result is received.
     * @param result Detail information on the find result.
     */
    void onFindResultAvailable(FindNotificationDetails result);

    /**
     * Called when the rects corresponding to the find matches are received.
     * @param result Detail information on the matched rects.
     */
    void onFindMatchRectsAvailable(FindMatchRectsDetails result);

    /**
     * Called when the root Id of tab is changed.
     * @param newRootId New root ID to be set.
     */
    void onRootIdChanged(Tab tab, int newRootId);
}
