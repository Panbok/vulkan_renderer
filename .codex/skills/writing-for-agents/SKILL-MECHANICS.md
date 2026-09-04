# Skill packaging

Each skill requires `SKILL.md` with YAML frontmatter containing a nonempty
`name` and `description`. Match the lowercase hyphenated name to its directory.
The description is the discovery cue; the body supplies the procedure.

Keep automatic discovery unless the user requests explicit-only invocation.
For Codex, explicit-only selection is configured in `agents/openai.yaml`:

```yaml
policy:
  allow_implicit_invocation: false
```

Preserve existing interface, policy and dependency fields when editing that
file. Do not use `disable-model-invocation` as portable frontmatter or claim
that invocation settings remove all context cost. See the
[official skill documentation](https://learn.chatgpt.com/docs/build-skills)
and the installed `skill-creator` instructions for the current host schema.
Do not change another agent's invocation policy based on a Codex setting.

Keep references as ordinary linked files. A router states the condition for
reading each skill/reference; it does not require creating another agent or
loading every target. Scripts earn a place when they replace repeated error-prone
work. Run them and document actual dependencies and failure behavior.

In this repository, mirror every skill file, script and metadata file into
`.claude/skills/` and check `diff -rq .codex/skills .claude/skills`.
Use the installed skill creator's `scripts/quick_validate.py` when available;
its YAML parser requires PyYAML. Install a missing dependency in an isolated
temporary environment, or report exactly which structural checks an available
parser performed. A frontmatter pass proves packaging only; validate tool
behavior and workflow decisions separately.
