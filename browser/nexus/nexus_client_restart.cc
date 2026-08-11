#include "custom_browser/browser/nexus/nexus_client_restart.h"

#include "base/functional/bind.h"
#include "base/location.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "content/public/browser/browser_thread.h"

namespace nexus {

void RestartClient() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&chrome::AttemptRestart));
}

}  // namespace nexus
