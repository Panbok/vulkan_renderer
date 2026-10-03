---
name: unslop
description: Write output for the user in controlled English (about 80% of ASD-STE100), structure it with tables and diagrams, and move deep explanations into throwaway HTML pages; also edit project prose for concrete technical meaning. Use for reports, explanations, investigation summaries, PR descriptions, and documentation edits; skip for one-line answers.
---

# Unslop

Make output easy to ingest. First choose the format from the shape of the
content. Then write the words in controlled English.

## Choose the format

| Content shape | Format |
|---|---|
| Direct answer or short status | Prose in chat |
| Two or more items compared on two or more attributes | Table |
| Order, data flow, ownership, lifetime, state, or dependency | ASCII diagram, 44 columns or fewer |
| Deep topic: several sections that need diagrams, tables, or charts, or chat output longer than about 80 lines | HTML page plus a chat summary |

Typical deep topics are performance investigations, rendering techniques,
architecture walkthroughs, and comparisons of design options. Also make a page
when the user asks for one. Do not make a page for status updates, review
findings, commit messages, or answers that fit in chat. To make a page, read
[HTML-REPORTS.md](HTML-REPORTS.md).

## Write controlled English

ASD-STE100 is the controlled English of aerospace maintenance manuals. Apply
about 80% of it to all output for the user: chat answers, reports, PR
descriptions, and HTML pages.

- Put the result or the required action first.
- Write one claim or one instruction in each sentence.
- Keep a sentence to 20 words or fewer for a step and 25 or fewer for a
  description. Split a longer sentence at its clauses.
- Use active voice. Use present tense for facts and the imperative for steps.
  Use past tense only for work that is complete.
- Keep the articles "a", "an", and "the". Do not drop words to look terse.
- Use one term for one concept, and give each term only one meaning.
- Do not stack more than three nouns. Rewrite "Bistro scale-100 meshlet gate
  case" as "the meshlet gate case for Bistro at scale 100".
- Name the subject when "it", "this", or "that" could refer to two things.
- Keep a paragraph to one topic and six sentences or fewer.

The other 20% is exempt. Keep code, identifiers, paths, units, quotations, and
established project terms exact, even when they break a rule above. Keep a long
sentence when a split would separate a condition from its rule.

Do not rewrite existing documentation, `AGENTS.md`, or skills into this style
unless the user asks for it. Agent instructions use compact wording on purpose.

## Draw diagrams

Put chat diagrams in a fenced `text` block no wider than 44 columns. The user
reads chat on a phone, a laptop, and a desktop, and 44 columns fit all three.
Draw flows from top to bottom with ASCII boxes and arrows. Move a diagram that
cannot fit 44 columns into an HTML page. Label nodes with the exact pass, resource,
thread, or function names. Show one relation in each diagram, such as the pass
order or the owner of a buffer over the frame. Put a one-sentence caption above
the diagram that states what it shows. Do not draw a diagram for a sequence of
two steps; one sentence is shorter.

## Remove slop

- Replace praise, slogans and claims such as "robust" or "efficient" with the
  mechanism, constraint or measured result. Keep an established project motto
  when the surrounding rules define what it requires.
- Cut filler, promotional adjectives, generic conclusions, invented metaphors
  and repeated summaries. Do not add opinions, anecdotes or deliberate disorder
  to make technical writing seem human.
- Turn vague instructions into a trigger, action and observable completion
  condition. State who owns a value and when it changes.
- Use lists for parallel requirements and sentence-case headings. Use bold
  text only for words that the reader must not miss.
- Remove canned contrast such as "not just X, but Y". State the required
  behavior directly. Keep a contrast when it expresses a necessary technical
  boundary.
- Preserve uncertainty and evidence. Punctuation and technical vocabulary are
  tools, not words to ban by pattern.

## Check before sending

Remove each sentence that repeats another sentence or adds no action, fact,
constraint, or reason. Check that shortening did not remove an exception, a
lifetime rule, or an acceptance condition. Check each sentence longer than 25
words and each noun stack longer than three words, and keep it only if it is
exempt.
