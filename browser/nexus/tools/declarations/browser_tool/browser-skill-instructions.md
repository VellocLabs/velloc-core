## Browser tools — workflow

You have a flat catalog of browser-control tools (navigate, click, input, search, read_page, find_text, …). One browser tool call per turn. Before each call, write a short plain-text note explaining what the latest observation showed and what you intend to do next, phrased so the next observation can grade it. Working memory and reasoning live in this prose, not in tool arguments.

## Picking a tool

- The user wants to find information online → `search`. Don't `navigate` to a search homepage and then type into a search box — `search` does both in one call.
- The user gave you a direct URL → `navigate` (or `create_tab` if you need a side-by-side view).
- You need to interact with a specific element (button, form field, link) → look up the element's `index` from the latest observation's `elements[]` and call the matching index-targeted tool: `click`, `input`, `select`, `hover`, `drag_and_drop`, `scroll_to`.
- You need text that's not surfaced as an actionable element (article body, paragraph copy) → `read_page` (returns the full page text), or `read_element` for a single container.
- You need to find a specific phrase that might be a JS-onclick suggestion (autocomplete, custom dropdown, menu item that isn't surfaced as actionable) → `find_text`. The result resolves the text node to the nearest clickable ancestor's `index`; pass THAT to `click`, never the text node itself.
- You're done → `done` with `success: true|false` and a one-sentence `text` summary the user will see verbatim.

## Autocomplete / typeahead fields (ZIP, city, location, search-as-you-type)

After you `input` into an autocomplete/combobox field, a suggestion dropdown usually appears — but many sites (e.g. react-select) render those suggestion rows as bare elements with NO actionable role, so they do NOT show up in `elements[]`. In that case typing the text alone often leaves the site's internal selected value UNSET, and submitting yields wrong/unfiltered results. So: after typing into such a field, re-observe; if the suggestion you want is NOT in `elements[]` as a clickable row, call `find_text` with the value you typed and `click` the returned `clickable_ancestor_index` to commit the selection BEFORE submitting. Do not submit the raw typed text assuming the suggestion was accepted.

## Framework-bound widgets (text-found, no clickable_ancestor_index)

Some React/Angular/Vue widgets bind click handlers via framework internals (Angular `(click)` templates, React synthetic events) and ship ZERO DOM-level interactivity signal: no `role`, no `cursor: pointer`, no `tabindex`, no `onclick` attribute. A human can click them (the framework's runtime event delegation catches the event), but they are undetectable as actionable by `IsActionable` and therefore absent from `elements[]`. The canonical example is the AKC event-search event-type accordion: the visible `<div class="comp-event-row">` row expands on click but carries zero web-standard interactivity signals.

Diagnostic: BOTH of the following are true on the same turn:
  1. `find_text("that-text")` matches the visible string but returns NO `clickable_ancestor_index`, AND
  2. The same element does NOT appear in `elements[]` with an actionable role.

When you see this pair — text is in the page but no index can target it — STOP using index-based tools (click / input / scroll_to / hover / fill_form / find_text). They all key off `elements[]` and will keep failing the same way. Each retry burns a turn and the page state doesn't change.

Fallback: `BrowserEvaluate` is the only path. Call `.click()` (or set `.value` + dispatch input event) on the element directly. Example for an accordion-style row:

```js
(() => {
  for (const el of document.querySelectorAll('div')) {
    if (/^Companion Events$/i.test(el.textContent.trim()) &&
        el.getBoundingClientRect().width > 0) {
      // walk up to the click target — usually the parent row container.
      let row = el; while (row && row.parentElement && row.parentElement.children.length < 4) row = row.parentElement;
      row.click(); return 'clicked';
    }
  }
  return 'not found';
})()
```

`BrowserEvaluate` is always available — use it for this fallback whenever index-based tools can't target an element. Do not loop trying other browser tools — the wall is the page's missing accessibility, not your tool choice.

## Element targeting

Indexes come from observation.elements[].index in the latest observation. They are stable for the document's lifetime — an index from an earlier turn is still valid until the page navigates. Cite the element's type and visible text/label/placeholder in your narration so a reader can audit your choice; do not guess indexes.

If an element is in the observation but the call fails with ELEMENT_OFFSCREEN, precede the click with `scroll_to{index}` (or a directional `scroll`) and re-observe before retrying.

## When to use a screenshot

The default observation already lists every actionable element, so most tasks don't need a screenshot. Ask for one only when visual context (layout, color, image content, video frame) actually matters to the decision. When only one element matters, pass its `index` — the image is cropped to that element, which costs far fewer image tokens than the whole viewport.

## When to stop

- Goal achieved → `done` with `success: true`.
- Goal blocked (paywall, login wall, captcha, missing capability) → `done` with `success: false` and a one-sentence reason. Do not loop on a wall.
- Repeating yourself (same scroll, same click, same observation) → step back, change approach (different tab, different query, different URL).
