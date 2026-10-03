# HTML reports

An HTML report is a throwaway page that explains a deep topic to the user. It
adds to the chat answer and does not replace it. It is not documentation.
Durable conclusions go to `docs/` through `vkr-docs`.

## File

- Copy [assets/report-template.html](assets/report-template.html) to
  `.scratch/reports/<YYYY-MM-DD>-<topic>.html`. The `.scratch/` directory is
  ignored by git.
- Keep the template's CSS and script. Set `<title>`, then replace everything
  between `BEGIN CONTENT` and `END CONTENT`. Delete examples that you do not use.
- Inline all CSS, JavaScript, SVG, and data. Do not load a CDN, an external
  font, or any network resource. The page must open offline.
- Do not commit the page, link to it from `docs/`, or cite it as evidence.
- Use a new file name for a new topic. Overwrite only a page that you made for
  the same topic in this task, because parallel sessions share `.scratch/`.

## Visual style

The user takes in a diagram, a code excerpt, or a chart faster than prose.

- Start with the title, the date and commit, the question, the role legend,
  and two to four key cards that state the main results. Put any decision
  that the user must make in a `callout key` directly below the cards.
- Start each section with a visual: a diagram, a code excerpt, a table, a
  chart, or key cards. Keep the prose next to it to three sentences or fewer.
  Move supporting detail into `<details>`.
- Give each role color (`a`, `b`, `c`) one concept for the whole page, for
  example shared code, Metal, and Vulkan. Name each role in the legend. Use the
  same role in diagrams, cards, table headers, bars, and `.r` text marks. Draw
  everything else in neutral boxes. The three template colors pass the dataviz
  palette checks in both themes; do not add a fourth role color.
- Mark at most one decisive phrase in a paragraph with `<mark class="key">`.
- Use `callout key` for a rule that the reader must keep, `callout warn` for a
  defect, a risk, or an unavailable check, and `callout ok` for a passed check.
  Each callout shows an icon, so color is never the only signal.
- Keep each diagram to one relation and eight boxes or fewer.

## Code excerpts

- Copy excerpts from the source without edits. Put the source path and first
  line in the caption, set `data-start` to that line, and mark the decisive
  lines with `data-mark`, for example `data-mark="29,31-33"`.
- Escape `<`, `>`, and `&` in the excerpt text. The script highlights `c`,
  `objc`, `metal`, `slang`, `hlsl`, `glsl`, `json`, and `sh`.
- Keep an excerpt to about 25 contiguous lines. Write "pseudocode" in the
  caption of any code that is not a source excerpt.

## Content

- Order the sections as the reader needs them: context, mechanism, evidence,
  options, recommendation. Add a contents list in `<details>` when the page has
  more than four sections.
- Write all text in the controlled English from [SKILL.md](SKILL.md).
- Draw diagrams as inline SVG with the exact names of passes, resources, and
  functions. Reference the template's shared `url(#arrow)` marker. Load the
  `dataviz` skill before you draw a chart when that skill is available.
- Mark the status of each claim with a `tag`: Source, Measured, Inferred, or
  Proposed. Give each measured number its command or report path. Do not show
  a number that the evidence does not contain.
- Give source locations as `path:line`.

## Mobile layout

The user often reads pages on a Galaxy S23 phone, which is 360 CSS px wide in
portrait. The template is laid out for 360 px first and widens on a desktop.
Keep these properties when you add content:

- The page must not scroll horizontally at 360 px. Put each wide table in a
  `.scroll` container. Code blocks and diagrams already scroll or scale.
- Draw SVG in a `viewBox` about 340 wide, from top to bottom, so that labels
  stay 11 px or larger at 360 px.
- Prefer tables with four columns or fewer. Prefer horizontal bars for charts
  with many categories.
- Make each tap target at least 48 px high. Give each hover interaction a tap
  equivalent, because a touch screen has no hover.

## Delivery

1. Check that every number and excerpt on the page matches its source.
2. Run `.codex/skills/unslop/scripts/check_mobile.sh <page> <png>` and look at
   the screenshot. Fix the page until the script prints `PASS`. The script
   needs Google Chrome. When the script exits with 2, report that the mobile
   check is unavailable.
3. On macOS, open the page with `open <absolute path>`.
4. In chat, give a summary of ten lines or fewer in controlled English. State
   the result, any decision that the user must make, and the absolute path of
   the page.
