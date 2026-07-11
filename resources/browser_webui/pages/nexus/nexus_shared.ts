import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {css, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {BoolKey} from '../../browser_constants.mojom-webui.js';

export interface ModelInfo {
  contextWindow?: number;
  id: string;
  name: string;
  supportsTools?: boolean;
  supportsVision?: boolean;
}

export interface VerificationMetadata {
  baseUrl?: string;
  modelCount?: number;
  providerId?: string;
  verifiedAtMs?: number;
}

export interface ProviderInfo {
  baseUrl?: string;
  defaultModel?: string;
  hasStoredApiKey?: boolean;
  id: string;
  isConfigured: boolean;
  lastVerification?: VerificationMetadata;
  model?: string;
  models: ModelInfo[];
  name: string;
  type: 'cloud'|'local'|'proxy';
}

export interface ProviderResult {
  activeProviderId?: string;
  providers?: ProviderInfo[];
}

export interface ProviderConfigUpdates {
  api_key?: string;
  base_url?: string;
  default_model?: string;
  last_verification?: VerificationMetadata;
}

export interface UpdateProviderConfigResult {
  success?: boolean;
}

export interface ResetProviderConfigResult {
  activeProviderId?: string;
  success?: boolean;
}

export interface VerifyProviderConfigResult {
  baseUrl?: string;
  errorCode?: string;
  modelCount?: number;
  models?: ModelInfo[];
  success?: boolean;
  verifiedAtMs?: number;
}

export type NexusPaneState = 'warning'|'settings'|'nexus-view';
export type NexusSettingsSection =
    'models'|'mcp'|'permissions'|'tools'|'shortcuts'|'about';
export type SettingsStatus = 'idle'|'draft'|'verifying'|'ready'|'error';
export type StatusTone = 'error'|'info'|'success'|'warning';

export const DEFAULT_BASE_URLS: Record<string, string> = {
  anthropic: 'https://api.anthropic.com',
  glm: 'https://open.bigmodel.cn/api/paas/v4',
  google: 'https://generativelanguage.googleapis.com/v1beta',
  kimi: 'https://api.moonshot.cn/v1',
  minimax: 'https://api.minimax.io/v1',
  ollama: 'http://localhost:11434',
  openai: 'https://api.openai.com/v1',
  qwen: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
};
export const WEBAPP_URL_PREF = 'nexus.webapp_url';

export const PROVIDER_NAME_KEYS: Record<string, string> = {
  anthropic: 'customBrowserNexusProviderAnthropic',
  glm: 'customBrowserNexusProviderGlm',
  google: 'customBrowserNexusProviderGoogle',
  kimi: 'customBrowserNexusProviderKimi',
  minimax: 'customBrowserNexusProviderMinimax',
  ollama: 'customBrowserNexusProviderOllama',
  openai: 'customBrowserNexusProviderOpenai',
  qwen: 'customBrowserNexusProviderQwen',
};

export const NEXUS_WARNING_KEY = BoolKey[BoolKey.kNexusWarningAcknowledged];

export function getString(key: string, fallback = ''): string {
  try {
    return loadTimeData.valueExists(key) ? loadTimeData.getString(key) :
                                           fallback;
  } catch {
    return fallback;
  }
}

export function getStringF(
    key: string, fallback: string,
    ...replacements: Array<number|string>): string {
  const values = replacements.map(String);
  const fallbackValue = fallback.replace(/\$(\d+)/g, (_, rawIndex: string) => {
    return values[Number(rawIndex) - 1] ?? '';
  });

  try {
    return loadTimeData.valueExists(key) ? loadTimeData.getStringF(key, ...values) :
                                           fallbackValue;
  } catch {
    return fallbackValue;
  }
}

export function getInitialWarningAcknowledged(): boolean {
  if (!loadTimeData.valueExists(NEXUS_WARNING_KEY)) {
    return false;
  }
  return loadTimeData.getBoolean(NEXUS_WARNING_KEY);
}

export function getProviderDisplayName(provider: ProviderInfo): string {
  const key = PROVIDER_NAME_KEYS[provider.id];
  return key ? getString(key, provider.name) : provider.name;
}

export function getBaseUrlValidationMessage(url: string): string {
  const trimmed = url.trim();
  if (!trimmed) {
    return '';
  }

  try {
    const parsed = new URL(trimmed);
    return parsed.protocol === 'http:' || parsed.protocol === 'https:' ? '' :
        getString(
            'customBrowserNexusErrorInvalidBaseUrl',
            'Invalid Base URL - enter a valid API endpoint');
  } catch {
    return getString(
        'customBrowserNexusErrorInvalidBaseUrl',
        'Invalid Base URL - enter a valid API endpoint');
  }
}

export function hasDefaultBaseUrl(providerId: string): boolean {
  return Object.prototype.hasOwnProperty.call(DEFAULT_BASE_URLS, providerId);
}

export function getWebappUrlValidationMessage(url: string): string {
  const trimmed = url.trim();
  if (!trimmed) {
    return '';
  }

  try {
    const parsed = new URL(trimmed);
    if (parsed.protocol === 'https:') {
      return '';
    }
    if (parsed.protocol === 'http:' &&
        (parsed.hostname === 'localhost' || parsed.hostname === '127.0.0.1')) {
      return '';
    }
    return getString(
        'customBrowserNexusInvalidWebappUrl',
        'Use HTTPS, or HTTP only for localhost.');
  } catch {
    return getString(
        'customBrowserNexusInvalidWebappUrlFormat',
        'Enter a valid web app URL.');
  }
}

export function isValidWebappUrl(url: string): boolean {
  return getWebappUrlValidationMessage(url) === '';
}

// --- Model selection history (localStorage) ---

const MODEL_HISTORY_KEY = 'nexus.model_selection_history';
const MODEL_HISTORY_MAX = 10;

export interface ModelHistoryEntry {
  modelId: string;
  modelName: string;
  providerId: string;
  providerName: string;
}

export function loadModelHistory(): ModelHistoryEntry[] {
  try {
    const raw = localStorage.getItem(MODEL_HISTORY_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

export function saveModelHistory(entries: ModelHistoryEntry[]) {
  try {
    localStorage.setItem(
        MODEL_HISTORY_KEY, JSON.stringify(entries.slice(0, MODEL_HISTORY_MAX)));
  } catch {
    // localStorage may be unavailable (e.g. in some test harnesses).
  }
}

export function pushModelHistory(entry: ModelHistoryEntry): ModelHistoryEntry[] {
  const history = loadModelHistory().filter(
      e => !(e.providerId === entry.providerId && e.modelId === entry.modelId));
  history.unshift(entry);
  const trimmed = history.slice(0, MODEL_HISTORY_MAX);
  saveModelHistory(trimmed);
  return trimmed;
}

export const paneStyles: CSSResultGroup = css`
  :host {
    color: var(--cr-primary-text-color, var(--text-primary));
    display: flex;
    flex: 1 1 auto;
    height: 100%;
    min-height: 0;
    -webkit-app-region: no-drag;
  }

  .panel-shell {
    box-sizing: border-box;
    flex: 1 1 auto;
    min-height: 0;
    padding: 0 0 24px;
  }

  .panel-card {
    background: var(--cr-card-background-color, var(--app-surface));
    border-radius: var(--cr-card-border-radius, 12px);
    box-shadow: var(--cr-card-shadow, 0 1px 2px rgba(60, 64, 67, 0.15));
    box-sizing: border-box;
    color: var(--cr-primary-text-color, var(--text-primary));
    overflow: hidden;
    padding: 24px;
  }

  .panel-title {
    color: var(--cr-primary-text-color, var(--text-primary));
    font-size: 108%;
    font-weight: 400;
    letter-spacing: 0.25px;
    margin: 0 0 12px;
    padding-top: 8px;
  }

  .panel-description,
  .panel-copy {
    color: var(--cr-secondary-text-color, var(--text-secondary));
    line-height: 20px;
    margin: 0;
  }

  .panel-copy {
    margin-top: 12px;
  }

  .settings-page-section + .settings-page-section {
    margin-top: 24px;
  }

  .settings-page-section-header {
    margin: 0 0 12px;
    padding-inline-end: var(--cr-section-padding, 24px);
  }

  .settings-page-section-title {
    color: var(--cr-primary-text-color, var(--text-primary));
    font-size: 108%;
    font-weight: 400;
    letter-spacing: 0.25px;
    margin: 0;
    padding-bottom: 4px;
    padding-top: 8px;
  }

  .settings-page-section-description {
    color: var(--cr-secondary-text-color, var(--text-secondary));
    line-height: 20px;
    margin: 0;
  }

  .section-card {
    background: var(--cr-card-background-color, var(--app-surface));
    border-radius: var(--cr-card-border-radius, 12px);
    box-shadow: var(--cr-card-shadow, 0 1px 2px rgba(60, 64, 67, 0.15));
    overflow: hidden;
  }

  .card-row {
    align-items: center;
    box-sizing: border-box;
    display: flex;
    gap: 16px;
    min-height: var(--cr-section-min-height, 56px);
    padding: 0 var(--cr-section-padding, 24px);
  }

  .card-row + .card-row {
    border-top: var(
        --cr-separator-line,
        1px solid var(--cr-separator-color, rgba(0, 0, 0, 0.12)));
  }

  .card-row.block {
    align-items: stretch;
    display: block;
    min-height: 0;
    padding-block: var(--cr-section-vertical-padding, 18px);
  }

  .card-row.start-aligned {
    align-items: flex-start;
  }

  .card-row-content {
    flex: 1 1 auto;
    min-width: 0;
    padding-block: var(--cr-section-vertical-padding, 18px);
  }

  .card-row-title {
    color: var(--cr-primary-text-color, var(--text-primary));
    font-weight: 500;
    margin: 0;
  }

  .card-row-description {
    color: var(--cr-secondary-text-color, var(--text-secondary));
    line-height: 20px;
    margin: 4px 0 0;
  }

  .card-row-action {
    flex: none;
    margin-inline-start: 16px;
  }

  .section + .section {
    border-top: var(--cr-separator-line, 1px solid var(--cr-separator-color));
    margin-top: 24px;
    padding-top: 24px;
  }

  .section-title {
    color: var(--cr-primary-text-color, var(--text-primary));
    font-size: 0.95rem;
    font-weight: 500;
    margin: 0 0 12px;
  }

  .field + .field {
    margin-top: 16px;
  }

  .field-label {
    color: var(--cr-primary-text-color, var(--text-primary));
    display: block;
    font-weight: 500;
    margin-bottom: 8px;
  }

  .field-label.with-description {
    margin-bottom: 0;
  }

  .field-description {
    margin: 4px 0 12px;
  }

  .field-control {
    background: var(
        --md-background-color,
        var(--cr-card-background-color, var(--app-surface)));
    border: 1px solid var(--cr-separator-color);
    border-radius: 12px;
    box-sizing: border-box;
    color: var(--cr-primary-text-color, var(--text-primary));
    font: inherit;
    min-height: 48px;
    padding: 12px 14px;
    width: 100%;
  }

  .field-control:focus {
    border-color: var(--cr-focus-outline-color);
    box-shadow: 0 0 0 1px var(--cr-focus-outline-color);
    outline: none;
  }

  select.field-control {
    appearance: none;
    background-image:
        linear-gradient(45deg, transparent 50%, currentColor 50%),
        linear-gradient(135deg, currentColor 50%, transparent 50%);
    background-position:
        calc(100% - 18px) calc(50% - 3px),
        calc(100% - 12px) calc(50% - 3px);
    background-repeat: no-repeat;
    background-size: 6px 6px;
    padding-inline-end: 34px;
  }

  .helper-text,
  .provider-note,
  .empty-state {
    color: var(--cr-secondary-text-color, var(--text-secondary));
    font-size: 0.70rem;
    line-height: 20px;
    margin: 8px 0 0;
  }

  .helper-text.error {
    color: var(--cr-error-color, var(--google-red-700));
  }

  .status-card {
    background: var(
        --md-background-color,
        var(--cr-card-background-color, var(--app-surface)));
    border: 1px solid var(--cr-separator-color);
    border-radius: 12px;
    padding: 16px;
  }

  .status-header {
    align-items: flex-start;
    display: flex;
    gap: 12px;
  }

  .status-chip {
    border-radius: 999px;
    display: inline-flex;
    flex: none;
    font-size: 0.78rem;
    font-weight: 500;
    padding: 4px 10px;
    white-space: nowrap;
  }

  .status-chip.info {
    background: var(--google-blue-50);
    color: var(--google-blue-900);
  }

  .status-chip.success {
    background: var(--google-green-50);
    color: var(--google-green-900);
  }

  .status-chip.warning {
    background: var(--google-yellow-50);
    color: var(--google-yellow-900);
  }

  .status-chip.error {
    background: var(--google-red-50);
    color: var(--google-red-900);
  }

  .status-copy {
    min-width: 0;
  }

  .status-title {
    color: var(--cr-primary-text-color, var(--text-primary));
    font-weight: 500;
  }

  .status-detail {
    color: var(--cr-secondary-text-color, var(--text-secondary));
    line-height: 20px;
    margin-top: 4px;
  }

  .status-progress {
    animation: nexus-progress 1.2s linear infinite;
    background: linear-gradient(
        90deg,
        var(--google-blue-600),
        var(--google-blue-300),
        var(--google-blue-600));
    background-size: 200% 100%;
    border-radius: 999px;
    height: 4px;
    margin-top: 12px;
    width: 100%;
  }

  .button-row {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    justify-content: flex-end;
    padding: 16px var(--cr-section-padding, 24px);
  }

  .section-card .button-row {
    border-top: var(
        --cr-separator-line,
        1px solid var(--cr-separator-color, rgba(0, 0, 0, 0.12)));
  }

  .button-row cr-button {
    margin-inline-start: 0;
  }

  @media (forced-colors: active) {
    .panel-card,
    .section-card {
      border: var(--cr-border-hcm);
      box-shadow: none;
    }
  }

  @keyframes nexus-progress {
    0% {
      background-position: 200% 0;
    }

    100% {
      background-position: 0 0;
    }
  }

  @media (max-width: 900px) {
    .panel-shell {
      padding-bottom: 16px;
    }

    .panel-card {
      padding: 20px;
    }
  }
`;
