#include "custom_browser/ui/view/native_ui/pinned_toolbar_actions_container_native_ui.h"

#include <iterator>

#include "base/check.h"
#include "base/containers/span.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/native_ui_factory_registry.h"
#include "ui/actions/action_id.h"
#include "ui/actions/actions.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/layout/animating_layout_manager.h"
#include "ui/views/view.h"

namespace custom_browser::native_ui {

namespace {

const base::flat_map<std::string_view,
                     std::variant<ui::ElementIdentifier, actions::ActionId>>&
GetActionClassNameToIdentifierMap() {
  static const base::NoDestructor<base::flat_map<
      std::string_view, std::variant<ui::ElementIdentifier, actions::ActionId>>>
      action_class_name_to_identifier_map({
          {"PinnedAvatarButton", kToolbarAvatarButtonElementId},
          // {"ChromeLabsButton", kToolbarChromeLabsButtonElementId}, TODO
          {"PinnedExtensionsMenuButton", kExtensionsMenuButtonElementId},
          {"PinnedClearBrowsingDataButton", kActionClearBrowsingData},
          {"PinnedCopyLinkButton", kActionCopyUrl},
          {"PinnedDeveloperToolsButton", kActionDevTools},
          {"PinnedNewIncognitoWindowButton", kActionNewIncognitoWindow},
          {"PinnedPrintButton", kActionPrint},
          {"PinnedQrCodeGeneratorButton", kActionQrCodeGenerator},
          {"PinnedCastButton", kActionRouteMedia},
          {"PinnedSendTabToSelfButton", kActionSendTabToSelf},
          {"PinnedShowAddressesBubbleOrPageButton",
           kActionShowAddressesBubbleOrPage},
          {"PinnedShowChromeLabsButton", kActionShowChromeLabs},
          {"PinnedShowDownloadsButton", kActionShowDownloads},
          {"PinnedShowPasswordsBubbleOrPageButton",
           kActionShowPasswordsBubbleOrPage},
          {"PinnedShowPaymentsBubbleOrPageButton",
           kActionShowPaymentsBubbleOrPage},
          {"PinnedShowTranslateButton", kActionShowTranslate},
          {"PinnedShowBookmarkSidePanelButton", kActionSidePanelShowBookmarks},
          {"PinnedShowReadAnythingSidePanelButton",
           kActionSidePanelShowReadAnything},
          {"PinnedShowHistorySidePanelButton",
           kActionSidePanelShowHistoryCluster},
          {"PinnedShowReadingListSidePanelButton",
           kActionSidePanelShowReadingList},
          {"PinnedShowSearchCompanionSidePanelButton",
           kActionSidePanelShowSearchCompanion},
          {"PinnedTaskManagerButton", kActionTaskManager},
          {"PinnedShowLensOverlayResultsSidePanelButton",
           kActionSidePanelShowLensOverlayResults},
          {"SharedTabGroupFeedbackButton", kActionSendSharedTabGroupFeedback},
          {"PinnedTabSearchButton", kActionTabSearch},
      });
  return *action_class_name_to_identifier_map;
}

std::vector<actions::ActionId> GetAllPinnedActionIds() {
  const auto& action_class_name_to_identifier_map =
      GetActionClassNameToIdentifierMap();
  std::vector<actions::ActionId> action_ids;
  for (const auto& [class_name, id] : action_class_name_to_identifier_map) {
    if (std::holds_alternative<actions::ActionId>(id)) {
      action_ids.push_back(std::get<actions::ActionId>(id));
    }
  }
  return action_ids;
}
}  // namespace

PinnedToolbarActionsContainerNativeUI::PinnedToolbarActionsContainerNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* layout_params,
    views::View* view)
    : ViewNativeUI(host_view, layout_params, view) {}

PinnedToolbarActionsContainerNativeUI::
    ~PinnedToolbarActionsContainerNativeUI() {}

std::optional<actions::ActionId>
PinnedToolbarActionsContainerNativeUI::GetActionIdFromClassName(
    const std::string& class_name) {
  if (DCHECK_IS_ON()) {
    for (const auto& [entry_class_name, id] :
         GetActionClassNameToIdentifierMap()) {
      auto mapped_action_name =
          ToolbarController::GetActionNameFromElementIdentifier(id);
      auto class_name_without_pinned_prefix =
          entry_class_name.substr(strlen("Pinned"));
      DCHECK(mapped_action_name.find(class_name_without_pinned_prefix) !=
             std::string::npos);
    }
  }

  auto it = GetActionClassNameToIdentifierMap().find(class_name);
  if (it != GetActionClassNameToIdentifierMap().end()) {
    if (std::holds_alternative<actions::ActionId>(it->second)) {
      return std::get<actions::ActionId>(it->second);
    }
  }
  return std::nullopt;
}

void PinnedToolbarActionsContainerNativeUI::OnPrepare() {
  ViewNativeUI::OnPrepare();
  auto* actions_container = static_cast<PinnedToolbarActionsContainer*>(view());
  views::AnimatingLayoutManager* layout_manager =
      static_cast<views::AnimatingLayoutManager*>(
          actions_container->GetLayoutManager());
  layout_manager->SetBoundsAnimationMode(
      views::AnimatingLayoutManager::BoundsAnimationMode::kUseHostBounds);
  auto all_pinned_action_ids = GetAllPinnedActionIds();
  for (const auto& action_id : all_pinned_action_ids) {
    // Pin the action without announcing it.
    if (actions_container->GetActionItemFor(action_id)) {
      bool shown_in_toolbar = action_id != kActionShowDownloads &&
                              action_id != kActionTabSearch;
      actions_container->UpdatePinnedStateAndAnnounce(
          action_id, shown_in_toolbar);
    }
  }
}
}  // namespace custom_browser::native_ui
