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
  detail, tabs that compare Metal and Vulkan, or chart values on hover.
- Support light and dark themes through `prefers-color-scheme`. Keep the text
  column narrow enough to read, about 75 characters.

## Delivery

1. Check that every number on the page matches its source report or command.
2. On macOS, open the page with `open <absolute path>`.
3. In chat, give a summary of ten lines or fewer in controlled English. State
   the result, any decision that the user must make, and the absolute path of
   the page.
