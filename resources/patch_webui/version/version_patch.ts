import '/strings.m.js';
import {loadTimeData} from '//resources/js/load_time_data.js';

const VERSION_LINE_KEY = 'customBrowserVersionLine';
const CHROMIUM_LINE_KEY = 'customBrowserBasedOnChromiumLine';

function getString(key: string): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : '';
}

function getStringF(key: string, ...args: Array<string>): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getStringF(key, ...args) :
                                        '';
}

function buildVersionLine(): string {
  const productVersion = getString('customBrowserProductVersion');
  const official = getString('official');
  const modifier = getString('version_modifier');
  return getStringF(VERSION_LINE_KEY, productVersion, official, modifier);
}

function buildChromiumLine(): string {
  const chromiumVersion = getString('customBrowserChromiumVersion');
  return getStringF(CHROMIUM_LINE_KEY, chromiumVersion);
}

function applyVersionLines() {
  const copyContent = document.getElementById('copy-content');
  if (!copyContent) {
    return;
  }
  if (copyContent.dataset['customBrowserVersionPatched'] === 'true') {
    return;
  }
  copyContent.dataset['customBrowserVersionPatched'] = 'true';

  while (copyContent.firstChild) {
    copyContent.removeChild(copyContent.firstChild);
  }

  const firstLine = document.createElement('span');
  firstLine.textContent = buildVersionLine();

  const secondLine = document.createElement('span');
  secondLine.textContent = buildChromiumLine();

  copyContent.append(firstLine, document.createElement('br'), secondLine);
}

document.addEventListener('DOMContentLoaded', applyVersionLines);
