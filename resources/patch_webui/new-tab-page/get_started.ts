import '//resources/cr_elements/cr_icon_button/cr_icon_button.js';
import '/strings.m.js';
import {loadTimeData} from '//resources/js/load_time_data.js';

function getString(key: string): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : '';
}

function closePage() {
  window.location.href = 'chrome://new-tab-page/';
}

const closeButton = document.getElementById('closeButton');
closeButton?.addEventListener('click', closePage);

window.addEventListener('keydown', (event: KeyboardEvent) => {
  if (event.key === 'Escape') {
    event.preventDefault();
    closePage();
  }
});

window.addEventListener('DOMContentLoaded', () => {
  const pageTitle = getString('customNtpGetStartedPageTitle');
  document.title = pageTitle;

  const content = document.getElementById('content');
  if (content) {
    content.textContent = getString('customNtpGetStartedPageBody');
  }

  const closeLabel = getString('customNtpGetStartedCloseLabel');
  if (closeButton) {
    closeButton.setAttribute('aria-label', closeLabel);
    closeButton.title = closeLabel;
    closeButton.focus();
  }
});
