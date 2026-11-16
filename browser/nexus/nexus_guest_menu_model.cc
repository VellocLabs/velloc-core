#include "custom_browser/browser/nexus/nexus_guest_menu_model.h"

#include "third_party/blink/public/common/context_menu_data/edit_flags.h"

namespace nexus {

namespace {

bool HasFlag(int edit_flags, blink::ContextMenuDataEditFlags flag) {
  return (edit_flags & flag) != 0;
}

}  // namespace

std::vector<GuestMenuItem> BuildGuestMenuItems(bool is_editable,
                                               int edit_flags,
                                               bool inspect_enabled) {
  std::vector<GuestMenuItem> items;
  auto add_separator = [&items] {
    if (!items.empty() &&
        items.back().command != GuestMenuCommand::kSeparator) {
      items.push_back({GuestMenuCommand::kSeparator, true});
    }
  };

  if (is_editable) {
    items.push_back({GuestMenuCommand::kUndo,
                     HasFlag(edit_flags, blink::kCanUndo)});
    items.push_back({GuestMenuCommand::kRedo,
                     HasFlag(edit_flags, blink::kCanRedo)});
    add_separator();
    items.push_back({GuestMenuCommand::kCut,
                     HasFlag(edit_flags, blink::kCanCut)});
    items.push_back({GuestMenuCommand::kCopy,
                     HasFlag(edit_flags, blink::kCanCopy)});
    items.push_back({GuestMenuCommand::kPaste,
                     HasFlag(edit_flags, blink::kCanPaste)});
    add_separator();
    items.push_back({GuestMenuCommand::kSelectAll,
                     HasFlag(edit_flags, blink::kCanSelectAll)});
  }

  if (inspect_enabled) {
    add_separator();
    items.push_back({GuestMenuCommand::kInspect, true});
  }
  return items;
}

}  // namespace nexus
