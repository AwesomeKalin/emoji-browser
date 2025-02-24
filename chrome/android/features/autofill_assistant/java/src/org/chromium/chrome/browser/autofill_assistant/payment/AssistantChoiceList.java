// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill_assistant.payment;

import android.content.Context;
import android.content.res.TypedArray;
import android.support.annotation.Nullable;
import android.support.v7.widget.GridLayout;
import android.util.AttributeSet;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.CompoundButton;
import android.widget.ImageView;
import android.widget.RadioButton;
import android.widget.Space;
import android.widget.TextView;

import org.chromium.base.ApiCompatibilityUtils;
import org.chromium.base.Callback;
import org.chromium.chrome.autofill_assistant.R;
import org.chromium.ui.widget.ChromeImageView;

import java.util.ArrayList;
import java.util.List;

/**
 * A widget that displays a list of choices in a regular grid. It is similar to a RadioGroup, but
 * much more customizable, because it allows arbitrary content views rather than only TextViews.
 *
 * The layout is as follows:
 * [radio-button] |    content   | [edit-button]
 * [radio-button] |    content   | [edit-button]
 * ...
 * [add-icon]     | [add-button] |
 *
 * A number of custom view attributes control the layout and look of this widget.
 *  - Edit-buttons are optional on an item-by-item basis.
 *  - The add-button at the end of the list is optional.
 *  - Spacing between rows and columns can be customized.
 *  - The text for the `add' and `edit' buttons can be customized.
 */
public class AssistantChoiceList extends GridLayout {
    /**
     * Represents a single choice with a radio button, customizable content and an edit button.
     */
    private class Item {
        final CompoundButton mCompoundButton;
        final Callback<Boolean> mOnSelectedListener;
        final View mContent;
        final View mEditButton;
        final View mSpacer;

        Item(@Nullable CompoundButton compoundButton, @Nullable Callback onSelectedListener,
                View content, @Nullable View editButton, @Nullable View spacer) {
            this.mCompoundButton = compoundButton;
            this.mOnSelectedListener = onSelectedListener;
            this.mContent = content;
            this.mEditButton = editButton;
            this.mSpacer = spacer;
        }
    }

    /**
     * |mCanAddItems| is true if the custom view parameter |add_button_text| was specified.
     * If true, the list will have an additional 'add' button at the end.
     */
    private final boolean mCanAddItems;
    /**
     * |mAddButton| and |mAddButtonLabel| are guaranteed to be non-null if |mCanAddItems| is true.
     */
    private final ChromeImageView mAddButton;
    private final TextView mAddButtonLabel;
    private final int mRowSpacing;
    private final int mColumnSpacing;
    private final int mAddButtonSpacing;
    private final List<Item> mItems = new ArrayList<>();
    private boolean mAllowMultipleChoices;
    private Runnable mAddButtonListener;

    public AssistantChoiceList(Context context, AttributeSet attrs) {
        super(context, attrs);
        TypedArray a = context.getTheme().obtainStyledAttributes(
                attrs, R.styleable.AssistantChoiceList, 0, 0);
        mCanAddItems = a.getBoolean(R.styleable.AssistantChoiceList_can_add_items, true);
        String addButtonText =
                a.hasValue(R.styleable.AssistantChoiceList_add_button_text) && mCanAddItems
                ? a.getString(R.styleable.AssistantChoiceList_add_button_text)
                : null;
        mRowSpacing = a.getDimensionPixelSize(R.styleable.AssistantChoiceList_row_spacing, 0);
        mColumnSpacing = a.getDimensionPixelSize(R.styleable.AssistantChoiceList_column_spacing, 0);
        mAddButtonSpacing = context.getResources().getDimensionPixelSize(
                R.dimen.autofill_assistant_choicelist_add_button_spacing);
        a.recycle();

        // One column for the radio buttons, one for the content, one for the edit buttons.
        setColumnCount(3);

        if (mCanAddItems) {
            mAddButton = createAddButtonIcon();
            mAddButtonLabel = createAddButtonLabel(addButtonText);

            addViewInternal(mAddButton, -1, createRadioButtonLayoutParams());
            GridLayout.LayoutParams lp =
                    new GridLayout.LayoutParams(GridLayout.spec(UNDEFINED), GridLayout.spec(1, 2));
            lp.setGravity(Gravity.FILL_HORIZONTAL | Gravity.CENTER_VERTICAL);
            lp.width = 0;
            addViewInternal(mAddButtonLabel, -1, lp);

            // Set margin to 0 because list is currently empty.
            updateAddButtonMargins(0);
        } else {
            mAddButton = null;
            mAddButtonLabel = null;
        }
    }

    /**
     * Set whether this list allows multiple choices to be selected at the same time. This method
     * can only be called when no items have been added, otherwise it will throw an exception.
     */
    public void setAllowMultipleChoices(boolean allowMultipleChoices) {
        if (!mItems.isEmpty()) {
            throw new UnsupportedOperationException(
                    "Calling #setAllowMultipleChoices is not allowed when items have already been "
                    + "added.");
        }

        mAllowMultipleChoices = allowMultipleChoices;
    }

    /**
     * Children of this container are automatically added as selectable items to the list.
     *
     * This method is automatically called by layout inflaters and xml files. In code, you usually
     * want to call |addItem| directly.
     */
    @Override
    public void addView(View view, int index, ViewGroup.LayoutParams lp) {
        assert index == -1;
        addItem(view);
    }

    public void addItem(View view) {
        addItem(view, true);
    }

    public void addItem(View view, boolean hasEditButton) {
        addItem(view, hasEditButton, null, null);
    }

    /**
     * Adds an item to the list. Additional widgets to select and edit the item are created as
     * necessary.
     *
     * @param view The view to add to the list.
     * @param hasEditButton Whether an edit button should be offered.
     * @param itemSelectedListener Optional listener which is notified when the item is selected or
     * deselected.
     * @param itemEditedListener Optional listener which is notified when the item is edited.
     */
    public void addItem(View view, boolean hasEditButton,
            @Nullable Callback<Boolean> itemSelectedListener,
            @Nullable Runnable itemEditedListener) {
        CompoundButton radioButton =
                mAllowMultipleChoices ? new CheckBox(getContext()) : new RadioButton(getContext());
        // Insert at end, before the `add' button (if any).
        int viewIndex = mCanAddItems ? indexOfChild(mAddButton) : getChildCount();
        addViewInternal(radioButton, viewIndex++, createRadioButtonLayoutParams());
        addViewInternal(view, viewIndex++, createContentLayoutParams());

        ChromeImageView editButton = null;
        View spacer = null;
        if (hasEditButton) {
            editButton = new ChromeImageView(getContext());
            editButton.setImageResource(R.drawable.ic_edit_24dp);
            editButton.setScaleType(ImageView.ScaleType.CENTER_INSIDE);
            editButton.setOnClickListener(unusedView -> {
                if (itemEditedListener != null) {
                    itemEditedListener.run();
                }
            });
            addViewInternal(editButton, viewIndex++, createEditButtonLayoutParams());
        } else {
            spacer = new Space(getContext());
            addViewInternal(spacer, viewIndex++, createEditButtonLayoutParams());
        }

        Item item = new Item(radioButton, itemSelectedListener, view, editButton, spacer);

        // When clicking a checkbox, invert its checked value. A radio button will always be
        // selected when clicked.
        View.OnClickListener clickListener = unusedView
                -> setChecked(
                        view, mAllowMultipleChoices ? !item.mCompoundButton.isChecked() : true);
        radioButton.setOnClickListener(clickListener);
        view.setOnClickListener(clickListener);
        mItems.add(item);

        // Need to adjust button margins after first item was inserted.
        if (mItems.size() == 1) {
            updateAddButtonMargins(mAddButtonSpacing);
        }
    }

    /**
     * Removes all items from the list.
     */
    public void clearItems() {
        for (int i = 0; i < mItems.size(); i++) {
            Item item = mItems.get(i);
            removeView(item.mContent);
            removeView(item.mCompoundButton);
            if (item.mEditButton != null) {
                removeView(item.mEditButton);
            }
            if (item.mSpacer != null) {
                removeView(item.mSpacer);
            }
        }
        mItems.clear();
        updateAddButtonMargins(0);
    }

    public View getItem(int index) {
        if (index >= 0 && index < mItems.size()) {
            return mItems.get(index).mContent;
        }
        return null;
    }

    public int getItemCount() {
        return mItems.size();
    }

    /**
     * Selects the specified item. If this choice list does not allow checking multiple choice, this
     * will also deselect all other items.
     *
     * @param content The content view to select, as specified in |addItem|. Can be null to indicate
     * that all items should be de-selected.
     */
    public void setCheckedItem(@Nullable View content) {
        if (content == null) {
            for (Item item : mItems) {
                item.mCompoundButton.setChecked(false);
                if (item.mOnSelectedListener != null) {
                    item.mOnSelectedListener.onResult(false);
                }
            }
            return;
        }

        setChecked(content, true);
    }

    /**
     * Sets whether the specified item is checked or not. If this choice list does not allow
     * checking multiple choice and {@code checked} is true, this will also deselect all other
     * items.
     *
     * @param content The content view to (un)select, as specified in |addItem|.
     */
    public void setChecked(View content, boolean checked) {
        for (Item item : mItems) {
            boolean notifyListener = false;
            if (item.mContent == content) {
                item.mCompoundButton.setChecked(checked);
                notifyListener = true;
            } else if (checked && !mAllowMultipleChoices) {
                item.mCompoundButton.setChecked(false);
                notifyListener = true;
            }

            if (notifyListener && item.mOnSelectedListener != null) {
                item.mOnSelectedListener.onResult(item.mCompoundButton.isChecked());
            }
        }
    }

    /**
     * Allows to change the label of the 'add' button.
     */
    public void setAddButtonLabel(String label) {
        if (mAddButtonLabel != null) {
            mAddButtonLabel.setText(label);
        }
    }

    public void setOnAddButtonClickedListener(Runnable listener) {
        mAddButtonListener = listener;
    }

    /**
     * Adds a view to the underlying gridlayout.
     *
     * This method is used internally to add a view to the actual layout. A single call to |addView|
     * will result in multiple calls to |addViewInternal|, because additional widgets are
     * automatically generated (e.g., radio-buttons and edit-buttons).
     *
     * @param view The view to add to the layout.
     * @param index The index at which to insert the view into the layout. Note that this - along
     * with the column width specified in |lp| - will determine the column in which the view will
     * end up in.
     * @param lp Additional layout parameters, see |GridLayout.LayoutParams|.
     */
    private void addViewInternal(View view, int index, ViewGroup.LayoutParams lp) {
        super.addView(view, index, lp);
    }

    private ChromeImageView createAddButtonIcon() {
        ChromeImageView addButtonIcon = new ChromeImageView(getContext());
        addButtonIcon.setImageResource(R.drawable.ic_autofill_assistant_add_circle_24dp);
        addButtonIcon.setOnClickListener(unusedView -> {
            if (mAddButtonListener != null) {
                mAddButtonListener.run();
            }
        });
        return addButtonIcon;
    }

    private TextView createAddButtonLabel(String addButtonText) {
        TextView addButtonLabel = new TextView(getContext());
        ApiCompatibilityUtils.setTextAppearance(
                addButtonLabel, R.style.TextAppearance_BlueButtonText2);
        addButtonLabel.setText(addButtonText);
        addButtonLabel.setOnClickListener(unusedView -> {
            if (mAddButtonListener != null) {
                mAddButtonListener.run();
            }
        });
        return addButtonLabel;
    }

    private GridLayout.LayoutParams createContentLayoutParams() {
        // Set layout params to let content grow to maximum size.
        GridLayout.LayoutParams lp =
                new GridLayout.LayoutParams(GridLayout.spec(UNDEFINED), GridLayout.spec(1, 1));
        lp.setGravity(Gravity.FILL_HORIZONTAL | Gravity.CENTER_VERTICAL);
        lp.width = 0;
        lp.topMargin = mItems.isEmpty() ? 0 : mRowSpacing;
        return lp;
    }

    private GridLayout.LayoutParams createRadioButtonLayoutParams() {
        GridLayout.LayoutParams lp =
                new GridLayout.LayoutParams(GridLayout.spec(UNDEFINED), GridLayout.spec(0, 1));
        lp.setGravity(Gravity.CENTER);
        lp.setMarginEnd(mColumnSpacing);
        lp.topMargin = mItems.isEmpty() ? 0 : mRowSpacing;
        return lp;
    }

    private GridLayout.LayoutParams createEditButtonLayoutParams() {
        int editButtonSize = getContext().getResources().getDimensionPixelSize(
                R.dimen.autofill_assistant_choicelist_edit_button_size);
        GridLayout.LayoutParams lp =
                new GridLayout.LayoutParams(GridLayout.spec(UNDEFINED), GridLayout.spec(2, 1));
        lp.setGravity(Gravity.CENTER_VERTICAL | Gravity.FILL_VERTICAL);
        lp.setMarginStart(mColumnSpacing);
        lp.width = editButtonSize;
        lp.height = editButtonSize;
        lp.topMargin = mItems.isEmpty() ? 0 : mRowSpacing;
        return lp;
    }

    /**
     * Adjusts the margins of the 'add' button.
     *
     * For empty lists, the margins should be 0. For non-empty lists, the margins should be equal
     * to |mRowSpacing|.
     */
    private void updateAddButtonMargins(int marginTop) {
        if (!mCanAddItems) {
            return;
        }

        LayoutParams lp = (LayoutParams) mAddButton.getLayoutParams();
        lp.setMargins(lp.leftMargin, marginTop, lp.rightMargin, lp.bottomMargin);
        mAddButton.setLayoutParams(lp);

        lp = (LayoutParams) mAddButtonLabel.getLayoutParams();
        lp.setMargins(lp.leftMargin, marginTop, lp.rightMargin, lp.bottomMargin);
        mAddButtonLabel.setLayoutParams(lp);
    }
}
