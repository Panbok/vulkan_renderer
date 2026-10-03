# HTML reports

An HTML report is a throwaway page that explains a deep topic to the user. It
adds to the chat answer and does not replace it. It is not documentation.
Durable conclusions go to `docs/` through `vkr-docs`.

## File

- Write one self-contained file at
  `.scratch/reports/<YYYY-MM-DD>-<topic>.html`. The `.scratch/` directory is
  ignored by git.
- Inline all CSS, JavaScript, SVG, and data. Do not load a CDN, an external
  font, or any network resource. The page must open offline.
- Do not commit the page, link to it from `docs/`, or cite it as evidence.
- Use a new file name for a new topic. Overwrite only a page that you made for
  the same topic in this task, because parallel sessions share `.scratch/`.

## Content

- Start with the title, the date, the question, and a summary of three to five
  sentences that states the result and any decision that the user must make.
- Order the sections as the reader needs them: context, mechanism, evidence,
  options, recommendation. Add a contents list when the page has more than four
  sections.
- Write all text in the controlled English from [SKILL.md](SKILL.md).
- Draw diagrams as inline SVG with the exact names of passes, resources, and
  functions. Load the `dataviz` skill before you draw a chart when that skill
  is available.
- Mark the status of each claim: measured, read in source, inferred, or
  proposed. Give each measured number its command or report path. Do not show a
  number that the evidence does not contain.
- Show code and identifiers in a monospace font. Give source locations as
  `path:line`.
- Add interaction only when it helps the reader, for example collapsible
  detail, tabs that compare Metal and Vulkan, or chart values on tap.
- Support light and dark themes through `prefers-color-scheme`. On a wide
  screen, limit the text column to about 75 characters.

## Mobile layout

The user often reads pages on a Galaxy S23 phone, which is 360 CSS px wide in
portrait. Design for 360 px first. Use `min-width` media queries to widen the
layout for a desktop.

- Include `<meta name="viewport" content="width=device-width, initial-scale=1">`.
- The page must not scroll horizontally at 360 px. Put each wide table, code
  block, and diagram in its own container with `overflow-x: auto`. Apply
  `overflow-wrap: anywhere` to long identifiers and paths in text.
- Use body text of 16 px or more, a line height of about 1.5, and page padding
  of about 16 px.
- Give each SVG a `viewBox` and `width: 100%`. Draw flows from top to bottom so
  that labels stay 11 px or larger at 360 px. Give a diagram that cannot stay
  readable a fixed minimum width inside a scroll container.
- Prefer tables with four columns or fewer. Prefer horizontal bars for charts
  with many categories, because the labels then fit.
- Make each tap target at least 48 px high. Give each hover interaction a tap
  equivalent, because a touch screen has no hover.
- Use `<details>` for collapsible sections and for the contents list on a
  narrow screen. Do not use a fixed or sticky element taller than about 56 px.

## Delivery

1. Check that every number on the page matches its source report or command.
2. Run `.codex/skills/unslop/scripts/check_mobile.sh <page> <png>` and look at
   the screenshot. Fix the page until the script prints `PASS`. The script
   needs Google Chrome. When the script exits with 2, report that the mobile
   check is unavailable.
3. On macOS, open the page with `open <absolute path>`.
4. In chat, give a summary of ten lines or fewer in controlled English. State
   the result, any decision that the user must make, and the absolute path of
   the page.
