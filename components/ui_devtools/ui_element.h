// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_UI_DEVTOOLS_UI_ELEMENT_H_
#define COMPONENTS_UI_DEVTOOLS_UI_ELEMENT_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/macros.h"
#include "components/ui_devtools/devtools_export.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/native_widget_types.h"

namespace ui_devtools {

class UIElementDelegate;

// UIElement type.
enum UIElementType { WINDOW, WIDGET, VIEW, ROOT, FRAMESINK, SURFACE };

class UI_DEVTOOLS_EXPORT UIElement {
 public:
  using UIElements = std::vector<UIElement*>;

  // resets node ids to 0 so that they are reusable
  static void ResetNodeId();

  virtual ~UIElement();
  int node_id() const { return node_id_; }
  std::string GetTypeName() const;
  UIElement* parent() const { return parent_; }
  void set_parent(UIElement* parent) { parent_ = parent; }
  UIElementDelegate* delegate() const { return delegate_; }
  UIElementType type() const { return type_; }
  const UIElements& children() const { return children_; }
  bool is_updating() const { return is_updating_; }
  void set_is_updating(bool is_updating) { is_updating_ = is_updating; }
  void set_owns_children(bool owns_children) { owns_children_ = owns_children; }

  using ElementCompare = bool (*)(const UIElement*, const UIElement*);

  // Inserts |child| in front of |before|. If |before| is null, it is inserted
  // at the end. Parent takes ownership of the added child.
  void AddChild(UIElement* child, UIElement* before = nullptr);

  // Inserts |child| according to a custom ordering function. |notify_delegate|
  // calls OnUIElementAdded(), which creates the subtree of UIElements at
  // |child|, and the corresponding DOM nodes.
  void AddOrderedChild(UIElement* child,
                       ElementCompare compare,
                       bool notify_delegate = true);

  // Removes all elements from |children_|. Caller is responsible for destroying
  // children.
  void ClearChildren();

  // Removes |child| out of |children_| without destroying |child|. The caller
  // is responsible for destroying |child|. |notify_delegate| calls
  // OnUIElementRemoved(), which destroys the DOM node for |child|.
  void RemoveChild(UIElement* child, bool notify_delegate = true);

  // Moves |child| to position |index| in |children_|.
  void ReorderChild(UIElement* child, int index);

  template <class T>
  int FindUIElementIdForBackendElement(T* element) const;

  // Returns properties' names and values.
  virtual std::vector<std::pair<std::string, std::string>> GetCustomProperties()
      const = 0;

  virtual void GetBounds(gfx::Rect* bounds) const = 0;
  virtual void SetBounds(const gfx::Rect& bounds) = 0;
  virtual void GetVisible(bool* visible) const = 0;
  virtual void SetVisible(bool visible) = 0;

  // Set this element's property values according to |text|.
  // |text| is the string passed in through StyleDeclarationEdit::text from
  // the frontend.
  virtual bool SetPropertiesFromString(const std::string& text);

  // If element exists, returns its associated native window and its screen
  // bounds. Otherwise, returns null and empty bounds.
  virtual std::pair<gfx::NativeWindow, gfx::Rect> GetNodeWindowAndScreenBounds()
      const = 0;

  // Returns a list of interleaved keys and values of attributes to be displayed
  // on the element in the dev tools hierarchy view.
  virtual std::vector<std::string> GetAttributes() const = 0;

  template <typename BackingT, typename T>
  static BackingT* GetBackingElement(const UIElement* element) {
    return T::From(element);
  }

 protected:
  UIElement(const UIElementType type,
            UIElementDelegate* delegate,
            UIElement* parent);

 private:
  const int node_id_;
  const UIElementType type_;
  UIElements children_;
  UIElement* parent_;
  UIElementDelegate* delegate_;
  bool is_updating_ = false;
  bool owns_children_ = true;

  DISALLOW_COPY_AND_ASSIGN(UIElement);
};

}  // namespace ui_devtools

#endif  // COMPONENTS_UI_DEVTOOLS_UI_ELEMENT_H_
