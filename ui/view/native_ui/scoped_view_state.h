#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_SCOPED_VIEW_STATE_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_SCOPED_VIEW_STATE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "base/memory/raw_ptr.h"
#include "ui/compositor/layer.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_tracker.h"

namespace custom_browser::native_ui {

struct ScopedViewStateOptions {
  bool ensure_layer = false;
  std::optional<bool> notify_enter_exit_on_child;
  std::optional<bool> fills_bounds_opaquely;
  std::optional<bool> masks_to_bounds;
  std::optional<bool> ignored_by_layout;
  raw_ptr<views::View> parent = nullptr;
  std::optional<size_t> parent_index;
};

// RAII helper that tracks temporary view changes and restores previous state
// when reset/destroyed.
template <typename ViewT = views::View>
class ScopedViewState {
 public:
  ScopedViewState() = default;

  ScopedViewState(const ScopedViewState&) = delete;
  ScopedViewState& operator=(const ScopedViewState&) = delete;

  ~ScopedViewState() { Reset(); }

  ScopedViewState& operator=(ViewT* view) {
    Assign(view);
    return *this;
  }

  void Assign(ViewT* view, const ScopedViewStateOptions& options = {}) {
    Reset();
    if (!view) {
      return;
    }

    tracker_.SetView(view);
    original_parent_tracker_.SetView(view->parent());
    if (view->parent()) {
      const auto& siblings = view->parent()->children();
      const auto it = std::find(siblings.begin(), siblings.end(), view);
      if (it != siblings.end()) {
        original_parent_index_ = static_cast<size_t>(it - siblings.begin());
      }
    }

    if (options.ensure_layer || options.fills_bounds_opaquely.has_value() ||
        options.masks_to_bounds.has_value()) {
      EnsureLayer();
    }
    if (options.notify_enter_exit_on_child.has_value()) {
      SetNotifyEnterExitOnChild(*options.notify_enter_exit_on_child);
    }
    if (options.fills_bounds_opaquely.has_value()) {
      SetFillsBoundsOpaquely(*options.fills_bounds_opaquely);
    }
    if (options.masks_to_bounds.has_value()) {
      SetMasksToBounds(*options.masks_to_bounds);
    }
    if (options.ignored_by_layout.has_value()) {
      SetIgnoredByLayout(*options.ignored_by_layout);
    }
    if (options.parent) {
      ReparentTo(options.parent, options.parent_index);
    }
  }

  // Backward-compatible shorthand for layer + fills-bounds-opaquely behavior.
  void Assign(ViewT* view, std::optional<bool> fills_bounds_opaquely) {
    ScopedViewStateOptions options;
    options.fills_bounds_opaquely = fills_bounds_opaquely;
    Assign(view, options);
  }

  void Reset() {
    ViewT* tracked_view = view();
    RestoreReference(tracked_view);
    if (tracked_view) {
      RestoreParent(tracked_view);
      RestoreIgnoredByLayout(tracked_view);
      RestoreNotifyEnterExitOnChild(tracked_view);
      RestoreFillsBoundsOpaquely(tracked_view);
      RestoreMasksToBounds(tracked_view);
      DestroyCreatedLayer(tracked_view);
    }

    tracker_.SetView(nullptr);
    original_parent_tracker_.SetView(nullptr);

    original_parent_index_.reset();
    reparented_ = false;
    changed_ignored_by_layout_ = false;
    previous_ignored_by_layout_ = false;
    changed_notify_enter_exit_on_child_ = false;
    previous_notify_enter_exit_on_child_ = false;
    created_layer_ = false;
    changed_fills_bounds_opaquely_ = false;
    previous_fills_bounds_opaquely_ = false;
    changed_masks_to_bounds_ = false;
    previous_masks_to_bounds_ = false;
    reference_slot_address_ = 0;
    restore_reference_fn_ = nullptr;
  }

  bool EnsureLayer() {
    views::View* tracked_view = tracker_.view();
    if (!tracked_view) {
      return false;
    }
    if (!tracked_view->layer()) {
      tracked_view->SetPaintToLayer();
      created_layer_ = true;
    }
    return tracked_view->layer() != nullptr;
  }

  bool SetFillsBoundsOpaquely(bool value) {
    views::View* tracked_view = tracker_.view();
    if (!tracked_view) {
      return false;
    }
    if (!tracked_view->layer() && !EnsureLayer()) {
      return false;
    }
    if (!changed_fills_bounds_opaquely_) {
      previous_fills_bounds_opaquely_ =
          tracked_view->layer()->fills_bounds_opaquely();
      changed_fills_bounds_opaquely_ = true;
    }
    tracked_view->layer()->SetFillsBoundsOpaquely(value);
    return true;
  }

  bool SetMasksToBounds(bool masks_to_bounds) {
    views::View* tracked_view = tracker_.view();
    if (!tracked_view) {
      return false;
    }
    if (!tracked_view->layer() && !EnsureLayer()) {
      return false;
    }
    if (!changed_masks_to_bounds_) {
      previous_masks_to_bounds_ = tracked_view->layer()->GetMasksToBounds();
      changed_masks_to_bounds_ = true;
    }
    tracked_view->layer()->SetMasksToBounds(masks_to_bounds);
    return true;
  }

  bool SetNotifyEnterExitOnChild(bool notify) {
    views::View* tracked_view = tracker_.view();
    if (!tracked_view) {
      return false;
    }
    if (!changed_notify_enter_exit_on_child_) {
      previous_notify_enter_exit_on_child_ =
          tracked_view->GetNotifyEnterExitOnChild();
      changed_notify_enter_exit_on_child_ = true;
    }
    tracked_view->SetNotifyEnterExitOnChild(notify);
    return true;
  }

  bool SetIgnoredByLayout(bool ignored) {
    views::View* tracked_view = tracker_.view();
    if (!tracked_view) {
      return false;
    }
    if (!changed_ignored_by_layout_) {
      previous_ignored_by_layout_ =
          tracked_view->GetProperty(views::kViewIgnoredByLayoutKey);
      changed_ignored_by_layout_ = true;
    }
    tracked_view->SetProperty(views::kViewIgnoredByLayoutKey, ignored);
    return true;
  }

  bool ReparentTo(views::View* new_parent) {
    return ReparentTo(new_parent, std::nullopt);
  }

  bool SetParent(views::View* new_parent) {
    return ReparentTo(new_parent);
  }

  bool SetParent(views::View* new_parent, size_t index) {
    return ReparentTo(new_parent, index);
  }

  bool ReparentTo(views::View* new_parent, std::optional<size_t> index) {
    views::View* tracked_view = tracker_.view();
    if (!tracked_view || !new_parent) {
      return false;
    }

    reparented_ = true;

    if (tracked_view->parent() == new_parent) {
      if (index.has_value()) {
        const size_t max_index = new_parent->children().empty()
                                     ? 0
                                     : new_parent->children().size() - 1;
        new_parent->ReorderChildView(tracked_view, std::min(*index, max_index));
      }
      return true;
    }

    if (index.has_value()) {
      new_parent->AddChildViewAt(tracked_view,
                                 std::min(*index, new_parent->children().size()));
    } else {
      new_parent->AddChildView(tracked_view);
    }
    return true;
  }

  template <typename ReferenceSlotT>
  bool TemporarilyClearReference(ReferenceSlotT* reference_slot) {
    static_assert(std::is_assignable_v<ReferenceSlotT&, ViewT*>,
                  "Reference slot must be assignable from ViewT*.");

    ViewT* tracked_view = view();
    if (!tracked_view || !reference_slot || restore_reference_fn_) {
      return false;
    }

    ViewT* current_reference = *reference_slot;
    DCHECK(current_reference == tracked_view);
    if (current_reference != tracked_view) {
      return false;
    }

    reference_slot_address_ = reinterpret_cast<uintptr_t>(reference_slot);
    restore_reference_fn_ = &RestoreReferenceSlot<ReferenceSlotT>;
    *reference_slot = nullptr;
    return true;
  }

  void RestoreReference(ViewT* tracked_view) {
    if (!reference_slot_address_ || !restore_reference_fn_) {
      return;
    }

    restore_reference_fn_(reference_slot_address_, tracked_view);
    reference_slot_address_ = 0;
    restore_reference_fn_ = nullptr;
  }

  ViewT* view() { return static_cast<ViewT*>(tracker_.view()); }
  const ViewT* view() const {
    return static_cast<const ViewT*>(tracker_.view());
  }

  ViewT* operator->() { return view(); }
  const ViewT* operator->() const { return view(); }

  explicit operator bool() const { return view() != nullptr; }

 private:
  template <typename ReferenceSlotT>
  static void RestoreReferenceSlot(uintptr_t slot_address, ViewT* tracked_view) {
    *reinterpret_cast<ReferenceSlotT*>(slot_address) = tracked_view;
  }

  void RestoreParent(views::View* tracked_view) {
    if (!reparented_) {
      return;
    }

    views::View* original_parent = original_parent_tracker_.view();
    if (!original_parent || tracked_view->parent() == original_parent) {
      return;
    }

    if (original_parent_index_.has_value()) {
      original_parent->AddChildViewAt(
          tracked_view,
          std::min(*original_parent_index_, original_parent->children().size()));
    } else {
      original_parent->AddChildView(tracked_view);
    }
  }

  void RestoreIgnoredByLayout(views::View* tracked_view) {
    if (!changed_ignored_by_layout_) {
      return;
    }
    tracked_view->SetProperty(views::kViewIgnoredByLayoutKey,
                              previous_ignored_by_layout_);
  }

  void RestoreNotifyEnterExitOnChild(views::View* tracked_view) {
    if (!changed_notify_enter_exit_on_child_) {
      return;
    }
    tracked_view->SetNotifyEnterExitOnChild(
        previous_notify_enter_exit_on_child_);
  }

  void RestoreFillsBoundsOpaquely(views::View* tracked_view) {
    if (!changed_fills_bounds_opaquely_ || !tracked_view->layer()) {
      return;
    }
    tracked_view->layer()->SetFillsBoundsOpaquely(
        previous_fills_bounds_opaquely_);
  }

  void RestoreMasksToBounds(views::View* tracked_view) {
    if (!changed_masks_to_bounds_ || !tracked_view->layer()) {
      return;
    }
    tracked_view->layer()->SetMasksToBounds(previous_masks_to_bounds_);
  }

  void DestroyCreatedLayer(views::View* tracked_view) {
    if (!created_layer_ || !tracked_view->layer()) {
      return;
    }
    tracked_view->DestroyLayer();
  }

  views::ViewTracker tracker_;
  views::ViewTracker original_parent_tracker_;

  using RestoreReferenceFn = void (*)(uintptr_t, ViewT*);
  uintptr_t reference_slot_address_ = 0;
  RestoreReferenceFn restore_reference_fn_ = nullptr;

  std::optional<size_t> original_parent_index_;
  bool reparented_ = false;

  bool changed_ignored_by_layout_ = false;
  bool previous_ignored_by_layout_ = false;

  bool changed_notify_enter_exit_on_child_ = false;
  bool previous_notify_enter_exit_on_child_ = false;

  bool created_layer_ = false;
  bool changed_fills_bounds_opaquely_ = false;
  bool previous_fills_bounds_opaquely_ = false;
  bool changed_masks_to_bounds_ = false;
  bool previous_masks_to_bounds_ = false;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_SCOPED_VIEW_STATE_H_
