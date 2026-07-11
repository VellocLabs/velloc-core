"""One-shot helper: strip translation entries from .xtb files for IDS_*
that had their source string structurally changed (placeholder added or
text materially modified). The English source falls back automatically
once the entries are gone.

Run after `custom_browser_strings.grd` has been edited to add or remove
<ph> placeholders. After running, re-run `gn gen` / build.

This file is a developer utility, not a build step. Safe to delete.
"""

import re
import pathlib

# IDs whose source strings were changed in the centralization refactor
# (custom_browser/common/branding_constants.h). Translations of these IDs
# no longer match the new source structure and must be removed so GRIT
# falls back to the English source until re-translated.
STALE_IDS = {
    "IDS_CUSTOM_BROWSER_MODE_NEXUS",
    "IDS_CUSTOM_BROWSER_MODE_NEXUS_DESCRIPTION",
    "IDS_NEXUS_PANEL_TOOLTIP",
    "IDS_NEXUS_PANEL_ACCESSIBLE_NAME",
    "IDS_NEXUS_CONSENT_TITLE",
    "IDS_NEXUS_CONSENT_BODY",
    "IDS_NEXUS_CONSENT_ACCEPT",
    "IDS_NEXUS_WARNING_INTRO",
    "IDS_NEXUS_WARNING_BODY_1",
    "IDS_NEXUS_WARNING_ACKNOWLEDGMENT",
    "IDS_NEXUS_SETTINGS_TITLE",
    "IDS_NEXUS_SETTINGS_WEBAPP_URL_LABEL",
    "IDS_NEXUS_SETTINGS_WEBAPP_URL_PLACEHOLDER",
    "IDS_NEXUS_SETTINGS_SECTION_DESCRIPTION_LINE_1",
    "IDS_NEXUS_SETTINGS_PROVIDER_DESCRIPTION",
    "IDS_NEXUS_SETTINGS_PROVIDER_ROW_SUBLABEL",
    "IDS_NEXUS_SETTINGS_PROVIDER_CONNECTION_DESCRIPTION",
    "IDS_NEXUS_SETTINGS_WEBAPP_ROW_SUBLABEL",
    "IDS_NEXUS_SETTINGS_WEBAPP_DESCRIPTION",
    "IDS_NEXUS_SETTINGS_OLLAMA_NOTE",
    "IDS_NEXUS_SETTINGS_NO_PROVIDER_STATE",
    "IDS_NEXUS_SETTINGS_WEBAPP_DEFAULT_DESCRIPTION",
}

XTB_DIR = pathlib.Path(__file__).resolve().parent.parent / "resources"
XTB_FILES = sorted(XTB_DIR.glob("custom_browser_strings_*.xtb"))


def strip_file(path: pathlib.Path) -> int:
    text = path.read_text(encoding="utf-8")
    removed = 0
    for ids in STALE_IDS:
        # Match a complete <translation id="IDS_..."> ... </translation>
        # element followed by its trailing newline. DOTALL handles
        # multi-line bodies (e.g. CONSENT_BODY).
        pattern = re.compile(
            r'^[ \t]*<translation id="' + re.escape(ids) + r'">.*?</translation>\r?\n',
            re.MULTILINE | re.DOTALL,
        )
        new_text, n = pattern.subn("", text)
        if n:
            text = new_text
            removed += n
    if removed:
        path.write_text(text, encoding="utf-8")
    return removed


def main() -> None:
    if not XTB_FILES:
        print(f"No .xtb files found under {XTB_DIR}")
        return
    for path in XTB_FILES:
        n = strip_file(path)
        print(f"{path.name}: removed {n} stale translation entries")


if __name__ == "__main__":
    main()
