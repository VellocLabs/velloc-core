import json
import re
from pathlib import Path
from typing import Dict, List, Tuple


class Replacer:
    """Regex-based multilingual text replacer with blacklist support."""

    def __init__(self, cfg_path: str | Path):
        raw_cfg = self._load_raw_cfg(cfg_path)
        self._rules = self._compile_all(raw_cfg)
        # What %brand% becomes in text a user reads. branding_path_component
        # names the install / user-data directories and registry keys and
        # stays a single token; the display name may not ("Velloc Code").
        # Empty means the path component doubles as the display name.
        self.display_name: str = raw_cfg.get("display_name", "")

    # ------------------------------------------------------------------ public
    def replace(self, text: str, lang: str = "") -> str:
        """Apply language-specific rules, then default rules."""
        text = self._apply(text, self._rules.get(lang, None))
        text = self._apply(text, self._rules["default"])
        return text

    # ------------------------------------------------------------------ internals
    def _apply(
        self,
        text: str,
        compiled: Tuple[
            List[Tuple[re.Pattern, str]],
            List[Tuple[re.Pattern, str]],
        ] | None,
    ) -> str:
        if not compiled:
            return text
        repls, blacks = compiled

        def _collect_black_spans() -> List[Tuple[int, int, str]]:
            spans = []
            for pat, label in blacks:
                for m in pat.finditer(text):
                    spans.append((m.start(), m.end(), label))
            return spans

        def _match_black(
            match_start: int,
            match_end: int,
            black_spans: List[Tuple[int, int, str]],
        ) -> str | None:
            for blk_start, blk_end, label in black_spans:
                if match_start < blk_end and match_end > blk_start:
                    return label
            return None

        for pat, repl in repls:
            black_spans = _collect_black_spans()

            def _sub(m: re.Match) -> str:
                frag = m.group(0)
                pat_hit = _match_black(m.start(), m.end(), black_spans)
                if pat_hit:
                    # ---- print a clear log line for the block ----------------
                    # print(f'[replacer] ? blocked: "{frag}"  (by: r"{pat_hit}")')
                    return frag        # leave text unchanged
                return repl            # perform replacement

            text = pat.sub(_sub, text)
        return text

    # ------------------------------------------------------------------ compile
    @staticmethod
    def _load_raw_cfg(cfg_path: str | Path) -> Dict:
        raw = json.loads(Path(cfg_path).read_text(encoding="utf-8"))
        raw.setdefault("default", {"replacements": [], "blacklist": []})
        raw.setdefault("languages", {})
        return raw

    @staticmethod
    def _compile_all(
        raw_cfg: Dict,
    ) -> Dict[str, Tuple[List[Tuple[re.Pattern, str]], List[Tuple[re.Pattern, str]]]]:

        def _compile_pattern(
            entry,
            default_type: str = "regex",
        ) -> Tuple[re.Pattern, str]:
            if isinstance(entry, str):
                pattern = entry
                match_type = default_type
            elif isinstance(entry, dict):
                if "pattern" in entry:
                    pattern = entry["pattern"]
                    match_type = (
                        entry.get("type")
                        or entry.get("match")
                        or entry.get("pattern_type")
                        or default_type
                    )
                elif "text" in entry:
                    pattern = entry["text"]
                    match_type = (
                        entry.get("type")
                        or entry.get("match")
                        or entry.get("pattern_type")
                        or "string"
                    )
                else:
                    raise KeyError("pattern")
            else:
                raise TypeError("Pattern entry must be a string or dict.")

            match_type = (match_type or default_type).lower()
            if match_type in ("string", "literal", "text"):
                return re.compile(re.escape(pattern), re.I), pattern
            return re.compile(pattern, re.I), pattern

        def compile_block(block):
            repl = []
            for r in block.get("replacements", []):
                pat, _label = _compile_pattern(r)
                repl.append((pat, r["replacement"]))
            blk = [_compile_pattern(p) for p in block.get("blacklist", [])]
            return repl, blk

        compiled = {"default": compile_block(raw_cfg["default"])}
        for lang, data in raw_cfg["languages"].items():
            compiled[lang] = compile_block(data)
        return compiled
