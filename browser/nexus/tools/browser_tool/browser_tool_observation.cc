#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/session/nexus_session_features.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/frame_document.h"
#include "custom_browser/browser/nexus/upload/nexus_pending_agent_notice.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

}  // namespace

namespace {

// ContentAttributeTypeName, ExtractNodeText, TruncateUtf8Snippet,
// IsActionable, and `kDescendantTextSnippetMax` live in
// browser_tool_internal.h — this file's `using namespace
// browser_tool_internal;` directive makes them callable here without
// qualification.

std::string ExtractDescendantTextSnippet(const apc::ContentNode& node) {
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (child.has_content_attributes()) {
      const apc::ContentAttributes& cattrs = child.content_attributes();
      // Skip nested actionable nodes — their text belongs to their own
      // observation entry, not the wrapper's.
      if (cattrs.attribute_type() == apc::CONTENT_ATTRIBUTE_ANCHOR ||
          cattrs.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL) {
        continue;
      }
      if (cattrs.has_label() && !cattrs.label().empty()) {
        return TruncateUtf8Snippet(cattrs.label(),
                                    kDescendantTextSnippetMax);
      }
      if (cattrs.has_text_data() &&
          !cattrs.text_data().text_content().empty()) {
        return TruncateUtf8Snippet(cattrs.text_data().text_content(),
                                    kDescendantTextSnippetMax);
      }
      if (cattrs.has_image_data() &&
          !cattrs.image_data().image_caption().empty()) {
        return TruncateUtf8Snippet(cattrs.image_data().image_caption(),
                                    kDescendantTextSnippetMax);
      }
      if (cattrs.has_svg_root_data() &&
          !cattrs.svg_root_data().inner_text().empty()) {
        return TruncateUtf8Snippet(cattrs.svg_root_data().inner_text(),
                                    kDescendantTextSnippetMax);
      }
    }
    // Recurse — actionable wrappers often have a deeply nested
    // <span><span>text</span></span> structure (icon button libraries).
    std::string nested = ExtractDescendantTextSnippet(child);
    if (!nested.empty()) {
      return nested;
    }
  }
  return std::string();
}

// Walks a node's descendants and returns the first non-empty visible
// bounding box. Used as a fallback when the wrapper's own geometry is
// missing or zero-sized, which is common for inline anchors that
// inherit their hit area from a child <img> or <span>.
std::optional<base::Value> ExtractDescendantBoundingBox(
    const apc::ContentNode& node) {
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (child.has_content_attributes() &&
        child.content_attributes().has_geometry()) {
      const apc::Geometry& geo = child.content_attributes().geometry();
      if (geo.has_visible_bounding_box()) {
        const apc::BoundingRect& r = geo.visible_bounding_box();
        if (r.width() > 0 || r.height() > 0 || r.x() != 0 || r.y() != 0) {
          base::ListValue box;
          box.Append(r.x());
          box.Append(r.y());
          box.Append(r.width());
          box.Append(r.height());
          return base::Value(std::move(box));
        }
      }
    }
    if (auto nested = ExtractDescendantBoundingBox(child)) {
      return nested;
    }
  }
  return std::nullopt;
}

// Maps the proto's FORM_CONTROL_TYPE_* enum to the same short names HTML
// developers use ("text", "search", "checkbox", …). The LLM relies on
// these to tell a search box apart from a number input or radio button
// — without it, every form control reads as `form_control`.
const char* FormControlTypeName(apc::FormControlType t) {
  switch (t) {
    case apc::FORM_CONTROL_TYPE_CONTENT_EDITABLE:
      return "contenteditable";
    case apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX:
      return "checkbox";
    case apc::FORM_CONTROL_TYPE_INPUT_COLOR:
      return "color";
    case apc::FORM_CONTROL_TYPE_INPUT_DATE:
      return "date";
    case apc::FORM_CONTROL_TYPE_INPUT_DATETIME_LOCAL:
      return "datetime-local";
    case apc::FORM_CONTROL_TYPE_INPUT_EMAIL:
      return "email";
    case apc::FORM_CONTROL_TYPE_INPUT_FILE:
      // The model needs this to find the upload control for BrowserUpload;
      // before, file inputs carried no input_type at all.
      return "file";
    case apc::FORM_CONTROL_TYPE_INPUT_MONTH:
      return "month";
    case apc::FORM_CONTROL_TYPE_INPUT_NUMBER:
      return "number";
    case apc::FORM_CONTROL_TYPE_INPUT_PASSWORD:
      return "password";
    case apc::FORM_CONTROL_TYPE_INPUT_RADIO:
      return "radio";
    case apc::FORM_CONTROL_TYPE_INPUT_RANGE:
      return "range";
    case apc::FORM_CONTROL_TYPE_INPUT_SEARCH:
      return "search";
    case apc::FORM_CONTROL_TYPE_INPUT_TELEPHONE:
      return "tel";
    case apc::FORM_CONTROL_TYPE_INPUT_TEXT:
      return "text";
    case apc::FORM_CONTROL_TYPE_INPUT_TIME:
      return "time";
    case apc::FORM_CONTROL_TYPE_INPUT_URL:
      return "url";
    case apc::FORM_CONTROL_TYPE_INPUT_WEEK:
      return "week";
    case apc::FORM_CONTROL_TYPE_SELECT_ONE:
      return "select";
    case apc::FORM_CONTROL_TYPE_SELECT_MULTIPLE:
      return "select-multiple";
    case apc::FORM_CONTROL_TYPE_TEXT_AREA:
      return "textarea";
    // <input type="button|submit|reset"> and <button …>: report the HTML type and
    // role "button" so buttons in forms do not read as text fields.
    case apc::FORM_CONTROL_TYPE_INPUT_BUTTON:
    case apc::FORM_CONTROL_TYPE_BUTTON_BUTTON:
      return "button";
    case apc::FORM_CONTROL_TYPE_INPUT_SUBMIT:
    case apc::FORM_CONTROL_TYPE_BUTTON_SUBMIT:
      return "submit";
    case apc::FORM_CONTROL_TYPE_INPUT_RESET:
    case apc::FORM_CONTROL_TYPE_BUTTON_RESET:
      return "reset";
    case apc::FORM_CONTROL_TYPE_BUTTON_POPOVER:
      return "popover";
    // <input type="image"> — a clickable image submit button.
    // Common pattern: logo-as-submit on e-commerce sites, magnifying-
    // glass icon submitting a search form. Same bug class as the
    // button family above: emits role:"textbox" with no input_type
    // if we don't handle it. Surfaces as input_type:"image" so the
    // model can recognize the icon-button pattern.
    case apc::FORM_CONTROL_TYPE_INPUT_IMAGE:
      return "image";
    default:
      return nullptr;
  }
}

// Heuristic ARIA-role for an element. Pulls from the proto's
// `annotated_roles` first (the renderer's role layer already merges
// computed-aria + html-tag), falls back to the form_control type for
// inputs and to "link" for anchors / "button" for clickable form
// controls. Keeping the fallback narrow avoids guessing a role for
// every clickable div, which would dilute the signal.
std::string ExtractRole(const apc::ContentAttributes& attrs) {
  // (We deliberately don't enumerate every AnnotatedRoleType here —
  // the proto's enum is large and frequently extended; we forward the
  // first role with a recognizable string form by reading the enum's
  // descriptor at runtime would be heavier than warranted. The label /
  // input_type fields below carry most of the disambiguation power.)
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_ANCHOR) {
    return "link";
  }
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_FORM_CONTROL &&
      attrs.has_form_control_data()) {
    apc::FormControlType t = attrs.form_control_data().form_control_type();
    if (t == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX) return "checkbox";
    if (t == apc::FORM_CONTROL_TYPE_INPUT_RADIO) return "radio";
    if (t == apc::FORM_CONTROL_TYPE_SELECT_ONE ||
        t == apc::FORM_CONTROL_TYPE_SELECT_MULTIPLE) {
      return "combobox";
    }
    if (t == apc::FORM_CONTROL_TYPE_INPUT_SEARCH) return "searchbox";
    // <input type="button|submit|reset"> AND <button type="button|submit
    // |reset|popover">. Surface as role:"button" so the model recognizes
    // submit/CTA controls as actuators rather than mistaking them for
    // text fields. The empirical AKC failure was caused by RETRIEVE
    // EVENTS surfacing as role:"textbox" — fixed by recognizing the
    // button family here.
    if (t == apc::FORM_CONTROL_TYPE_INPUT_BUTTON ||
        t == apc::FORM_CONTROL_TYPE_INPUT_SUBMIT ||
        t == apc::FORM_CONTROL_TYPE_INPUT_RESET ||
        t == apc::FORM_CONTROL_TYPE_INPUT_IMAGE ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_BUTTON ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_SUBMIT ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_RESET ||
        t == apc::FORM_CONTROL_TYPE_BUTTON_POPOVER) {
      return "button";
    }
    return "textbox";  // text / email / password / url / number / tel /
                       // contenteditable all behave as text input for
                       // the LLM's purposes
  }
  if (attrs.has_interaction_info() &&
      attrs.interaction_info().is_clickable()) {
    return "button";
  }
  // Clickable images (`<input type="image">`, `<img onclick>`, …) arrive as
  // CONTENT_ATTRIBUTE_IMAGE without is_clickable; IsActionable accepted them, so
  // surface them as buttons.
  if (attrs.attribute_type() == apc::CONTENT_ATTRIBUTE_IMAGE) {
    return "button";
  }
  // ARIA-only interactive widgets (not anchor / form control, not flagged
  // clickable): emit the ARIA role from `RoleStringFromAXRole`, the same table
  // `IsActionable` checks.
  if (attrs.has_aria_role()) {
    if (const char* role = RoleStringFromAXRole(attrs.aria_role())) {
      return role;
    }
  }
  return std::string();
}

// Compresses a Geometry message into a 4-tuple [x, y, w, h] that the
// LLM can reason about cheaply (e.g. "clickable element near top of
// viewport"). Returns std::nullopt when the visible bounding box is
// empty, since an empty box means "not hit-testable / offscreen" and
// shipping zeros would be misleading.
std::optional<base::Value> ExtractBoundingBox(
    const apc::ContentAttributes& attrs) {
  if (!attrs.has_geometry()) {
    return std::nullopt;
  }
  const apc::Geometry& geo = attrs.geometry();
  if (!geo.has_visible_bounding_box()) {
    return std::nullopt;
  }
  const apc::BoundingRect& r = geo.visible_bounding_box();
  // Skip empty rectangles (proto's BoundingRect zero-init = offscreen).
  if (r.width() == 0 && r.height() == 0 && r.x() == 0 && r.y() == 0) {
    return std::nullopt;
  }
  base::ListValue box;
  box.Append(r.x());
  box.Append(r.y());
  box.Append(r.width());
  box.Append(r.height());
  return base::Value(std::move(box));
}

}  // namespace

void BrowserTool::WalkContentNode(const apc::ContentNode& node,
                                   const std::string& document_id,
                                   int tab_id,
                                   base::ListValue& out_elements) {
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    // Skip emitting a standalone `<label for=...>` node: its clickable
    // role is folded into the control it labels (a hidden checkbox/radio
    // gets RE-TARGETED to this label below), so emitting it separately
    // would produce a duplicate element pointing at the same DOM node.
    // Children still recurse at the bottom so inner text/elements surface.
    if (IsActionable(attrs) && attrs.has_common_ancestor_dom_node_id() &&
        !attrs.has_label_for_dom_node_id()) {
      // Use the APC's content_node_id directly as the LLM-facing
      // index. Stable for the document's lifetime in the renderer,
      // so the LLM can refer to the same DOM element by the same index
      // across observations / turns.
      const int node_id = attrs.common_ancestor_dom_node_id();
      const char* type_name = ContentAttributeTypeName(attrs.attribute_type());
      std::string text = ExtractNodeText(attrs);
      // Wrapper anchors / buttons usually carry their visible signal in
      // descendants (icon caption, inner span text). Fall back so the
      // LLM sees `text: "Sign in"` instead of a bare `{index, type}`.
      if (text.empty()) {
        text = ExtractDescendantTextSnippet(node);
      }
      // Still no accessible name? Borrow the associated `<label for=...>`'s
      // text. A form control whose label is a separate element (the input and
      // its name are siblings, not nested) otherwise emits as a bare
      // `{index, input_type:checkbox}` — so on a filter list of identical
      // checkboxes the model can't tell which is "Honda" and resorts to
      // guessing by position. Surfacing the label text lets it target the
      // control by name. (Keyed by document + node_id: APC guarantees the label's
      // `label_for_dom_node_id` equals the input's `common_ancestor_dom_node_id`.)
      if (text.empty()) {
        const LabelTarget* label =
            observation_label_targets_.Find(document_id, node_id);
        if (label && !label->text.empty()) {
          text = label->text;
        }
      }

      const bool is_main_document = document_id == walk_main_document_id_;
      const int index = node_registry_.AssignIndex(tab_id, document_id, node_id,
                                                   is_main_document);
      NodeRegistry::Entry entry;
      entry.document_id = document_id;
      entry.content_node_id = node_id;
      entry.tab_id = tab_id;
      entry.type = type_name;
      entry.text = text;
      node_registry_.Set(index, entry);

      // Rich element payload: index + type required, other keys only when non-empty.
      // `type` is the proto's structural kind, `role` the ARIA-derived interaction
      // role; for form controls they diverge.
      base::DictValue elem;
      elem.Set("index", index);
      elem.Set("type", type_name);
      if (!text.empty()) {
        elem.Set("text", text);
      }
      // ARIA role / heuristic role (link / button / textbox / …).
      std::string role = ExtractRole(attrs);
      if (!role.empty()) {
        elem.Set("role", std::move(role));
      }
      // Form control specifics — placeholder, current value, input
      // type ("text" / "search" / "email" / "checkbox" / …),
      // is_required / is_checked. These solve the "search input vs.
      // search button" disambiguation the previous schema couldn't.
      if (attrs.has_form_control_data()) {
        const apc::FormControlData& fc = attrs.form_control_data();
        if (fc.has_placeholder() && !fc.placeholder().empty()) {
          elem.Set("placeholder", fc.placeholder());
        }
        if (fc.has_field_value() && !fc.field_value().empty()) {
          elem.Set("value", fc.field_value());
        }
        if (fc.has_field_name() && !fc.field_name().empty()) {
          elem.Set("name", fc.field_name());
        }
        if (fc.has_form_control_type()) {
          if (const char* it = FormControlTypeName(fc.form_control_type())) {
            elem.Set("input_type", it);
          }
        }
        if (fc.has_is_required() && fc.is_required()) {
          elem.Set("is_required", true);
        }
        if (fc.has_is_checked()) {
          // Always emit for checkable controls so the LLM can read
          // current state (not just `is_checked: true` when checked).
          if (fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX ||
              fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_RADIO) {
            elem.Set("is_checked", fc.is_checked());
          }
        }
        // Native <select> options: emit `options: [{value, text, selected?,
        // disabled?}]` so the model can select by value (the popup is OS-rendered);
        // capped, with `options_truncated: true` when cut.
        if (fc.select_options_size() > 0) {
          constexpr int kMaxSelectOptions = 200;
          const int total = fc.select_options_size();
          const int emit_count =
              total < kMaxSelectOptions ? total : kMaxSelectOptions;
          base::ListValue options;
          for (int i = 0; i < emit_count; ++i) {
            const apc::SelectOption& opt = fc.select_options(i);
            base::DictValue o;
            o.Set("value", opt.value());
            if (!opt.text().empty()) {
              o.Set("text", opt.text());
            }
            if (opt.is_selected()) {
              o.Set("selected", true);
            }
            if (opt.is_disabled()) {
              o.Set("disabled", true);
            }
            options.Append(std::move(o));
          }
          elem.Set("options", std::move(options));
          if (total > emit_count) {
            elem.Set("options_truncated", true);
          }
        }
      }
      // Anchor specifics — destination URL. Critical for telling search
      // result links apart on a results page.
      if (attrs.has_anchor_data() && !attrs.anchor_data().url().empty()) {
        // T0-a: slim the emitted href — drop scheme+host for same-origin
        // links, strip tracking params, cap length. `last_observation_url_`
        // is set from main_frame_data before this walk runs, so it names
        // the working tab's page. The dom_hash projection above keeps the
        // RAW href (`h=`), so this slimming never perturbs the fingerprint.
        bool href_truncated = false;
        elem.Set("href",
                 browser_tool_internal::CompressHref(
                     attrs.anchor_data().url(), last_observation_url_,
                     &href_truncated));
        if (href_truncated) {
          elem.Set("href_truncated", true);
        }
      }
      // Viewport bounding box — `[x, y, w, h]`. Useful when the LLM has
      // multiple candidate matches and wants to prefer the one near
      // the top of the viewport (e.g. first search result). Inline
      // anchors often have an empty box at the wrapper itself but a
      // real one on a descendant <img>/<span>; fall back to that so
      // every interactable element ships some position info.
      auto box = ExtractBoundingBox(attrs);
      if (!box) {
        box = ExtractDescendantBoundingBox(node);
      }

      // Hidden-control retarget: an empty-rect checkbox/radio with a visible
      // `<label for>` is retargeted to the label's node_id + box (clicking it
      // toggles the input), keeping the input's role, type, name and checked state.
      if (!box && attrs.has_form_control_data()) {
        const apc::FormControlType fct =
            attrs.form_control_data().form_control_type();
        if (fct == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX ||
            fct == apc::FORM_CONTROL_TYPE_INPUT_RADIO) {
          const LabelTarget* label =
              observation_label_targets_.Find(document_id, node_id);
          if (label) {
            const int label_node_id = label->node_id;
            base::ListValue lbox;
            lbox.Append(label->box[0]);
            lbox.Append(label->box[1]);
            lbox.Append(label->box[2]);
            lbox.Append(label->box[3]);
            box = base::Value(std::move(lbox));
            // Register the label too, targeting the label node, so a
            // BrowserClick on the emitted index lands on the visible label.
            NodeRegistry::Entry label_entry = entry;
            label_entry.content_node_id = label_node_id;
            const int label_index = node_registry_.AssignIndex(
                tab_id, document_id, label_node_id, is_main_document);
            node_registry_.Set(label_index, std::move(label_entry));
            // Overwrite the index set earlier with the label's.
            elem.Set("index", label_index);
          }
        }
      }

      if (box) {
        elem.Set("box", std::move(*box));
      }

      // Drop bare wrapper entries that we couldn't enrich with any
      // text / href / box / form-state. Keeping them would just inflate
      // the observation with unselectable noise — the LLM has nothing
      // to cite as element_reason and can't tell what the index targets.
      // node_registry_.Set above still ran, so a later observation that
      // DOES enrich this index can win; we just skip the wire append
      // and the normalized-projection update for this turn.
      const bool has_useful_signal =
          !text.empty() || elem.contains("href") || elem.contains("box") ||
          elem.contains("placeholder") || elem.contains("value") ||
          elem.contains("name") || elem.contains("input_type") ||
          elem.contains("is_checked");
      if (!has_useful_signal) {
        // Recurse into children below so descendants still surface.
        const std::string child_document_id =
            ChildDocumentId(node, document_id);
        for (const apc::ContentNode& child : node.children_nodes()) {
          WalkContentNode(child, child_document_id, tab_id, out_elements);
        }
        return;
      }

      out_elements.Append(std::move(elem));

      // Append an identifier-free projection of this element to the
      // rolling normalized projection used for dom_hash. Identifiers
      // (content_node_id, tab_id, document_id) are excluded so the projection
      // captures the page's interactive surface SHAPE, not bookkeeping.
      // Form-state fields (value, is_checked) are INCLUDED — typing
      // into a field IS meaningful page-state change. Walk order is
      // deterministic for a given DOM, so the projection is stable
      // across re-observations of the same page.
      base::StrAppend(&last_observation_normalized_projection_,
                      {"|", type_name, ";"});
      if (!text.empty()) {
        base::StrAppend(&last_observation_normalized_projection_,
                        {"t=", text, ";"});
      }
      if (attrs.has_form_control_data()) {
        const apc::FormControlData& fc = attrs.form_control_data();
        if (fc.has_form_control_type()) {
          if (const char* it = FormControlTypeName(fc.form_control_type())) {
            base::StrAppend(&last_observation_normalized_projection_,
                            {"it=", it, ";"});
          }
        }
        if (fc.has_placeholder() && !fc.placeholder().empty()) {
          base::StrAppend(&last_observation_normalized_projection_,
                          {"ph=", fc.placeholder(), ";"});
        }
        if (fc.has_field_value() && !fc.field_value().empty()) {
          base::StrAppend(&last_observation_normalized_projection_,
                          {"v=", fc.field_value(), ";"});
        }
        if (fc.has_is_required() && fc.is_required()) {
          last_observation_normalized_projection_.append("r=1;");
        }
        if (fc.has_is_checked() &&
            (fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_CHECKBOX ||
             fc.form_control_type() == apc::FORM_CONTROL_TYPE_INPUT_RADIO)) {
          base::StrAppend(&last_observation_normalized_projection_,
                          {"c=", fc.is_checked() ? "1" : "0", ";"});
        }
      }
      if (attrs.has_anchor_data() && !attrs.anchor_data().url().empty()) {
        base::StrAppend(&last_observation_normalized_projection_,
                        {"h=", attrs.anchor_data().url(), ";"});
      }
      ++last_observation_interactive_count_;
      // focus_target_id is set once per observation from
      // `apc_data.page_interaction_info().focused_node_id()` in
      // BuildObservationDict — it's the stable DOM content_node_id of
      // the focused element, not anything we'd derive from
      // WalkContentNode's element walk.
    }
  }
  // Elements inside an iframe live in that frame's document (F-21).
  const std::string child_document_id = ChildDocumentId(node, document_id);
  for (const apc::ContentNode& child : node.children_nodes()) {
    WalkContentNode(child, child_document_id, tab_id, out_elements);
  }
}

base::DictValue BrowserTool::BuildObservationDict(
    const apc::ActionsResult& result_proto,
    bool include_screenshot_image,
    bool force_full) {
  // A projection staged by an earlier walk that was never resolved (e.g.
  // the settle guard dropped that observation) is dead — this walk stages
  // its own, and only the caller resolving THIS result commits.
  observation_baseline_.Discard();
  // Only the working tab (`LastWorkTab()`, recorded by `RecordWorkTab`) gets the
  // full APC walk, `elements[]`, screenshot, `last_observation_*` and registry
  // updates; other session tabs get `tab_id` / `url` / `title`. With no working
  // tab every entry is basics-only.
  const std::optional<tabs::TabHandle> working_handle = LastWorkTab();
  const std::optional<int32_t> working_tab_id =
      working_handle.has_value()
          ? std::make_optional(working_handle->raw_value())
          : std::nullopt;
  last_observation_url_.clear();
  last_observation_title_.clear();
  last_observation_interactive_count_ = 0;
  last_observation_tab_id_.reset();
  last_observation_normalized_projection_.clear();
  last_observation_focus_target_present_ = false;
  last_observation_focus_target_id_.clear();

  // When session-scoped tabs are enabled, observations must hide tabs
  // owned by other sessions (or by the user outside any session). This
  // is the single choke point — every tool's observation flows through
  // here, so a filter here is sufficient to strictly hide cross-session
  // tabs from the model's view.
  const bool session_scope =
      ::nexus::features::IsSessionScopedTabsEnabled() &&
      !GetSessionKey().empty();
  const std::string this_session_key = GetSessionKey();

  base::DictValue observation;
  base::ListValue tabs;
  for (const apc::TabObservation& tab_obs : result_proto.tabs()) {
    if (session_scope) {
      tabs::TabInterface* tab_iface = tabs::TabHandle(tab_obs.id()).Get();
      content::WebContents* contents =
          tab_iface ? tab_iface->GetContents() : nullptr;
      const std::string* tag =
          ::nexus::NexusSessionTabTag::SessionKeyFor(contents);
      if (!tag || *tag != this_session_key) {
        continue;
      }
    }

    const bool is_working = working_tab_id.has_value() &&
                            tab_obs.id() == *working_tab_id;

    base::DictValue tab_dict;
    tab_dict.Set("tab_id", tab_obs.id());

    // Surface observation-fetch failures: upstream returns no page content on an
    // APC or screenshot failure, which would otherwise read as an empty page.
    if (tab_obs.result() != apc::TabObservation::TAB_OBSERVATION_OK) {
      tab_dict.Set(
          "observation_error",
          base::StrCat(
              {"tab observation failed: result=",
               apc::TabObservation::TabObservationResult_Name(
                   tab_obs.result()),
               " apc=",
               apc::TabObservation::AnnotatedPageContentResult_Name(
                   tab_obs.annotated_page_content_result()),
               " screenshot=",
               apc::TabObservation::ScreenshotResult_Name(
                   tab_obs.screenshot_result())}));
    }

    // Pull `url` / `title` from main_frame_data — emitted for every
    // tab (basics for non-working, full envelope's basics for the
    // working one too).
    if (tab_obs.has_annotated_page_content() &&
        tab_obs.annotated_page_content().has_main_frame_data()) {
      const apc::FrameData& mf =
          tab_obs.annotated_page_content().main_frame_data();
      if (!mf.url().empty()) {
        tab_dict.Set("url", mf.url());
      }
      if (!mf.title().empty()) {
        tab_dict.Set("title", mf.title());
      }
    }

    if (!is_working) {
      // Non-working tab — basics only. No elements walk, no screenshot,
      // no node_registry / projection updates.
      tabs.Append(std::move(tab_dict));
      continue;
    }

    // ── Working tab: full payload ─────────────────────────────────
    // Pixels only for BrowserScreenshot, with no availability flag: the
    // `screenshot` key's presence is the signal. (`tab_obs.has_screenshot()` is
    // the proto getter.)
    if (include_screenshot_image && tab_obs.has_screenshot() &&
        !tab_obs.screenshot().empty()) {
      // The bridge browser-result transport is JSON-only, and the model API
      // takes base64 regardless, so base64 here is the right carrier — JS
      // lifts it straight into an image content block. `screenshot` is raw
      // bytes; `screenshot_mime_type` is the captured format (defaults to JPEG
      // when the framework didn't set one).
      base::DictValue shot;
      shot.Set("mime", (tab_obs.has_screenshot_mime_type() &&
                        !tab_obs.screenshot_mime_type().empty())
                           ? tab_obs.screenshot_mime_type()
                           : std::string("image/jpeg"));
      shot.Set("data_base64", base::Base64Encode(tab_obs.screenshot()));
      tab_dict.Set("screenshot", std::move(shot));
    }

    base::ListValue elements;
    if (tab_obs.has_annotated_page_content()) {
      const apc::AnnotatedPageContent& apc_data =
          tab_obs.annotated_page_content();

      // Pull the document_identifier from the main frame's metadata —
      // it pairs with content_node_id when building actor targets.
      std::string document_id;
      if (apc_data.has_main_frame_data() &&
          apc_data.main_frame_data().has_document_identifier()) {
        document_id =
            apc_data.main_frame_data().document_identifier().serialized_token();
      }

      // Populate the agent_meta snapshot from the working tab.
      last_observation_tab_id_ = tab_obs.id();
      if (apc_data.has_main_frame_data()) {
        last_observation_url_ = apc_data.main_frame_data().url();
        last_observation_title_ = apc_data.main_frame_data().title();
      }

      // Stable focus signal for multi-act guards. The proto's
      // `page_interaction_info.focused_node_id` is the focused
      // element's DOM content_node_id — same identifier space as
      // the LLM-facing index, so multi-act guards, focus tracking,
      // and registry lookups all share the same key.
      if (apc_data.has_page_interaction_info() &&
          apc_data.page_interaction_info().has_focused_node_id() &&
          apc_data.page_interaction_info().focused_node_id() != 0) {
        last_observation_focus_target_present_ = true;
        last_observation_focus_target_id_ = base::NumberToString(
            apc_data.page_interaction_info().focused_node_id());
      }

      const int tab_id_for_registry =
          apc_data.has_tab_id() ? apc_data.tab_id() : tab_obs.id();

      if (apc_data.has_root_node()) {
        // Evict stale registry entries when this tab's document changed
        // since we last walked it — their content_node_ids belong to a
        // defunct document and would otherwise resolve to actor-rejected
        // targets. (NodeRegistry::ClearTab wiring.)
        auto doc_it = registry_document_by_tab_.find(tab_id_for_registry);
        if (doc_it != registry_document_by_tab_.end() &&
            doc_it->second != document_id) {
          node_registry_.ClearTab(tab_id_for_registry);
        }
        registry_document_by_tab_[tab_id_for_registry] = document_id;

        // Pre-pass: index `<label for=...>` → control so `WalkContentNode`
        // can retarget hidden checkboxes/radios to their visible label.
        // The label's text may sit on the label node or in a child span.
        observation_label_targets_.Build(
            apc_data.root_node(), document_id,
            [](const apc::ContentNode& label) {
              std::string text = ExtractNodeText(label.content_attributes());
              return text.empty() ? ExtractDescendantTextSnippet(label)
                                  : text;
            });
        walk_main_document_id_ = document_id;
        WalkContentNode(apc_data.root_node(), document_id, tab_id_for_registry,
                        elements);
      }

      // T0-c: short-circuit an unchanged working-tab observation. The walk
      // above already refreshed node_registry_ and the last_observation_*
      // snapshot (so indexes still resolve and agent_meta is correct); we
      // only replace the bulky elements[] payload with a compact stub when
      // the interactive surface is byte-identical to the last full send on
      // the same document. Never for BrowserScreenshot (the model asked for
      // pixels + a fresh look) and never when the caller explicitly asked
      // for the elements (`force_full` — `include_observation:true`).
      const bool short_circuit =
          !include_screenshot_image && !force_full &&
          observation_baseline_.ShouldShortCircuit(
              observation_diff_enabled_, document_id,
              last_observation_normalized_projection_);
      if (short_circuit) {
        base::DictValue unchanged;
        unchanged.Set("unchanged", true);
        unchanged.Set("count", static_cast<int>(elements.size()));
        tab_dict.Set("elements", std::move(unchanged));
        tabs.Append(std::move(tab_dict));
        continue;
      }
      // Full walk — STAGE this projection/doc. It becomes the baseline
      // only when the resolving caller commits it (the elements[] really
      // went to the model); a caller that strips the observation to basics
      // or throws it away discards instead, so the baseline keeps tracking
      // what is actually in the model's context.
      observation_baseline_.Stage(document_id,
                                  last_observation_normalized_projection_);
    }
    tab_dict.Set("elements", std::move(elements));
    tabs.Append(std::move(tab_dict));
  }

  observation.Set("tabs", std::move(tabs));

  // Drain any out-of-band notices recorded on the working tab since the last
  // observation — e.g. a native file dialog the RunFileChooser hook suppressed
  // on this agent tab — so the model sees facts the page may have swallowed.
  // Sticky until drained: the chooser fires asynchronously relative to the
  // action that caused it, so a notice can surface on a later observation than
  // the click that triggered it.
  if (working_handle.has_value()) {
    tabs::TabInterface* work_tab = working_handle->Get();
    content::WebContents* work_contents =
        work_tab ? work_tab->GetContents() : nullptr;
    NexusPendingAgentNotice* pending =
        work_contents
            ? NexusPendingAgentNotice::FromWebContents(work_contents)
            : nullptr;
    if (pending) {
      std::vector<AgentNotice> drained = pending->TakeAll();
      if (!drained.empty()) {
        base::ListValue notices;
        for (const AgentNotice& notice : drained) {
          notices.Append(notice.ToDict());
        }
        observation.Set("notices", std::move(notices));
      }
    }
  }
  return observation;
}

const NodeRegistry::Entry* BrowserTool::LookupNode(
    int node_id,
    std::optional<int> tab_id,
    ToolCallback& done,
    std::string_view tool_name) {
  const NodeRegistry::Entry* entry = tab_id.has_value()
      ? node_registry_.FindIn(node_id, *tab_id)
      : node_registry_.Find(node_id);
  if (!entry) {
    {
      base::DictValue reject_params;
      reject_params.Set("tool", tool_name);
      reject_params.Set("index", node_id);
      if (tab_id.has_value()) {
        reject_params.Set("tab_id", *tab_id);
      }
      std::move(done).Reject("index_not_found", std::move(reject_params));
    }
  }
  return entry;
}

}  // namespace nexus
