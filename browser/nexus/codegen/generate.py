#!/usr/bin/env python3
"""Codegen for the Nexus bridge API declarations (*.api.json).

Reads `api/declarations/<category>/<name>.api.json` and emits, per category,
`<category>_api.gen.{h,cc}`: one typed input struct + extractor per action,
and a `<Category>ApiBase` abstract class whose ctor registers every action's
dispatcher on the bridge capability handler. The hand-written adapter
inherits the base and overrides `Handle<Name>OnUI(...)`.

This script is intentionally dependency-free (stdlib only). JSON Schema
validation is opt-in via --validate; if jsonschema is unavailable we skip
validation rather than failing.

Usage:
  generate.py
      --api-declarations-dir <path>
      --api-output-dir       <path>
      [--validate]            # validate against api_schema.json
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
# The declared input types and their C++ spellings.
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


# =============================================================================
# Declaration model — typed mirror of the .api.json schema, populated by
# `parse_api_declaration`. Codegen consumes these structs, never the raw
# JSON dict, so adding a new field requires touching exactly one place.
# =============================================================================

@dataclass
class InputDecl:
    name: str
    type_kind: str           # one of CPP_TYPE_BY_KIND keys
    required: bool = False
    description: str = ""
    # Optional override for the JSON key the extractor reads. Keeps the
    # generated C++ field name (`name`) a snake_case identifier while the wire
    # key can be anything the runtime's tool schema uses, e.g. grep's `-A`,
    # `-B`, `-C`, `-n` and `-i`.
    wire_name: str | None = None

    @property
    def cpp_type(self) -> str:
        base = CPP_TYPE_BY_KIND[self.type_kind]
        return base if self.required else f"std::optional<{base}>"

    @property
    def schema_name(self) -> str:
        return self.wire_name or self.name


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


# =============================================================================
# JSON Schema validation (opt-in, requires the `jsonschema` pip package)
# =============================================================================


# =============================================================================
# Bridge-API codegen — *.api.json
#
# Each *.api.json
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
# Schema (api_schema.json):
#     name / description / inputs[] / runtime
# `inputs[]` carries {name, type, required, description, wire_name}, plus a
# `passthrough_payload` pseudo-type for actions whose payload is opaque and
# not worth enumerating field-by-field.
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
    # Declared error codes: (code, [param names]).
    errors: list[tuple[str, list[str]]] = field(default_factory=list)
    # Inferred from path:
    category_snake: str = ""   # "fs" / "llm"
    category_pascal: str = ""  # "Fs" / "Llm"

    @property
    def virtual_name(self) -> str:
        """fs.stat (thread_pool)        → HandleStatOnThreadPool
        fs.readFile (thread_pool)        → HandleReadFileOnThreadPool
        llm.rawStreamOpen (ui)           → HandleRawStreamOpenOnUI
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
        wire_name = raw_input.get("wire_name")
        if wire_name is not None and (
            not isinstance(wire_name, str) or not wire_name
        ):
            raise ValueError(
                f"{decl_path}: inputs[{i}].wire_name must be a non-empty string")
        inputs.append(
            InputDecl(
                name=raw_input["name"],
                type_kind=raw_input["type"],
                required=required,
                description=raw_input.get("description", ""),
                wire_name=wire_name,
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

    errors: list[tuple[str, list[str]]] = []
    for i, raw_error in enumerate(raw.get("errors", []) or []):
        if not isinstance(raw_error, dict) or not isinstance(raw_error.get("code"), str):
            raise ValueError(f"{decl_path}: errors[{i}] must be an object with a string 'code'")
        params = raw_error.get("params", []) or []
        if not all(isinstance(p, str) for p in params):
            raise ValueError(f"{decl_path}: errors[{i}].params must be strings")
        errors.append((raw_error["code"], list(params)))
    return ApiActionDecl(
        name=raw["name"],
        description=raw["description"],
        inputs=inputs,
        runtime_thread=thread,
        struct_name=struct_name,
        errors=errors,
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
    # The JSON key is the wire name when one is declared (`wire_name`),
    # so a C++-safe field (`context_after`) can sit behind a model-facing
    # key such as "-A". The struct field is always `name`.
    wire = inp.schema_name
    quoted = f'"{wire}"'
    miss = f'"Missing required field \\"{wire}\\""'
    bad_type = f'"Field \\"{wire}\\" has wrong type"'

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
    # Declared error codes, deduplicated across the category's actions in
    # first-seen order.
    seen_codes: dict[str, list[str]] = {}
    for d in decls:
        for code, params in d.errors:
            seen_codes.setdefault(code, params)
    base_class_pascal = f"{category_pascal}"
    if seen_codes:
        const_lines = [f"namespace {category_snake}_errors {{"]
        for code, params in seen_codes.items():
            pascal_code = "".join(part.capitalize() for part in code.split("_"))
            params_note = f"  // params: {', '.join(params)}" if params else ""
            const_lines.append(
                f"inline constexpr char k{pascal_code}[] = \"{code}\";{params_note}")
        const_lines.append(f"}}  // namespace {category_snake}_errors")
        const_lines.append("")
        const_lines.append(f"bool Is{base_class_pascal}ErrorCode(std::string_view code);")
        error_codes = "\n".join(const_lines)
        codes_array = ",\n".join(f"      \"{c}\"" for c in seen_codes)
        error_code_defs = (
            f"bool Is{base_class_pascal}ErrorCode(std::string_view code) {{\n"
            f"  static constexpr std::string_view kCodes[] = {{\n{codes_array},\n  }};\n"
            f"  for (std::string_view known : kCodes) {{\n"
            f"    if (known == code) {{\n      return true;\n    }}\n  }}\n"
            f"  return false;\n}}"
        )
    else:
        error_codes = f"// (none declared)\nbool Is{base_class_pascal}ErrorCode(std::string_view code);"
        error_code_defs = (
            f"bool Is{base_class_pascal}ErrorCode(std::string_view /*code*/) {{\n"
            f"  return false;\n}}"
        )

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
#include <string_view>
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

// ---- Declared error codes -------------------------------------------------
// The codes the category's actions answer with (`{{code, params}}`); the
// runtime's text table is keyed by the same codes.

{error_codes}

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

{error_code_defs}

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


# =============================================================================
# Skill registry emission — `nexus_skills_registry.gen.{h,cc}`.
#
# Reads every *.skill.json into a SkillDecl and emits an `AllSkills()`
# accessor returning a vector<::nexus::SkillSchema>. The catalog adapter
# (`catalog.listSkills`) consumes this and derives each skill's toolNames
# from the tool schemas (whose `skill` field links membership). See
# giggly-watching-pie.md Phase 4.
# =============================================================================

    # No deferral marker — whether a skill defers is decided SOLELY by the
    # nexus_web session template's `deferredTools` (resolveDeferrableSkills).


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Codegen Nexus bridge API adapters from *.api.json declarations.")
    parser.add_argument(
        "--api-declarations-dir",
        type=Path,
        required=True,
        help="Root directory containing <category>/<name>.api.json files.",
    )
    parser.add_argument(
        "--api-output-dir",
        type=Path,
        required=True,
        help="Output directory for API codegen. Each category gets a "
             "subdirectory; output paths look like "
             "<api-output-dir>/<category>/<category>_api.gen.{h,cc}.",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Validate each declaration against api_schema.json. Requires "
             "the `jsonschema` pip package; falls back to skip-with-warning.",
    )
    args = parser.parse_args(argv)
    api_schema_path = Path(__file__).parent / "api_schema.json"

    api_paths = list(discover_api_declarations(args.api_declarations_dir))
    if not api_paths:
        print(f"[generate.py] No *.api.json found under "
              f"{args.api_declarations_dir}", file=sys.stderr)
        return 0
    print(f"[generate.py] Processing {len(api_paths)} API declaration(s)...")
    api_decls: list[ApiActionDecl] = []
    for api_path in api_paths:
        if args.validate:
            validate_api(api_path, api_schema_path)
        api_decl = parse_api_declaration(api_path)
        infer_api_implementation(api_decl, api_path)
        api_decls.append(api_decl)
        print(f"  {api_path.parent.name}/{api_path.name}")
    by_category: dict[str, list[ApiActionDecl]] = {}
    for d in api_decls:
        by_category.setdefault(d.category_snake, []).append(d)
    for category_snake, category_decls in sorted(by_category.items()):
        h_out, cc_out = emit_api_category(
            category_snake, category_decls, args.api_output_dir
        )
        print(f"  api category -> {h_out}")
        print(f"  api category -> {cc_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
