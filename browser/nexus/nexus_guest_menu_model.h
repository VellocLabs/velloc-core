#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_MENU_MODEL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_MENU_MODEL_H_

#include <vector>

namespace nexus {

// Items of the Nexus guest's native context menu. Values double as the
// menu command ids, so they must stay non-zero and unique.
enum class GuestMenuCommand {
  kSeparator = -1,
  kUndo = 1,
  kRedo,
  kCut,
  kCopy,
  kPaste,
  kSelectAll,
  kInspect,
};

struct GuestMenuItem {
  GuestMenuCommand command;
  bool enabled;

  bool operator==(const GuestMenuItem&) const = default;
};

// Contract: returns the menu for one right-click in the guest, in display
// order. `is_editable` / `edit_flags` are the renderer's ContextMenuParams
// fields (blink::ContextMenuDataEditFlags bits). An editable target gets
// Undo, Redo | Cut, Copy, Paste | Select All, each enabled by its flag;
// `inspect_enabled` appends Inspect. An empty result means show no menu.
// Separators never lead, trail or repeat. Pure; any thread.
std::vector<GuestMenuItem> BuildGuestMenuItems(bool is_editable,
                                               int edit_flags,
                                               bool inspect_enabled);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_GUEST_MENU_MODEL_H_
