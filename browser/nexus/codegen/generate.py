#!/usr/bin/env python3
"""Codegen for Nexus tool declarations (*.tool.json).

Reads declarations under --declarations-dir, emits per-tool C++ headers
under --output-dir. Each .tool.json produces:

  - generated/<tool_name>.gen.h   ← typed input struct + abstract base class

The header is a contract between the JSON-declared schema and the
implementation. The implementation class inherits the generated base and
overrides the single Run<Name>(<Name>Input, ToolCallback) virtual.

Path-inferred conventions:
  declarations/<category_snake>/<tool_name>.tool.json
                ↑                ↑
                C++ class        method = Run<PascalCase(name)>
                (PascalCase)

E.g. declarations/browser_tool/screenshot.tool.json →
       BrowserTool::RunScreenshot(ScreenshotInput, ToolCallback)

This script is intentionally dependency-free (stdlib only) so it works
in any Python 3.10+ environment without pip installs. JSON Schema
validation is opt-in via --validate; if jsonschema is unavailable we
skip validation rather than failing.

Usage:

  generate.py
      --declarations-dir <path>
      --output-dir       <path>
      [--validate]            # validate against tool_schema.json
      [--single <path>]       # process just one .tool.json (debugging)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable


# =============================================================================
# Type mapping: declared "type" → C++ type
#
# Mirrors the JsonSchemaKind enum in
# custom_browser/browser/nexus/tools/nexus_tool_schema.h. When this list
# changes, update both sides.
# =============================================================================

CPP_TYPE_BY_KIND: dict[str, str] = {
    "string":          "std::string",
    "integer":         "int32_t",
    "number":          "double",
    "boolean":         "bool",
    "array<string>":   "std::vector<std::string>",
    "array<integer>":  "std::vector<int32_t>",
    "array<number>":   "std::vector<double>",
    # Raw dict — the Chromium codebase uses the typedef base::DictValue
    # for base::Value::Dict (similarly base::ListValue for ::List).
    # The typedef is what's spelled in our existing headers, so use it
    # consistently in generated code too.
    "array<object>":   "std::vector<base::DictValue>",
    "object":          "base::DictValue",
}


# =============================================================================
# Identifier helpers — snake_case ↔ PascalCase, with consistent rules so
# the codegen mapping is reversible and grep-able.
# =============================================================================

def to_pascal_case(snake: str) -> str:
    """browser_tool → BrowserTool, media_seek → MediaSeek."""
    return "".join(part.capitalize() for part in snake.split("_") if part)


def header_guard(tool_name: str) -> str:
    """screenshot → GEN_NEXUS_TOOL_SCREENSHOT_H_."""
    upper = re.sub(r"[^A-Z0-9]", "_", tool_name.upper())
    return f"GEN_NEXUS_TOOL_{upper}_H_"


# =============================================================================
# Declaration model — typed mirror of the .tool.json schema, populated
# by `parse_declaration`. Codegen consumes these structs, never the raw
# JSON dict, so adding a new field requires touching exactly one place.
# =============================================================================

@dataclass
class InputDecl:
    name: str
    type_kind: str           # one of CPP_TYPE_BY_KIND keys
    required: bool = False
    description: str = ""
    # Optional override for the model-facing JSON property key. Keeps the
    # generated C++ field name (`name`) snake_case while letting a tool expose
    # CC-compatible wire keys that are not valid C++ identifiers, e.g. Grep's
    # `-A`, `-B`, `-C`, `-n`, and `-i`.
    wire_name: str | None = None
    # Host-set transport field. When True the field stays in the C++ input
    # struct AND the Extract() parser (so the host / runtime / hook-runner
    # can populate it across the bridge) but is OMITTED from the model-facing
    # schema — both `schema.parameters` and the PopulateRegistry tool-catalog
    # builder. Mirrors Claude Code's BashTool, which keeps `_simulatedSedEdit`
    # in the input *type* (`fullInputSchema`) but `.omit()`s it from the
    # `inputSchema` sent to the model. Mutually exclusive with `required`
    # (a model-required field the model can't see is a contradiction).
    internal: bool = False

    @property
    def cpp_type(self) -> str:
        base = CPP_TYPE_BY_KIND[self.type_kind]
        return base if self.required else f"std::optional<{base}>"

    @property
    def schema_name(self) -> str:
        return self.wire_name or self.name


@dataclass
class PromptDecl:
    """Per-tool prompt-section content. Populated from the `prompt` block
    in *.tool.json. Drives `prompt_tool_entries.gen.h` consumed by
    `api/catalog/prompt_section_renderer.cc`."""
    section: str            # one of PROMPT_SECTION_NAMES
    line: str               # the bullet line shown to the LLM
    group: str = ""         # optional sub-group within a section


# Section names accepted in `prompt.section`. The renderer composes them
# in this order; adding a new section here requires a matching emission
# block in `api/catalog/prompt_section_renderer.cc`.
PROMPT_SECTION_NAMES: tuple[str, ...] = (
    "browser-tools",
    "file-shell-tools",
    "task-tracking",
    "skills",
)


@dataclass
class ToolDecl:
    name: str                # snake_case (e.g. "screenshot")
    description: str
    inputs: list[InputDecl] = field(default_factory=list)
    skill: str | None = None
    runtime_thread: str = "ui"
    output_kind: str = "object"
    # Optional override for the LLM-facing schema name. When set, the
    # generated `BuildXxxSchema()` populates `schema.name = wire_name`
    # so the model sees e.g. "Read" while the on-disk identifier and
    # C++ method names remain lowercase. When unset, the wire name
    # equals `name` (the existing convention for every browser_tool).
    wire_name: str | None = None
    # Runtime scheduling hint. When true, the generated ToolSchema and
    # PopulateRegistry path mark the tool as safe to overlap with adjacent
    # tool_use blocks from the same assistant response.
    concurrency_safe: bool = False
    # Per-provider variance, mirroring the .tool.json `providers` block.
    # Each value is the raw provider-specific config dict (e.g.
    # {"strict": true, "input_examples": [...]}). Schema-builder
    # emission picks the Anthropic block for `input_examples` since
    # that's the only provider that surfaces it on the wire today.
    providers: dict[str, dict] = field(default_factory=dict)
    # Optional hand-crafted strict-mode JSON Schema variant. When set,
    # codegen emits a `schema.strict_input_schema = ...` block that
    # ships through the host manifest as `parametersStrict` to the JS
    # guest. The JS-side `deps.ts:serializeToolsForModel` picks this
    # variant (over the regular `parameters`) when the active model's
    # catalog flag `supportsStrictTools: true`, and sets the outbound
    # `strict: true` flag on Anthropic / OpenAI tool definitions.
    # Authors are responsible for keeping the schema strict-compliant:
    #   - `additionalProperties: false` on every nested object
    #   - no `pattern` / `format` / `maxItems` / `minItems` (Anthropic
    #     strict subset rejects these)
    # See docs for the strict subset: https://docs.anthropic.com/en/docs/build-with-claude/tool-use#strict-mode
    parameters_strict: dict | None = None
    # Optional system-prompt content. Tools without a prompt block are
    # not advertised in the {{TOOL_CATALOG}} placeholder (they're still
    # callable on the wire — the model just doesn't see them in the
    # prompt).
    prompt: PromptDecl | None = None
    # Path-inferred fields — populated post-parse by `infer_implementation`.
    category_snake: str = ""
    category_pascal: str = ""
    method_name: str = ""

    @property
    def schema_wire_name(self) -> str:
        """The string that ends up in `schema.name` and reaches the model
        on the wire. Falls back to `name` for the common case where the
        on-disk identifier is also what the model sees."""
        return self.wire_name or self.name

    @property
    def input_struct_name(self) -> str:
        """ScreenshotInput, MediaControlInput."""
        return f"{to_pascal_case(self.name)}Input"

    @property
    def base_class_name(self) -> str:
        """ScreenshotToolBase — the generated abstract class the impl inherits."""
        return f"{to_pascal_case(self.name)}ToolBase"

    @property
    def schema_builder_name(self) -> str:
        """Build<Name>Schema — the inline factory the registry calls."""
        return f"Build{to_pascal_case(self.name)}Schema"


# Map runtime.thread → suffix appended to the typed virtual's name. The
# suffix makes the threading contract self-documenting on the call
# site: a reader sees a `…OnThreadPool` suffix and immediately knows the
# body runs off the UI thread; `RunNavigateOnUI` is unambiguously the
# main-thread variant. The codegen also uses these to derive the matching
# `Run<Name><Suffix>` / `Handle<Name><Suffix>` virtual name everywhere
# it's emitted.
#
# The map is shared by both pipelines and kept total, but the pipelines
# allow different subsets:
#   - Tools: only "ui" / "any" (parse_declaration rejects "io" /
#     "thread_pool"). A per-session tool category can't be safely posted
#     off-UI via base::Unretained, so it hops blocking work inside its
#     body instead — see file_tool and docs/agents/nexus-tools-api.md §6.
#   - API: "ui" / "thread_pool" (and "io" if ever needed). Its adapters'
#     lifetime is the port service, so off-UI dispatch is permitted.
THREAD_METHOD_SUFFIX_BY_VALUE: dict[str, str] = {
    "ui":          "OnUI",
    "io":          "OnIO",
    "any":         "OnAny",
    "thread_pool": "OnThreadPool",
}


def parse_declaration(decl_path: Path) -> ToolDecl:
    """Read one .tool.json file into a typed ToolDecl. Raises on shape errors
    that JSON Schema validation would catch — kept here as a redundant
    sanity check so the codegen produces clean errors even when validation
    is skipped."""
    with decl_path.open("r", encoding="utf-8") as f:
        raw = json.load(f)

    # Required fields.
    for required in ("name", "description", "inputs"):
        if required not in raw:
            raise ValueError(f"{decl_path}: missing required field '{required}'")
    if not isinstance(raw["inputs"], list):
        raise ValueError(f"{decl_path}: 'inputs' must be an array")

    # Filename ↔ name consistency check. Codegen relies on the filename
    # to derive the method; declaring a different `name` inside the file
    # would silently break the dispatcher mapping.
    expected_name = decl_path.stem.removesuffix(".tool")
    if raw["name"] != expected_name:
        raise ValueError(
            f"{decl_path}: 'name' field is '{raw['name']}' but filename "
            f"declares '{expected_name}'. Rename one to match."
        )

    inputs: list[InputDecl] = []
    for i, raw_input in enumerate(raw["inputs"]):
        if not isinstance(raw_input, dict):
            raise ValueError(f"{decl_path}: inputs[{i}] must be an object")
        for k in ("name", "type"):
            if k not in raw_input:
                raise ValueError(
                    f"{decl_path}: inputs[{i}] missing required '{k}'")
        if raw_input["type"] not in CPP_TYPE_BY_KIND:
            raise ValueError(
                f"{decl_path}: inputs[{i}].type '{raw_input['type']}' is not "
                f"a known kind. Valid: {sorted(CPP_TYPE_BY_KIND)}"
            )
        is_required = bool(raw_input.get("required", False))
        is_internal = bool(raw_input.get("internal", False))
        if is_internal and is_required:
            raise ValueError(
                f"{decl_path}: inputs[{i}] '{raw_input['name']}' is both "
                f"'internal' and 'required'. An internal field is host-set and "
                f"hidden from the model-facing schema, so it cannot also be a "
                f"required model parameter."
            )
        wire_name = raw_input.get("wire_name")
        if wire_name is not None:
            if not isinstance(wire_name, str) or not wire_name:
                raise ValueError(
                    f"{decl_path}: inputs[{i}].wire_name must be a non-empty string")
        inputs.append(
            InputDecl(
                name=raw_input["name"],
                type_kind=raw_input["type"],
                required=is_required,
                description=raw_input.get("description", ""),
                wire_name=wire_name,
                internal=is_internal,
            )
        )

    # Tools always run SYNCHRONOUSLY on the bridge port service's UI
    # sequence — thread selection was removed from the tool protocol.
    # Rationale: a tool category is a per-session, value-member object on
    # ToolSession that ToolsAdapter can destroy mid-flight (session delete
    # / panel close). Posting its member function off-UI via
    # base::Unretained(category) is a use-after-free — the worker can run
    # the body against a freed object (see docs/agents/nexus-tools-api.md
    # §6). The off-UI options ("io" / "thread_pool") were unsafe and "any"
    # merely duplicated "ui", so a `runtime` block is no longer accepted.
    # A tool that needs blocking work hops it inside the body with
    # base::ThreadPool::PostTaskAndReplyWithResult + a static helper + a
    # WeakPtr-bound reply (see tools/file_tool/file_tool.cc). (The API
    # pipeline keeps "io" / "thread_pool" for its port-service-lifetime
    # adapters — that validation lives in the *.api.json parser.)
    if "runtime" in raw:
        raise ValueError(
            f"{decl_path}: 'runtime' is no longer supported in tool "
            f"declarations — tools always run synchronously on UI. Remove "
            f"the block; for blocking work, hop to base::ThreadPool inside "
            f"the body (see tools/file_tool/file_tool.cc and "
            f"docs/agents/nexus-tools-api.md §6).")
    thread = "ui"

    providers = raw.get("providers", {}) or {}
    if not isinstance(providers, dict):
        raise ValueError(f"{decl_path}: 'providers' must be an object")

    raw_concurrency_safe = raw.get("concurrencySafe", False)
    if not isinstance(raw_concurrency_safe, bool):
        raise ValueError(f"{decl_path}: 'concurrencySafe' must be a boolean")

    # `wire_name` is optional. When present we sanity-check the basic
    # shape (non-empty string starting with a letter) here so the
    # codegen produces a clear error even when the JSON-Schema
    # validation is skipped. Full pattern validation is in
    # tool_schema.json.
    wire_name = raw.get("wire_name")
    if wire_name is not None:
        if not isinstance(wire_name, str) or not wire_name:
            raise ValueError(
                f"{decl_path}: 'wire_name' must be a non-empty string")
        if not wire_name[0].isalpha():
            raise ValueError(
                f"{decl_path}: 'wire_name' must start with a letter; "
                f"got {wire_name!r}")

    # Optional `prompt` block. Same shape-checks here as JSON Schema
    # would catch — but we want clear errors even without --validate.
    prompt: PromptDecl | None = None
    raw_prompt = raw.get("prompt")
    if raw_prompt is not None:
        if not isinstance(raw_prompt, dict):
            raise ValueError(f"{decl_path}: 'prompt' must be an object")
        for k in ("section", "line"):
            if k not in raw_prompt:
                raise ValueError(
                    f"{decl_path}: prompt.{k!r} is required when 'prompt' "
                    f"is present")
        section = raw_prompt["section"]
        if section not in PROMPT_SECTION_NAMES:
            raise ValueError(
                f"{decl_path}: prompt.section {section!r} must be one of "
                f"{list(PROMPT_SECTION_NAMES)}")
        if not isinstance(raw_prompt["line"], str) or not raw_prompt["line"]:
            raise ValueError(
                f"{decl_path}: prompt.line must be a non-empty string")
        group = raw_prompt.get("group", "")
        if group is not None and not isinstance(group, str):
            raise ValueError(
                f"{decl_path}: prompt.group must be a string when set")
        prompt = PromptDecl(
            section=section,
            line=raw_prompt["line"],
            group=group or "",
        )

    # Optional `parametersStrict` block — hand-crafted strict-mode
    # JSON Schema. Validated only loosely here (must be a dict with
    # `type: "object"`); full structural validation is the author's
    # responsibility because Anthropic / OpenAI strict-mode subsets
    # are slightly different.
    raw_parameters_strict = raw.get("parametersStrict")
    if raw_parameters_strict is not None:
        if not isinstance(raw_parameters_strict, dict):
            raise ValueError(
                f"{decl_path}: 'parametersStrict' must be an object")
        if raw_parameters_strict.get("type") != "object":
            raise ValueError(
                f"{decl_path}: parametersStrict.type must be 'object' "
                f"(got {raw_parameters_strict.get('type')!r})")

    return ToolDecl(
        name=raw["name"],
        description=raw["description"],
        inputs=inputs,
        skill=raw.get("skill"),
        runtime_thread=thread,
        output_kind=raw.get("output_kind", "object"),
        wire_name=wire_name,
        concurrency_safe=raw_concurrency_safe,
        providers=providers,
        prompt=prompt,
        parameters_strict=raw_parameters_strict,
    )


def infer_implementation(decl: ToolDecl, decl_path: Path) -> None:
    """Populate category + method names from the file path.

    Convention:
        declarations/<category_snake>/<tool_name>.tool.json
        →  category_pascal = PascalCase(<category_snake>)
        →  method_name     = Run + PascalCase(<tool_name>) + <thread suffix>

    The thread suffix makes the threading contract visible at every call
    site without forcing readers to cross-reference the *.tool.json.
    Tools always run synchronously on UI, so a tool method's suffix is
    always `OnUI` (parse_declaration rejects any `runtime` block). The
    API pipeline shares THREAD_METHOD_SUFFIX_BY_VALUE and may also emit
    `OnAny`, but neither pipeline emits `OnIO` / `OnThreadPool` any more —
    both reject off-UI threads (parse_declaration / parse_api_declaration);
    those entries stay in the map only to keep it total.

    E.g. declarations/browser_tool/screenshot.tool.json (thread=ui) →
            category_pascal = "BrowserTool"
            method_name     = "RunScreenshotOnUI"

         declarations/file_tool/read.tool.json (thread=ui) →
            category_pascal = "FileTool"
            method_name     = "RunReadOnUI"
    """
    parent = decl_path.parent.name
    if not parent or parent == "declarations":
        raise ValueError(
            f"{decl_path}: must live in declarations/<category>/<name>.tool.json. "
            f"Got parent folder '{parent}' which is not a valid category."
        )
    decl.category_snake = parent
    decl.category_pascal = to_pascal_case(parent)
    suffix = THREAD_METHOD_SUFFIX_BY_VALUE[decl.runtime_thread]
    decl.method_name = f"Run{to_pascal_case(decl.name)}{suffix}"

    # Phase 13.3: cross-cutting reflection-field injection for the
    # browser_tool category. Every browser tool gets three required
    # string inputs (`evaluation_previous_goal`, `memory`, `next_goal`)
    # prepended to its `inputs` list. They restore the browser-use
    # reflection-fields contract retired in Phase 4 (when the
    # `browser_actions` umbrella tool split into flat per-action tools)
    # so the model carries explicit working memory across turns even
    # though no per-turn batch wrapper exists anymore.
    #
    # Schema-side: each browser tool's JSON-Schema gains three required
    # string properties. The model MUST send them with every call;
    # codegen extractors reject missing ones with `invalid_args`.
    #
    # C++-side: each BrowserXxxInput struct gains three `std::string`
    # members. Bodies don't currently read them — the fields exist
    # purely to force the model to keep a structured scratchpad. A
    # future enhancement could surface them in agent_meta / telemetry.
    #
    # Out of scope for file_tool / process_tool / other categories —
    # reflection is a browser-loop concern.
    if decl.category_snake == "browser_tool":
        _inject_reflection_fields(decl)


# Phase 13.3: the three required reflection-field input declarations
# auto-injected into every browser_tool tool. Tuple of (name,
# description) pairs; type/required are hardcoded since this is the
# whole point of the injection (string, required).
_BROWSER_REFLECTION_FIELDS: list[tuple[str, str]] = [
    ("evaluation_previous_goal",
     "Required one-sentence judgement of whether the previous turn's "
     "`next_goal` was achieved, partially achieved, or failed, citing "
     "concrete evidence from the latest observation (e.g. \"Login form "
     "submitted; landing page now shows the user's name\"). On the first "
     "turn, state that there is no prior goal yet."),
    ("memory",
     "Required running scratchpad of facts and decisions worth carrying "
     "forward across turns (URLs visited, search terms tried, IDs found, "
     "dead ends ruled out, sub-results collected). Append to and prune "
     "what you wrote last turn; don't re-derive it from scratch."),
    ("next_goal",
     "Required single concrete intent this call is meant to accomplish, "
     "phrased so the next turn's `evaluation_previous_goal` can check it. "
     "One sentence, action-shaped (e.g. \"Open the first organic result "
     "and read the article body\")."),
]


def _inject_reflection_fields(decl: ToolDecl) -> None:
    """Prepend the three reflection fields to `decl.inputs` if they
    aren't already declared. Idempotent — a tool that re-declares a
    reflection field (e.g. to override its description) keeps the
    author-declared version; the auto-injector only adds missing ones.

    Order is preserved: reflection fields come first, then the tool's
    own inputs. This matches the model's mental model (decide → act)
    and keeps the JSON-Schema `required` list ordered consistently."""
    existing_names = {inp.name for inp in decl.inputs}
    injected: list[InputDecl] = []
    for name, description in _BROWSER_REFLECTION_FIELDS:
        if name in existing_names:
            continue
        injected.append(
            InputDecl(
                name=name,
                type_kind="string",
                required=True,
                description=description,
            )
        )
    decl.inputs = injected + decl.inputs


# =============================================================================
# Code emission — one .gen.h per tool with three things:
#   (1) Typed input struct
#   (2) Abstract base class with the typed Run<Name> virtual
#   (3) Inline schema builder Build<Name>Schema()
#   (4) Inline extractor Extract(args, &input)
# Plus one combined registry .gen.cc with AllSchemas().
#
# Single string template; the layout matches what an experienced C++
# author would write by hand, minus the typing.
# =============================================================================

# Mapping declared "type" → JsonSchemaKind enum for the schema builder.
# Mirrors the enum in nexus_tool_schema.h.
JSON_KIND_BY_TYPE: dict[str, str] = {
    "string":         "::nexus::JsonSchemaKind::kString",
    "integer":        "::nexus::JsonSchemaKind::kInteger",
    "number":         "::nexus::JsonSchemaKind::kNumber",
    "boolean":        "::nexus::JsonSchemaKind::kBoolean",
    "array<string>":  "::nexus::JsonSchemaKind::kArrayOfString",
    "array<integer>": "::nexus::JsonSchemaKind::kArrayOfInteger",
    "array<number>":  "::nexus::JsonSchemaKind::kArrayOfNumber",
    "array<object>":  "::nexus::JsonSchemaKind::kArrayOfObject",
    "object":         "::nexus::JsonSchemaKind::kObject",
}

# Mapping output_kind → JsonSchemaKind. The output_kind values are
# coarser than parameter types (we don't enumerate every array shape
# for outputs since the wire format doesn't currently encode them).
OUTPUT_KIND_BY_VALUE: dict[str, str] = {
    "string":  "::nexus::JsonSchemaKind::kString",
    "integer": "::nexus::JsonSchemaKind::kInteger",
    "number":  "::nexus::JsonSchemaKind::kNumber",
    "boolean": "::nexus::JsonSchemaKind::kBoolean",
    "array":   "::nexus::JsonSchemaKind::kArrayOfObject",
    "object":  "::nexus::JsonSchemaKind::kObject",
    "any":     "::nexus::JsonSchemaKind::kAny",
}

HEADER_TEMPLATE = """\
// AUTO-GENERATED FROM: {source_relpath}
// DO NOT EDIT THIS FILE. Edit the source declaration instead and re-run
// codegen (or rebuild — GN tracks the dependency).
//
// This per-tool header carries:
//   (1) typed input struct
//   (2) inline schema builder (Build<Name>Schema)
//   (3) inline typed extractor (Extract → into the input struct)
//
// The abstract base class for the category (e.g. BrowserToolBase) is
// emitted ONCE in the category's own header (e.g. browser_tool.gen.h)
// and aggregates one virtual method per tool in the category. Adding
// a new tool → add a *.tool.json under declarations/<category>/, and
// the category's BrowserToolBase grows one more virtual automatically.

#ifndef {guard}
#define {guard}

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

namespace nexus::generated {{

// =============================================================================
// (1) Typed input struct mirroring the declaration's inputs[] in order.
// Required fields are bare; optional fields are wrapped in std::optional.
// =============================================================================
struct {input_struct} {{
{input_struct_fields}
}};

// =============================================================================
// (2) Schema builder. Returns a fully-populated `nexus::ToolSchema`
// describing this tool's name / description / parameters / output_kind /
// per-provider blocks (input_examples for Anthropic). Called once during
// `ToolCatalog::Get()` initialization via PopulateRegistry().
// =============================================================================
inline ::nexus::ToolSchema {schema_builder}() {{
  ::nexus::ToolSchema schema;
  schema.name = "{name}";
  schema.description =
      {description_literal};
  schema.output_kind = {output_kind_enum};
{concurrency_safe_emit}{skill_emit}{parameters_emit}{input_examples_emit}{strict_input_schema_emit}  return schema;
}}

// =============================================================================
// (3) Typed extractor. Reads the incoming `base::DictValue` (i.e. the
// LLM's tool_use input) into the typed input struct. Returns std::nullopt
// on success; an error message string on shape failure (missing required
// field, wrong type). The dispatcher rejects with `invalid_args` + this
// message on any non-nullopt return.
// =============================================================================
inline std::optional<std::string> Extract(
    const base::DictValue& args, {input_struct}& out) {{
{extractor_body}  return std::nullopt;
}}

}}  // namespace nexus::generated

#endif  // {guard}
"""


def cpp_string_literal(s: str) -> str:
    """Render a Python string as a C-style string literal, escaping
    backslashes, quotes, newlines, and tabs. Long strings get split
    across multiple lines for readability — the C++ compiler
    concatenates adjacent string literals automatically."""
    escaped = (
        s.replace("\\", "\\\\")
         .replace("\"", "\\\"")
         .replace("\n", "\\n")
         .replace("\t", "\\t")
    )
    # Single-line if short enough; otherwise wrap at ~70 chars.
    if len(escaped) <= 70:
        return f'"{escaped}"'
    # Naive wrap on word boundaries inside the escaped form.
    words = escaped.split(" ")
    lines: list[str] = []
    cur = ""
    for w in words:
        candidate = f"{cur} {w}" if cur else w
        if len(candidate) > 64:
            lines.append(f'"{cur} "')
            cur = w
        else:
            cur = candidate
    if cur:
        lines.append(f'"{cur}"')
    # Indent continuation lines for clarity.
    return ("\n      ").join(lines)


def _doc_comment_lines(text: str, indent: str = "  ") -> list[str]:
    """Render `text` as a sequence of C++ // doc comments, one per line.
    Multi-line descriptions must prefix every line with `//` — otherwise the
    second+ lines leak through as raw code."""
    text = text.rstrip("\n")
    return [f"{indent}//" if not line else f"{indent}// {line}"
            for line in text.split("\n")]


def render_input_struct_fields(inputs: list[InputDecl]) -> str:
    if not inputs:
        return "  // (no inputs)"
    lines = []
    for inp in inputs:
        if inp.description:
            lines.extend(_doc_comment_lines(inp.description))
        lines.append(f"  {inp.cpp_type} {inp.name};")
    return "\n".join(lines)


def render_parameters_emit(inputs: list[InputDecl]) -> str:
    """Emit `schema.parameters.emplace_back(...)` per MODEL-FACING input.

    `internal` inputs (host-set transport fields like the hook-runner's
    hook_shell / hook_stdin / hook_env or the runtime-injected per-call cwd)
    are deliberately omitted from the model-facing schema — they stay in the
    typed input struct + Extract() so the host can still populate them. This
    mirrors Claude Code's BashTool keeping `_simulatedSedEdit` in the input
    type but omitting it from the schema sent to the model."""
    visible = [inp for inp in inputs if not inp.internal]
    if not visible:
        return "  // (no parameters)\n"
    lines = []
    for inp in visible:
        kind = JSON_KIND_BY_TYPE[inp.type_kind]
        req = "true" if inp.required else "false"
        lines.append(f"  schema.parameters.emplace_back(")
        lines.append(f"      {cpp_string_literal(inp.schema_name)},")
        lines.append(f"      {cpp_string_literal(inp.description)},")
        lines.append(f"      {kind},")
        lines.append(f"      /*required=*/{req});")
    return "\n".join(lines) + "\n"


def render_skill_emit(decl: ToolDecl) -> str:
    """Emit `schema.skill = "<skill>";` when the tool declares a skill
    bucket. A tool with no `skill` (or skill linked to an always-on
    skill) leaves `schema.skill` at its default empty string, which the
    wire builder treats as 'always loaded'. Skill-gating (omit deferred
    tools until Skill() activates them) is driven by this field plus the
    *.skill.json `default` state — see giggly-watching-pie.md Phase 4."""
    if not decl.skill:
        return ""
    return f"  schema.skill = {cpp_string_literal(decl.skill)};\n"


def render_concurrency_safe_emit(decl: ToolDecl) -> str:
    if not decl.concurrency_safe:
        return ""
    return "  schema.concurrency_safe = true;\n"


def render_input_examples_emit(decl: ToolDecl) -> str:
    """Pull Anthropic-block input_examples and emit population code.
    Other providers' blocks are reserved for future expansion (currently
    only Anthropic surfaces input_examples on the wire).

    Uses Chromium's `base::ListValue` / `base::DictValue` typedefs (the
    `base::Value::List` / `base::Value::Dict` spellings are not
    accessible as types in this codebase — the namespace exposes the
    typedefs only)."""
    anthropic = decl.providers.get("anthropic", {}) or {}
    examples = anthropic.get("input_examples")
    if not isinstance(examples, list) or not examples:
        return ""
    lines = ["  // Per-provider input_examples (Anthropic only surfaces these"
             " on the wire).",
             "  base::ListValue examples;"]
    counter = [0]

    def fresh(prefix: str) -> str:
        counter[0] += 1
        return f"{prefix}{counter[0]}"

    for example in examples:
        if not isinstance(example, dict):
            # Skip malformed entries — JSON Schema validation should
            # catch these earlier; we're just being defensive.
            continue
        lines.append("  {")
        if not example:
            lines.append("    examples.Append(base::DictValue());")
        else:
            lines.append("    base::DictValue ex;")
            for key, value in example.items():
                pre, expr = render_value_statements(value, fresh, "    ")
                lines.extend(pre)
                lines.append(f"    ex.Set(\"{key}\", {expr});")
            lines.append("    examples.Append(std::move(ex));")
        lines.append("  }")
    lines.append("  schema.input_examples = std::move(examples);")
    return "\n".join(lines) + "\n"


def render_strict_input_schema_emit(decl: ToolDecl) -> str:
    """Emit C++ code that builds the hand-crafted strict-mode JSON
    Schema variant and assigns it to `schema.strict_input_schema`. Fires
    only when the .tool.json declared `parametersStrict`. The JS guest
    picks this variant over the regular parameters when the active
    model's catalog has `supportsStrictTools: true`, and sets the
    outbound `strict: true` flag on the tool definition.
    """
    if decl.parameters_strict is None:
        return ""
    counter = [0]

    def fresh(prefix: str) -> str:
        counter[0] += 1
        return f"{prefix}{counter[0]}"

    pre, expr = render_value_statements(decl.parameters_strict, fresh, "  ")
    lines = [
        "  // Per-tool strict-mode JSON Schema variant. The JS guest",
        "  // (deps.ts:serializeToolsForModel) picks this over the",
        "  // permissive `parameters` block when the active model's",
        "  // catalog has `supportsStrictTools: true`, and sets the",
        "  // outbound `strict: true` flag on the tool definition.",
    ]
    lines.extend(pre)
    # `render_value_statements` for a dict returns an expression like
    # `std::move(dN)`. Convert it into a base::DictValue we can assign
    # to the optional<DictValue> field.
    lines.append(f"  base::Value strict_v({expr});")
    lines.append("  if (auto* strict_dict = strict_v.GetIfDict()) {")
    lines.append("    schema.strict_input_schema = std::move(*strict_dict);")
    lines.append("  }")
    return "\n".join(lines) + "\n"


def render_value_literal(value) -> str:
    """Translate a JSON-decoded Python primitive into a
    base::Value-constructible C++ literal expression. Primitives only;
    callers that may receive nested dicts/lists must use
    `render_value_statements` (which falls back to a preamble + named
    variable for nested shapes)."""
    if value is None:
        return "base::Value()"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        return f"{value!r}"
    if isinstance(value, str):
        # Inline string literal; escape with same logic as the description.
        escaped = (
            value.replace("\\", "\\\\")
                 .replace("\"", "\\\"")
                 .replace("\n", "\\n")
                 .replace("\t", "\\t")
        )
        return f'"{escaped}"'
    # For nested dicts/lists, callers should use render_value_statements.
    raise ValueError(
        f"render_value_literal: unsupported nested value {value!r}. "
        f"Use render_value_statements for dict/list values."
    )


def render_value_statements(value, fresh, indent: str):
    """Translate any JSON-decoded value (primitive or nested) into a
    (preamble_lines, expression) pair that the caller can splice into
    generated code.

    For primitives: preamble is empty, expression is the inline literal
    from `render_value_literal`.

    For dict/list: emits typed local variables (base::DictValue /
    base::ListValue) into the preamble with chained Set/Append calls,
    then returns `std::move(varN)` as the expression. Nested dicts and
    lists recurse, each getting its own fresh local. `fresh(prefix)`
    is a caller-supplied generator that returns unique short variable
    names (e.g. "d1", "l2") so emissions inside one outer scope don't
    collide.

    The base::Value::Dict::Set / base::Value::List::Append overloads
    accept rvalues for nested Dict/List arguments, so emitting
    `std::move(varN)` is the safe way to feed a child container into
    its parent."""
    if isinstance(value, (type(None), bool, int, float, str)):
        return ([], render_value_literal(value))
    if isinstance(value, dict):
        var = fresh("d")
        pre = [f"{indent}base::DictValue {var};"]
        for k, v in value.items():
            sub_pre, sub_expr = render_value_statements(v, fresh, indent)
            pre.extend(sub_pre)
            key_escaped = k.replace("\\", "\\\\").replace("\"", "\\\"")
            pre.append(f'{indent}{var}.Set("{key_escaped}", {sub_expr});')
        return (pre, f"std::move({var})")
    if isinstance(value, list):
        var = fresh("l")
        pre = [f"{indent}base::ListValue {var};"]
        for item in value:
            sub_pre, sub_expr = render_value_statements(item, fresh, indent)
            pre.extend(sub_pre)
            pre.append(f"{indent}{var}.Append({sub_expr});")
        return (pre, f"std::move({var})")
    raise ValueError(
        f"render_value_statements: unsupported value type "
        f"{type(value).__name__}: {value!r}")


def render_extractor_body(inputs: list[InputDecl]) -> str:
    """Emit per-field FindXxx() / required-check code."""
    if not inputs:
        return "  // (no inputs to extract)\n"
    lines = []
    for inp in inputs:
        # Each `type` has its own extraction pattern. We handle the
        # primitives directly via base::Value::Dict::FindXxx; arrays
        # and objects need a richer extractor (deferred to later
        # phase for now — declared input types other than primitives
        # raise a NotImplementedError so codegen fails loudly).
        if inp.type_kind == "string":
            lines.append(_extract_string(inp))
        elif inp.type_kind == "integer":
            lines.append(_extract_int(inp))
        elif inp.type_kind == "number":
            lines.append(_extract_double(inp))
        elif inp.type_kind == "boolean":
            lines.append(_extract_bool(inp))
        elif inp.type_kind == "object":
            lines.append(_extract_object(inp))
        elif inp.type_kind == "array<object>":
            lines.append(_extract_array_object(inp))
        else:
            # Other array<*> types (array<string>/<integer>/<number>)
            # not yet codegen-supported. They're declared in
            # CPP_TYPE_BY_KIND but no tool exercises them today —
            # flag here so authors see immediately rather than puzzle
            # out a runtime crash.
            raise NotImplementedError(
                f"extractor for type '{inp.type_kind}' on field "
                f"'{inp.name}': only array<object> is implemented today "
                f"(see _extract_array_object). Add the matching "
                f"_extract_array_xxx helper or pick a different field type."
            )
    return "".join(lines)


def _extract_string(inp: InputDecl) -> str:
    key = inp.schema_name
    if inp.required:
        return (
            f"  // {inp.name}: required string\n"
            f"  if (auto* v = args.FindString(\"{key}\")) {{\n"
            f"    out.{inp.name} = *v;\n"
            f"  }} else {{\n"
            f"    return std::string(\"missing required string '{key}'\");\n"
            f"  }}\n"
        )
    return (
        f"  // {inp.name}: optional string\n"
        f"  if (auto* v = args.FindString(\"{key}\")) {{\n"
        f"    out.{inp.name} = *v;\n"
        f"  }}\n"
    )


def _extract_int(inp: InputDecl) -> str:
    key = inp.schema_name
    if inp.required:
        return (
            f"  // {inp.name}: required integer\n"
            f"  if (auto v = args.FindInt(\"{key}\")) {{\n"
            f"    out.{inp.name} = *v;\n"
            f"  }} else if (const std::string* s = args.FindString(\"{key}\")) {{\n"
            f"    int parsed;\n"
            f"    if (base::StringToInt(*s, &parsed)) {{\n"
            f"      out.{inp.name} = parsed;\n"
            f"    }} else {{\n"
            f"      return std::string(\"invalid integer \'{key}\': \" + *s);\n"
            f"    }}\n"
            f"  }} else {{\n"
            f"    return std::string(\"missing required integer '{key}'\");\n"
            f"  }}\n"
        )
    return (
        f"  // {inp.name}: optional integer\n"
        f"  if (auto v = args.FindInt(\"{key}\")) {{\n"
        f"    out.{inp.name} = *v;\n"
        f"  }} else if (const std::string* s = args.FindString(\"{key}\")) {{\n"
        f"    int parsed;\n"
        f"    if (base::StringToInt(*s, &parsed)) {{\n"
        f"      out.{inp.name} = parsed;\n"
        f"    }}\n"
        f"  }}\n"
    )


def _extract_double(inp: InputDecl) -> str:
    key = inp.schema_name
    if inp.required:
        return (
            f"  // {inp.name}: required number\n"
            f"  if (auto v = args.FindDouble(\"{key}\")) {{\n"
            f"    out.{inp.name} = *v;\n"
            f"  }} else {{\n"
            f"    return std::string(\"missing required number '{key}'\");\n"
            f"  }}\n"
        )
    return (
        f"  // {inp.name}: optional number\n"
        f"  if (auto v = args.FindDouble(\"{key}\")) {{\n"
        f"    out.{inp.name} = *v;\n"
        f"  }}\n"
    )


def _extract_bool(inp: InputDecl) -> str:
    key = inp.schema_name
    if inp.required:
        return (
            f"  // {inp.name}: required boolean\n"
            f"  if (auto v = args.FindBool(\"{key}\")) {{\n"
            f"    out.{inp.name} = *v;\n"
            f"  }} else {{\n"
            f"    return std::string(\"missing required boolean '{key}'\");\n"
            f"  }}\n"
        )
    return (
        f"  // {inp.name}: optional boolean\n"
        f"  if (auto v = args.FindBool(\"{key}\")) {{\n"
        f"    out.{inp.name} = *v;\n"
        f"  }}\n"
    )


def _extract_object(inp: InputDecl) -> str:
    """For raw object inputs we just clone the dict — runtime
    validation is the tool body's job."""
    key = inp.schema_name
    if inp.required:
        return (
            f"  // {inp.name}: required object (raw dict)\n"
            f"  if (auto* d = args.FindDict(\"{key}\")) {{\n"
            f"    out.{inp.name} = d->Clone();\n"
            f"  }} else {{\n"
            f"    return std::string(\"missing required object '{key}'\");\n"
            f"  }}\n"
        )
    return (
        f"  // {inp.name}: optional object (raw dict)\n"
        f"  if (auto* d = args.FindDict(\"{key}\")) {{\n"
        f"    out.{inp.name} = d->Clone();\n"
        f"  }}\n"
    )


def _extract_array_object(inp: InputDecl) -> str:
    """Extract array<object> into a `std::vector<base::DictValue>`.
    Each list element is verified to be a dict (non-dict elements
    cause a structured rejection so the LLM gets a clear error rather
    than a silent skip) and cloned into the vector. Per-element shape
    validation (which keys/types each dict carries) is the tool body's
    job — keeps the codegen simple for nested heterogeneous shapes
    like BrowserFillForm's `fields[]`."""
    key = inp.schema_name
    if inp.required:
        return (
            f"  // {inp.name}: required array<object>\n"
            f"  if (auto* lst = args.FindList(\"{key}\")) {{\n"
            f"    out.{inp.name}.reserve(lst->size());\n"
            f"    for (const base::Value& item : *lst) {{\n"
            f"      if (const base::DictValue* d = item.GetIfDict()) {{\n"
            f"        out.{inp.name}.push_back(d->Clone());\n"
            f"      }} else {{\n"
            f"        return std::string(\"'{key}' "
            f"contains a non-object entry\");\n"
            f"      }}\n"
            f"    }}\n"
            f"  }} else {{\n"
            f"    return std::string(\"missing required "
            f"array<object> '{key}'\");\n"
            f"  }}\n"
        )
    return (
        f"  // {inp.name}: optional array<object>\n"
        f"  if (auto* lst = args.FindList(\"{key}\")) {{\n"
        f"    out.{inp.name}.reserve(lst->size());\n"
        f"    for (const base::Value& item : *lst) {{\n"
        f"      if (const base::DictValue* d = item.GetIfDict()) {{\n"
        f"        out.{inp.name}.push_back(d->Clone());\n"
        f"      }}\n"
        f"    }}\n"
        f"  }}\n"
    )


def emit_header(decl: ToolDecl, source_path: Path, output_dir: Path) -> Path:
    """Write generated/<tool_name>.gen.h. Returns the output path."""
    output_path = output_dir / f"{decl.name}.gen.h"
    rendered = HEADER_TEMPLATE.format(
        source_relpath=str(source_path).replace("\\", "/"),
        guard=header_guard(decl.name),
        input_struct=decl.input_struct_name,
        input_struct_fields=render_input_struct_fields(decl.inputs),
        # The schema's wire-facing `name` reads from `schema_wire_name`,
        # which is `wire_name` if set or `name` otherwise. The on-disk
        # identifier used for the filename / header guard / method
        # name still uses `name`.
        name=decl.schema_wire_name,
        description_literal=cpp_string_literal(decl.description),
        output_kind_enum=OUTPUT_KIND_BY_VALUE.get(
            decl.output_kind, "::nexus::JsonSchemaKind::kAny"),
        skill_emit=render_skill_emit(decl),
        parameters_emit=render_parameters_emit(decl.inputs),
        concurrency_safe_emit=render_concurrency_safe_emit(decl),
        input_examples_emit=render_input_examples_emit(decl),
        strict_input_schema_emit=render_strict_input_schema_emit(decl),
        schema_builder=decl.schema_builder_name,
        extractor_body=render_extractor_body(decl.inputs),
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8")
    return output_path


# =============================================================================
# Per-category header emission. ONE .gen.h per category (e.g.
# `browser_tool.gen.h`) carrying ONE abstract base class
# (e.g. `BrowserToolBase`) whose virtual methods aggregate every tool
# in that category.
#
# Implementation classes inherit ONE base per category, not one per
# tool. So `class BrowserTool : public ToolBase, public
# ::nexus::generated::BrowserToolBase {…}` picks up all tool methods at
# once. Adding a new *.tool.json under declarations/<category>/
# automatically grows the base class with one more pure virtual on the
# next codegen run.
# =============================================================================

CATEGORY_HEADER_TEMPLATE = """\
// AUTO-GENERATED. Aggregate base class for all tools in this category.
// DO NOT EDIT THIS FILE. Edit the per-tool *.tool.json sources instead
// and re-run codegen (or rebuild — GN tracks the dependency).
//
// One pure virtual per *.tool.json under declarations/{category_snake}/.
// The implementation class (e.g. {category_pascal}) inherits this base
// alongside ToolBase and overrides each Run<Name>. The codegen-emitted
// dispatcher in nexus_tools_registry.gen.cc routes by tool name into
// the corresponding virtual via CategoryAccessor<{category_pascal}>::Get(session).
//
// Including this header pulls in every per-tool .gen.h in the
// category, so the implementation TU does not need to add per-tool
// includes alongside the inheritance line.
//
// Sources:
{source_listing}

#ifndef {guard}
#define {guard}

#include <utility>

#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"

{per_tool_includes}

namespace nexus::generated {{

class {base_class} {{
 public:
  virtual ~{base_class}() = default;

{virtual_decls}
}};

}}  // namespace nexus::generated

#endif  // {guard}
"""


def category_header_guard(category_snake: str) -> str:
    upper = re.sub(r"[^A-Z0-9]", "_", category_snake.upper())
    return f"GEN_NEXUS_CATEGORY_{upper}_H_"


def emit_category_header(
    category_snake: str,
    decls_in_category: list[ToolDecl],
    output_dir: Path,
) -> Path:
    """Write generated/<category_snake>.gen.h. Returns the output path."""
    sorted_decls = sorted(decls_in_category, key=lambda d: d.name)
    category_pascal = to_pascal_case(category_snake)
    base_class = f"{category_pascal}Base"

    per_tool_includes = "\n".join(
        f'#include "custom_browser/browser/nexus/tools/{d.name}.gen.h"'
        for d in sorted_decls
    )

    source_listing = "\n".join(
        f"//   declarations/{category_snake}/{d.name}.tool.json"
        for d in sorted_decls
    )

    # One pure virtual per tool. Each takes the typed input struct from
    # its own .gen.h plus the trailing ToolCallback. Tools always run on
    # the UI sequence, so every method carries the fixed `OnUI` suffix and
    # the virtual reads as e.g. `RunReadOnUI` / `RunNavigateOnUI`.
    virtual_lines: list[str] = []
    for d in sorted_decls:
        method = d.method_name
        virtual_lines.append(
            f"  // From declarations/{category_snake}/{d.name}.tool.json"
            f" (runs on UI)"
        )
        virtual_lines.append(
            f"  virtual void {method}({d.input_struct_name} input,"
        )
        # Align continuation under the opening paren.
        indent = " " * (len(f"  virtual void {method}(") )
        virtual_lines.append(
            f"{indent}::nexus::ToolCallback done) = 0;"
        )
        virtual_lines.append("")
    # Drop trailing blank line for tidiness.
    if virtual_lines and virtual_lines[-1] == "":
        virtual_lines.pop()
    virtual_decls = "\n".join(virtual_lines)

    rendered = CATEGORY_HEADER_TEMPLATE.format(
        category_snake=category_snake,
        category_pascal=category_pascal,
        base_class=base_class,
        guard=category_header_guard(category_snake),
        per_tool_includes=per_tool_includes,
        source_listing=source_listing,
        virtual_decls=virtual_decls,
    )

    output_path = output_dir / f"{category_snake}.gen.h"
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8")
    return output_path


# =============================================================================
# Combined registry emission — one .gen.cc that pulls in every per-tool
# .gen.h and exposes `AllSchemas()` returning a vector<ToolSchema>.
#
# Phase 2 stops here: the registry exists but isn't yet consumed by
# `ToolCatalog::Get()`. Phase 3 wires it in alongside the existing
# macro-based dispatchers.
# =============================================================================

REGISTRY_HEADER_TEMPLATE = """\
// AUTO-GENERATED. Combined registry of all *.tool.json declarations.
// DO NOT EDIT THIS FILE. Edit individual *.tool.json sources instead.

#ifndef GEN_NEXUS_TOOLS_REGISTRY_H_
#define GEN_NEXUS_TOOLS_REGISTRY_H_

#include <vector>

#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

namespace nexus::generated {

// Returns one `ToolSchema` per registered *.tool.json declaration.
// Order is deterministic (sorted by tool name) so cache keys are
// stable across builds.
std::vector<::nexus::ToolSchema> AllSchemas();

// Pushes one `ToolEntry` (schema + dispatcher) per registered
// declaration into `registry`. Called from `ToolCatalog::Get()` after
// the existing macro-based DefineTools() pass; the two systems
// coexist while migration is in progress.
void PopulateRegistry(::nexus::ToolRegistry& registry);

}  // namespace nexus::generated

#endif  // GEN_NEXUS_TOOLS_REGISTRY_H_
"""


REGISTRY_SOURCE_TEMPLATE = """\
// AUTO-GENERATED. Combined registry of all *.tool.json declarations.
// DO NOT EDIT THIS FILE. Edit individual *.tool.json sources instead.

#include "{registry_header_relpath}"

#include <utility>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_macros.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"

{per_tool_includes}
{category_includes}

namespace nexus::generated {{

std::vector<::nexus::ToolSchema> AllSchemas() {{
  std::vector<::nexus::ToolSchema> schemas;
  schemas.reserve({tool_count});
{schema_pushes}  return schemas;
}}

// =============================================================================
// Per-tool dispatcher trampolines. Each one runs the same four steps:
//   (1) Extract typed input from the JSON dict. `Extract` returns a
//       string error on shape failure; the trampoline rejects with
//       `invalid_args` + that message in that case.
//   (2) Look up the category instance from the session via
//       `CategoryAccessor<Category>::Get`. nullptr means the session
//       doesn't carry that category — reject cleanly with
//       `no_category` rather than dereferencing.
//   (3) Invoke the typed virtual (`Run<Name>OnUI` / `…OnAny`)
//       SYNCHRONOUSLY on the dispatcher's own sequence — the bridge port
//       service's UI sequence. Tool categories are restricted to
//       "ui" / "any" (parse_declaration rejects off-UI threads), so the
//       trampoline never PostTasks the category onto another sequence.
//
// A tool that needs blocking work keeps "ui" and hops it inside its body
// with base::ThreadPool::PostTaskAndReplyWithResult + a static helper +
// a WeakPtr-bound reply (see tools/file_tool/file_tool.cc). The category
// is therefore never dereferenced off its home sequence — avoiding the
// use-after-free a `base::Unretained` worker-bind would cause if the
// session were torn down mid-flight (see docs/agents/nexus-tools-api.md
// §6). The `done` ToolCallback's internal WeakPtr<ToolsAdapter> still
// drops responses cleanly when the adapter goes away.
//
// Per-action wall-clock budget (`PausableBudgetWrap`) is intentionally
// NOT applied here — every category implementation already wraps its
// internal actor calls with the session's action_timeout. Wrapping
// again at the dispatcher level would double-fire the timer and
// confuse first-write-wins semantics.
// =============================================================================

{dispatcher_definitions}

void PopulateRegistry(::nexus::ToolRegistry& registry) {{
{registry_pushes}}}

}}  // namespace nexus::generated
"""


DISPATCHER_TEMPLATE = """\
namespace {{

void Dispatch{name_pascal}(::nexus::ToolSession* session,
                           base::DictValue args,
                           ::nexus::ToolCallback done) {{
  // (1) Thread context. The dispatcher is invoked on the bridge port
  // service's sequence (UI today) and calls the typed virtual directly
  // on that sequence — tools always run synchronously on UI (thread
  // selection was removed from the tool protocol). A tool that needs
  // off-UI work hops it inside its own body (see file_tool), so the
  // dispatcher never posts a per-session category onto another sequence.

  // (2) Extract typed input.
  {input_struct} input;
  if (auto err = Extract(args, input); err) {{
    std::move(done).Reject("invalid_args", *err);
    return;
  }}

  // (3) Find the per-session category instance. The lookup happens on
  // the dispatcher's calling sequence — `ToolSession`'s category members
  // live there. Tool categories are invoked SYNCHRONOUSLY below (only
  // "ui" / "any" are allowed; parse_declaration rejects off-UI threads),
  // so the pointer is used inline and cannot dangle. A tool that needs
  // blocking work keeps "ui" and hops it inside its body with a
  // WeakPtr-bound reply — never via base::Unretained onto a worker, which
  // would be a use-after-free if the session is torn down mid-flight.
  // See file_tool/file_tool.h and docs/agents/nexus-tools-api.md §6.
  auto* category =
      ::nexus::detail::CategoryAccessor<{category_qualified}>::Get(session);
  if (!category) {{
    std::move(done).Reject(
        "no_category",
        "{category_qualified}::CategoryAccessor returned null on this session");
    return;
  }}

  // (4) Invoke the typed virtual on the declared thread.
{thread_invoke}
}}

}}  // namespace
"""


# Invoke snippet for section (4) of the dispatcher. Tools always run
# SYNCHRONOUSLY on the dispatcher's own sequence (the bridge port
# service's UI sequence) — thread selection was removed from the tool
# protocol (parse_declaration rejects a `runtime` block), so there is a
# single snippet and it never binds the category onto another sequence.
#
# A tool that needs blocking work keeps its body on UI and hops the work
# itself (base::ThreadPool::PostTaskAndReplyWithResult + a static helper +
# a WeakPtr reply), so the category is never dereferenced off its home
# sequence. See tools/file_tool/file_tool.cc.
#
# The off-UI `base::Unretained(category)` routing that used to live here
# (io / thread_pool / any) was removed: a per-session category can be torn
# down mid-flight, so posting its member function to another sequence was
# a use-after-free (see docs/agents/nexus-tools-api.md §6). The API
# pipeline keeps an off-UI route in `_emit_api_invoke_block` for its
# port-service-lifetime adapters.
THREAD_INVOKE_TEMPLATE_BY_VALUE: dict[str, str] = {
    "ui": "  category->{method_name}(std::move(input), std::move(done));",
}


def emit_registry(
    decls: list[ToolDecl],
    output_dir: Path,
    registry_header_relpath: str,
) -> tuple[Path, Path]:
    """Write nexus_tools_registry.gen.h and .gen.cc. Returns (h_path, cc_path).

    The header declares AllSchemas() + PopulateRegistry(). The .cc
    includes every per-tool .gen.h plus every category's header (for
    `CategoryAccessor<X>` specializations), defines the per-tool
    dispatcher trampolines, and registers schema + dispatcher pairs
    via PopulateRegistry().
    """
    sorted_decls = sorted(decls, key=lambda d: d.name)

    h_path = output_dir / "nexus_tools_registry.gen.h"
    cc_path = output_dir / "nexus_tools_registry.gen.cc"

    h_path.parent.mkdir(parents=True, exist_ok=True)
    h_path.write_text(REGISTRY_HEADER_TEMPLATE, encoding="utf-8")

    # Per-tool .gen.h includes (one per tool).
    per_tool_includes = "\n".join(
        f'#include "custom_browser/browser/nexus/tools/{d.name}.gen.h"'
        for d in sorted_decls
    )

    # Per-category .h includes (one per UNIQUE category). Each category
    # supplies a CategoryAccessor<X> specialization in its .h. Convention:
    # each category lives in its own subfolder (tools/<cat>/) and the
    # main implementation header is named after the category, so the
    # include path is tools/<cat>/<cat>.h.
    unique_categories = sorted({d.category_snake for d in sorted_decls})
    category_includes = "\n".join(
        f'#include "custom_browser/browser/nexus/tools/{cat}/{cat}.h"'
        for cat in unique_categories
    )

    schema_pushes = "".join(
        f"  schemas.push_back({d.schema_builder_name}());\n"
        for d in sorted_decls
    )

    # One dispatcher function per tool, in an anonymous namespace
    # within the .cc (no need to expose them outside this TU).
    dispatcher_definitions: list[str] = []
    for d in sorted_decls:
        # First fill in the per-thread invoke snippet. It carries
        # references to {category_qualified} and {method_name}, plus
        # any `{{...}}` brace-pairs (e.g. ThreadPool task traits) that
        # have to make it through this format step intact and into the
        # final source as literal `{...}`. Doing it as a separate
        # format pass keeps the outer DISPATCHER_TEMPLATE simple.
        thread_invoke = THREAD_INVOKE_TEMPLATE_BY_VALUE[d.runtime_thread].format(
            category_qualified=f"::nexus::{d.category_pascal}",
            method_name=d.method_name,
        )
        dispatcher_definitions.append(
            DISPATCHER_TEMPLATE.format(
                name_pascal=to_pascal_case(d.name),
                input_struct=d.input_struct_name,
                category_qualified=f"::nexus::{d.category_pascal}",
                thread=d.runtime_thread,
                thread_invoke=thread_invoke,
            )
        )
    dispatcher_definitions = "\n".join(dispatcher_definitions)

    # Registration: one block per tool. ToolBuilder::Bind is rvalue-
    # qualified (`void Bind(...) &&;`) and the chained mutators
    # (.Required/.Optional/.Returns) return ToolBuilder& (lvalue), so a
    # direct chain `Tool(...).Required(...).Bind(...)` does not compile
    # (lvalue-to-rvalue mismatch). The macro DSL solves this by capturing
    # the builder in a local var and writing `std::move(b).Bind(...)`;
    # the codegen mirrors that pattern in a per-tool brace-scope so the
    # locals don't collide across tools in the same function body.
    registry_pushes = ""
    for i, d in enumerate(sorted_decls):
        name_pascal = to_pascal_case(d.name)
        # The registry is keyed by the WIRE name (what the LLM emits in
        # tool_use), so pass `schema_wire_name` here. For tools without
        # a `wire_name` override this is identical to `name`. The
        # comment still cites the on-disk name so a `grep <name>.tool.json`
        # locates the source.
        registry_pushes += f"  // Tool: {d.name} (declarations/{d.category_snake}/{d.name}.tool.json)\n"
        registry_pushes += "  {\n"
        registry_pushes += (
            f'    auto builder = registry.Tool("{d.schema_wire_name}",\n'
            f'                                  {cpp_string_literal(d.description)});\n'
        )
        # Skill bucket — drives skill-gating (deferred-skill tools are
        # omitted from tools[] until activated). Empty/unset → always
        # loaded. The schema-builder path (BuildXxxSchema) sets this too,
        # but PopulateRegistry's builder is what feeds ToolCatalog, so it
        # MUST set skill here or the wire never carries it.
        if d.skill:
            registry_pushes += f'    builder.Skill("{d.skill}");\n'
        if d.concurrency_safe:
            registry_pushes += "    builder.ConcurrencySafe();\n"
        for inp in d.inputs:
            if inp.internal:
                # Host-set transport field — omit from the model-facing tool
                # catalog (same rationale as render_parameters_emit). Still
                # carried in the typed struct + parsed by Extract().
                continue
            method = "Required" if inp.required else "Optional"
            kind = JSON_KIND_BY_TYPE[inp.type_kind]
            registry_pushes += (
                f'    builder.{method}({cpp_string_literal(inp.schema_name)},\n'
                f'                    {cpp_string_literal(inp.description)},\n'
                f'                    {kind});\n'
            )
        if d.parameters_strict is not None:
            counter = [0]

            def fresh(prefix: str) -> str:
                counter[0] += 1
                return f"strict_{prefix}{counter[0]}"

            pre, expr = render_value_statements(d.parameters_strict, fresh, "    ")
            registry_pushes += "    // Live hosted-tool strict schema variant.\n"
            for line in pre:
                registry_pushes += f"{line}\n"
            registry_pushes += f"    base::Value strict_schema_v({expr});\n"
            registry_pushes += "    if (auto* strict_schema = strict_schema_v.GetIfDict()) {\n"
            registry_pushes += "      builder.StrictInputSchema(std::move(*strict_schema));\n"
            registry_pushes += "    }\n"
        if d.output_kind != "object":
            registry_pushes += (
                f"    builder.Returns({OUTPUT_KIND_BY_VALUE.get(d.output_kind, '::nexus::JsonSchemaKind::kAny')});\n"
            )
        registry_pushes += (
            f"    std::move(builder).Bind(\n"
            f"        base::BindRepeating(&Dispatch{name_pascal}));\n"
        )
        registry_pushes += "  }\n"
        if i + 1 < len(sorted_decls):
            registry_pushes += "\n"

    rendered = REGISTRY_SOURCE_TEMPLATE.format(
        registry_header_relpath=registry_header_relpath,
        per_tool_includes=per_tool_includes,
        category_includes=category_includes,
        tool_count=len(sorted_decls),
        schema_pushes=schema_pushes,
        dispatcher_definitions=dispatcher_definitions,
        registry_pushes=registry_pushes,
    )
    cc_path.write_text(rendered, encoding="utf-8")
    return h_path, cc_path


# =============================================================================
# Phase 13.9 retired the per-tool `prompt_tool_entries.gen.h` table.
# Catalog-section prose (intro/outro/per-tool bullets) now lives in
# consolidated section .md files referenced from
# `tool-catalog-frame.json`. The renderer emits each .md verbatim once
# its section's gating condition (e.g. "any browser tool enabled") is
# satisfied. The `prompt` block on a *.tool.json is therefore obsolete;
# the parser still accepts it for backwards compat but the codegen
# silently ignores any data inside.
# =============================================================================


# =============================================================================
# Prose-field resolver: lets JSON manifest fields point at sibling .md
# files instead of inlining long prose as a JSON string literal.
#
# Any string value whose content begins with `@file:<path>` is treated
# as a reference: the codegen loads `<path>` relative to the JSON file's
# parent directory, strips a single trailing newline (so the embedded
# C++ constant matches the look of the prior inline JSON form), and
# substitutes the contents in place. Plain strings pass through
# unchanged — short labels like `"## Browser tools"` or `"tools_lead"`
# stay inline where authoring a separate file would be overkill.
#
# The resolver is intentionally minimal: no template substitution, no
# markdown parsing, no multi-section files. One .md per prose block.
# =============================================================================

_FILE_REF_PREFIX = "@file:"


def _resolve_prose_field(value, base_dir: Path, field_path: str) -> str:
    """If `value` starts with `@file:`, load the named file relative
    to `base_dir` and return its contents (UTF-8, trimmed of a single
    trailing newline). Otherwise return `value` unchanged.

    `field_path` is used only for error messages — pass a human-readable
    "<json-file>: <dotted.field.path>" so a missing or empty .md file
    points back at the JSON site that referenced it."""
    if not isinstance(value, str) or not value.startswith(_FILE_REF_PREFIX):
        return value
    rel = value[len(_FILE_REF_PREFIX):].strip()
    if not rel:
        raise ValueError(f"{field_path}: '@file:' reference is empty")
    md_path = (base_dir / rel).resolve()
    try:
        text = md_path.read_text(encoding="utf-8")
    except FileNotFoundError as e:
        raise ValueError(
            f"{field_path}: referenced file not found: {md_path}") from e
    # Strip exactly one trailing newline so the embedded constant
    # matches how the JSON-literal form looked (no implicit final \n).
    if text.endswith("\n"):
        text = text[:-1]
    return text


def load_tool_catalog_frame(frame_path: Path) -> dict:
    """Load and validate `tool-catalog-frame.json`.

    Phase 13.9 shape: each top-level section key is a single string
    (typically an `@file:<path>` reference to a .md file). The codegen
    resolver expands the reference to the file's contents; the emitter
    embeds each as a constexpr string_view in `tool_catalog_frame.gen.h`.
    Validation here mirrors the JSON-Schema check but runs inline so
    the codegen produces clean errors even when --validate is skipped.

    Returns the parsed document with `@file:` references resolved to
    file contents (so the emitter doesn't need to re-resolve)."""
    with frame_path.open("r", encoding="utf-8") as f:
        doc = json.load(f)
    if not isinstance(doc, dict):
        raise ValueError(f"{frame_path}: root must be an object")

    # Every section value is a single string (`@file:<path>` or a
    # literal). Required keys are validated together; an empty .md or
    # missing file fails with the JSON field path attached.
    for required_key in (
        "browser_tools",
        "file_shell_tools",
        "task_tracking",
        "skills",
        "parallelism",
    ):
        v = _resolve_prose_field(
            doc.get(required_key),
            frame_path.parent,
            f"{frame_path}: {required_key}",
        )
        if not isinstance(v, str) or not v:
            raise ValueError(
                f"{frame_path}: '{required_key}' must be a non-empty "
                f"string (or `@file:<path>` reference to a .md file).")
        doc[required_key] = v

    return doc


# =============================================================================
# Phase 13.2: system-prompt-tools content — `system_prompt_tools.gen.h`
#
# Reads a single top-level declaration at
# `tools/declarations/system-prompt-tools.md` (verbatim markdown — no
# JSON wrapper, no rendering, what you write is what the LLM sees) and
# emits a header containing one constexpr `std::string_view
# kSystemPromptToolsContent`. The renderer in
# `api/catalog/prompt_section_renderer.cc` returns this string verbatim
# when asked for the `system-prompt-tools` section.
#
# Source-of-truth design: previously these bullets lived as hand-written
# prose in `nexus_web/public/system_prompts/*.md` and drifted out-of-
# date as the tool surface evolved (Phase 13 retired tools like
# `wait` / `forward` / `create_tab` but the .md still referenced them).
# A first pass migrated to a JSON shape with `sections[].bullets[]`;
# Phase 13.7 dropped the JSON wrapper entirely — authoring multi-
# paragraph prose as JSON string literals was unergonomic and produced
# typos that survived review (`"Each fills entry"` lived inside a
# JSON string for several iterations before anyone noticed). The .md
# source is now the single artifact the author edits.
# =============================================================================

SYSTEM_PROMPT_TOOLS_TEMPLATE = """\
// AUTO-GENERATED FROM: {source_relpath}
// DO NOT EDIT THIS FILE. Edit the source declaration instead and re-run
// codegen (or rebuild — GN tracks the dependency).
//
// Cross-cutting native-tool prose for the `{{{{system-prompt-tools}}}}`
// placeholder in `nexus_web/public/system_prompts/*.md`. The catalog
// renderer at `api/catalog/prompt_section_renderer.cc` returns this
// string verbatim via `bridge.catalog.renderPromptSection({{name:
// 'system-prompt-tools'}})`.

#ifndef GEN_NEXUS_SYSTEM_PROMPT_TOOLS_H_
#define GEN_NEXUS_SYSTEM_PROMPT_TOOLS_H_

#include <string_view>

namespace nexus::generated {{

inline constexpr std::string_view kSystemPromptToolsContent =
{content_literal};

}}  // namespace nexus::generated

#endif  // GEN_NEXUS_SYSTEM_PROMPT_TOOLS_H_
"""


def _emit_raw_string_literal(content: str) -> str:
    """Wrap a Python string in a C++ raw string literal. Uses a delimiter
    that's vanishingly unlikely to appear in the content
    (`NEXUSPROMPT`) so we don't have to escape internal `)` characters."""
    delim = "NEXUSPROMPT"
    if f"){delim}\"" in content:
        # Defensive — should never trigger given our delimiter, but
        # raise loudly rather than emit broken C++.
        raise ValueError(
            "_emit_raw_string_literal: content contains the chosen raw-string "
            f"delimiter '){delim}\"'. Pick a different delimiter or escape "
            "the content.")
    return f"    R\"{delim}({content}){delim}\""


def emit_system_prompt_tools(
    source_path: Path,
    output_dir: Path,
) -> Path:
    """Read `system-prompt-tools.md` verbatim and emit
    `system_prompt_tools.gen.h` with the file contents as a single
    constexpr string_view. No section walking, no markdown rendering —
    what the author writes is what the LLM sees. A single trailing
    newline is stripped so the embedded constant doesn't end with
    a dangling blank line."""
    rendered = source_path.read_text(encoding="utf-8")
    if rendered.endswith("\n"):
        rendered = rendered[:-1]
    if not rendered:
        raise ValueError(
            f"{source_path}: file is empty — write the system-prompt-tools "
            f"markdown directly in this .md file.")

    out_path = output_dir / "system_prompt_tools.gen.h"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        SYSTEM_PROMPT_TOOLS_TEMPLATE.format(
            source_relpath=str(source_path).replace("\\", "/"),
            content_literal=_emit_raw_string_literal(rendered),
        ),
        encoding="utf-8",
    )
    return out_path


# =============================================================================
# Phase 13.4 / 13.8: tool-catalog-frame content — `tool_catalog_frame.gen.h`
#
# Source-of-truth for the prose that frames the `{{TOOL_CATALOG}}`
# rendered section: the "## Browser tools" block (now a single
# consolidated .md), the "## File and shell tools" header + read-
# before-edit rule, and the JS-only section blocks (TodoWrite / Skill /
# parallelism). Previously all hand-written constexpr blocks inside
# `prompt_section_renderer.cc`.
#
# Phase 13.8: the browser-tools section was simplified from a per-piece
# composition (header + intro + call_shape + per-group bullets + outro)
# to a single `kBrowserToolsContent` string that the renderer emits
# verbatim. Per-tool bullets used to be aggregated from each
# browser_tool/*.tool.json's `prompt` block; those blocks were removed
# and the same prose now lives directly in
# `browser_tool/browser-tools-catalog.md` (referenced by
# tool-catalog-frame.json `browser_tools`). The renderer detects "is any
# browser tool enabled" by checking the session's enabled-tool set
# against `kBrowserToolWireNames` (emitted from the parsed decls).
# =============================================================================

TOOL_CATALOG_FRAME_TEMPLATE = """\
// AUTO-GENERATED FROM: {source_relpath}
// DO NOT EDIT THIS FILE. Edit the source declaration instead and re-run
// codegen (or rebuild — GN tracks the dependency).
//
// Frame prose for the `{{{{TOOL_CATALOG}}}}` placeholder, rendered by
// `api/catalog/prompt_section_renderer.cc::RenderUsingYourToolsSection`.

#ifndef GEN_NEXUS_TOOL_CATALOG_FRAME_H_
#define GEN_NEXUS_TOOL_CATALOG_FRAME_H_

#include <array>
#include <string_view>

namespace nexus::generated {{

// Each section string is emitted verbatim by the renderer when its
// gating condition is satisfied. The string carries its own "## ..."
// markdown header (no separate header constant any more).
inline constexpr std::string_view kBrowserToolsContent =
{browser_content_literal};
inline constexpr std::string_view kFileShellToolsContent =
{file_shell_content_literal};
inline constexpr std::string_view kTaskTrackingContent =
{task_tracking_literal};
inline constexpr std::string_view kSkillsContent =
{skills_literal};
inline constexpr std::string_view kParallelismContent =
{parallelism_literal};

// Wire names of every tool in the `browser_tool/` category. The
// renderer checks this against the session's enabled-tool set to
// decide whether to emit `kBrowserToolsContent`.
inline constexpr std::array<std::string_view, {browser_tool_count}>
    kBrowserToolWireNames = {{{{
{browser_tool_wire_names}
    }}}};

// Wire names of every tool in `file_tool/` and `process_tool/`
// (collectively the "file and shell tools" section). Same role as
// `kBrowserToolWireNames` for the file-shell section's gating.
inline constexpr std::array<std::string_view, {file_shell_tool_count}>
    kFileShellToolWireNames = {{{{
{file_shell_tool_wire_names}
    }}}};

}}  // namespace nexus::generated

#endif  // GEN_NEXUS_TOOL_CATALOG_FRAME_H_
"""


def emit_tool_catalog_frame(
    source_path: Path,
    output_dir: Path,
    decls: list,
) -> Path:
    """Read `tool-catalog-frame.json`, emit `tool_catalog_frame.gen.h`
    with one constexpr string_view per section (each loaded verbatim
    from a sibling .md file) plus two wire-name arrays so the renderer
    can detect "any browser tool enabled" / "any file-shell tool
    enabled" without scanning per-tool prompt entries.

    `decls` is the parsed `*.tool.json` list (used to extract the
    wire names per category). `load_tool_catalog_frame()` is called
    on `source_path` to load and resolve the section fields."""
    doc = load_tool_catalog_frame(source_path)

    def wire_names_for(categories: tuple[str, ...]) -> list[str]:
        return sorted(
            d.schema_wire_name for d in decls
            if d.category_snake in categories
        )

    browser_wire_names = wire_names_for(("browser_tool",))
    file_shell_wire_names = wire_names_for(("file_tool", "process_tool"))

    def render_array(names: list[str]) -> str:
        return "\n".join(
            f"        {cpp_string_literal(name)}," for name in names
        )

    out_path = output_dir / "tool_catalog_frame.gen.h"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        TOOL_CATALOG_FRAME_TEMPLATE.format(
            source_relpath=str(source_path).replace("\\", "/"),
            browser_content_literal=_emit_raw_string_literal(
                doc["browser_tools"]),
            file_shell_content_literal=_emit_raw_string_literal(
                doc["file_shell_tools"]),
            task_tracking_literal=_emit_raw_string_literal(
                doc["task_tracking"]),
            skills_literal=_emit_raw_string_literal(doc["skills"]),
            parallelism_literal=_emit_raw_string_literal(
                doc["parallelism"]),
            browser_tool_count=len(browser_wire_names),
            browser_tool_wire_names=render_array(browser_wire_names),
            file_shell_tool_count=len(file_shell_wire_names),
            file_shell_tool_wire_names=render_array(file_shell_wire_names),
        ),
        encoding="utf-8",
    )
    return out_path


# =============================================================================
# JSON Schema validation (opt-in, requires the `jsonschema` pip package)
# =============================================================================

def maybe_validate(decl_path: Path, schema_path: Path) -> None:
    try:
        import jsonschema  # type: ignore
    except ImportError:
        # Soft-fail: keep codegen dependency-free, just emit a note.
        print(
            f"[generate.py] jsonschema not installed — skipping validation "
            f"of {decl_path.name}. (pip install jsonschema to enable.)",
            file=sys.stderr,
        )
        return
    with schema_path.open("r", encoding="utf-8") as f:
        schema = json.load(f)
    with decl_path.open("r", encoding="utf-8") as f:
        instance = json.load(f)
    jsonschema.validate(instance=instance, schema=schema)


# =============================================================================
# Bridge-API codegen — *.api.json
#
# Parallel pipeline to the *.tool.json one above. Each *.api.json
# declares ONE bridge handler (a "wire action" like fs/stat or
# llm/streamStart). Codegen emits a per-category abstract base
# (`<Category>ApiBase`) that:
#   (a) inherits NexusBridgeCapabilityHandler,
#   (b) declares one typed input struct + Handle*() pure virtual per
#       action,
#   (c) wires every dispatcher in its ctor body. The dispatcher
#       extracts the typed input, manufactures a BridgeResponseCallback,
#       PostTasks to the declared thread (if not "ui"/"any"), and
#       invokes the virtual.
#
# Schema mirrors tool_schema.json:
#     name / description / inputs[] / runtime
# `inputs[]` carries the same {name, type, required, description}
# shape, plus a `passthrough_payload` pseudo-type for actions whose
# payload is opaque and not worth enumerating field-by-field.
#
# Concrete adapters (NexusFsBridgeAdapter, NexusLlmBridgeAdapter)
# inherit the generated base and only override the per-action methods.
# Their signature is uniform across all shapes:
#
#     void Handle<Name>On<Thread>(<Name>Input input,
#                                 std::vector<MessagePortDescriptor> ports,
#                                 BridgeResponseCallback done) override;
# =============================================================================

# Pseudo-type. The whole payload dict gets cloned into the field. Implies
# required=true.
API_PASSTHROUGH_TYPE = "passthrough_payload"


@dataclass
class ApiActionDecl:
    name: str                  # camelCase wire-format action name
    description: str
    inputs: list[InputDecl] = field(default_factory=list)
    runtime_thread: str = "ui"
    # Optional override for the generated C++ input-struct name. All API
    # input structs share one ::nexus::generated namespace, so a generic
    # action name ("getStatus" → GetStatusInput) can collide across
    # categories. Set this to a globally-unique PascalCase name (WITHOUT the
    # trailing "Input", which is appended) when the derived name would be
    # too generic, e.g. remote_access's getStatus → struct_name
    # "RemoteAccessGetStatus" → RemoteAccessGetStatusInput. The WIRE action
    # name (`name`) is unaffected.
    struct_name: str = ""
    # Inferred from path:
    category_snake: str = ""   # "fs" / "llm"
    category_pascal: str = ""  # "Fs" / "Llm"

    @property
    def virtual_name(self) -> str:
        """fs.stat (thread_pool)        → HandleStatOnThreadPool
        fs.readFile (thread_pool)        → HandleReadFileOnThreadPool
        llm.streamStart (ui)             → HandleStreamStartOnUI
        """
        suffix = THREAD_METHOD_SUFFIX_BY_VALUE[self.runtime_thread]
        return f"Handle{to_pascal_case_camel(self.name)}{suffix}"

    @property
    def input_struct_name(self) -> str:
        """stat → StatInput, readFileBytes → ReadFileBytesInput. Honors a
        struct_name override (returns it verbatim + "Input")."""
        if self.struct_name:
            return f"{self.struct_name}Input"
        return f"{to_pascal_case_camel(self.name)}Input"

    @property
    def extractor_name(self) -> str:
        """stat → ExtractStatInput. The free function the dispatcher
        calls before invoking the virtual. Derived from the (possibly
        overridden) struct name so it stays unique alongside it."""
        return f"Extract{self.input_struct_name}"


def to_pascal_case_camel(camel: str) -> str:
    """streamStart → StreamStart, readFileBytes → ReadFileBytes."""
    return camel[:1].upper() + camel[1:] if camel else camel


def parse_api_declaration(decl_path: Path) -> ApiActionDecl:
    """Read one *.api.json into a typed ApiActionDecl. Mirrors
    parse_declaration for tools — uses the same inputs[] schema."""
    with decl_path.open("r", encoding="utf-8") as f:
        raw = json.load(f)

    for required in ("name", "description", "inputs"):
        if required not in raw:
            raise ValueError(f"{decl_path}: missing required field '{required}'")
    if not isinstance(raw["inputs"], list):
        raise ValueError(f"{decl_path}: 'inputs' must be an array")

    expected_name = decl_path.stem.removesuffix(".api")
    if raw["name"] != expected_name:
        raise ValueError(
            f"{decl_path}: 'name' field is '{raw['name']}' but filename "
            f"declares '{expected_name}'. Rename one to match."
        )

    inputs: list[InputDecl] = []
    valid_types = set(CPP_TYPE_BY_KIND) | {API_PASSTHROUGH_TYPE, "bytes"}
    for i, raw_input in enumerate(raw["inputs"]):
        if not isinstance(raw_input, dict):
            raise ValueError(f"{decl_path}: inputs[{i}] must be an object")
        for k in ("name", "type"):
            if k not in raw_input:
                raise ValueError(
                    f"{decl_path}: inputs[{i}] missing required '{k}'")
        if raw_input["type"] not in valid_types:
            raise ValueError(
                f"{decl_path}: inputs[{i}].type '{raw_input['type']}' is not "
                f"a known kind. Valid: {sorted(valid_types)}"
            )
        # passthrough_payload is implicitly always-required (the bridge
        # always delivers a payload; the field captures the whole dict).
        is_passthrough = raw_input["type"] == API_PASSTHROUGH_TYPE
        required = (
            True if is_passthrough else bool(raw_input.get("required", False))
        )
        inputs.append(
            InputDecl(
                name=raw_input["name"],
                type_kind=raw_input["type"],
                required=required,
                description=raw_input.get("description", ""),
            )
        )

    runtime = raw.get("runtime", {}) or {}
    if not isinstance(runtime, dict):
        raise ValueError(f"{decl_path}: 'runtime' must be an object")
    thread = runtime.get("thread", "ui")
    # API actions dispatch SYNCHRONOUSLY on UI. Off-UI routing ("io" /
    # "thread_pool") used to bind the capability adapter onto a worker via
    # base::Unretained, which is a use-after-free when the port service
    # tears down mid-op (panel close / webview reload) — the same class of
    # bug as the Tools pipeline. An adapter that needs blocking work hops it
    # inside its own body with PostTaskAndReplyWithResult, binding the work
    # to refcounted/owned state (NOT the adapter) and replying weak-guarded;
    # see api/fs/nexus_fs_bridge_adapter.cc. So only "ui" / "any" are valid.
    if thread not in ("ui", "any"):
        raise ValueError(
            f"{decl_path}: runtime.thread '{thread}' is not allowed for an "
            f"API action — only 'ui' or 'any' (synchronous) are. Off-UI work "
            f"must be hopped inside the handler body via "
            f"PostTaskAndReplyWithResult bound to refcounted/owned state (not "
            f"the adapter); see api/fs/nexus_fs_bridge_adapter.cc and "
            f"docs/agents/nexus-tools-api.md §6.")

    struct_name = raw.get("struct_name", "")
    if not isinstance(struct_name, str):
        raise ValueError(f"{decl_path}: 'struct_name' must be a string")
    if struct_name and not struct_name[:1].isupper():
        raise ValueError(
            f"{decl_path}: 'struct_name' must be PascalCase (start with an "
            f"uppercase letter); got {struct_name!r}. Omit the trailing "
            f"'Input' — it is appended."
        )

    return ApiActionDecl(
        name=raw["name"],
        description=raw["description"],
        inputs=inputs,
        runtime_thread=thread,
        struct_name=struct_name,
    )


def infer_api_implementation(decl: ApiActionDecl, decl_path: Path) -> None:
    """Populate category_{snake,pascal} from the declarations/<cat>/<name>.api.json
    folder layout. The category becomes the C++ class prefix, e.g.
    declarations/fs/stat.api.json → FsApiBase."""
    parent = decl_path.parent.name
    if not parent or parent == "declarations":
        raise ValueError(
            f"{decl_path}: must live in declarations/<category>/<name>.api.json. "
            f"Got parent folder '{parent}' which is not a valid category."
        )
    decl.category_snake = parent
    decl.category_pascal = to_pascal_case(parent)


def _api_cpp_type(input_decl: InputDecl) -> str:
    """Map an API input's declared type to the C++ field type. Adds two
    API-only pseudo-types on top of the tools-side mapping:
      - `passthrough_payload` → base::DictValue (always populated)
      - `bytes`               → std::vector<uint8_t> (always populated;
                                 the dispatcher races inline-vs-port to
                                 ensure it's filled before invoking the
                                 implementation)
    """
    if input_decl.type_kind == API_PASSTHROUGH_TYPE:
        return "base::DictValue"
    if input_decl.type_kind == "bytes":
        # bytes is always-populated by construction (the dispatcher
        # awaits the port if inline isn't present). No std::optional
        # wrap regardless of `required` — required=true is the only
        # supported shape.
        return "std::vector<uint8_t>"
    base = CPP_TYPE_BY_KIND[input_decl.type_kind]
    return base if input_decl.required else f"std::optional<{base}>"


def _emit_api_input_struct(decl: ApiActionDecl) -> str:
    """Per-action input struct. Goes in the per-category .gen.h so the
    implementation can spell its parameter type."""
    if not decl.inputs:
        # An empty struct is the simplest match for "no inputs". Avoids
        # needing a special case in the implementation signature.
        return (
            f"// {decl.input_struct_name} — wire action \"{decl.name}\" "
            f"takes no extracted inputs.\n"
            f"struct {decl.input_struct_name} {{}};\n"
        )

    field_lines: list[str] = []
    for inp in decl.inputs:
        comment = inp.description.strip()
        if comment:
            field_lines.extend(_doc_comment_lines(comment))
        field_lines.append(f"  {_api_cpp_type(inp)} {inp.name};")
    fields = "\n".join(field_lines)
    return (
        f"// {decl.input_struct_name} — extracted from the wire payload "
        f"for action \"{decl.name}\".\n"
        f"struct {decl.input_struct_name} {{\n"
        f"{fields}\n"
        f"}};\n"
    )


def _emit_api_extractor_decl(decl: ApiActionDecl) -> str:
    """Forward declaration of the extractor. Returns std::nullopt on
    success, an error message string on validation failure."""
    return (
        f"std::optional<std::string> {decl.extractor_name}(\n"
        f"    const base::DictValue& payload,\n"
        f"    {decl.input_struct_name}& out);\n"
    )


def _emit_api_extractor_def(decl: ApiActionDecl) -> str:
    """Definition of the extractor. Walks each input and pulls it from
    the payload dict, returning the first error message it hits."""
    body_lines: list[str] = []
    for inp in decl.inputs:
        body_lines.append(_emit_api_extract_input(inp))
    if not body_lines:
        body_lines.append("  // No inputs to extract.\n")
    body = "\n".join(body_lines)
    return (
        f"std::optional<std::string> {decl.extractor_name}(\n"
        f"    const base::DictValue& payload,\n"
        f"    {decl.input_struct_name}& out) {{\n"
        f"{body}\n"
        f"  return std::nullopt;\n"
        f"}}\n"
    )


def _emit_api_extract_input(inp: InputDecl) -> str:
    """Per-input extraction snippet. Indented for the extractor body."""
    name = inp.name
    quoted = f'"{name}"'
    miss = f'"Missing required field \\"{name}\\""'
    bad_type = f'"Field \\"{name}\\" has wrong type"'

    if inp.type_kind == API_PASSTHROUGH_TYPE:
        # The whole payload dict — always succeeds, just clone it.
        return f"  out.{name} = payload.Clone();\n"

    if inp.type_kind == "bytes":
        # Inline-bytes path: payload may carry the bytes as a JSON
        # array of integers. If absent, leave the field empty — the
        # dispatcher will fall back to the binary-port path before
        # invoking the implementation. Either way the implementation
        # receives a populated vector.
        return (
            f"  if (const base::ListValue* l = payload.FindList({quoted}); "
            f"l) {{\n"
            f"    out.{name}.reserve(l->size());\n"
            f"    for (const base::Value& v : *l) {{\n"
            f"      if (auto i = v.GetIfInt()) {{\n"
            f"        out.{name}.push_back("
            f"static_cast<uint8_t>(std::clamp(*i, 0, 255)));\n"
            f"      }} else if (auto d = v.GetIfDouble()) {{\n"
            f"        out.{name}.push_back("
            f"static_cast<uint8_t>(std::clamp("
            f"static_cast<int>(*d), 0, 255)));\n"
            f"      }}\n"
            f"    }}\n"
            f"  }}\n"
        )

    if inp.type_kind == "string":
        if inp.required:
            return (
                f"  if (const std::string* p = payload.FindString({quoted}); "
                f"p) {{\n"
                f"    out.{name} = *p;\n"
                f"  }} else {{\n"
                f"    return {miss};\n"
                f"  }}\n"
            )
        return (
            f"  if (const std::string* p = payload.FindString({quoted}); p) {{\n"
            f"    out.{name} = *p;\n"
            f"  }}\n"
        )

    if inp.type_kind == "integer":
        # bridge_support::FindIntegralValue handles JS-side double-encoded
        # integers, returns std::optional<int64_t>. We checked_cast down
        # to int32_t to match CPP_TYPE_BY_KIND's "integer" → int32_t.
        if inp.required:
            return (
                f"  if (auto v = ::nexus::bridge_support::FindIntegralValue("
                f"payload, {quoted}); v) {{\n"
                f"    out.{name} = base::checked_cast<int32_t>(*v);\n"
                f"  }} else {{\n"
                f"    return {miss};\n"
                f"  }}\n"
            )
        return (
            f"  if (auto v = ::nexus::bridge_support::FindIntegralValue("
            f"payload, {quoted}); v) {{\n"
            f"    out.{name} = base::checked_cast<int32_t>(*v);\n"
            f"  }}\n"
        )

    if inp.type_kind == "number":
        if inp.required:
            return (
                f"  if (auto v = payload.FindDouble({quoted}); v) {{\n"
                f"    out.{name} = *v;\n"
                f"  }} else {{\n"
                f"    return {miss};\n"
                f"  }}\n"
            )
        return (
            f"  if (auto v = payload.FindDouble({quoted}); v) {{\n"
            f"    out.{name} = *v;\n"
            f"  }}\n"
        )

    if inp.type_kind == "boolean":
        if inp.required:
            return (
                f"  if (auto v = payload.FindBool({quoted}); v) {{\n"
                f"    out.{name} = *v;\n"
                f"  }} else {{\n"
                f"    return {miss};\n"
                f"  }}\n"
            )
        return (
            f"  if (auto v = payload.FindBool({quoted}); v) {{\n"
            f"    out.{name} = *v;\n"
            f"  }}\n"
        )

    if inp.type_kind == "object":
        if inp.required:
            return (
                f"  if (const base::DictValue* d = payload.FindDict({quoted}); "
                f"d) {{\n"
                f"    out.{name} = d->Clone();\n"
                f"  }} else {{\n"
                f"    return {miss};\n"
                f"  }}\n"
            )
        return (
            f"  if (const base::DictValue* d = payload.FindDict({quoted}); "
            f"d) {{\n"
            f"    out.{name} = d->Clone();\n"
            f"  }}\n"
        )

    if inp.type_kind in ("array<string>", "array<integer>", "array<number>",
                         "array<object>"):
        # For arrays, the implementation typically wants the raw List
        # to feed into bridge_support helpers (e.g. ValueListToBytes for
        # byte arrays). We expose the list as a typed C++ vector, but
        # the type detail differs per kind. For now: extract via FindList
        # + a per-kind conversion shim.
        if inp.type_kind == "array<integer>":
            convert = (
                f"std::vector<int32_t> tmp; tmp.reserve(l->size()); "
                f"for (const base::Value& v : *l) {{ "
                f"if (auto i = v.GetIfInt()) tmp.push_back(*i); "
                f"else if (auto d = v.GetIfDouble()) "
                f"tmp.push_back(base::checked_cast<int32_t>(*d)); }} "
                f"out.{name} = std::move(tmp);"
            )
        elif inp.type_kind == "array<number>":
            convert = (
                f"std::vector<double> tmp; tmp.reserve(l->size()); "
                f"for (const base::Value& v : *l) {{ "
                f"if (auto d = v.GetIfDouble()) tmp.push_back(*d); }} "
                f"out.{name} = std::move(tmp);"
            )
        elif inp.type_kind == "array<string>":
            convert = (
                f"std::vector<std::string> tmp; tmp.reserve(l->size()); "
                f"for (const base::Value& v : *l) {{ "
                f"if (const std::string* s = v.GetIfString()) "
                f"tmp.push_back(*s); }} "
                f"out.{name} = std::move(tmp);"
            )
        else:  # array<object>
            convert = (
                f"std::vector<base::DictValue> tmp; tmp.reserve(l->size()); "
                f"for (const base::Value& v : *l) {{ "
                f"if (const base::DictValue* d = v.GetIfDict()) "
                f"tmp.push_back(d->Clone()); }} "
                f"out.{name} = std::move(tmp);"
            )
        if inp.required:
            return (
                f"  if (const base::ListValue* l = payload.FindList({quoted}); "
                f"l) {{\n"
                f"    {convert}\n"
                f"  }} else {{\n"
                f"    return {miss};\n"
                f"  }}\n"
            )
        return (
            f"  if (const base::ListValue* l = payload.FindList({quoted}); "
            f"l) {{\n"
            f"    {convert}\n"
            f"  }}\n"
        )

    raise ValueError(f"Unhandled type_kind '{inp.type_kind}' in API extractor")


def _emit_api_virtual(decl: ApiActionDecl) -> str:
    """Pure virtual declaration. Uniform signature across all actions:
    (InputT input, ports, BridgeResponseCallback done)."""
    return (
        f"  // wire action \"{decl.name}\", "
        f"runtime.thread=\"{decl.runtime_thread}\"\n"
        f"  // {decl.description}\n"
        f"  virtual void {decl.virtual_name}(\n"
        f"      {decl.input_struct_name} input,\n"
        f"      std::vector<blink::MessagePortDescriptor> ports,\n"
        f"      ::nexus::BridgeResponseCallback done) = 0;\n"
    )


def _emit_api_invoke_block(
    decl: ApiActionDecl, base_class_name: str, indent: str
) -> str:
    """Emit the C++ that invokes the implementation virtual. The given
    `indent` is the leading whitespace for each line (the wrapping context
    can be the dispatcher's lambda body or the bytes-await continuation,
    with different indentation).

    API actions always dispatch SYNCHRONOUSLY on the dispatcher's own
    sequence (UI) — parse_api_declaration rejects io / thread_pool, because
    posting a port-service adapter off-UI via base::Unretained is a
    use-after-free on teardown. An adapter that needs blocking work hops it
    inside its body (see api/fs/nexus_fs_bridge_adapter.cc), so this emitter
    never PostTasks the adapter onto another sequence."""
    del base_class_name  # only needed by the removed off-UI branches
    return (
        f"{indent}self_raw->{decl.virtual_name}(\n"
        f"{indent}    std::move(input),\n"
        f"{indent}    std::move(ports),\n"
        f"{indent}    std::move(done));\n"
    )


def _emit_api_register_call(decl: ApiActionDecl, base_class_name: str) -> str:
    """Emit the RegisterHandler block for ONE action.

    Three modes, all via RegisterHandler (the typed Register overloads
    are unused now — extraction and response are codegen-managed):

    - **No `bytes` inputs:** extract the typed input synchronously,
      then invoke the virtual on the declared thread.

    - **One `bytes` input, populated inline:** same as above — the
      extractor already filled the field from the payload's
      array<integer>, so we proceed straight to dispatch.

    - **One `bytes` input, NOT inline:** the dispatcher takes the first
      transferred MessagePort and calls `AwaitBinaryFromPort`. When the
      port delivers an ArrayBuffer (or disconnects), the continuation
      fires on UI: it populates the bytes field on the input struct
      and PostTasks the dispatch to the target thread (or fires
      done.InvalidArgs on disconnect).

    Multiple bytes inputs per action aren't currently supported —
    enforced at codegen time.

    The implementation always sees `(InputT input, ports, done)` with
    every field populated; it never knows whether the bytes came inline
    or via a port.
    """
    thread = decl.runtime_thread

    # Find any `bytes` inputs. We support 0 or 1 per action.
    bytes_inputs = [inp for inp in decl.inputs if inp.type_kind == "bytes"]
    if len(bytes_inputs) > 1:
        raise ValueError(
            f"action '{decl.name}' declares {len(bytes_inputs)} `bytes` "
            f"inputs; codegen currently supports at most one"
        )

    # ---- Body shared across all modes -------------------------------------
    # The dispatcher always:
    #   1. constructs `done`,
    #   2. runs the extractor (which silently leaves any `bytes` field
    #      empty if not inline),
    #   3. THEN either dispatches directly or kicks off a port-await.
    common_prelude = (
        f"        ::nexus::BridgeResponseCallback done(self->AsWeakPtr(), id, "
        f"route.Clone());\n"
        f"        {decl.input_struct_name} input;\n"
        f"        if (auto err = ::nexus::generated::{decl.extractor_name}("
        f"payload, input); err) {{\n"
        f"          std::move(done).InvalidArgs(*err);\n"
        f"          return;\n"
        f"        }}\n"
    )

    if not bytes_inputs:
        # ---- Plain path: just dispatch ------------------------------------
        invoke = _emit_api_invoke_block(decl, base_class_name, indent="        ")
        body = (
            f"{common_prelude}"
            f"        {base_class_name}* self_raw = self;\n"
            f"{invoke}"
        )
    else:
        # ---- Bytes path: inline-or-port resolution -----------------------
        bytes_field = bytes_inputs[0].name
        # Continuation invoke (with one extra level of indent inside the
        # BindOnce'd lambda).
        await_invoke = _emit_api_invoke_block(
            decl, base_class_name, indent="            ")
        # Inline-path invoke (same indent as the regular dispatcher body).
        inline_invoke = _emit_api_invoke_block(
            decl, base_class_name, indent="        ")

        body = (
            f"{common_prelude}"
            f"        {base_class_name}* self_raw = self;\n"
            f"        // bytes input \"{bytes_field}\" — if extractor "
            f"didn't populate it from\n"
            f"        // the payload's inline array<integer>, fall back "
            f"to a transferred\n"
            f"        // MessagePort. Either way the implementation "
            f"receives populated bytes.\n"
            f"        if (!input.{bytes_field}.empty()) {{\n"
            f"{inline_invoke}"
            f"          return;\n"
            f"        }}\n"
            f"        if (ports.empty()) {{\n"
            f"          std::move(done).InvalidArgs(\n"
            f"              \"Missing required field "
            f"\\\"{bytes_field}\\\" "
            f"(inline or via port)\");\n"
            f"          return;\n"
            f"        }}\n"
            f"        blink::MessagePortDescriptor port = "
            f"std::move(ports.front());\n"
            f"        ports.erase(ports.begin());\n"
            f"        self->AwaitBinaryFromPort(\n"
            f"            std::move(port),\n"
            f"            base::BindOnce(\n"
            f"                []({base_class_name}* self_raw,\n"
            f"                   {decl.input_struct_name} input,\n"
            f"                   std::vector<blink::MessagePortDescriptor> "
            f"ports,\n"
            f"                   ::nexus::BridgeResponseCallback done,\n"
            f"                   ::nexus::BinaryReceiveResult result) {{\n"
            f"                  if (!result.ok) {{\n"
            f"                    std::move(done).InvalidArgs(result.error);\n"
            f"                    return;\n"
            f"                  }}\n"
            f"                  input.{bytes_field} = "
            f"std::move(result.bytes);\n"
            f"{await_invoke}"
            f"                }},\n"
            f"                base::Unretained(self),\n"
            f"                std::move(input),\n"
            f"                std::move(ports),\n"
            f"                std::move(done)));\n"
        )

    return (
        f"  // wire action \"{decl.name}\", "
        f"runtime.thread=\"{thread}\"\n"
        f"  RegisterHandler(\n"
        f"      \"{decl.name}\",\n"
        f"      [self = this](const std::string& id,\n"
        f"                    const base::DictValue& route,\n"
        f"                    const base::DictValue& payload,\n"
        f"                    std::vector<blink::MessagePortDescriptor> ports) {{\n"
        f"{body}"
        f"      }});\n"
    )


def emit_api_category(
    category_snake: str,
    decls: list[ApiActionDecl],
    output_dir: Path,
) -> tuple[Path, Path]:
    """Write `<category>_api.gen.h` + `<category>_api.gen.cc` under
    output_dir/<category>/. Returns (header_path, cc_path).

    Layout:
      .gen.h:
        - One InputT struct per action.
        - Forward-declared extractor (Extract<Name>Input).
        - <Category>ApiBase abstract class with one Handle*() virtual
          per action; uniform signature
          (InputT input, ports, BridgeResponseCallback done).

      .gen.cc:
        - Per-action extractor definitions.
        - <Category>ApiBase ctor that calls RegisterHandler(...) for
          every action, with an inline lambda that constructs the
          BridgeResponseCallback, runs the extractor, and invokes the
          virtual on the declared thread.
    """
    category_pascal = to_pascal_case(category_snake)
    base_class = f"{category_pascal}ApiBase"
    guard = (
        f"GEN_NEXUS_API_{category_snake.upper()}_"
        f"{category_snake.upper()}_API_H_"
    )

    # Sort by name (lowercase) for deterministic output.
    decls_sorted = sorted(decls, key=lambda d: d.name.lower())

    input_structs = "\n".join(_emit_api_input_struct(d) for d in decls_sorted)
    extractor_decls = "\n".join(
        _emit_api_extractor_decl(d) for d in decls_sorted
    )
    virtuals = "\n".join(_emit_api_virtual(d) for d in decls_sorted)

    extractor_defs = "\n\n".join(
        _emit_api_extractor_def(d) for d in decls_sorted
    )
    register_calls = "".join(
        _emit_api_register_call(d, base_class) for d in decls_sorted
    )

    header = f"""\
// AUTO-GENERATED FROM: api/declarations/{category_snake}/*.api.json
// DO NOT EDIT THIS FILE. Edit the source declarations instead and re-run
// codegen (or rebuild — GN tracks the dependency).
//
// {category_pascal} bridge-API base class + per-action input structs.
// The base class inherits NexusBridgeCapabilityHandler and declares one
// Handle*() pure virtual per action with the uniform signature
//
//     void Handle<Name>On<Thread>({category_pascal}<Name>Input input,
//                                 std::vector<MessagePortDescriptor> ports,
//                                 BridgeResponseCallback done);
//
// Concrete adapters inherit this base and override the Handle*()
// methods. The dispatcher (in the ctor body, defined in the .gen.cc)
// extracts the typed input from the wire payload, manufactures a
// BridgeResponseCallback for the response, and routes to the
// declared thread.

#ifndef {guard}
#define {guard}

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

namespace nexus {{
class NexusBridgeResponseDelegate;
}}  // namespace nexus

namespace nexus::generated {{

// ---- Input structs ---------------------------------------------------------

{input_structs}

// ---- Extractor forward declarations ---------------------------------------
// Each turns a wire payload dict into a typed input struct. Returns
// std::nullopt on success, or an error message on validation failure
// (the dispatcher routes it into done.InvalidArgs).

{extractor_decls}

// ---- Abstract base class --------------------------------------------------

class {base_class} : public ::nexus::NexusBridgeCapabilityHandler {{
 public:
  explicit {base_class}(::nexus::NexusBridgeResponseDelegate* delegate);
  ~{base_class}() override;

  {base_class}(const {base_class}&) = delete;
  {base_class}& operator=(const {base_class}&) = delete;

 protected:
{virtuals}
}};

}}  // namespace nexus::generated

#endif  // {guard}
"""

    # Conditional .gen.cc includes — only pull in thread headers when
    # at least one action needs them, plus base::checked_cast for any
    # integer extraction.
    threads_used = {d.runtime_thread for d in decls_sorted}
    needs_int_cast = any(
        inp.type_kind == "integer" for d in decls_sorted for inp in d.inputs
    )
    needs_int_helper = any(
        inp.type_kind == "integer" for d in decls_sorted for inp in d.inputs
    )

    cc_extra_includes: list[str] = ['#include <optional>',
                                    '#include <utility>',
                                    '#include <vector>']
    cc_extra_includes.append('#include "base/functional/bind.h"')
    cc_extra_includes.append('#include "base/location.h"')
    if needs_int_cast:
        cc_extra_includes.append(
            '#include "base/numerics/safe_conversions.h"')
    if needs_int_helper:
        cc_extra_includes.append(
            '#include "custom_browser/browser/nexus/nexus_bridge_support.h"')
    if "thread_pool" in threads_used:
        cc_extra_includes.append('#include "base/task/task_traits.h"')
        cc_extra_includes.append('#include "base/task/thread_pool.h"')
    if "io" in threads_used:
        cc_extra_includes.append(
            '#include "content/public/browser/browser_thread.h"')
    cc_extra_includes_text = "\n".join(cc_extra_includes)

    cc = f"""\
// AUTO-GENERATED FROM: api/declarations/{category_snake}/*.api.json
// DO NOT EDIT THIS FILE. Edit the source declarations instead and re-run
// codegen (or rebuild — GN tracks the dependency).

#include "custom_browser/browser/nexus/api/{category_snake}/{category_snake}_api.gen.h"

{cc_extra_includes_text}

namespace nexus::generated {{

// ---- Per-action extractor definitions -------------------------------------

{extractor_defs}

// ---- Base class implementation --------------------------------------------

{base_class}::{base_class}(::nexus::NexusBridgeResponseDelegate* delegate)
    : ::nexus::NexusBridgeCapabilityHandler(delegate) {{
{register_calls}}}

{base_class}::~{base_class}() = default;

}}  // namespace nexus::generated
"""

    cat_dir = output_dir / category_snake
    cat_dir.mkdir(parents=True, exist_ok=True)
    h_path = cat_dir / f"{category_snake}_api.gen.h"
    cc_path = cat_dir / f"{category_snake}_api.gen.cc"
    h_path.write_text(header, encoding="utf-8")
    cc_path.write_text(cc, encoding="utf-8")
    return h_path, cc_path


def discover_api_declarations(api_declarations_dir: Path) -> Iterable[Path]:
    """Yield every *.api.json under api_declarations_dir, sorted.

    Mirrors discover_declarations for tools. Skips silently when the
    directory doesn't exist so the build works even before any API
    declarations have been authored.
    """
    if not api_declarations_dir.is_dir():
        return
    for path in sorted(api_declarations_dir.rglob("*.api.json")):
        yield path


def validate_api(api_path: Path, schema_path: Path) -> None:
    """Validate one *.api.json against api_schema.json. Soft-fails when
    `jsonschema` isn't installed (matches `maybe_validate` for tools)."""
    try:
        import jsonschema  # type: ignore
    except ImportError:
        print(
            f"[generate.py] jsonschema not installed — skipping validation "
            f"of {api_path.name}. (pip install jsonschema to enable.)",
            file=sys.stderr,
        )
        return
    with schema_path.open("r", encoding="utf-8") as f:
        schema = json.load(f)
    with api_path.open("r", encoding="utf-8") as f:
        instance = json.load(f)
    jsonschema.validate(instance=instance, schema=schema)


# =============================================================================
# Discovery + main
# =============================================================================

def discover_declarations(declarations_dir: Path) -> Iterable[Path]:
    """Yield every *.tool.json under declarations_dir, sorted for
    deterministic output."""
    for path in sorted(declarations_dir.rglob("*.tool.json")):
        yield path


def discover_skills(declarations_dir: Path) -> Iterable[Path]:
    """Yield every *.skill.json under declarations_dir/skills/, sorted.

    Skills are scoped to a `declarations/skills/` subdirectory so they
    can't be confused with tool declarations; the file extensions are
    distinct anyway (`*.skill.json` vs `*.tool.json`) but keeping the
    layout strict makes the file tree easier to read.
    """
    skills_dir = declarations_dir / "skills"
    if not skills_dir.is_dir():
        return
    for path in sorted(skills_dir.glob("*.skill.json")):
        yield path


def validate_skill(skill_path: Path, schema_path: Path) -> None:
    """Validate one *.skill.json against skill_schema.json. Soft-fails
    when `jsonschema` isn't installed so codegen stays dependency-free
    in stripped-down environments (matches `maybe_validate` for tools).

    Skill files are also checked for filename↔name consistency so a
    rename of one without the other doesn't silently produce two
    different identifiers for the same skill.
    """
    with skill_path.open("r", encoding="utf-8") as f:
        instance = json.load(f)

    expected_name = skill_path.stem.removesuffix(".skill")
    if instance.get("name") != expected_name:
        raise ValueError(
            f"{skill_path}: 'name' field is '{instance.get('name')}' but "
            f"filename declares '{expected_name}'. Rename one to match."
        )

    try:
        import jsonschema  # type: ignore
    except ImportError:
        print(
            f"[generate.py] jsonschema not installed — skipping validation "
            f"of {skill_path.name}. (pip install jsonschema to enable.)",
            file=sys.stderr,
        )
        return
    with schema_path.open("r", encoding="utf-8") as f:
        schema = json.load(f)
    jsonschema.validate(instance=instance, schema=schema)


# =============================================================================
# Skill registry emission — `nexus_skills_registry.gen.{h,cc}`.
#
# Reads every *.skill.json into a SkillDecl and emits an `AllSkills()`
# accessor returning a vector<::nexus::SkillSchema>. The catalog adapter
# (`catalog.listSkills`) consumes this and derives each skill's toolNames
# from the tool schemas (whose `skill` field links membership). See
# giggly-watching-pie.md Phase 4.
# =============================================================================

@dataclass
class SkillDecl:
    name: str
    description: str
    instructions: str = ""
    # No deferral marker — whether a skill defers is decided SOLELY by the
    # nexus_web session template's `deferredTools` (resolveDeferrableSkills).


def parse_skill_declaration(path: Path) -> SkillDecl:
    with path.open("r", encoding="utf-8") as f:
        raw = json.load(f)
    # `description` and `instructions` accept the `@file:<path>` reference
    # form (resolved relative to this skill.json's directory) so long prose
    # can live in clean .md files instead of inlined JSON string literals —
    # the same mechanism the tool-catalog frame uses. Inline strings are
    # returned unchanged, so skills that don't use a reference are unaffected.
    return SkillDecl(
        name=raw["name"],
        description=_resolve_prose_field(
            raw["description"], path.parent, f"{path.name}:description"),
        instructions=_resolve_prose_field(
            raw.get("instructions", ""), path.parent,
            f"{path.name}:instructions"),
    )


SKILL_REGISTRY_HEADER_TEMPLATE = """\
// AUTO-GENERATED. Combined registry of all *.skill.json declarations.
// DO NOT EDIT THIS FILE. Edit individual *.skill.json sources instead.

#ifndef GEN_NEXUS_SKILLS_REGISTRY_H_
#define GEN_NEXUS_SKILLS_REGISTRY_H_

#include <vector>

#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

namespace nexus::generated {

// Returns one `SkillSchema` per registered *.skill.json declaration.
// Order is deterministic (sorted by skill name).
std::vector<::nexus::SkillSchema> AllSkills();

}  // namespace nexus::generated

#endif  // GEN_NEXUS_SKILLS_REGISTRY_H_
"""


SKILL_REGISTRY_SOURCE_TEMPLATE = """\
// AUTO-GENERATED. Combined registry of all *.skill.json declarations.
// DO NOT EDIT THIS FILE. Edit individual *.skill.json sources instead.

#include "{registry_header_relpath}"

#include <utility>

namespace nexus::generated {{

std::vector<::nexus::SkillSchema> AllSkills() {{
  std::vector<::nexus::SkillSchema> skills;
  skills.reserve({skill_count});
{skill_pushes}  return skills;
}}

}}  // namespace nexus::generated
"""


def render_skill_push(decl: SkillDecl) -> str:
    """Emit one braced block that populates + pushes a SkillSchema."""
    lines: list[str] = []
    lines.append("  {")
    lines.append("    ::nexus::SkillSchema s;")
    lines.append(f'    s.name = "{decl.name}";')
    lines.append("    s.description =")
    lines.append(f"        {cpp_string_literal(decl.description)};")
    if decl.instructions:
        lines.append("    s.instructions =")
        lines.append(f"{_emit_raw_string_literal(decl.instructions)};")
    lines.append("    skills.push_back(std::move(s));")
    lines.append("  }")
    return "\n".join(lines) + "\n"


def emit_skill_registry(
    skill_decls: list[SkillDecl],
    output_dir: Path,
    registry_header_relpath: str,
) -> tuple[Path, Path]:
    """Write nexus_skills_registry.gen.{h,cc}. Returns (h, cc)."""
    ordered = sorted(skill_decls, key=lambda d: d.name)
    skill_pushes = "".join(render_skill_push(d) for d in ordered)

    h_path = output_dir / "nexus_skills_registry.gen.h"
    cc_path = output_dir / "nexus_skills_registry.gen.cc"
    h_path.parent.mkdir(parents=True, exist_ok=True)
    h_path.write_text(SKILL_REGISTRY_HEADER_TEMPLATE, encoding="utf-8")
    cc_path.write_text(
        SKILL_REGISTRY_SOURCE_TEMPLATE.format(
            registry_header_relpath=registry_header_relpath,
            skill_count=len(ordered),
            skill_pushes=skill_pushes,
        ),
        encoding="utf-8",
    )
    return h_path, cc_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Codegen Nexus tool headers from *.tool.json declarations.")
    parser.add_argument(
        "--declarations-dir",
        type=Path,
        required=True,
        help="Root directory containing <category>/<name>.tool.json files.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        required=True,
        help="Output directory for generated headers.",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Validate each declaration against tool_schema.json. Requires "
             "the `jsonschema` pip package; falls back to skip-with-warning.",
    )
    parser.add_argument(
        "--single",
        type=Path,
        help="Process only this single .tool.json (for debugging).",
    )
    # Bridge-API codegen — independent pipeline. Optional; codegen runs
    # without it when these args are absent (used for tools-only debug
    # runs or before any .api.json files exist).
    parser.add_argument(
        "--api-declarations-dir",
        type=Path,
        default=None,
        help="Root directory containing <category>/<name>.api.json files. "
             "When set, codegen also emits <category>_api.gen.{h,cc} per "
             "category. The directory may be missing — empty output is fine.",
    )
    parser.add_argument(
        "--api-output-dir",
        type=Path,
        default=None,
        help="Output directory for API codegen. Required iff "
             "--api-declarations-dir is set. Each category gets a "
             "subdirectory; output paths look like "
             "<api-output-dir>/<category>/<category>_api.gen.{h,cc}.",
    )
    args = parser.parse_args(argv)

    schema_path = Path(__file__).parent / "tool_schema.json"
    skill_schema_path = Path(__file__).parent / "skill_schema.json"
    api_schema_path = Path(__file__).parent / "api_schema.json"

    if args.single is not None:
        decl_paths: list[Path] = [args.single]
    else:
        decl_paths = list(discover_declarations(args.declarations_dir))

    # Skills are discovered alongside tools but currently don't drive
    # any C++ emission — see plan giggly-watching-pie.md (Phase 5+).
    # Validation here catches malformed declarations early; the actual
    # runtime gating ("omit deferred tools from tools[] until the
    # Skill tool activates them") is a JS-bridge change that will
    # land separately and read these files via a TS-side parser.
    skill_paths: list[Path] = []
    if args.single is None:
        skill_paths = list(discover_skills(args.declarations_dir))

    if not decl_paths and not skill_paths:
        print(
            f"[generate.py] No *.tool.json or *.skill.json found under "
            f"{args.declarations_dir}",
            file=sys.stderr,
        )
        return 0

    skill_decls: list[SkillDecl] = []
    if skill_paths:
        print(f"[generate.py] Validating {len(skill_paths)} skill(s)...")
        for skill_path in skill_paths:
            validate_skill(skill_path, skill_schema_path)
            skill_decls.append(parse_skill_declaration(skill_path))
            print(f"  skill: {skill_path.name}")

    print(f"[generate.py] Processing {len(decl_paths)} declaration(s)...")

    decls: list[ToolDecl] = []
    for decl_path in decl_paths:
        if args.validate:
            maybe_validate(decl_path, schema_path)
        decl = parse_declaration(decl_path)
        infer_implementation(decl, decl_path)
        out = emit_header(decl, decl_path, args.output_dir)
        # ASCII-only output for Windows cp1252 console compatibility
        # (Chromium dev hosts often default to legacy code pages).
        print(f"  {decl_path.name} -> {out}")
        decls.append(decl)

    # Per-category aggregate base class: one .gen.h per unique category
    # (e.g. browser_tool.gen.h) that declares the abstract base class
    # holding one pure virtual per tool in the category. Skipped when
    # --single is used (the registry rewrite is also skipped, and
    # debugging-one-tool runs shouldn't drop the per-category aggregate
    # for everyone else's tools).
    if args.single is None:
        by_category: dict[str, list[ToolDecl]] = {}
        for d in decls:
            by_category.setdefault(d.category_snake, []).append(d)
        for category_snake, category_decls in sorted(by_category.items()):
            cat_out = emit_category_header(
                category_snake, category_decls, args.output_dir
            )
            print(f"  category -> {cat_out}")

        # Combined registry: one .gen.h + one .gen.cc that exposes
        # AllSchemas() + PopulateRegistry() across every declaration in
        # the tree.
        h_out, cc_out = emit_registry(
            decls,
            args.output_dir,
            registry_header_relpath=(
                "custom_browser/browser/nexus/tools/nexus_tools_registry.gen.h"
            ),
        )
        print(f"  registry -> {h_out}")
        print(f"  registry -> {cc_out}")

        # Skill registry: one .gen.h + one .gen.cc exposing AllSkills()
        # across every *.skill.json. Emitted even when there are no
        # skills (empty AllSkills()) so the catalog adapter can link
        # against it unconditionally.
        skh_out, skcc_out = emit_skill_registry(
            skill_decls,
            args.output_dir,
            registry_header_relpath=(
                "custom_browser/browser/nexus/tools/"
                "nexus_skills_registry.gen.h"
            ),
        )
        print(f"  skills   -> {skh_out}")
        print(f"  skills   -> {skcc_out}")

        # Phase 13.4 / 13.8 / 13.9: tool-catalog-frame —
        # `tool_catalog_frame.gen.h`. Emits one constexpr string per
        # catalog section (loaded verbatim from a .md) plus the
        # browser-tool and file-shell-tool wire-name arrays the
        # renderer uses for section gating. After 13.9 every .tool.json
        # is purely a tool declaration — none carry catalog-prose
        # `prompt` blocks any more.
        frame_json = args.declarations_dir / "tool-catalog-frame.json"
        if frame_json.exists():
            frame_out = emit_tool_catalog_frame(
                frame_json, args.output_dir, decls
            )
            print(f"  frame    -> {frame_out}")

        # Phase 13.2: system-prompt-tools content —
        # `system_prompt_tools.gen.h`. The source is a single .md file
        # at `tools/declarations/system-prompt-tools.md` (Phase 13.7
        # dropped the JSON wrapper — see the emit-function docstring).
        # Cross-cutting native-tool prose (batching rules, error-
        # recovery cheatsheet, etc.) that previously lived as hand-
        # written prose in the public/.md templates and drifted out-of-
        # date as the tool surface evolved. The renderer at
        # `api/catalog/prompt_section_renderer.cc` surfaces this string
        # via `bridge.catalog.renderPromptSection({name:
        # 'system-prompt-tools'})`; the JS prompt loader substitutes
        # `{{system-prompt-tools}}` in the public/.md templates with
        # the result.
        sys_prompt_md = args.declarations_dir / "system-prompt-tools.md"
        if sys_prompt_md.exists():
            sys_out = emit_system_prompt_tools(
                sys_prompt_md, args.output_dir
            )
            print(f"  sys-prompt -> {sys_out}")

    # -------------------------------------------------------------------
    # Bridge-API codegen — runs after the tool pipeline so the two are
    # cleanly separable. Either may be absent (e.g. --single mode runs
    # only the tool pipeline; absent --api-declarations-dir runs only
    # the tool pipeline).
    # -------------------------------------------------------------------
    if args.api_declarations_dir is not None and args.single is None:
        if args.api_output_dir is None:
            parser.error("--api-output-dir is required when "
                         "--api-declarations-dir is set")

        api_paths = list(discover_api_declarations(args.api_declarations_dir))
        if api_paths:
            print(f"[generate.py] Processing {len(api_paths)} API "
                  f"declaration(s)...")
            api_decls: list[ApiActionDecl] = []
            for api_path in api_paths:
                if args.validate:
                    validate_api(api_path, api_schema_path)
                api_decl = parse_api_declaration(api_path)
                infer_api_implementation(api_decl, api_path)
                api_decls.append(api_decl)
                print(f"  {api_path.parent.name}/{api_path.name}")

            # Group by category and emit one per-category aggregate.
            by_category: dict[str, list[ApiActionDecl]] = {}
            for d in api_decls:
                by_category.setdefault(d.category_snake, []).append(d)
            for category_snake, category_decls in sorted(by_category.items()):
                h_out, cc_out = emit_api_category(
                    category_snake, category_decls, args.api_output_dir
                )
                print(f"  api category -> {h_out}")
                print(f"  api category -> {cc_out}")
        else:
            print(f"[generate.py] No *.api.json found under "
                  f"{args.api_declarations_dir}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
