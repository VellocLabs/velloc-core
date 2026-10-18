#include "custom_browser/ui/view/nexus_native_menus.h"

#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_commands.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/menus/simple_menu_model.h"

namespace custom_browser {

bool BuildNexusSystemMenu(ui::SimpleMenuModel* model, Browser* browser) {
  if (!window_util::IsNexusBrowser(browser) &&
      !window_util::IsSingleWebBrowser(browser)) {
    return false;
  }

  // Purpose-built menu for the tabless agent window and the one-tab
  // single-web window. All commands are existing IDC_* ids that the
  // SystemMenuModelDelegate routes; AddItem(id, label) only sets the label.
  //
  // "Open browser window" → IDC_NEW_WINDOW. From the kNexus window this opens
  // a NORMAL (non-kNexus) window: the profile's startup default is already
  // consumed, so CustomBrowserView::InitializeCustomBrowser resolves
  // want_nexus=false.
  model->AddItem(IDC_NEW_WINDOW,
                 l10n_util::GetStringUTF16(IDS_NEXUS_SYSTEM_MENU_OPEN_BROWSER));

  if (chrome::CanOpenTaskManager()) {
    model->AddSeparator(ui::NORMAL_SEPARATOR);
    model->AddItem(
        IDC_TASK_MANAGER_CONTEXT_MENU,
        l10n_util::GetStringUTF16(IDS_NEXUS_SYSTEM_MENU_TASK_MANAGER));
  }

  model->AddSeparator(ui::NORMAL_SEPARATOR);
  model->AddItem(IDC_CLOSE_WINDOW,
                 l10n_util::GetStringUTF16(IDS_NEXUS_SYSTEM_MENU_CLOSE_WINDOW));
  return true;
}

}  // namespace custom_browser
