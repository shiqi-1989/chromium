// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/aura/window.h"

#include <algorithm>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "ui/aura/client/capture_client.h"
#include "ui/aura/client/cursor_client.h"
#include "ui/aura/client/event_client.h"
#include "ui/aura/client/focus_client.h"
#include "ui/aura/client/screen_position_client.h"
#include "ui/aura/client/visibility_client.h"
#include "ui/aura/env.h"
#include "ui/aura/layout_manager.h"
#include "ui/aura/root_window.h"
#include "ui/aura/window_delegate.h"
#include "ui/aura/window_observer.h"
#include "ui/aura/window_tracker.h"
#include "ui/compositor/compositor.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/animation/multi_animation.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/path.h"
#include "ui/gfx/screen.h"

namespace aura {

class ScopedCursorHider {
 public:
  explicit ScopedCursorHider(Window* window)
      : window_(window),
        hid_cursor_(false) {
    if (!window_->HasDispatcher())
      return;
    const bool cursor_is_in_bounds = window_->GetBoundsInScreen().Contains(
        Env::GetInstance()->last_mouse_location());
    client::CursorClient* cursor_client = client::GetCursorClient(window_);
    if (cursor_is_in_bounds && cursor_client &&
        cursor_client->IsCursorVisible()) {
      cursor_client->HideCursor();
      hid_cursor_ = true;
    }
  }
  ~ScopedCursorHider() {
    if (!window_->HasDispatcher())
      return;

    // Update the device scale factor of the cursor client only when the last
    // mouse location is on this root window.
    if (hid_cursor_) {
      client::CursorClient* cursor_client = client::GetCursorClient(window_);
      if (cursor_client) {
        const gfx::Display& display =
            gfx::Screen::GetScreenFor(window_)->GetDisplayNearestWindow(
                window_);
        cursor_client->SetDisplay(display);
        cursor_client->ShowCursor();
      }
    }
  }

 private:
  Window* window_;
  bool hid_cursor_;

  DISALLOW_COPY_AND_ASSIGN(ScopedCursorHider);
};

Window::Window(WindowDelegate* delegate)
    : dispatcher_(NULL),
      type_(client::WINDOW_TYPE_UNKNOWN),
      owned_by_parent_(true),
      delegate_(delegate),
      parent_(NULL),
      transient_parent_(NULL),
      visible_(false),
      id_(-1),
      transparent_(false),
      user_data_(NULL),
      ignore_events_(false),
      // Don't notify newly added observers during notification. This causes
      // problems for code that adds an observer as part of an observer
      // notification (such as the workspace code).
      observers_(ObserverList<WindowObserver>::NOTIFY_EXISTING_ONLY) {
  set_target_handler(delegate_);
}

Window::~Window() {
  // layer_ can be NULL if Init() wasn't invoked, which can happen
  // only in tests.
  if (layer_)
    layer_->SuppressPaint();

  // Let the delegate know we're in the processing of destroying.
  if (delegate_)
    delegate_->OnWindowDestroying();
  FOR_EACH_OBSERVER(WindowObserver, observers_, OnWindowDestroying(this));

  // Let the root know so that it can remove any references to us.
  WindowEventDispatcher* dispatcher = GetDispatcher();
  if (dispatcher)
    dispatcher->OnWindowDestroying(this);

  // Then destroy the children.
  RemoveOrDestroyChildren();

  // Removes ourselves from our transient parent (if it hasn't been done by the
  // RootWindow).
  if (transient_parent_)
    transient_parent_->RemoveTransientChild(this);

  // The window needs to be removed from the parent before calling the
  // WindowDestroyed callbacks of delegate and the observers.
  if (parent_)
    parent_->RemoveChild(this);

  // Destroy transient children, only after we've removed ourselves from our
  // parent, as destroying an active transient child may otherwise attempt to
  // refocus us.
  Windows transient_children(transient_children_);
  STLDeleteElements(&transient_children);
  DCHECK(transient_children_.empty());

  // Delegate and observers need to be notified after transients are deleted.
  if (delegate_)
    delegate_->OnWindowDestroyed();
  FOR_EACH_OBSERVER(WindowObserver, observers_, OnWindowDestroyed(this));

  // Clear properties.
  for (std::map<const void*, Value>::const_iterator iter = prop_map_.begin();
       iter != prop_map_.end();
       ++iter) {
    if (iter->second.deallocator)
      (*iter->second.deallocator)(iter->second.value);
  }
  prop_map_.clear();

  // If we have layer it will either be destroyed by layer_owner_'s dtor, or by
  // whoever acquired it. We don't have a layer if Init() wasn't invoked, which
  // can happen in tests.
  if (layer_)
    layer_->set_delegate(NULL);
  layer_ = NULL;
}

void Window::Init(ui::LayerType layer_type) {
  layer_ = new ui::Layer(layer_type);
  layer_owner_.reset(layer_);
  layer_->SetVisible(false);
  layer_->set_delegate(this);
  UpdateLayerName(name_);
  layer_->SetFillsBoundsOpaquely(!transparent_);

  Env::GetInstance()->NotifyWindowInitialized(this);
}

ui::Layer* Window::RecreateLayer() {
  // Disconnect the old layer, but don't delete it.
  ui::Layer* old_layer = AcquireLayer();
  if (!old_layer)
    return NULL;

  old_layer->set_delegate(NULL);

  layer_ = new ui::Layer(old_layer->type());
  layer_owner_.reset(layer_);
  layer_->SetVisible(old_layer->visible());
  layer_->set_scale_content(old_layer->scale_content());
  layer_->set_delegate(this);
  layer_->SetMasksToBounds(old_layer->GetMasksToBounds());

  if (delegate_)
    delegate_->DidRecreateLayer(old_layer, layer_);

  UpdateLayerName(name_);
  layer_->SetFillsBoundsOpaquely(!transparent_);
  // Install new layer as a sibling of the old layer, stacked below it.
  if (old_layer->parent()) {
    old_layer->parent()->Add(layer_);
    old_layer->parent()->StackBelow(layer_, old_layer);
  }
  // Migrate all the child layers over to the new layer. Copy the list because
  // the items are removed during iteration.
  std::vector<ui::Layer*> children_copy = old_layer->children();
  for (std::vector<ui::Layer*>::const_iterator it = children_copy.begin();
       it != children_copy.end();
       ++it) {
    ui::Layer* child = *it;
    layer_->Add(child);
  }
  return old_layer;
}

void Window::SetType(client::WindowType type) {
  // Cannot change type after the window is initialized.
  DCHECK(!layer());
  type_ = type;
}

void Window::SetName(const std::string& name) {
  name_ = name;

  if (layer())
    UpdateLayerName(name_);
}

void Window::SetTransparent(bool transparent) {
  transparent_ = transparent;
  if (layer())
    layer_->SetFillsBoundsOpaquely(!transparent_);
}

Window* Window::GetRootWindow() {
  return const_cast<Window*>(
      static_cast<const Window*>(this)->GetRootWindow());
}

const Window* Window::GetRootWindow() const {
  return dispatcher_ ? this : parent_ ? parent_->GetRootWindow() : NULL;
}

WindowEventDispatcher* Window::GetDispatcher() {
  return const_cast<WindowEventDispatcher*>(const_cast<const Window*>(this)->
      GetDispatcher());
}

const WindowEventDispatcher* Window::GetDispatcher() const {
  const Window* root_window = GetRootWindow();
  return root_window ? root_window->dispatcher_ : NULL;
}

void Window::Show() {
  SetVisible(true);
}

void Window::Hide() {
  for (Windows::iterator it = transient_children_.begin();
       it != transient_children_.end(); ++it) {
    (*it)->Hide();
  }
  SetVisible(false);
  ReleaseCapture();
}

bool Window::IsVisible() const {
  // Layer visibility can be inconsistent with window visibility, for example
  // when a Window is hidden, we want this function to return false immediately
  // after, even though the client may decide to animate the hide effect (and
  // so the layer will be visible for some time after Hide() is called).
  return visible_ && layer_ && layer_->IsDrawn();
}

gfx::Rect Window::GetBoundsInRootWindow() const {
  // TODO(beng): There may be a better way to handle this, and the existing code
  //             is likely wrong anyway in a multi-display world, but this will
  //             do for now.
  if (!GetRootWindow())
    return bounds();
  gfx::Point origin = bounds().origin();
  ConvertPointToTarget(parent_, GetRootWindow(), &origin);
  return gfx::Rect(origin, bounds().size());
}

gfx::Rect Window::GetBoundsInScreen() const {
  gfx::Rect bounds(GetBoundsInRootWindow());
  const Window* root = GetRootWindow();
  if (root) {
    aura::client::ScreenPositionClient* screen_position_client =
        aura::client::GetScreenPositionClient(root);
    if (screen_position_client) {
      gfx::Point origin = bounds.origin();
      screen_position_client->ConvertPointToScreen(root, &origin);
      bounds.set_origin(origin);
    }
  }
  return bounds;
}

void Window::SetTransform(const gfx::Transform& transform) {
  WindowEventDispatcher* dispatcher = GetDispatcher();
  bool contained_mouse = IsVisible() && dispatcher &&
      ContainsPointInRoot(dispatcher->GetLastMouseLocationInRoot());
  layer()->SetTransform(transform);
  if (dispatcher)
    dispatcher->OnWindowTransformed(this, contained_mouse);
}

void Window::SetLayoutManager(LayoutManager* layout_manager) {
  if (layout_manager == layout_manager_)
    return;
  layout_manager_.reset(layout_manager);
  if (!layout_manager)
    return;
  // If we're changing to a new layout manager, ensure it is aware of all the
  // existing child windows.
  for (Windows::const_iterator it = children_.begin();
       it != children_.end();
       ++it)
    layout_manager_->OnWindowAddedToLayout(*it);
}

void Window::SetBounds(const gfx::Rect& new_bounds) {
  if (parent_ && parent_->layout_manager())
    parent_->layout_manager()->SetChildBounds(this, new_bounds);
  else
    SetBoundsInternal(new_bounds);
}

void Window::SetBoundsInScreen(const gfx::Rect& new_bounds_in_screen,
                               const gfx::Display& dst_display) {
  Window* root = GetRootWindow();
  if (root) {
    gfx::Point origin = new_bounds_in_screen.origin();
    aura::client::ScreenPositionClient* screen_position_client =
        aura::client::GetScreenPositionClient(root);
    screen_position_client->SetBounds(this, new_bounds_in_screen, dst_display);
    return;
  }
  SetBounds(new_bounds_in_screen);
}

gfx::Rect Window::GetTargetBounds() const {
  return layer_->GetTargetBounds();
}

const gfx::Rect& Window::bounds() const {
  return layer_->bounds();
}

void Window::SchedulePaintInRect(const gfx::Rect& rect) {
  if (layer_->SchedulePaint(rect)) {
    FOR_EACH_OBSERVER(
        WindowObserver, observers_, OnWindowPaintScheduled(this, rect));
  }
}

void Window::StackChildAtTop(Window* child) {
  if (children_.size() <= 1 || child == children_.back())
    return;  // In the front already.
  StackChildAbove(child, children_.back());
}

void Window::StackChildAbove(Window* child, Window* target) {
  StackChildRelativeTo(child, target, STACK_ABOVE);
}

void Window::StackChildAtBottom(Window* child) {
  if (children_.size() <= 1 || child == children_.front())
    return;  // At the bottom already.
  StackChildBelow(child, children_.front());
}

void Window::StackChildBelow(Window* child, Window* target) {
  StackChildRelativeTo(child, target, STACK_BELOW);
}

void Window::AddChild(Window* child) {
  WindowObserver::HierarchyChangeParams params;
  params.target = child;
  params.new_parent = this;
  params.old_parent = child->parent();
  params.phase = WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGING;
  NotifyWindowHierarchyChange(params);

  Window* old_root = child->GetRootWindow();

  DCHECK(std::find(children_.begin(), children_.end(), child) ==
      children_.end());
  if (child->parent())
    child->parent()->RemoveChildImpl(child, this);
  child->parent_ = this;

  layer_->Add(child->layer_);

  children_.push_back(child);
  if (layout_manager_)
    layout_manager_->OnWindowAddedToLayout(child);
  FOR_EACH_OBSERVER(WindowObserver, observers_, OnWindowAdded(child));
  child->OnParentChanged();

  Window* root_window = GetRootWindow();
  if (root_window && old_root != root_window) {
    root_window->GetDispatcher()->OnWindowAddedToRootWindow(child);
    child->NotifyAddedToRootWindow();
  }

  params.phase = WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGED;
  NotifyWindowHierarchyChange(params);
}

void Window::RemoveChild(Window* child) {
  WindowObserver::HierarchyChangeParams params;
  params.target = child;
  params.new_parent = NULL;
  params.old_parent = this;
  params.phase = WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGING;
  NotifyWindowHierarchyChange(params);

  RemoveChildImpl(child, NULL);

  params.phase = WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGED;
  NotifyWindowHierarchyChange(params);
}

bool Window::Contains(const Window* other) const {
  for (const Window* parent = other; parent; parent = parent->parent_) {
    if (parent == this)
      return true;
  }
  return false;
}

void Window::AddTransientChild(Window* child) {
  if (child->transient_parent_)
    child->transient_parent_->RemoveTransientChild(child);
  DCHECK(std::find(transient_children_.begin(), transient_children_.end(),
                   child) == transient_children_.end());
  transient_children_.push_back(child);
  child->transient_parent_ = this;
  FOR_EACH_OBSERVER(WindowObserver, observers_,
                    OnAddTransientChild(this, child));
}

void Window::RemoveTransientChild(Window* child) {
  Windows::iterator i =
      std::find(transient_children_.begin(), transient_children_.end(), child);
  DCHECK(i != transient_children_.end());
  transient_children_.erase(i);
  if (child->transient_parent_ == this)
    child->transient_parent_ = NULL;
  FOR_EACH_OBSERVER(WindowObserver, observers_,
                    OnRemoveTransientChild(this, child));
}

Window* Window::GetChildById(int id) {
  return const_cast<Window*>(const_cast<const Window*>(this)->GetChildById(id));
}

const Window* Window::GetChildById(int id) const {
  Windows::const_iterator i;
  for (i = children_.begin(); i != children_.end(); ++i) {
    if ((*i)->id() == id)
      return *i;
    const Window* result = (*i)->GetChildById(id);
    if (result)
      return result;
  }
  return NULL;
}

// static
void Window::ConvertPointToTarget(const Window* source,
                                  const Window* target,
                                  gfx::Point* point) {
  if (!source)
    return;
  if (source->GetRootWindow() != target->GetRootWindow()) {
    client::ScreenPositionClient* source_client =
        client::GetScreenPositionClient(source->GetRootWindow());
    source_client->ConvertPointToScreen(source, point);

    client::ScreenPositionClient* target_client =
        client::GetScreenPositionClient(target->GetRootWindow());
    target_client->ConvertPointFromScreen(target, point);
  } else {
    ui::Layer::ConvertPointToLayer(source->layer(), target->layer(), point);
  }
}

void Window::MoveCursorTo(const gfx::Point& point_in_window) {
  Window* root_window = GetRootWindow();
  DCHECK(root_window);
  gfx::Point point_in_root(point_in_window);
  ConvertPointToTarget(this, root_window, &point_in_root);
  root_window->GetDispatcher()->MoveCursorTo(point_in_root);
}

gfx::NativeCursor Window::GetCursor(const gfx::Point& point) const {
  return delegate_ ? delegate_->GetCursor(point) : gfx::kNullCursor;
}

void Window::SetEventFilter(ui::EventHandler* event_filter) {
  if (event_filter_)
    RemovePreTargetHandler(event_filter_.get());
  event_filter_.reset(event_filter);
  if (event_filter)
    AddPreTargetHandler(event_filter);
}

void Window::AddObserver(WindowObserver* observer) {
  observers_.AddObserver(observer);
}

void Window::RemoveObserver(WindowObserver* observer) {
  observers_.RemoveObserver(observer);
}

bool Window::HasObserver(WindowObserver* observer) {
  return observers_.HasObserver(observer);
}

bool Window::ContainsPointInRoot(const gfx::Point& point_in_root) const {
  const Window* root_window = GetRootWindow();
  if (!root_window)
    return false;
  gfx::Point local_point(point_in_root);
  ConvertPointToTarget(root_window, this, &local_point);
  return gfx::Rect(GetTargetBounds().size()).Contains(local_point);
}

bool Window::ContainsPoint(const gfx::Point& local_point) const {
  return gfx::Rect(bounds().size()).Contains(local_point);
}

bool Window::HitTest(const gfx::Point& local_point) {
  // Expand my bounds for hit testing (override is usually zero but it's
  // probably cheaper to do the math every time than to branch).
  gfx::Rect local_bounds(gfx::Point(), bounds().size());
  local_bounds.Inset(aura::Env::GetInstance()->is_touch_down() ?
      hit_test_bounds_override_outer_touch_ :
      hit_test_bounds_override_outer_mouse_);

  if (!delegate_ || !delegate_->HasHitTestMask())
    return local_bounds.Contains(local_point);

  gfx::Path mask;
  delegate_->GetHitTestMask(&mask);

  SkRegion clip_region;
  clip_region.setRect(local_bounds.x(), local_bounds.y(),
                      local_bounds.width(), local_bounds.height());
  SkRegion mask_region;
  return mask_region.setPath(mask, clip_region) &&
      mask_region.contains(local_point.x(), local_point.y());
}

Window* Window::GetEventHandlerForPoint(const gfx::Point& local_point) {
  return GetWindowForPoint(local_point, true, true);
}

Window* Window::GetTopWindowContainingPoint(const gfx::Point& local_point) {
  return GetWindowForPoint(local_point, false, false);
}

Window* Window::GetToplevelWindow() {
  Window* topmost_window_with_delegate = NULL;
  for (aura::Window* window = this; window != NULL; window = window->parent()) {
    if (window->delegate())
      topmost_window_with_delegate = window;
  }
  return topmost_window_with_delegate;
}

void Window::Focus() {
  client::FocusClient* client = client::GetFocusClient(this);
  DCHECK(client);
  client->FocusWindow(this);
}

void Window::Blur() {
  client::FocusClient* client = client::GetFocusClient(this);
  DCHECK(client);
  client->FocusWindow(NULL);
}

bool Window::HasFocus() const {
  client::FocusClient* client = client::GetFocusClient(this);
  return client && client->GetFocusedWindow() == this;
}

bool Window::CanFocus() const {
  if (dispatcher_)
    return IsVisible();

  // NOTE: as part of focusing the window the ActivationClient may make the
  // window visible (by way of making a hidden ancestor visible). For this
  // reason we can't check visibility here and assume the client is doing it.
  if (!parent_ || (delegate_ && !delegate_->CanFocus()))
    return false;

  // The client may forbid certain windows from receiving focus at a given point
  // in time.
  client::EventClient* client = client::GetEventClient(GetRootWindow());
  if (client && !client->CanProcessEventsWithinSubtree(this))
    return false;

  return parent_->CanFocus();
}

bool Window::CanReceiveEvents() const {
  if (dispatcher_)
    return IsVisible();

  // The client may forbid certain windows from receiving events at a given
  // point in time.
  client::EventClient* client = client::GetEventClient(GetRootWindow());
  if (client && !client->CanProcessEventsWithinSubtree(this))
    return false;

  return parent_ && IsVisible() && parent_->CanReceiveEvents();
}

void Window::SetCapture() {
  if (!IsVisible())
    return;

  Window* root_window = GetRootWindow();
  if (!root_window)
    return;
  client::GetCaptureClient(root_window)->SetCapture(this);
}

void Window::ReleaseCapture() {
  Window* root_window = GetRootWindow();
  if (!root_window)
    return;
  client::GetCaptureClient(root_window)->ReleaseCapture(this);
}

bool Window::HasCapture() {
  Window* root_window = GetRootWindow();
  if (!root_window)
    return false;
  client::CaptureClient* capture_client = client::GetCaptureClient(root_window);
  return capture_client && capture_client->GetCaptureWindow() == this;
}

void Window::SuppressPaint() {
  layer_->SuppressPaint();
}

// {Set,Get,Clear}Property are implemented in window_property.h.

void Window::SetNativeWindowProperty(const char* key, void* value) {
  SetPropertyInternal(
      key, key, NULL, reinterpret_cast<int64>(value), 0);
}

void* Window::GetNativeWindowProperty(const char* key) const {
  return reinterpret_cast<void*>(GetPropertyInternal(key, 0));
}

void Window::OnDeviceScaleFactorChanged(float device_scale_factor) {
  ScopedCursorHider hider(this);
  if (dispatcher_)
    dispatcher_->DeviceScaleFactorChanged(device_scale_factor);
  if (delegate_)
    delegate_->OnDeviceScaleFactorChanged(device_scale_factor);
}

#ifndef NDEBUG
std::string Window::GetDebugInfo() const {
  return base::StringPrintf(
      "%s<%d> bounds(%d, %d, %d, %d) %s %s opacity=%.1f",
      name().empty() ? "Unknown" : name().c_str(), id(),
      bounds().x(), bounds().y(), bounds().width(), bounds().height(),
      visible_ ? "WindowVisible" : "WindowHidden",
      layer_->GetTargetVisibility() ? "LayerVisible" : "LayerHidden",
      layer_->opacity());
}

void Window::PrintWindowHierarchy(int depth) const {
  printf("%*s%s\n", depth * 2, "", GetDebugInfo().c_str());
  for (Windows::const_iterator it = children_.begin();
       it != children_.end(); ++it) {
    Window* child = *it;
    child->PrintWindowHierarchy(depth + 1);
  }
}
#endif

void Window::RemoveOrDestroyChildren() {
  while (!children_.empty()) {
    Window* child = children_[0];
    if (child->owned_by_parent_) {
      delete child;
      // Deleting the child so remove it from out children_ list.
      DCHECK(std::find(children_.begin(), children_.end(), child) ==
             children_.end());
    } else {
      // Even if we can't delete the child, we still need to remove it from the
      // parent so that relevant bookkeeping (parent_ back-pointers etc) are
      // updated.
      RemoveChild(child);
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// Window, private:

int64 Window::SetPropertyInternal(const void* key,
                                  const char* name,
                                  PropertyDeallocator deallocator,
                                  int64 value,
                                  int64 default_value) {
  int64 old = GetPropertyInternal(key, default_value);
  if (value == default_value) {
    prop_map_.erase(key);
  } else {
    Value prop_value;
    prop_value.name = name;
    prop_value.value = value;
    prop_value.deallocator = deallocator;
    prop_map_[key] = prop_value;
  }
  FOR_EACH_OBSERVER(WindowObserver, observers_,
                    OnWindowPropertyChanged(this, key, old));
  return old;
}

int64 Window::GetPropertyInternal(const void* key,
                                  int64 default_value) const {
  std::map<const void*, Value>::const_iterator iter = prop_map_.find(key);
  if (iter == prop_map_.end())
    return default_value;
  return iter->second.value;
}

void Window::SetBoundsInternal(const gfx::Rect& new_bounds) {
  gfx::Rect actual_new_bounds(new_bounds);

  // Ensure we don't go smaller than our minimum bounds.
  if (delegate_) {
    const gfx::Size& min_size = delegate_->GetMinimumSize();
    actual_new_bounds.set_width(
        std::max(min_size.width(), actual_new_bounds.width()));
    actual_new_bounds.set_height(
        std::max(min_size.height(), actual_new_bounds.height()));
  }

  gfx::Rect old_bounds = GetTargetBounds();

  // Always need to set the layer's bounds -- even if it is to the same thing.
  // This may cause important side effects such as stopping animation.
  layer_->SetBounds(actual_new_bounds);

  // If we are currently not the layer's delegate, we will not get bounds
  // changed notification from the layer (this typically happens after animating
  // hidden). We must notify ourselves.
  if (layer_->delegate() != this)
    OnLayerBoundsChanged(old_bounds, ContainsMouse());
}

void Window::SetVisible(bool visible) {
  if (visible == layer_->GetTargetVisibility())
    return;  // No change.

  FOR_EACH_OBSERVER(WindowObserver, observers_,
                    OnWindowVisibilityChanging(this, visible));

  WindowEventDispatcher* dispatcher = GetDispatcher();
  if (dispatcher)
    dispatcher->DispatchMouseExitToHidingWindow(this);

  client::VisibilityClient* visibility_client =
      client::GetVisibilityClient(this);
  if (visibility_client)
    visibility_client->UpdateLayerVisibility(this, visible);
  else
    layer_->SetVisible(visible);
  visible_ = visible;
  SchedulePaint();
  if (parent_ && parent_->layout_manager_)
    parent_->layout_manager_->OnChildWindowVisibilityChanged(this, visible);

  if (delegate_)
    delegate_->OnWindowTargetVisibilityChanged(visible);

  NotifyWindowVisibilityChanged(this, visible);

  if (dispatcher)
    dispatcher->OnWindowVisibilityChanged(this, visible);
}

void Window::SchedulePaint() {
  SchedulePaintInRect(gfx::Rect(0, 0, bounds().width(), bounds().height()));
}

Window* Window::GetWindowForPoint(const gfx::Point& local_point,
                                  bool return_tightest,
                                  bool for_event_handling) {
  if (!IsVisible())
    return NULL;

  if ((for_event_handling && !HitTest(local_point)) ||
      (!for_event_handling && !ContainsPoint(local_point)))
    return NULL;

  // Check if I should claim this event and not pass it to my children because
  // the location is inside my hit test override area.  For details, see
  // set_hit_test_bounds_override_inner().
  if (for_event_handling && !hit_test_bounds_override_inner_.empty()) {
    gfx::Rect inset_local_bounds(gfx::Point(), bounds().size());
    inset_local_bounds.Inset(hit_test_bounds_override_inner_);
    // We know we're inside the normal local bounds, so if we're outside the
    // inset bounds we must be in the special hit test override area.
    DCHECK(HitTest(local_point));
    if (!inset_local_bounds.Contains(local_point))
      return delegate_ ? this : NULL;
  }

  if (!return_tightest && delegate_)
    return this;

  for (Windows::const_reverse_iterator it = children_.rbegin(),
           rend = children_.rend();
       it != rend; ++it) {
    Window* child = *it;

    if (for_event_handling) {
      if (child->ignore_events_)
        continue;
      // The client may not allow events to be processed by certain subtrees.
      client::EventClient* client = client::GetEventClient(GetRootWindow());
      if (client && !client->CanProcessEventsWithinSubtree(child))
        continue;
      if (delegate_ && !delegate_->ShouldDescendIntoChildForEventHandling(
              child, local_point)) {
        continue;
      }
    }

    gfx::Point point_in_child_coords(local_point);
    ConvertPointToTarget(this, child, &point_in_child_coords);
    Window* match = child->GetWindowForPoint(point_in_child_coords,
                                             return_tightest,
                                             for_event_handling);
    if (match)
      return match;
  }

  return delegate_ ? this : NULL;
}

void Window::RemoveChildImpl(Window* child, Window* new_parent) {
  if (layout_manager_)
    layout_manager_->OnWillRemoveWindowFromLayout(child);
  FOR_EACH_OBSERVER(WindowObserver, observers_, OnWillRemoveWindow(child));
  Window* root_window = child->GetRootWindow();
  Window* new_root_window = new_parent ? new_parent->GetRootWindow() : NULL;
  if (root_window && root_window != new_root_window) {
    root_window->GetDispatcher()->OnWindowRemovedFromRootWindow(
        child, new_root_window);
    child->NotifyRemovingFromRootWindow();
  }
  child->parent_ = NULL;
  // We should only remove the child's layer if the child still owns that layer.
  // Someone else may have acquired ownership of it via AcquireLayer() and may
  // expect the hierarchy to go unchanged as the Window is destroyed.
  if (child->layer_owner_)
    layer_->Remove(child->layer_);
  Windows::iterator i = std::find(children_.begin(), children_.end(), child);
  DCHECK(i != children_.end());
  children_.erase(i);
  child->OnParentChanged();
  if (layout_manager_)
    layout_manager_->OnWindowRemovedFromLayout(child);
}

void Window::OnParentChanged() {
  FOR_EACH_OBSERVER(
      WindowObserver, observers_, OnWindowParentChanged(this, parent_));
}

bool Window::GetAllTransientAncestors(Window* window,
                                      Windows* ancestors) const {
  for (; window; window = window->transient_parent()) {
    if (window->parent() == this)
      ancestors->push_back(window);
  }
  return (!ancestors->empty());
}

void Window::FindCommonSiblings(Window** window1, Window** window2) const {
  DCHECK(window1);
  DCHECK(window2);
  DCHECK(*window1);
  DCHECK(*window2);
  // Assemble chains of ancestors of both windows.
  Windows ancestors1;
  Windows ancestors2;
  if (!GetAllTransientAncestors(*window1, &ancestors1) ||
      !GetAllTransientAncestors(*window2, &ancestors2)) {
    return;
  }
  // Walk the two chains backwards and look for the first difference.
  Windows::const_reverse_iterator it1 = ancestors1.rbegin();
  Windows::const_reverse_iterator it2 = ancestors2.rbegin();
  for (; it1  != ancestors1.rend() && it2  != ancestors2.rend(); ++it1, ++it2) {
    if (*it1 != *it2) {
      *window1 = *it1;
      *window2 = *it2;
      break;
    }
  }
}

bool Window::HasTransientAncestor(const Window* ancestor) const {
  if (transient_parent_ == ancestor)
    return true;
  return transient_parent_ ?
      transient_parent_->HasTransientAncestor(ancestor) : false;
}

void Window::StackChildRelativeTo(Window* child,
                                  Window* target,
                                  StackDirection direction) {
  DCHECK_NE(child, target);
  DCHECK(child);
  DCHECK(target);
  DCHECK_EQ(this, child->parent());
  DCHECK_EQ(this, target->parent());

  // Consider all transient children of both child's and target's ancestors
  // up to the common ancestor if such exists and stack them as a unit.
  // This prevents one transient group from being inserted in the middle of
  // another.
  FindCommonSiblings(&child, &target);

  const size_t target_i =
      std::find(children_.begin(), children_.end(), target) - children_.begin();

  // When stacking above skip to the topmost transient descendant of the target.
  size_t final_target_i = target_i;
  if (direction == STACK_ABOVE && !child->HasTransientAncestor(target)) {
    while (final_target_i + 1 < children_.size() &&
           children_[final_target_i + 1]->HasTransientAncestor(target)) {
      ++final_target_i;
    }
  }

  // By convention we don't stack on top of windows with layers with NULL
  // delegates.  Walk backward to find a valid target window.
  // See tests WindowTest.StackingMadrigal and StackOverClosingTransient
  // for an explanation of this.
  while (final_target_i > 0 &&
         children_[direction == STACK_ABOVE ? final_target_i :
                                              final_target_i - 1]->layer()
             ->delegate() == NULL) {
    --final_target_i;
  }

  Window* final_target = children_[final_target_i];

  // If we couldn't find a valid target position, don't move anything.
  if (direction == STACK_ABOVE && final_target->layer()->delegate() == NULL)
    return;

  // Don't try to stack a child above itself.
  if (child == final_target)
    return;

  // Move the child.
  StackChildRelativeToImpl(child, final_target, direction);

  // Stack any transient children that share the same parent to be in front of
  // 'child'. Preserve the existing stacking order by iterating in the order
  // those children appear in children_ array.
  Window* last_transient = child;
  Windows children(children_);
  for (Windows::iterator it = children.begin(); it != children.end(); ++it) {
    Window* transient_child = *it;
    if (transient_child != last_transient &&
        transient_child->HasTransientAncestor(child)) {
      StackChildRelativeToImpl(transient_child, last_transient, STACK_ABOVE);
      last_transient = transient_child;
    }
  }
}

void Window::StackChildRelativeToImpl(Window* child,
                                      Window* target,
                                      StackDirection direction) {
  DCHECK_NE(child, target);
  DCHECK(child);
  DCHECK(target);
  DCHECK_EQ(this, child->parent());
  DCHECK_EQ(this, target->parent());

  const size_t child_i =
      std::find(children_.begin(), children_.end(), child) - children_.begin();
  const size_t target_i =
      std::find(children_.begin(), children_.end(), target) - children_.begin();

  // Don't move the child if it is already in the right place.
  if ((direction == STACK_ABOVE && child_i == target_i + 1) ||
      (direction == STACK_BELOW && child_i + 1 == target_i))
    return;

  const size_t dest_i =
      direction == STACK_ABOVE ?
      (child_i < target_i ? target_i : target_i + 1) :
      (child_i < target_i ? target_i - 1 : target_i);
  children_.erase(children_.begin() + child_i);
  children_.insert(children_.begin() + dest_i, child);

  if (direction == STACK_ABOVE)
    layer()->StackAbove(child->layer(), target->layer());
  else
    layer()->StackBelow(child->layer(), target->layer());

  child->OnStackingChanged();
}

void Window::OnStackingChanged() {
  FOR_EACH_OBSERVER(WindowObserver, observers_, OnWindowStackingChanged(this));
}

void Window::NotifyRemovingFromRootWindow() {
  FOR_EACH_OBSERVER(WindowObserver, observers_,
                    OnWindowRemovingFromRootWindow(this));
  for (Window::Windows::const_iterator it = children_.begin();
       it != children_.end(); ++it) {
    (*it)->NotifyRemovingFromRootWindow();
  }
}

void Window::NotifyAddedToRootWindow() {
  FOR_EACH_OBSERVER(WindowObserver, observers_,
                    OnWindowAddedToRootWindow(this));
  for (Window::Windows::const_iterator it = children_.begin();
       it != children_.end(); ++it) {
    (*it)->NotifyAddedToRootWindow();
  }
}

void Window::NotifyWindowHierarchyChange(
    const WindowObserver::HierarchyChangeParams& params) {
  params.target->NotifyWindowHierarchyChangeDown(params);
  switch (params.phase) {
  case WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGING:
    if (params.old_parent)
      params.old_parent->NotifyWindowHierarchyChangeUp(params);
    break;
  case WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGED:
    if (params.new_parent)
      params.new_parent->NotifyWindowHierarchyChangeUp(params);
    break;
  default:
    NOTREACHED();
    break;
  }
}

void Window::NotifyWindowHierarchyChangeDown(
    const WindowObserver::HierarchyChangeParams& params) {
  NotifyWindowHierarchyChangeAtReceiver(params);
  for (Window::Windows::const_iterator it = children_.begin();
       it != children_.end(); ++it) {
    (*it)->NotifyWindowHierarchyChangeDown(params);
  }
}

void Window::NotifyWindowHierarchyChangeUp(
    const WindowObserver::HierarchyChangeParams& params) {
  for (Window* window = this; window; window = window->parent())
    window->NotifyWindowHierarchyChangeAtReceiver(params);
}

void Window::NotifyWindowHierarchyChangeAtReceiver(
    const WindowObserver::HierarchyChangeParams& params) {
  WindowObserver::HierarchyChangeParams local_params = params;
  local_params.receiver = this;

  switch (params.phase) {
  case WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGING:
    FOR_EACH_OBSERVER(WindowObserver, observers_,
                      OnWindowHierarchyChanging(local_params));
    break;
  case WindowObserver::HierarchyChangeParams::HIERARCHY_CHANGED:
    FOR_EACH_OBSERVER(WindowObserver, observers_,
                      OnWindowHierarchyChanged(local_params));
    break;
  default:
    NOTREACHED();
    break;
  }
}

void Window::NotifyWindowVisibilityChanged(aura::Window* target,
                                           bool visible) {
  if (!NotifyWindowVisibilityChangedDown(target, visible)) {
    return; // |this| has been deleted.
  }
  NotifyWindowVisibilityChangedUp(target, visible);
}

bool Window::NotifyWindowVisibilityChangedAtReceiver(aura::Window* target,
                                                     bool visible) {
  // |this| may be deleted during a call to OnWindowVisibilityChanged() on one
  // of the observers. We create an local observer for that. In that case we
  // exit without further access to any members.
  WindowTracker tracker;
  tracker.Add(this);
  FOR_EACH_OBSERVER(WindowObserver, observers_,
                    OnWindowVisibilityChanged(target, visible));
  return tracker.Contains(this);
}

bool Window::NotifyWindowVisibilityChangedDown(aura::Window* target,
                                               bool visible) {
  if (!NotifyWindowVisibilityChangedAtReceiver(target, visible))
    return false; // |this| was deleted.
  std::set<const Window*> child_already_processed;
  bool child_destroyed = false;
  do {
    child_destroyed = false;
    for (Window::Windows::const_iterator it = children_.begin();
         it != children_.end(); ++it) {
      if (!child_already_processed.insert(*it).second)
        continue;
      if (!(*it)->NotifyWindowVisibilityChangedDown(target, visible)) {
        // |*it| was deleted, |it| is invalid and |children_| has changed.
        // We exit the current for-loop and enter a new one.
        child_destroyed = true;
        break;
      }
    }
  } while (child_destroyed);
  return true;
}

void Window::NotifyWindowVisibilityChangedUp(aura::Window* target,
                                             bool visible) {
  for (Window* window = this; window; window = window->parent()) {
    bool ret = window->NotifyWindowVisibilityChangedAtReceiver(target, visible);
    DCHECK(ret);
  }
}

void Window::OnLayerBoundsChanged(const gfx::Rect& old_bounds,
                                  bool contained_mouse) {
  if (layout_manager_)
    layout_manager_->OnWindowResized();
  if (delegate_)
    delegate_->OnBoundsChanged(old_bounds, bounds());
  FOR_EACH_OBSERVER(WindowObserver,
                    observers_,
                    OnWindowBoundsChanged(this, old_bounds, bounds()));
  WindowEventDispatcher* dispatcher = GetDispatcher();
  if (dispatcher)
    dispatcher->OnWindowBoundsChanged(this, contained_mouse);
}

void Window::OnPaintLayer(gfx::Canvas* canvas) {
  if (delegate_)
    delegate_->OnPaint(canvas);
}

base::Closure Window::PrepareForLayerBoundsChange() {
  return base::Bind(&Window::OnLayerBoundsChanged, base::Unretained(this),
                    bounds(), ContainsMouse());
}

bool Window::CanAcceptEvent(const ui::Event& event) {
  if (!IsVisible())
    return false;

  // The client may forbid certain windows from receiving events at a given
  // point in time.
  client::EventClient* client = client::GetEventClient(GetRootWindow());
  if (client && !client->CanProcessEventsWithinSubtree(this))
    return false;

  // The top-most window can always process an event.
  if (!parent_)
    return true;

  // For located events (i.e. mouse, touch etc.), an assumption is made that
  // windows that don't have a delegate cannot process the event (see more in
  // GetWindowForPoint()). This assumption is not made for key events.
  return event.IsKeyEvent() || delegate_;
}

ui::EventTarget* Window::GetParentTarget() {
  if (dispatcher_) {
    return client::GetEventClient(this) ?
        client::GetEventClient(this)->GetToplevelEventTarget() :
            Env::GetInstance();
  }
  return parent_;
}

void Window::UpdateLayerName(const std::string& name) {
#if !defined(NDEBUG)
  DCHECK(layer());

  std::string layer_name(name_);
  if (layer_name.empty())
    layer_name.append("Unnamed Window");

  if (id_ != -1) {
    char id_buf[10];
    base::snprintf(id_buf, sizeof(id_buf), " %d", id_);
    layer_name.append(id_buf);
  }
  layer()->set_name(layer_name);
#endif
}

bool Window::ContainsMouse() {
  bool contains_mouse = false;
  if (IsVisible()) {
    WindowEventDispatcher* dispatcher = GetDispatcher();
    contains_mouse = dispatcher &&
        ContainsPointInRoot(dispatcher->GetLastMouseLocationInRoot());
  }
  return contains_mouse;
}

}  // namespace aura
