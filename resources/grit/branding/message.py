
from branding.replacer import Replacer
from config.config import get_branding_config_path, get_branding_name
import time
import sys

_REPLACER = None

def _get_replacer():
    global _REPLACER
    if _REPLACER is None:
        _REPLACER = Replacer(get_branding_config_path())
    return _REPLACER

# ── 3.  GRIT entry point -------------------------------------------------
def PatchTranslateNodeMessage(self, lang: str, gender: str, result: str) -> str:  # noqa: N802
    """
    GRIT calls this hook for every <message> node.

    Args
    ----
    self   : the GRIT MessageNode (kept for API parity; unused here)
    lang   : target language code, e.g. "zh-CN"; may be empty/None
    gender : gender flag (not used yet, but kept for future logic)
    result : the raw string extracted from the .grd file

    Returns
    -------
    str : the rewritten string after applying configurable replacements
    """
    lang = lang or ""                    # fallback if None
    # start_ts = time.perf_counter()

    replacer = _get_replacer()
    try:
        new_text = replacer.replace(result, lang)
    except Exception as exc:             # pylint: disable=broad-except
        # Never break the whole build because of one bad rule.
        print(f"[grit-hook][WARN] replacer error → keep original: {exc!r}",
              file=sys.stderr)
        new_text = result

    # elapsed_ms = (time.perf_counter() - start_ts) * 1_000
    # if elapsed_ms > 1.0:                 # print only if noticeably slow
    #     print(f"[grit-hook] {elapsed_ms:5.1f} ms  {lang or 'default'}  "
    #           f"{result[:40]!r} → {new_text[:40]!r}")

    try:
        branding_name = get_branding_name()
    except Exception:
        branding_name = ""

    # %brand% is display text: the display name, not the path component.
    brand_text = replacer.display_name or branding_name
    if brand_text:
        new_text = new_text.replace("%brand%", brand_text)

    return new_text
