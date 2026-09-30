#include "editor_code.h"

#include "editor_internal.h"
#include "editor_scripts.h"

#include "core/logger.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "platform/vkr_platform.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if !defined(VKR_EDITOR_SCRIPT_SDK_ROOT)
#define VKR_EDITOR_SCRIPT_SDK_ROOT ""
#endif

#define CODE_DOCUMENT_MAX 8u
#define CODE_UNDO_MAX 64u
#define CODE_FILE_LIMIT (1u << 20)
#define CODE_INDENT 2u
#define CODE_TAB_COLUMNS 4u
#define CODE_FONT_PT 13.0f
#define CODE_LINE_PT 19.0f
#define CODE_VISIBLE_MAX 200u
#define CODE_SPAN_MAX 96u
#define CODE_COMPLETION_MAX 10u
#define CODE_SYMBOL_MAX 6144u
#define CODE_PROBLEM_ROWS 4u
#define CODE_ROW_PT 22.0f
#define CODE_COALESCE_SECONDS 1.0
#define CODE_POLL_SECONDS 1.0
#define CODE_REPEAT_DELAY 0.4
#define CODE_REPEAT_INTERVAL 0.035

// =============================================================================
// Types
// =============================================================================

typedef struct CodeSnapshot {
  uint8_t *text;
  uint32_t length;
  uint32_t caret;
  uint32_t anchor;
} CodeSnapshot;

typedef enum CodeEditKind {
  CODE_EDIT_NONE = 0,
  CODE_EDIT_TYPE,
  CODE_EDIT_DELETE,
  CODE_EDIT_OTHER,
} CodeEditKind;

typedef struct CodeDocument {
  char path[VKR_EDITOR_SCRIPT_PATH];
  char name[128];
  uint8_t *text;
  uint32_t length;
  uint32_t capacity;
  /* Line start offsets, and whether each line starts inside a block
     comment; rebuilt after every change. */
  uint32_t *lines;
  bool8_t *in_comment;
  uint32_t line_count;
  uint32_t line_capacity;
  uint32_t caret;
  uint32_t anchor;
  uint32_t goal_column;
  bool8_t goal_valid;
  uint32_t first_line;
  float32_t scroll_x_pt;
  bool8_t reveal;
  CodeSnapshot undo[CODE_UNDO_MAX];
  uint32_t undo_count;
  CodeSnapshot redo[CODE_UNDO_MAX];
  uint32_t redo_count;
  CodeEditKind last_edit;
  float64_t last_edit_time;
  bool8_t dirty;
  uint64_t disk_fingerprint;
  bool8_t changed_on_disk;
} CodeDocument;

typedef enum CodeSymbolKind {
  CODE_SYMBOL_KEYWORD = 0,
  CODE_SYMBOL_TYPE,
  CODE_SYMBOL_FUNCTION,
  CODE_SYMBOL_MEMBER,
  CODE_SYMBOL_CONSTANT,
  CODE_SYMBOL_LOCAL,
} CodeSymbolKind;

typedef struct CodeSymbol {
  const char *name;
  /* The declaration a completion row shows; NULL for none. */
  const char *detail;
  /* Struct whose member this is; NULL otherwise. */
  const char *owner;
  CodeSymbolKind kind;
} CodeSymbol;

typedef struct CodeSymbols {
  Arena *arena;
  VkrAllocator allocator;
  CodeSymbol items[CODE_SYMBOL_MAX];
  uint32_t count;
} CodeSymbols;

typedef struct CodeCompletion {
  bool8_t open;
  /* Byte offset where the completed word starts. */
  uint32_t start;
  uint32_t count;
  uint32_t selected;
  CodeSymbol items[CODE_COMPLETION_MAX];
} CodeCompletion;

struct VkrEditorCode {
  VkrAllocator *allocator;
  CodeDocument *documents[CODE_DOCUMENT_MAX];
  uint32_t document_count;
  uint32_t active;
  /* The script SDK's names, read once; the open file's, per completion. */
  CodeSymbols sdk;
  CodeSymbols local;
  bool8_t sdk_loaded;
  CodeCompletion completion;
  VkrUiId view_id;
  VkrUiRect view_rect;
  /* Opening, jumping or typing by command focuses the view next build. */
  bool8_t focus_request;
  uint32_t visible_lines;
  /* Last completion list rectangle: presses there are the list's. */
  VkrUiRect popup_rect;
  bool8_t focused;
  bool8_t dragging;
  Keys repeat_key;
  float64_t repeat_next;
  float64_t click_time;
  uint32_t click_offset;
  uint32_t click_count;
  float64_t poll_time;
  bool8_t naming;
  bool8_t naming_focus;
  uint8_t name_text[64];
  uint32_t name_length;
  char status[256];
};

// =============================================================================
// Text helpers
// =============================================================================

static bool8_t code_word_byte(uint8_t c) {
  return isalnum(c) || c == '_' || c >= 0x80u;
}

static uint32_t code_next_codepoint(const CodeDocument *doc, uint32_t at) {
  if (at >= doc->length) {
    return doc->length;
  }
  ++at;
  while (at < doc->length && (doc->text[at] & 0xc0u) == 0x80u) {
    ++at;
  }
  return at;
}

static uint32_t code_previous_codepoint(const CodeDocument *doc, uint32_t at) {
  if (!at) {
    return 0u;
  }
  --at;
  while (at > 0u && (doc->text[at] & 0xc0u) == 0x80u) {
    --at;
  }
  return at;
}

static uint64_t code_hash(const uint8_t *data, uint32_t length) {
  uint64_t hash = UINT64_C(1469598103934665603);
  for (uint32_t i = 0; i < length; ++i) {
    hash = (hash ^ data[i]) * UINT64_C(1099511628211);
  }
  return hash;
}

static uint32_t code_line_of(const CodeDocument *doc, uint32_t offset) {
  uint32_t low = 0u;
  uint32_t high = doc->line_count;
  while (low + 1u < high) {
    const uint32_t middle = (low + high) / 2u;
    if (doc->lines[middle] <= offset) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return low;
}

static uint32_t code_line_end(const CodeDocument *doc, uint32_t line) {
  return line + 1u < doc->line_count ? doc->lines[line + 1u] - 1u : doc->length;
}

/* Display column of `offset` within its line, with tabs expanded like the
 * code view draws them. */
static uint32_t code_column(const CodeDocument *doc, uint32_t offset) {
  const uint32_t line = code_line_of(doc, offset);
  uint32_t column = 0u;
  for (uint32_t at = doc->lines[line]; at < offset;
       at = code_next_codepoint(doc, at)) {
    column = doc->text[at] == '\t'
                 ? (column / CODE_TAB_COLUMNS + 1u) * CODE_TAB_COLUMNS
                 : column + 1u;
  }
  return column;
}

/* The offset on `line` nearest display column `column`. */
static uint32_t code_offset_at(const CodeDocument *doc, uint32_t line,
                               uint32_t column) {
  const uint32_t end = code_line_end(doc, line);
  uint32_t at = doc->lines[line];
  uint32_t current = 0u;
  while (at < end && current < column) {
    const uint32_t next =
        doc->text[at] == '\t'
            ? (current / CODE_TAB_COLUMNS + 1u) * CODE_TAB_COLUMNS
            : current + 1u;
    if (next > column && next - column > column - current) {
      break;
    }
    current = next;
    at = code_next_codepoint(doc, at);
  }
  return at;
}

static void code_index_lines(VkrEditorCode *code, CodeDocument *doc) {
  uint32_t count = 1u;
  for (uint32_t i = 0; i < doc->length; ++i) {
    count += doc->text[i] == '\n';
  }
  if (count > doc->line_capacity) {
    const uint32_t capacity = Max(count, doc->line_capacity * 2u);
    uint32_t *lines =
        vkr_allocator_alloc(code->allocator, capacity * sizeof(uint32_t),
                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    bool8_t *comments = vkr_allocator_alloc(code->allocator, capacity,
                                            VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    if (!lines || !comments) {
      return;
    }
    if (doc->lines) {
      vkr_allocator_free(code->allocator, doc->lines,
                         doc->line_capacity * sizeof(uint32_t),
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
      vkr_allocator_free(code->allocator, doc->in_comment, doc->line_capacity,
                         VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    }
    doc->lines = lines;
    doc->in_comment = comments;
    doc->line_capacity = capacity;
  }
  doc->line_count = 0u;
  doc->lines[doc->line_count] = 0u;
  doc->in_comment[doc->line_count++] = false_v;
  bool8_t comment = false_v;
  bool8_t in_string = false_v;
  uint8_t quote = 0u;
  for (uint32_t i = 0; i < doc->length; ++i) {
    const uint8_t c = doc->text[i];
    const uint8_t next = i + 1u < doc->length ? doc->text[i + 1u] : 0u;
    if (c == '\n') {
      in_string = false_v;
      doc->lines[doc->line_count] = i + 1u;
      doc->in_comment[doc->line_count++] = comment;
    } else if (comment) {
      if (c == '*' && next == '/') {
        comment = false_v;
        ++i;
      }
    } else if (in_string) {
      if (c == '\\') {
        ++i;
      } else if (c == quote) {
        in_string = false_v;
      }
    } else if (c == '/' && next == '*') {
      comment = true_v;
      ++i;
    } else if (c == '/' && next == '/') {
      while (i + 1u < doc->length && doc->text[i + 1u] != '\n') {
        ++i;
      }
    } else if (c == '"' || c == '\'') {
      in_string = true_v;
      quote = c;
    }
  }
}

static bool8_t code_reserve(VkrEditorCode *code, CodeDocument *doc,
                            uint32_t length) {
  if (length + 1u <= doc->capacity) {
    return true_v;
  }
  const uint32_t capacity = Max(length + 1u, Max(doc->capacity * 2u, 4096u));
  uint8_t *text = vkr_allocator_alloc(code->allocator, capacity,
                                      VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!text) {
    return false_v;
  }
  if (doc->text) {
    MemCopy(text, doc->text, doc->length);
    vkr_allocator_free(code->allocator, doc->text, doc->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  doc->text = text;
  doc->capacity = capacity;
  return true_v;
}

// =============================================================================
// Undo
// =============================================================================

static void code_snapshot_free(VkrEditorCode *code, CodeSnapshot *snapshot) {
  if (snapshot->text) {
    vkr_allocator_free(code->allocator, snapshot->text, snapshot->length + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  *snapshot = (CodeSnapshot){0};
}

static bool8_t code_snapshot_take(VkrEditorCode *code, const CodeDocument *doc,
                                  CodeSnapshot *out) {
  out->text = vkr_allocator_alloc(code->allocator, doc->length + 1u,
                                  VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (!out->text) {
    return false_v;
  }
  MemCopy(out->text, doc->text, doc->length);
  out->length = doc->length;
  out->caret = doc->caret;
  out->anchor = doc->anchor;
  return true_v;
}

static void code_stack_push(VkrEditorCode *code, CodeSnapshot *stack,
                            uint32_t *count, CodeSnapshot snapshot) {
  if (*count == CODE_UNDO_MAX) {
    code_snapshot_free(code, &stack[0]);
    MemCopy(stack, stack + 1, (CODE_UNDO_MAX - 1u) * sizeof(*stack));
    --*count;
  }
  stack[(*count)++] = snapshot;
}

static void code_stack_clear(VkrEditorCode *code, CodeSnapshot *stack,
                             uint32_t *count) {
  while (*count) {
    code_snapshot_free(code, &stack[--*count]);
  }
}

/* One undo step per run of typing or deleting, and per other edit. */
static void code_begin_edit(VkrEditorCode *code, CodeDocument *doc,
                            CodeEditKind kind) {
  const float64_t now = vkr_platform_get_absolute_time();
  if (kind != doc->last_edit || kind == CODE_EDIT_OTHER ||
      now - doc->last_edit_time > CODE_COALESCE_SECONDS || !doc->undo_count) {
    CodeSnapshot snapshot;
    if (code_snapshot_take(code, doc, &snapshot)) {
      code_stack_push(code, doc->undo, &doc->undo_count, snapshot);
    }
  }
  code_stack_clear(code, doc->redo, &doc->redo_count);
  doc->last_edit = kind;
  doc->last_edit_time = now;
}

static void code_restore(VkrEditorCode *code, CodeDocument *doc,
                         CodeSnapshot *from, CodeSnapshot *to_stack,
                         uint32_t *to_count) {
  CodeSnapshot current;
  if (!code_snapshot_take(code, doc, &current) ||
      !code_reserve(code, doc, from->length)) {
    return;
  }
  code_stack_push(code, to_stack, to_count, current);
  MemCopy(doc->text, from->text, from->length);
  doc->length = from->length;
  doc->caret = Min(from->caret, doc->length);
  doc->anchor = Min(from->anchor, doc->length);
  code_snapshot_free(code, from);
  doc->dirty = true_v;
  doc->last_edit = CODE_EDIT_NONE;
  doc->reveal = true_v;
  code_index_lines(code, doc);
}

static void code_undo(VkrEditorCode *code, CodeDocument *doc) {
  if (doc->undo_count) {
    CodeSnapshot snapshot = doc->undo[--doc->undo_count];
    code_restore(code, doc, &snapshot, doc->redo, &doc->redo_count);
  }
}

static void code_redo(VkrEditorCode *code, CodeDocument *doc) {
  if (doc->redo_count) {
    CodeSnapshot snapshot = doc->redo[--doc->redo_count];
    code_restore(code, doc, &snapshot, doc->undo, &doc->undo_count);
  }
}

// =============================================================================
// Editing
// =============================================================================

static uint32_t code_selection_begin(const CodeDocument *doc) {
  return Min(doc->caret, doc->anchor);
}

static uint32_t code_selection_end(const CodeDocument *doc) {
  return Max(doc->caret, doc->anchor);
}

/* Replaces [begin, end) with `text`; the caret lands after it. */
static bool8_t code_replace(VkrEditorCode *code, CodeDocument *doc,
                            uint32_t begin, uint32_t end, const uint8_t *text,
                            uint32_t length) {
  const uint32_t removed = end - begin;
  if (doc->length - removed + length > CODE_FILE_LIMIT ||
      !code_reserve(code, doc, doc->length - removed + length)) {
    return false_v;
  }
  MemCopy(doc->text + begin + length, doc->text + end, doc->length - end);
  if (length) {
    MemCopy(doc->text + begin, text, length);
  }
  doc->length = doc->length - removed + length;
  doc->caret = doc->anchor = begin + length;
  doc->goal_valid = false_v;
  doc->dirty = true_v;
  doc->reveal = true_v;
  code_index_lines(code, doc);
  return true_v;
}

static void code_insert(VkrEditorCode *code, CodeDocument *doc,
                        const char *text, CodeEditKind kind) {
  code_begin_edit(code, doc, kind);
  (void)code_replace(code, doc, code_selection_begin(doc),
                     code_selection_end(doc), (const uint8_t *)text,
                     (uint32_t)strlen(text));
}

static uint32_t code_indent_of(const CodeDocument *doc, uint32_t line) {
  uint32_t at = doc->lines[line];
  const uint32_t end = code_line_end(doc, line);
  while (at < end && (doc->text[at] == ' ' || doc->text[at] == '\t')) {
    ++at;
  }
  return at - doc->lines[line];
}

/* A newline keeps the line's indentation, adds a level after `{`, and puts a
 * closing brace right after the caret on its own line. */
static void code_newline(VkrEditorCode *code, CodeDocument *doc) {
  const uint32_t begin = code_selection_begin(doc);
  const uint32_t line = code_line_of(doc, begin);
  uint32_t indent = Min(code_indent_of(doc, line), begin - doc->lines[line]);
  uint32_t before = begin;
  while (before > doc->lines[line] && doc->text[before - 1u] == ' ') {
    --before;
  }
  const bool8_t opens =
      before > doc->lines[line] && doc->text[before - 1u] == '{';
  const bool8_t closes = code_selection_end(doc) < doc->length &&
                         doc->text[code_selection_end(doc)] == '}';
  char text[512];
  uint32_t length = 0u;
  text[length++] = '\n';
  for (uint32_t i = 0; i < indent && length + 1u < sizeof(text); ++i) {
    text[length++] = (char)doc->text[doc->lines[line] + i];
  }
  uint32_t caret = length;
  if (opens) {
    for (uint32_t i = 0; i < CODE_INDENT; ++i) {
      text[length++] = ' ';
    }
    caret = length;
    if (closes) {
      text[length++] = '\n';
      for (uint32_t i = 0; i < indent && length + 1u < sizeof(text); ++i) {
        text[length++] = (char)doc->text[doc->lines[line] + i];
      }
    }
  }
  text[length] = '\0';
  code_insert(code, doc, text, CODE_EDIT_OTHER);
  doc->caret = doc->anchor = begin + caret;
}

/* Typing `}` as the first text of a line removes one indentation level. */
static void code_type(VkrEditorCode *code, CodeDocument *doc,
                      uint32_t codepoint) {
  char text[5] = {0};
  if (codepoint < 0x80u) {
    text[0] = (char)codepoint;
  } else if (codepoint < 0x800u) {
    text[0] = (char)(0xc0u | (codepoint >> 6u));
    text[1] = (char)(0x80u | (codepoint & 0x3fu));
  } else if (codepoint < 0x10000u) {
    text[0] = (char)(0xe0u | (codepoint >> 12u));
    text[1] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
    text[2] = (char)(0x80u | (codepoint & 0x3fu));
  } else {
    text[0] = (char)(0xf0u | (codepoint >> 18u));
    text[1] = (char)(0x80u | ((codepoint >> 12u) & 0x3fu));
    text[2] = (char)(0x80u | ((codepoint >> 6u) & 0x3fu));
    text[3] = (char)(0x80u | (codepoint & 0x3fu));
  }
  code_insert(code, doc, text, CODE_EDIT_TYPE);
  if (codepoint == '}') {
    const uint32_t line = code_line_of(doc, doc->caret);
    const uint32_t indent = code_indent_of(doc, line);
    if (doc->lines[line] + indent + 1u == doc->caret && indent >= CODE_INDENT) {
      const uint32_t start = doc->lines[line] + indent - CODE_INDENT;
      (void)code_replace(code, doc, start, start + CODE_INDENT, NULL, 0u);
      doc->caret = doc->anchor = start + 1u;
    }
  }
}

/* Tab indents the selected lines, or inserts spaces to the next stop. */
static void code_indent(VkrEditorCode *code, CodeDocument *doc,
                        bool8_t outdent) {
  const uint32_t first = code_line_of(doc, code_selection_begin(doc));
  const uint32_t last = code_line_of(doc, code_selection_end(doc));
  if (first == last && !outdent) {
    const uint32_t column = code_column(doc, doc->caret);
    const uint32_t count = CODE_INDENT - column % CODE_INDENT;
    code_insert(code, doc, count == 1u ? " " : "  ", CODE_EDIT_TYPE);
    return;
  }
  code_begin_edit(code, doc, CODE_EDIT_OTHER);
  const bool8_t caret_first = doc->caret <= doc->anchor;
  for (uint32_t line = last + 1u; line-- > first;) {
    const uint32_t start = doc->lines[line];
    if (outdent) {
      const uint32_t remove = Min(code_indent_of(doc, line), CODE_INDENT);
      (void)code_replace(code, doc, start, start + remove, NULL, 0u);
    } else if (start < code_line_end(doc, line)) {
      (void)code_replace(code, doc, start, start, (const uint8_t *)"  ",
                         CODE_INDENT);
    }
  }
  const uint32_t begin = doc->lines[first];
  const uint32_t end = code_line_end(doc, Min(last, doc->line_count - 1u));
  doc->anchor = caret_first ? end : begin;
  doc->caret = caret_first ? begin : end;
}

static void code_delete(VkrEditorCode *code, CodeDocument *doc, bool8_t forward,
                        bool8_t word) {
  uint32_t begin = code_selection_begin(doc);
  uint32_t end = code_selection_end(doc);
  if (begin == end) {
    if (forward) {
      end = code_next_codepoint(doc, end);
      while (word && end < doc->length && code_word_byte(doc->text[end])) {
        ++end;
      }
    } else {
      const uint32_t line = code_line_of(doc, begin);
      const uint32_t indent = code_indent_of(doc, line);
      /* Backspace in leading spaces removes to the previous stop. */
      if (!word && begin > doc->lines[line] &&
          begin <= doc->lines[line] + indent) {
        const uint32_t column = begin - doc->lines[line];
        begin -= column % CODE_INDENT ? column % CODE_INDENT : CODE_INDENT;
        begin = Max(begin, doc->lines[line]);
      } else {
        begin = code_previous_codepoint(doc, begin);
        while (word && begin > 0u && code_word_byte(doc->text[begin - 1u])) {
          --begin;
        }
      }
    }
  }
  if (begin != end) {
    code_begin_edit(code, doc, CODE_EDIT_DELETE);
    (void)code_replace(code, doc, begin, end, NULL, 0u);
  }
}

static void code_select_word(CodeDocument *doc, uint32_t at) {
  uint32_t begin = at;
  uint32_t end = at;
  while (begin > 0u && code_word_byte(doc->text[begin - 1u])) {
    --begin;
  }
  while (end < doc->length && code_word_byte(doc->text[end])) {
    ++end;
  }
  doc->anchor = begin;
  doc->caret = end;
}

// =============================================================================
// Documents
// =============================================================================

static CodeDocument *code_active(VkrEditorCode *code) {
  return code->document_count && code->active < code->document_count
             ? code->documents[code->active]
             : NULL;
}

static void code_document_free(VkrEditorCode *code, CodeDocument *doc) {
  code_stack_clear(code, doc->undo, &doc->undo_count);
  code_stack_clear(code, doc->redo, &doc->redo_count);
  if (doc->text) {
    vkr_allocator_free(code->allocator, doc->text, doc->capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
  if (doc->lines) {
    vkr_allocator_free(code->allocator, doc->lines,
                       doc->line_capacity * sizeof(uint32_t),
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
    vkr_allocator_free(code->allocator, doc->in_comment, doc->line_capacity,
                       VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  }
  vkr_allocator_free(code->allocator, doc, sizeof(*doc),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

/* Reads the file, dropping carriage returns; false keeps the buffer. */
static bool8_t code_read_file(VkrEditorCode *code, CodeDocument *doc) {
  FILE *file = file_fopen(doc->path, "rb");
  if (!file) {
    return false_v;
  }
  bool8_t ok = false_v;
  if (fseek(file, 0, SEEK_END) == 0) {
    const long size = ftell(file);
    if (size >= 0 && size <= (long)CODE_FILE_LIMIT &&
        fseek(file, 0, SEEK_SET) == 0 &&
        code_reserve(code, doc, (uint32_t)size)) {
      const size_t read = fread(doc->text, 1u, (size_t)size, file);
      if (read == (size_t)size) {
        uint32_t length = 0u;
        for (uint32_t i = 0; i < (uint32_t)size; ++i) {
          if (doc->text[i] != '\r') {
            doc->text[length++] = doc->text[i];
          }
        }
        doc->length = length;
        doc->disk_fingerprint = code_hash(doc->text, doc->length);
        ok = true_v;
      }
    }
  }
  fclose(file);
  if (ok) {
    doc->caret = Min(doc->caret, doc->length);
    doc->anchor = Min(doc->anchor, doc->length);
    doc->dirty = false_v;
    doc->changed_on_disk = false_v;
    code_index_lines(code, doc);
  }
  return ok;
}

static bool8_t code_save(VkrEditorCode *code, VkrEditorUi *editor,
                         CodeDocument *doc) {
  FILE *file = file_fopen(doc->path, "wb");
  bool8_t ok = file && fwrite(doc->text, 1u, doc->length, file) == doc->length;
  if (file && fclose(file) != 0) {
    ok = false_v;
  }
  if (!ok) {
    snprintf(code->status, sizeof(code->status), "Cannot save %s", doc->name);
    return false_v;
  }
  doc->dirty = false_v;
  doc->changed_on_disk = false_v;
  doc->disk_fingerprint = code_hash(doc->text, doc->length);
  vkr_editor_scripts_rebuild_file(editor->scripts, doc->path);
  snprintf(code->status, sizeof(code->status), "Saved %s; rebuilding",
           doc->name);
  return true_v;
}

static void code_close(VkrEditorCode *code, uint32_t index) {
  if (index >= code->document_count) {
    return;
  }
  code_document_free(code, code->documents[index]);
  MemCopy(&code->documents[index], &code->documents[index + 1u],
          (code->document_count - index - 1u) * sizeof(code->documents[0]));
  code->document_count--;
  if (code->active >= code->document_count && code->active) {
    code->active = code->document_count - 1u;
  }
  code->completion.open = false_v;
}

/* Reloads unmodified tabs whose file changed; marks modified ones. */
static void code_poll_files(VkrEditorCode *code) {
  const float64_t now = vkr_platform_get_absolute_time();
  if (now - code->poll_time < CODE_POLL_SECONDS) {
    return;
  }
  code->poll_time = now;
  for (uint32_t i = 0; i < code->document_count; ++i) {
    CodeDocument *doc = code->documents[i];
    FILE *file = file_fopen(doc->path, "rb");
    if (!file) {
      continue;
    }
    uint8_t buffer[64u * 1024u];
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t read;
    while ((read = fread(buffer, 1u, sizeof(buffer), file)) > 0u) {
      for (size_t b = 0; b < read; ++b) {
        if (buffer[b] != '\r') {
          hash = (hash ^ buffer[b]) * UINT64_C(1099511628211);
        }
      }
    }
    fclose(file);
    if (hash == doc->disk_fingerprint) {
      continue;
    }
    if (doc->dirty) {
      doc->changed_on_disk = true_v;
    } else {
      (void)code_read_file(code, doc);
    }
  }
}

// =============================================================================
// Syntax highlighting
// =============================================================================

static const char *const s_keywords[] = {
    "auto",          "break",    "case",    "const",     "continue",
    "default",       "do",       "else",    "enum",      "extern",
    "for",           "goto",     "if",      "inline",    "register",
    "restrict",      "return",   "sizeof",  "static",    "struct",
    "switch",        "typedef",  "union",   "volatile",  "while",
    "_Alignas",      "_Alignof", "_Atomic", "_Noreturn", "_Static_assert",
    "_Thread_local", "true",     "false",   "true_v",    "false_v",
    "NULL",          "unsigned", "signed",  "long",      "short",
};

static const char *const s_types[] = {
    "void",     "char",    "int",       "float",     "double",    "bool",
    "_Bool",    "bool8_t", "bool32_t",  "float32_t", "float64_t", "int8_t",
    "int16_t",  "int32_t", "int64_t",   "uint8_t",   "uint16_t",  "uint32_t",
    "uint64_t", "size_t",  "uintptr_t", "intptr_t",  "String8",   "Vec2",
    "Vec3",     "Vec4",    "Mat4",      "Keys",
};

typedef enum CodeColor {
  CODE_COLOR_KEYWORD = 0,
  CODE_COLOR_TYPE,
  CODE_COLOR_FUNCTION,
  CODE_COLOR_STRING,
  CODE_COLOR_NUMBER,
  CODE_COLOR_COMMENT,
  CODE_COLOR_PREPROCESSOR,
  CODE_COLOR_MACRO,
  CODE_COLOR_COUNT
} CodeColor;

/* Authored sRGB, after common dark editor palettes. */
static const Vec4 s_colors[CODE_COLOR_COUNT] = {
    [CODE_COLOR_KEYWORD] = {0.77f, 0.53f, 0.95f, 1.0f},
    [CODE_COLOR_TYPE] = {0.31f, 0.79f, 0.69f, 1.0f},
    [CODE_COLOR_FUNCTION] = {0.86f, 0.86f, 0.60f, 1.0f},
    [CODE_COLOR_STRING] = {0.81f, 0.57f, 0.47f, 1.0f},
    [CODE_COLOR_NUMBER] = {0.71f, 0.81f, 0.66f, 1.0f},
    [CODE_COLOR_COMMENT] = {0.42f, 0.60f, 0.33f, 1.0f},
    [CODE_COLOR_PREPROCESSOR] = {0.77f, 0.53f, 0.75f, 1.0f},
    [CODE_COLOR_MACRO] = {0.34f, 0.61f, 0.84f, 1.0f},
};

static bool8_t code_in_list(const char *const *list, uint32_t count,
                            const uint8_t *word, uint32_t length) {
  for (uint32_t i = 0; i < count; ++i) {
    if (strlen(list[i]) == length && !MemCompare(list[i], word, length)) {
      return true_v;
    }
  }
  return false_v;
}

/* Class of one identifier from its spelling and the next non-blank byte. */
static int32_t code_identifier_color(const uint8_t *word, uint32_t length,
                                     uint8_t next) {
  if (code_in_list(s_keywords, ArrayCount(s_keywords), word, length)) {
    return CODE_COLOR_KEYWORD;
  }
  if (code_in_list(s_types, ArrayCount(s_types), word, length)) {
    return CODE_COLOR_TYPE;
  }
  if (next == '(') {
    return CODE_COLOR_FUNCTION;
  }
  bool8_t upper = false_v;
  bool8_t lower = false_v;
  for (uint32_t i = 0; i < length; ++i) {
    upper |= isupper(word[i]) != 0;
    lower |= islower(word[i]) != 0;
  }
  if (upper && !lower && length > 1u) {
    return CODE_COLOR_MACRO;
  }
  if (isupper(word[0]) && lower) {
    return CODE_COLOR_TYPE;
  }
  if (length > 2u && !MemCompare(word + length - 2u, "_t", 2u)) {
    return CODE_COLOR_TYPE;
  }
  return -1;
}

/* Colored spans of one line; `comment` is whether it starts inside a block
 * comment. */
static uint32_t code_tokenize(const uint8_t *text, uint32_t length,
                              bool8_t comment, VkrUiCodeSpan *spans,
                              uint32_t capacity) {
  uint32_t count = 0u;
  uint32_t i = 0u;
#define CODE_SPAN(start_, end_, color_)                                        \
  do {                                                                         \
    if (count < capacity && (end_) > (start_)) {                               \
      spans[count++] =                                                         \
          (VkrUiCodeSpan){(start_), (end_) - (start_), s_colors[(color_)]};    \
    }                                                                          \
  } while (0)
  if (comment) {
    while (i < length &&
           !(text[i] == '*' && i + 1u < length && text[i + 1u] == '/')) {
      ++i;
    }
    i = Min(length, i + 2u);
    CODE_SPAN(0u, i, CODE_COLOR_COMMENT);
  }
  uint32_t first = i;
  while (first < length && (text[first] == ' ' || text[first] == '\t')) {
    ++first;
  }
  if (first < length && text[first] == '#') {
    uint32_t end = first + 1u;
    while (end < length && text[end] == ' ') {
      ++end;
    }
    while (end < length && isalpha(text[end])) {
      ++end;
    }
    CODE_SPAN(first, end, CODE_COLOR_PREPROCESSOR);
    i = end;
    while (i < length && text[i] == ' ') {
      ++i;
    }
    if (i < length && text[i] == '<') {
      uint32_t close = i;
      while (close < length && text[close] != '>') {
        ++close;
      }
      CODE_SPAN(i, Min(length, close + 1u), CODE_COLOR_STRING);
      i = Min(length, close + 1u);
    }
  }
  while (i < length) {
    const uint8_t c = text[i];
    const uint8_t next = i + 1u < length ? text[i + 1u] : 0u;
    if (c == '/' && next == '/') {
      CODE_SPAN(i, length, CODE_COLOR_COMMENT);
      break;
    }
    if (c == '/' && next == '*') {
      uint32_t end = i + 2u;
      while (end < length && !(text[end] == '*' && end + 1u < length &&
                               text[end + 1u] == '/')) {
        ++end;
      }
      end = Min(length, end + 2u);
      CODE_SPAN(i, end, CODE_COLOR_COMMENT);
      i = end;
      continue;
    }
    if (c == '"' || c == '\'') {
      uint32_t end = i + 1u;
      while (end < length && text[end] != c) {
        end += text[end] == '\\' ? 2u : 1u;
      }
      end = Min(length, end + 1u);
      CODE_SPAN(i, end, CODE_COLOR_STRING);
      i = end;
      continue;
    }
    if (isdigit(c) || (c == '.' && isdigit(next))) {
      uint32_t end = i + 1u;
      while (end < length &&
             (isalnum(text[end]) || text[end] == '.' || text[end] == '_')) {
        ++end;
      }
      CODE_SPAN(i, end, CODE_COLOR_NUMBER);
      i = end;
      continue;
    }
    if (isalpha(c) || c == '_') {
      uint32_t end = i + 1u;
      while (end < length && (isalnum(text[end]) || text[end] == '_')) {
        ++end;
      }
      uint32_t after = end;
      while (after < length && text[after] == ' ') {
        ++after;
      }
      const int32_t color = code_identifier_color(
          text + i, end - i, after < length ? text[after] : 0u);
      if (color >= 0) {
        CODE_SPAN(i, end, color);
      }
      i = end;
      continue;
    }
    ++i;
  }
#undef CODE_SPAN
  return count;
}

// =============================================================================
// Symbols
// =============================================================================

static const char *code_symbols_copy(CodeSymbols *symbols, const char *text,
                                     uint32_t length) {
  char *copy = vkr_allocator_alloc(&symbols->allocator, length + 1u,
                                   VKR_ALLOCATOR_MEMORY_TAG_STRING);
  if (copy) {
    MemCopy(copy, text, length);
    copy[length] = '\0';
  }
  return copy;
}

static void code_symbols_add(CodeSymbols *symbols, const char *name,
                             uint32_t length, const char *detail,
                             uint32_t detail_length, const char *owner,
                             CodeSymbolKind kind) {
  if (symbols->count == CODE_SYMBOL_MAX || length < 2u) {
    return;
  }
  for (uint32_t i = 0; i < symbols->count; ++i) {
    const CodeSymbol *symbol = &symbols->items[i];
    if (strlen(symbol->name) == length &&
        !MemCompare(symbol->name, name, length) &&
        (symbol->owner == owner ||
         (symbol->owner && owner && !strcmp(symbol->owner, owner)))) {
      return;
    }
  }
  CodeSymbol *symbol = &symbols->items[symbols->count];
  symbol->name = code_symbols_copy(symbols, name, length);
  symbol->detail =
      detail_length ? code_symbols_copy(symbols, detail, detail_length) : NULL;
  symbol->owner = owner;
  symbol->kind = kind;
  if (symbol->name) {
    symbols->count++;
  }
}

static bool8_t code_symbols_reset(CodeSymbols *symbols) {
  if (!symbols->arena) {
    symbols->arena = arena_create(MB(1), KB(64));
    if (!symbols->arena) {
      return false_v;
    }
    symbols->allocator = (VkrAllocator){.ctx = symbols->arena};
    if (!vkr_allocator_arena(&symbols->allocator)) {
      return false_v;
    }
  } else {
    arena_clear(symbols->arena, ARENA_MEMORY_TAG_UNKNOWN);
  }
  symbols->count = 0u;
  return true_v;
}

/* Collapses runs of whitespace into single spaces. */
static uint32_t code_squeeze(const uint8_t *text, uint32_t length, char *out,
                             uint32_t capacity) {
  uint32_t count = 0u;
  bool8_t space = false_v;
  for (uint32_t i = 0; i < length && count + 1u < capacity; ++i) {
    const bool8_t blank = isspace(text[i]) != 0;
    if (blank && (space || !count)) {
      continue;
    }
    out[count++] = blank ? ' ' : (char)text[i];
    space = blank;
  }
  while (count && out[count - 1u] == ' ') {
    --count;
  }
  return count;
}

/* A shallow C scan: struct and enum members, file-scope functions, typedef
 * names and macros, each with the declaration it came from. Comments and
 * strings become spaces first. */
static void code_symbols_parse(CodeSymbols *symbols, uint8_t *text,
                               uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) {
    if (text[i] == '/' && i + 1u < length && text[i + 1u] == '/') {
      while (i < length && text[i] != '\n') {
        text[i++] = ' ';
      }
    } else if (text[i] == '/' && i + 1u < length && text[i + 1u] == '*') {
      while (i < length &&
             !(text[i] == '*' && i + 1u < length && text[i + 1u] == '/')) {
        text[i] = text[i] == '\n' ? '\n' : ' ';
        ++i;
      }
      if (i + 1u < length) {
        text[i] = text[i + 1u] = ' ';
      }
    } else if (text[i] == '"') {
      ++i;
      while (i < length && text[i] != '"' && text[i] != '\n') {
        text[i] = ' ';
        ++i;
      }
    }
  }
  uint32_t depth = 0u;
  uint32_t statement = 0u;
  const char *owner = NULL;
  bool8_t in_enum = false_v;
  char tag[64] = {0};
  for (uint32_t i = 0; i < length; ++i) {
    const uint8_t c = text[i];
    if (c == '#') {
      uint32_t at = i + 1u;
      while (at < length && text[at] == ' ') {
        ++at;
      }
      if (length - at > 6u && !MemCompare(text + at, "define", 6u)) {
        at += 6u;
        while (at < length && text[at] == ' ') {
          ++at;
        }
        uint32_t end = at;
        while (end < length && (isalnum(text[end]) || text[end] == '_')) {
          ++end;
        }
        uint32_t line_end = end;
        while (line_end < length && text[line_end] != '\n') {
          ++line_end;
        }
        char detail[160];
        const uint32_t detail_length =
            code_squeeze(text + i, line_end - i, detail, sizeof(detail));
        code_symbols_add(symbols, (const char *)text + at, end - at, detail,
                         detail_length, NULL, CODE_SYMBOL_CONSTANT);
      }
      while (i < length && text[i] != '\n') {
        i += text[i] == '\\' ? 2u : 1u;
      }
      statement = i + 1u;
      continue;
    }
    if (c == '{') {
      /* `struct Tag {` or `enum Tag {`: remember the owner of members. */
      if (depth == 0u) {
        char head[160];
        const uint32_t head_length =
            code_squeeze(text + statement, i - statement, head, sizeof(head));
        head[head_length] = '\0';
        const char *keyword = strstr(head, "struct ");
        const char *enumeration = strstr(head, "enum ");
        tag[0] = '\0';
        if (keyword || enumeration) {
          const char *name = keyword ? keyword + 7 : enumeration + 5;
          uint32_t n = 0u;
          while (name[n] &&
                 (isalnum((unsigned char)name[n]) || name[n] == '_') &&
                 n + 1u < sizeof(tag)) {
            tag[n] = name[n];
            ++n;
          }
          tag[n] = '\0';
        }
        in_enum = enumeration != NULL && keyword == NULL;
        owner = tag[0] ? code_symbols_copy(symbols, tag, (uint32_t)strlen(tag))
                       : NULL;
        if (tag[0]) {
          code_symbols_add(symbols, tag, (uint32_t)strlen(tag), NULL, 0u, NULL,
                           CODE_SYMBOL_TYPE);
        }
      }
      ++depth;
      statement = i + 1u;
      continue;
    }
    if (c == '}') {
      depth = depth ? depth - 1u : 0u;
      statement = i + 1u;
      continue;
    }
    if (depth == 1u && in_enum && (c == ',' || c == '}')) {
      uint32_t at = statement;
      while (at < i && !(isalpha(text[at]) || text[at] == '_')) {
        ++at;
      }
      uint32_t end = at;
      while (end < i && (isalnum(text[end]) || text[end] == '_')) {
        ++end;
      }
      code_symbols_add(symbols, (const char *)text + at, end - at, NULL, 0u,
                       NULL, CODE_SYMBOL_CONSTANT);
      statement = i + 1u;
      continue;
    }
    if (c != ';') {
      continue;
    }
    char detail[200];
    const uint32_t detail_length =
        code_squeeze(text + statement, i - statement, detail, sizeof(detail));
    detail[detail_length] = '\0';
    if (depth == 1u && owner && !in_enum) {
      /* A member: `(*name)(` for function pointers, else the last name
         before an array bound or bit field. */
      const char *name = strstr(detail, "(*");
      uint32_t name_length = 0u;
      if (name) {
        name += 2;
      } else {
        const char *cut = strpbrk(detail, "[:");
        const char *limit = cut ? cut : detail + detail_length;
        name = limit;
        while (name > detail &&
               (isalnum((unsigned char)name[-1]) || name[-1] == '_')) {
          --name;
        }
        while (name > detail && name[-1] == ' ' && name == limit) {
          --name;
        }
      }
      while (name[name_length] && (isalnum((unsigned char)name[name_length]) ||
                                   name[name_length] == '_')) {
        ++name_length;
      }
      code_symbols_add(symbols, name, name_length, detail, detail_length, owner,
                       CODE_SYMBOL_MEMBER);
    } else if (depth == 0u) {
      const char *paren = strchr(detail, '(');
      if (!strncmp(detail, "typedef", 7) || (paren && strstr(detail, "(*"))) {
        /* typedef ... Name; or `} Name;` after a struct body. */
        const char *end = detail + detail_length;
        const char *name = end;
        if (paren && strstr(detail, "(*")) {
          name = strstr(detail, "(*") + 2;
          end = name;
          while (*end && (isalnum((unsigned char)*end) || *end == '_')) {
            ++end;
          }
        } else {
          while (name > detail &&
                 (isalnum((unsigned char)name[-1]) || name[-1] == '_')) {
            --name;
          }
        }
        code_symbols_add(symbols, name, (uint32_t)(end - name), NULL, 0u, NULL,
                         CODE_SYMBOL_TYPE);
      } else if (paren) {
        const char *name = paren;
        while (name > detail && name[-1] == ' ') {
          --name;
        }
        const char *end = name;
        while (name > detail &&
               (isalnum((unsigned char)name[-1]) || name[-1] == '_')) {
          --name;
        }
        code_symbols_add(symbols, name, (uint32_t)(end - name), detail,
                         detail_length, NULL, CODE_SYMBOL_FUNCTION);
      } else if (detail_length && detail[0] == ' ') {
        /* Nothing: a stray statement. */
      } else if (tag[0] || owner) {
        /* `} Alias;` closing a typedef struct: members answer to it too. */
        const char *name = detail;
        uint32_t name_length = 0u;
        while (name[name_length] &&
               (isalnum((unsigned char)name[name_length]) ||
                name[name_length] == '_')) {
          ++name_length;
        }
        if (name_length) {
          code_symbols_add(symbols, name, name_length, NULL, 0u, NULL,
                           CODE_SYMBOL_TYPE);
        }
      }
      owner = NULL;
      tag[0] = '\0';
      in_enum = false_v;
    }
    statement = i + 1u;
  }
}

static void code_symbols_parse_file(VkrEditorCode *code, CodeSymbols *symbols,
                                    const char *path) {
  FILE *file = file_fopen(path, "rb");
  if (!file) {
    return;
  }
  uint8_t *text = NULL;
  long size = 0;
  if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 &&
      size < (long)CODE_FILE_LIMIT && fseek(file, 0, SEEK_SET) == 0) {
    text = vkr_allocator_alloc(code->allocator, (uint64_t)size + 1u,
                               VKR_ALLOCATOR_MEMORY_TAG_FILE);
    if (text && fread(text, 1u, (size_t)size, file) == (size_t)size) {
      code_symbols_parse(symbols, text, (uint32_t)size);
    }
  }
  fclose(file);
  if (text) {
    vkr_allocator_free(code->allocator, text, (uint64_t)size + 1u,
                       VKR_ALLOCATOR_MEMORY_TAG_FILE);
  }
}

/* The headers a script includes through script/vkr_script.h. */
static void code_load_sdk(VkrEditorCode *code) {
  if (code->sdk_loaded) {
    return;
  }
  code->sdk_loaded = true_v;
  if (!code_symbols_reset(&code->sdk)) {
    return;
  }
  static const char *const headers[] = {
      "runtime/src/script/vkr_script.h",
      "runtime/src/core/vkr_type_desc.h",
      "runtime/src/physics/vkr_physics.h",
      "runtime/src/renderer/systems/vkr_scene_physics.h",
      "runtime/src/core/vkr_entity.h",
      "lib/src/math/vec.h",
      "lib/src/math/mat.h",
      "lib/src/math/vkr_quat.h",
      "lib/src/defines.h",
      "lib/src/core/logger.h",
  };
  for (uint32_t i = 0; i < ArrayCount(headers); ++i) {
    char path[VKR_EDITOR_SCRIPT_PATH];
    snprintf(path, sizeof(path), "%s/%s", VKR_EDITOR_SCRIPT_SDK_ROOT,
             headers[i]);
    code_symbols_parse_file(code, &code->sdk, path);
  }
  for (uint32_t i = 0; i < ArrayCount(s_keywords); ++i) {
    code_symbols_add(&code->sdk, s_keywords[i], (uint32_t)strlen(s_keywords[i]),
                     NULL, 0u, NULL, CODE_SYMBOL_KEYWORD);
  }
  for (uint32_t i = 0; i < ArrayCount(s_types); ++i) {
    code_symbols_add(&code->sdk, s_types[i], (uint32_t)strlen(s_types[i]), NULL,
                     0u, NULL, CODE_SYMBOL_TYPE);
  }
}

// =============================================================================
// Completion
// =============================================================================

/* The struct a variable name most likely points to, from the script API's
 * naming conventions and the open file's declarations. */
static const char *code_owner_of(const CodeDocument *doc, uint32_t before,
                                 const uint8_t *name, uint32_t length,
                                 char *out, uint32_t capacity) {
  static const char *const known[][2] = {
      {"api", "VkrScriptApi"},         {"session", "VkrScriptSession"},
      {"frame", "VkrScriptFrame"},     {"view", "VkrScriptView"},
      {"desc", "VkrScriptModuleDesc"}, {"type", "VkrTypeDesc"},
  };
  for (uint32_t i = 0; i < ArrayCount(known); ++i) {
    if (strlen(known[i][0]) == length &&
        !MemCompare(known[i][0], name, length)) {
      snprintf(out, capacity, "%s", known[i][1]);
      return out;
    }
  }
  /* `Type *name` or `Type name` earlier in the file. */
  for (uint32_t at = before; at-- > 0u;) {
    if (at + length > doc->length || MemCompare(doc->text + at, name, length) ||
        (at && code_word_byte(doc->text[at - 1u])) ||
        (at + length < doc->length && code_word_byte(doc->text[at + length]))) {
      continue;
    }
    uint32_t cursor = at;
    while (cursor > 0u &&
           (doc->text[cursor - 1u] == ' ' || doc->text[cursor - 1u] == '*')) {
      --cursor;
    }
    uint32_t start = cursor;
    while (start > 0u && code_word_byte(doc->text[start - 1u])) {
      --start;
    }
    if (cursor > start && cursor - start < capacity &&
        isupper(doc->text[start])) {
      MemCopy(out, doc->text + start, cursor - start);
      out[cursor - start] = '\0';
      return out;
    }
  }
  return NULL;
}

static bool8_t code_prefix_match(const char *name, const uint8_t *prefix,
                                 uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) {
    if (!name[i] || tolower((unsigned char)name[i]) != tolower(prefix[i])) {
      return false_v;
    }
  }
  return true_v;
}

static void code_completion_offer(CodeCompletion *completion,
                                  const CodeSymbol *symbol,
                                  const uint8_t *prefix, uint32_t length) {
  if (!code_prefix_match(symbol->name, prefix, length) ||
      (strlen(symbol->name) == length &&
       !MemCompare(symbol->name, prefix, length))) {
    return;
  }
  for (uint32_t i = 0; i < completion->count; ++i) {
    if (!strcmp(completion->items[i].name, symbol->name)) {
      return;
    }
  }
  /* Exact-case prefixes first, then shorter names. */
  const bool8_t exact = !MemCompare(symbol->name, prefix, length);
  uint32_t at = completion->count;
  while (at > 0u) {
    const CodeSymbol *other = &completion->items[at - 1u];
    const bool8_t other_exact = !MemCompare(other->name, prefix, length);
    if (other_exact && !exact) {
      break;
    }
    if (other_exact == exact && strlen(other->name) <= strlen(symbol->name)) {
      break;
    }
    --at;
  }
  if (at >= CODE_COMPLETION_MAX) {
    return;
  }
  const uint32_t count = Min(completion->count, CODE_COMPLETION_MAX - 1u);
  MemCopy(&completion->items[at + 1u], &completion->items[at],
          (count - at) * sizeof(completion->items[0]));
  completion->items[at] = *symbol;
  completion->count = count + 1u;
}

/* Opens or refreshes the list for the word ending at the caret. `forced`
 * opens it for an empty word, such as after `->` or Ctrl+Space. */
static void code_complete(VkrEditorCode *code, CodeDocument *doc,
                          bool8_t forced) {
  CodeCompletion *completion = &code->completion;
  const uint32_t caret = doc->caret;
  uint32_t start = caret;
  while (start > 0u && code_word_byte(doc->text[start - 1u])) {
    --start;
  }
  const uint8_t *prefix = doc->text + start;
  const uint32_t length = caret - start;
  const bool8_t arrow = start >= 2u && doc->text[start - 1u] == '>' &&
                        doc->text[start - 2u] == '-';
  const bool8_t dot = start >= 1u && doc->text[start - 1u] == '.';
  if ((!forced && !arrow && !dot && length < 2u) ||
      (length && isdigit(prefix[0])) || doc->caret != doc->anchor) {
    completion->open = false_v;
    return;
  }
  code_load_sdk(code);
  if (code_symbols_reset(&code->local)) {
    uint8_t *copy = vkr_allocator_alloc(code->allocator, doc->length + 1u,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
    if (copy) {
      MemCopy(copy, doc->text, doc->length);
      code_symbols_parse(&code->local, copy, doc->length);
      /* Every identifier of the file, for locals the scan does not name. */
      for (uint32_t i = 0; i < doc->length;) {
        if ((isalpha(doc->text[i]) || doc->text[i] == '_') &&
            (i == 0u || !code_word_byte(doc->text[i - 1u]))) {
          uint32_t end = i;
          while (end < doc->length && code_word_byte(doc->text[end])) {
            ++end;
          }
          if (end - i >= 3u && (i != start)) {
            code_symbols_add(&code->local, (const char *)doc->text + i, end - i,
                             NULL, 0u, NULL, CODE_SYMBOL_LOCAL);
          }
          i = end;
        } else {
          ++i;
        }
      }
      vkr_allocator_free(code->allocator, copy, doc->length + 1u,
                         VKR_ALLOCATOR_MEMORY_TAG_STRING);
    }
  }
  completion->count = 0u;
  completion->selected = 0u;
  completion->start = start;
  const CodeSymbols *sources[] = {&code->local, &code->sdk};
  if (arrow || dot) {
    uint32_t end = start - (arrow ? 2u : 1u);
    uint32_t begin = end;
    while (begin > 0u && code_word_byte(doc->text[begin - 1u])) {
      --begin;
    }
    char owner[64] = {0};
    const char *type = code_owner_of(doc, begin, doc->text + begin, end - begin,
                                     owner, sizeof(owner));
    for (uint32_t s = 0; s < ArrayCount(sources); ++s) {
      for (uint32_t i = 0; i < sources[s]->count; ++i) {
        const CodeSymbol *symbol = &sources[s]->items[i];
        if (symbol->kind == CODE_SYMBOL_MEMBER &&
            (!type || (symbol->owner && !strcmp(symbol->owner, type)))) {
          code_completion_offer(completion, symbol, prefix, length);
        }
      }
    }
  } else {
    for (uint32_t s = 0; s < ArrayCount(sources); ++s) {
      for (uint32_t i = 0; i < sources[s]->count; ++i) {
        const CodeSymbol *symbol = &sources[s]->items[i];
        if (symbol->kind != CODE_SYMBOL_MEMBER) {
          code_completion_offer(completion, symbol, prefix, length);
        }
      }
    }
  }
  completion->open = completion->count > 0u;
}

static void code_accept_completion(VkrEditorCode *code, CodeDocument *doc) {
  CodeCompletion *completion = &code->completion;
  if (!completion->open || completion->selected >= completion->count) {
    return;
  }
  const char *name = completion->items[completion->selected].name;
  code_begin_edit(code, doc, CODE_EDIT_OTHER);
  (void)code_replace(code, doc, completion->start, doc->caret,
                     (const uint8_t *)name, (uint32_t)strlen(name));
  completion->open = false_v;
}

// =============================================================================
// Input
// =============================================================================

/* A press, or a held key's repeats after the platform's usual delay. */
static bool8_t code_key(VkrEditorCode *code, const InputState *input,
                        Keys key) {
  const float64_t now = vkr_platform_get_absolute_time();
  if (input_key_just_pressed((InputState *)input, key)) {
    code->repeat_key = key;
    code->repeat_next = now + CODE_REPEAT_DELAY;
    return true_v;
  }
  if (code->repeat_key != key || !input_is_key_down((InputState *)input, key) ||
      now < code->repeat_next) {
    return false_v;
  }
  code->repeat_next = now + CODE_REPEAT_INTERVAL;
  return true_v;
}

static void code_move(CodeDocument *doc, uint32_t to, bool8_t extend) {
  doc->caret = to;
  if (!extend) {
    doc->anchor = to;
  }
  doc->reveal = true_v;
}

static void code_vertical(CodeDocument *doc, int32_t lines, bool8_t extend) {
  const uint32_t line = code_line_of(doc, doc->caret);
  if (!doc->goal_valid) {
    doc->goal_column = code_column(doc, doc->caret);
    doc->goal_valid = true_v;
  }
  const int64_t target =
      Max((int64_t)0, Min((int64_t)doc->line_count - 1, (int64_t)line + lines));
  const uint32_t goal = doc->goal_column;
  code_move(doc, code_offset_at(doc, (uint32_t)target, goal), extend);
  doc->goal_valid = true_v;
  doc->goal_column = goal;
}

static void code_copy(const CodeDocument *doc) {
  const uint32_t begin = code_selection_begin(doc);
  const uint32_t end = code_selection_end(doc);
  if (end > begin) {
    (void)vkr_platform_clipboard_write_text(doc->text + begin, end - begin);
  }
}

static void code_paste(VkrEditorCode *code, CodeDocument *doc) {
  uint8_t *buffer = vkr_allocator_alloc(code->allocator, CODE_FILE_LIMIT,
                                        VKR_ALLOCATOR_MEMORY_TAG_STRING);
  uint32_t length = 0u;
  if (buffer &&
      vkr_platform_clipboard_read_text(buffer, CODE_FILE_LIMIT, &length)) {
    uint32_t kept = 0u;
    for (uint32_t i = 0; i < length; ++i) {
      if (buffer[i] != '\r') {
        buffer[kept++] = buffer[i];
      }
    }
    code_begin_edit(code, doc, CODE_EDIT_OTHER);
    (void)code_replace(code, doc, code_selection_begin(doc),
                       code_selection_end(doc), buffer, kept);
  }
  if (buffer) {
    vkr_allocator_free(code->allocator, buffer, CODE_FILE_LIMIT,
                       VKR_ALLOCATOR_MEMORY_TAG_STRING);
  }
}

/* Typed characters, then the completion list for the word they end. */
static void code_type_text(VkrEditorCode *code, CodeDocument *doc,
                           const uint32_t *characters, uint32_t count) {
  bool8_t typed = false_v;
  for (uint32_t i = 0; i < count; ++i) {
    if (characters[i] >= 0x20u && characters[i] != 0x7fu &&
        (characters[i] < 0xf700u || characters[i] > 0xf8ffu)) {
      code_type(code, doc, characters[i]);
      typed = true_v;
    }
  }
  if (!typed) {
    return;
  }
  const uint8_t last = doc->caret ? doc->text[doc->caret - 1u] : 0u;
  const bool8_t member = last == '.' || (last == '>' && doc->caret >= 2u &&
                                         doc->text[doc->caret - 2u] == '-');
  if (code_word_byte(last) || member) {
    code_complete(code, doc, member);
  } else {
    code->completion.open = false_v;
  }
}

/* Keys while the list is open: returns true when it consumed the key. */
static bool8_t code_completion_keys(VkrEditorCode *code, CodeDocument *doc,
                                    const InputState *input) {
  CodeCompletion *completion = &code->completion;
  if (!completion->open) {
    return false_v;
  }
  if (code_key(code, input, KEY_DOWN)) {
    completion->selected = (completion->selected + 1u) % completion->count;
    return true_v;
  }
  if (code_key(code, input, KEY_UP)) {
    completion->selected =
        (completion->selected + completion->count - 1u) % completion->count;
    return true_v;
  }
  if (input_key_just_pressed((InputState *)input, KEY_TAB) ||
      input_key_just_pressed((InputState *)input, KEY_ENTER)) {
    code_accept_completion(code, doc);
    return true_v;
  }
  if (input_key_just_pressed((InputState *)input, KEY_ESCAPE)) {
    completion->open = false_v;
    return true_v;
  }
  return false_v;
}

static void code_shortcuts(VkrEditorCode *code, VkrEditorUi *editor,
                           CodeDocument *doc, const InputState *input) {
  InputState *keys = (InputState *)input;
  const bool8_t shift_z =
      (input_key_press_modifiers(keys, KEY_Z) & VKR_INPUT_MOD_SHIFT) != 0;
  if (input_key_just_pressed(keys, KEY_S)) {
    (void)code_save(code, editor, doc);
  } else if (input_key_just_pressed(keys, KEY_Z)) {
    shift_z ? code_redo(code, doc) : code_undo(code, doc);
  } else if (input_key_just_pressed(keys, KEY_Y)) {
    code_redo(code, doc);
  } else if (input_key_just_pressed(keys, KEY_A)) {
    doc->anchor = 0u;
    doc->caret = doc->length;
  } else if (input_key_just_pressed(keys, KEY_C)) {
    code_copy(doc);
  } else if (input_key_just_pressed(keys, KEY_X)) {
    code_copy(doc);
    code_delete(code, doc, true_v, false_v);
  } else if (input_key_just_pressed(keys, KEY_V)) {
    code_paste(code, doc);
  }
  code->completion.open = false_v;
}

static void code_keys(VkrEditorCode *code, VkrEditorUi *editor,
                      CodeDocument *doc, const InputState *input) {
  InputState *keys = (InputState *)input;
  static const Keys shortcut_keys[] = {KEY_S, KEY_Z, KEY_Y, KEY_A,
                                       KEY_C, KEY_X, KEY_V};
  for (uint32_t i = 0; i < ArrayCount(shortcut_keys); ++i) {
    if (input_key_just_pressed(keys, shortcut_keys[i]) &&
        input_key_shortcut_modifier(keys, shortcut_keys[i])) {
      code_shortcuts(code, editor, doc, input);
      return;
    }
  }
  if (input_key_just_pressed(keys, KEY_SPACE) &&
      (input_key_press_modifiers(keys, KEY_SPACE) & VKR_INPUT_MOD_CONTROL)) {
    code_complete(code, doc, true_v);
    return;
  }
  if (code_completion_keys(code, doc, input)) {
    return;
  }
  static const Keys moves[] = {KEY_LEFT, KEY_RIGHT, KEY_UP,    KEY_DOWN,
                               KEY_HOME, KEY_END,   KEY_PRIOR, KEY_NEXT};
  const uint32_t page =
      code->visible_lines > 1u ? code->visible_lines - 1u : 1u;
  for (uint32_t i = 0; i < ArrayCount(moves); ++i) {
    if (!code_key(code, input, moves[i])) {
      continue;
    }
    const uint8_t modifiers = input_key_press_modifiers(keys, moves[i]);
    const bool8_t extend = (modifiers & VKR_INPUT_MOD_SHIFT) != 0;
    const bool8_t word =
        (modifiers & (VKR_INPUT_MOD_ALT | VKR_INPUT_MOD_CONTROL)) != 0;
    const bool8_t line_jump = (modifiers & VKR_INPUT_MOD_SUPER) != 0;
    const uint32_t line = code_line_of(doc, doc->caret);
    code->completion.open = false_v;
    if (moves[i] == KEY_UP || moves[i] == KEY_DOWN) {
      if (line_jump) {
        code_move(doc, moves[i] == KEY_UP ? 0u : doc->length, extend);
      } else {
        code_vertical(doc, moves[i] == KEY_UP ? -1 : 1, extend);
      }
      continue;
    }
    if (moves[i] == KEY_PRIOR || moves[i] == KEY_NEXT) {
      code_vertical(doc, moves[i] == KEY_PRIOR ? -(int32_t)page : (int32_t)page,
                    extend);
      continue;
    }
    doc->goal_valid = false_v;
    uint32_t to = doc->caret;
    if (moves[i] == KEY_HOME || (moves[i] == KEY_LEFT && line_jump)) {
      /* The first text of the line, then its very start. */
      const uint32_t text = doc->lines[line] + code_indent_of(doc, line);
      to = doc->caret == text ? doc->lines[line] : text;
    } else if (moves[i] == KEY_END || (moves[i] == KEY_RIGHT && line_jump)) {
      to = code_line_end(doc, line);
    } else if (moves[i] == KEY_LEFT) {
      if (!extend && doc->caret != doc->anchor) {
        to = code_selection_begin(doc);
      } else {
        to = code_previous_codepoint(doc, to);
        while (word && to > 0u && code_word_byte(doc->text[to - 1u])) {
          --to;
        }
      }
    } else {
      if (!extend && doc->caret != doc->anchor) {
        to = code_selection_end(doc);
      } else {
        to = code_next_codepoint(doc, to);
        while (word && to < doc->length && code_word_byte(doc->text[to])) {
          ++to;
        }
      }
    }
    code_move(doc, to, extend);
  }
  if (code_key(code, input, KEY_BACKSPACE)) {
    const uint8_t modifiers = input_key_press_modifiers(keys, KEY_BACKSPACE);
    code_delete(code, doc, false_v,
                (modifiers & (VKR_INPUT_MOD_ALT | VKR_INPUT_MOD_CONTROL)) != 0);
    if (code->completion.open) {
      code_complete(code, doc, false_v);
    }
  }
  if (code_key(code, input, KEY_DELETE)) {
    code_delete(code, doc, true_v, false_v);
    code->completion.open = false_v;
  }
  if (code_key(code, input, KEY_ENTER)) {
    code_newline(code, doc);
    code->completion.open = false_v;
  }
  if (code_key(code, input, KEY_TAB)) {
    code_indent(
        code, doc,
        (input_key_press_modifiers(keys, KEY_TAB) & VKR_INPUT_MOD_SHIFT) != 0);
  }
  if (input_key_just_pressed(keys, KEY_ESCAPE)) {
    doc->anchor = doc->caret;
  }
  uint32_t count = 0u;
  const uint32_t *characters = input_get_characters(input, &count);
  code_type_text(code, doc, characters, count);
}

/* Mouse: click places the caret, Shift extends, a double click selects a
 * word and a triple click the line; dragging selects. */
static void code_mouse(VkrEditorCode *code, CodeDocument *doc, VkrUiSystem *ui,
                       Vec2 cell_px, float32_t gutter_px) {
  const VkrUiRect rect = code->view_rect;
  const bool8_t inside =
      ui->mouse_x >= rect.x && ui->mouse_x < rect.x + rect.width &&
      ui->mouse_y >= rect.y && ui->mouse_y < rect.y + rect.height;
  const VkrUiRect popup = code->popup_rect;
  const bool8_t over_popup = code->completion.open && ui->mouse_x >= popup.x &&
                             ui->mouse_x < popup.x + popup.width &&
                             ui->mouse_y >= popup.y &&
                             ui->mouse_y < popup.y + popup.height;
  const bool8_t hot = ui->hot_id == code->view_id && !over_popup;
  if (hot && inside && ui->mouse_wheel) {
    const int64_t next =
        (int64_t)doc->first_line - (int64_t)ui->mouse_wheel * 3;
    doc->first_line =
        (uint32_t)Max((int64_t)0, Min((int64_t)doc->line_count - 1, next));
    ui->capture.mouse = true_v;
  }
  const float32_t text_left = rect.x + gutter_px + 4.0f * ui->content_scale -
                              doc->scroll_x_pt * ui->content_scale;
  const float32_t y = (float32_t)ui->mouse_y - rect.y;
  const int64_t line =
      (int64_t)doc->first_line + (int64_t)floorf(y / Max(1.0f, cell_px.y));
  const uint32_t target_line =
      (uint32_t)Max((int64_t)0, Min((int64_t)doc->line_count - 1, line));
  const float32_t column_f =
      ((float32_t)ui->mouse_x - text_left) / Max(1.0f, cell_px.x) + 0.5f;
  const uint32_t column = column_f > 0.0f ? (uint32_t)column_f : 0u;
  const uint32_t offset = code_offset_at(doc, target_line, column);
  if (ui->mouse_pressed && hot && inside) {
    const float64_t now = vkr_platform_get_absolute_time();
    const bool8_t again =
        now - code->click_time < 0.4 && code->click_offset == offset;
    code->click_count = again ? code->click_count + 1u : 1u;
    code->click_time = now;
    code->click_offset = offset;
    const bool8_t extend = input_is_key_down(ui->input, KEY_SHIFT);
    code->completion.open = false_v;
    doc->goal_valid = false_v;
    if (code->click_count == 2u) {
      code_select_word(doc, offset);
    } else if (code->click_count >= 3u) {
      doc->anchor = doc->lines[target_line];
      doc->caret = target_line + 1u < doc->line_count
                       ? doc->lines[target_line + 1u]
                       : doc->length;
    } else {
      code_move(doc, offset, extend);
      code->dragging = true_v;
    }
  } else if (code->dragging && input_is_button_down(ui->input, BUTTON_LEFT)) {
    code_move(doc, offset, true_v);
    ui->capture.mouse = true_v;
  }
  if (!input_is_button_down(ui->input, BUTTON_LEFT)) {
    code->dragging = false_v;
  }
}

// =============================================================================
// Window
// =============================================================================

static const VkrEditorScriptDiagnostic *
code_diagnostic_on(const VkrEditorUi *editor, const CodeDocument *doc,
                   uint32_t line) {
  const VkrEditorScriptDiagnostic *found = NULL;
  const uint32_t count = vkr_editor_scripts_diagnostic_count(editor->scripts);
  for (uint32_t i = 0; i < count; ++i) {
    const VkrEditorScriptDiagnostic *diagnostic =
        vkr_editor_scripts_diagnostic(editor->scripts, i);
    if (diagnostic->line == line + 1u &&
        file_path_equals(diagnostic->path, doc->path) &&
        (!found || (diagnostic->error && !found->error))) {
      found = diagnostic;
    }
  }
  return found;
}

/* Keeps the caret's line and column inside the view. */
static void code_reveal(CodeDocument *doc, uint32_t visible_lines,
                        float32_t text_width_pt, float32_t advance_pt) {
  /* Before its first layout the view has no size to reveal into. */
  if (!doc->reveal || text_width_pt <= advance_pt * 8.0f) {
    return;
  }
  doc->reveal = false_v;
  const uint32_t line = code_line_of(doc, doc->caret);
  if (line < doc->first_line) {
    doc->first_line = line;
  } else if (visible_lines && line >= doc->first_line + visible_lines) {
    doc->first_line = line - visible_lines + 1u;
  }
  const float32_t x = (float32_t)code_column(doc, doc->caret) * advance_pt;
  const float32_t margin = advance_pt * 4.0f;
  if (x < doc->scroll_x_pt + margin) {
    doc->scroll_x_pt = Max(0.0f, x - margin);
  } else if (x > doc->scroll_x_pt + text_width_pt - margin) {
    doc->scroll_x_pt = x - text_width_pt + margin;
  }
}

/* A text button filling its cell with room around its label, centered on
   the row. */
static VkrUiWidgetConfig code_button(uint32_t column, uint32_t row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiWidgetConfig config =
      vkr_editor_text_config(theme->font_body, theme->text);
  config.placement.column = column;
  config.placement.row = row;
  config.placement.justify = VKR_UI_ALIGN_STRETCH;
  config.placement.align = VKR_UI_ALIGN_CENTER;
  config.style.min_size_pt.y = theme->control_height;
  config.style.padding_pt = (VkrUiEdges){4, 10, 4, 10};
  config.icon_size_pt = 12.0f;
  return config;
}

static void code_build_tabs(VkrEditorCode *code, VkrEditorUi *editor,
                            VkrUiSystem *ui) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiTrack columns[CODE_DOCUMENT_MAX + 3u];
  uint32_t column_count = 0u;
  for (uint32_t i = 0; i < code->document_count; ++i) {
    columns[column_count++] = (VkrUiTrack){0, VKR_UI_TRACK_AUTO};
  }
  columns[column_count++] = (VkrUiTrack){1, VKR_UI_TRACK_FR};
  columns[column_count++] = (VkrUiTrack){112, VKR_UI_TRACK_PX};
  columns[column_count++] = (VkrUiTrack){80, VKR_UI_TRACK_PX};
  const VkrUiTrack row = {1, VKR_UI_TRACK_FR};
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement.column = 0u;
  bar.placement.row = 0u;
  bar.columns = columns;
  bar.column_count = column_count;
  bar.rows = &row;
  bar.row_count = 1u;
  /* Tabs rest on the bar's lower edge; the buttons sit centered. */
  bar.style.padding_pt = (VkrUiEdges){5, 8, 0, 8};
  bar.style.gap_pt = 4.0f;
  bar.style.background_color = theme->header;
  if (!vkr_ui_panel_begin(ui, string8_lit("code.tabs"), &bar)) {
    return;
  }
  uint32_t close = UINT32_MAX;
  for (uint32_t i = 0; i < code->document_count; ++i) {
    const CodeDocument *doc = code->documents[i];
    const bool8_t active = i == code->active;
    (void)vkr_ui_push_id_u64(ui, i);
    /* One chip per file, as the dock's tabs: the name, then its close
       button inside the chip's right edge. */
    VkrUiWidgetConfig tab = vkr_ui_widget_config_default();
    tab.placement.column = i;
    tab.placement.row = 0u;
    tab.placement.justify = VKR_UI_ALIGN_STRETCH;
    tab.placement.align = VKR_UI_ALIGN_STRETCH;
    tab.fill = true_v;
    tab.style.font_size_pt = theme->font_body;
    tab.style.padding_pt = (VkrUiEdges){4, 32, 4, 10};
    tab.style.corner_radius_pt = (Vec4){5, 5, 0, 0};
    tab.style.background_color = active ? theme->panel : (Vec4){0};
    tab.style.hover_background_color = active ? theme->panel : theme->row_hover;
    tab.style.border_pt = active ? (VkrUiEdges){2, 0, 0, 0} : (VkrUiEdges){0};
    tab.style.border_color = theme->accent;
    tab.style.text_color = active ? theme->text : theme->text_secondary;
    tab.icon = VKR_UI_ICON_CODE;
    tab.icon_size_pt = 13.0f;
    tab.icon_color = (Vec4){0.80f, 0.66f, 0.98f, active ? 1.0f : 0.7f};
    tab.tooltip =
        string8_create_from_cstr((const uint8_t *)doc->path, strlen(doc->path));
    const String8 label =
        string8_create_formatted(ui->frame_allocator, "%s%s", doc->name,
                                 doc->dirty ? " \xe2\x80\xa2" : "");
    if (vkr_ui_button(ui, string8_lit("tab"), label, &tab)) {
      code->active = i;
      code->completion.open = false_v;
    }
    VkrUiWidgetConfig x = vkr_editor_icon_button_config(
        i, 0u, VKR_UI_ICON_CLOSE,
        doc->dirty ? string8_lit("Close, discarding unsaved changes")
                   : string8_lit("Close"));
    x.placement.justify = VKR_UI_ALIGN_END;
    x.placement.align = VKR_UI_ALIGN_CENTER;
    x.placement.margin_pt.right = 6.0f;
    x.style.min_size_pt = x.style.max_size_pt = (Vec2){18.0f, 18.0f};
    x.style.padding_pt = (VkrUiEdges){3, 3, 3, 3};
    x.icon_size_pt = 11.0f;
    x.icon_color = active ? theme->text_secondary : theme->text_disabled;
    if (vkr_ui_button(ui, string8_lit("close"), (String8){0}, &x)) {
      close = i;
    }
    (void)vkr_ui_pop_id(ui);
  }
  VkrUiWidgetConfig create = code_button(column_count - 2u, 0u);
  create.placement.margin_pt.bottom = 5.0f;
  vkr_editor_ghost_style(&create);
  create.icon = VKR_UI_ICON_ADD;
  create.disabled = !vkr_editor_scripts_project_open(editor->scripts);
  create.tooltip = string8_lit("Create a script module in Scripts/");
  if (vkr_ui_button(ui, string8_lit("new"), string8_lit("New script"),
                    &create)) {
    code->naming = true_v;
    code->naming_focus = true_v;
  }
  CodeDocument *active = code_active(code);
  VkrUiWidgetConfig save = code_button(column_count - 1u, 0u);
  save.placement.margin_pt.bottom = 5.0f;
  vkr_editor_primary_style(&save, VKR_FONT_HANDLE_INVALID);
  save.icon = VKR_UI_ICON_SAVE;
  save.icon_size_pt = 12.0f;
  save.disabled = !active;
  save.tooltip = string8_lit("Save, rebuild and hot reload (Cmd/Ctrl+S)");
  if (vkr_ui_button(ui, string8_lit("save"), string8_lit("Save"), &save) &&
      active) {
    (void)code_save(code, editor, active);
  }
  (void)vkr_ui_panel_end(ui);
  if (close != UINT32_MAX) {
    code_close(code, close);
  }
}

static void code_build_naming(VkrEditorCode *code, VkrEditorUi *editor,
                              VkrUiSystem *ui) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrUiTrack columns[] = {{96, VKR_UI_TRACK_PX},
                                {1, VKR_UI_TRACK_FR},
                                {84, VKR_UI_TRACK_PX},
                                {84, VKR_UI_TRACK_PX}};
  const VkrUiTrack row = {1, VKR_UI_TRACK_FR};
  VkrUiPanelConfig bar = vkr_ui_panel_config_default();
  bar.placement.column = 0u;
  bar.placement.row = 1u;
  bar.columns = columns;
  bar.column_count = ArrayCount(columns);
  bar.rows = &row;
  bar.row_count = 1u;
  bar.style.padding_pt = (VkrUiEdges){5, 10, 5, 10};
  bar.style.gap_pt = 8.0f;
  bar.style.background_color = theme->panel;
  if (!vkr_ui_panel_begin(ui, string8_lit("code.naming"), &bar)) {
    return;
  }
  VkrUiWidgetConfig label =
      vkr_editor_text_config(theme->font_body, theme->text_secondary);
  label.placement.column = 0u;
  label.placement.row = 0u;
  label.placement.align = VKR_UI_ALIGN_CENTER;
  vkr_ui_label(ui, string8_lit("label"), string8_lit("Module name"), &label);
  VkrUiTextEditBuffer name = {.data = code->name_text,
                              .length = code->name_length,
                              .capacity = sizeof(code->name_text)};
  if (code->naming_focus) {
    ui->focused_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("name"));
    ui->focused_is_text = true_v;
    code->naming_focus = false_v;
  }
  VkrUiWidgetConfig field = code_button(1u, 0u);
  vkr_editor_field_style(&field);
  field.style.padding_pt = (VkrUiEdges){4, 8, 4, 8};
  field.tooltip = string8_lit("Letters, digits and underscores, such as Door");
  (void)vkr_ui_text_field(ui, string8_lit("name"), &name, &field);
  code->name_length = name.length;
  VkrUiWidgetConfig ok = code_button(2u, 0u);
  vkr_editor_primary_style(&ok, VKR_FONT_HANDLE_INVALID);
  ok.disabled = !code->name_length;
  const bool8_t enter =
      ui->focused_is_text && input_key_just_pressed(ui->input, KEY_ENTER);
  if (vkr_ui_button(ui, string8_lit("create"), string8_lit("Create"), &ok) ||
      (enter && code->name_length)) {
    char name_text[64];
    snprintf(name_text, sizeof(name_text), "%.*s", (int32_t)code->name_length,
             (const char *)code->name_text);
    char path[VKR_EDITOR_SCRIPT_PATH];
    char error[160];
    if (vkr_editor_scripts_create_module(editor->scripts, name_text, path,
                                         sizeof(path), error, sizeof(error))) {
      /* An object waiting for a new script gets this one once it loads. */
      if (editor->script_attach_entity.u64 &&
          !editor->script_attach_module[0]) {
        snprintf(editor->script_attach_module,
                 sizeof(editor->script_attach_module), "%s", name_text);
      }
      code->naming = false_v;
      code->name_length = 0u;
      snprintf(code->status, sizeof(code->status), "Created %s; building it",
               name_text);
      (void)vkr_editor_code_open(code, editor, path);
    } else {
      snprintf(code->status, sizeof(code->status), "%s", error);
    }
  }
  VkrUiWidgetConfig cancel = code_button(3u, 0u);
  vkr_editor_ghost_style(&cancel);
  if (vkr_ui_button(ui, string8_lit("cancel"), string8_lit("Cancel"),
                    &cancel)) {
    code->naming = false_v;
    if (!editor->script_attach_module[0]) {
      editor->script_attach_entity = VKR_ENTITY_ID_INVALID;
    }
  }
  (void)vkr_ui_panel_end(ui);
}

/* The visible lines, their spans and marks, in frame memory. */
static void code_build_view(VkrEditorCode *code, VkrEditorUi *editor,
                            CodeDocument *doc, VkrUiSystem *ui, uint32_t row,
                            bool8_t mouse) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const Vec2 cell = vkr_ui_code_cell_size(ui, editor->mono_font, CODE_FONT_PT);
  const float32_t gutter_pt =
      cell.x *
          (float32_t)Max(3u, (uint32_t)log10((double)doc->line_count) + 1u) +
      22.0f;
  const float32_t height_pt = code->view_rect.height / ui->content_scale;
  const uint32_t visible = Min(
      CODE_VISIBLE_MAX, (uint32_t)Max(1.0f, ceilf(height_pt / CODE_LINE_PT)));
  code->visible_lines = (uint32_t)Max(1.0f, floorf(height_pt / CODE_LINE_PT));
  code_reveal(doc, code->visible_lines,
              code->view_rect.width / ui->content_scale - gutter_pt - 8.0f,
              cell.x);
  doc->first_line = Min(doc->first_line, doc->line_count - 1u);
  const uint32_t count = Min(visible, doc->line_count - doc->first_line);
  VkrUiCodeLine *lines =
      vkr_allocator_alloc(ui->frame_allocator, sizeof(*lines) * Max(count, 1u),
                          VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  /* Spans hold a Vec4, so they need its alignment. */
  VkrUiCodeSpan *spans = vkr_allocator_alloc_aligned(
      ui->frame_allocator, sizeof(*spans) * CODE_SPAN_MAX * Max(count, 1u),
      AlignOf(VkrUiCodeSpan), VKR_ALLOCATOR_MEMORY_TAG_ARRAY);
  if (!lines || !spans) {
    return;
  }
  const uint32_t selection_begin = code_selection_begin(doc);
  const uint32_t selection_end = code_selection_end(doc);
  const uint32_t caret_line = code_line_of(doc, doc->caret);
  const bool8_t blink =
      !code->focused ||
      fmod(vkr_platform_get_absolute_time() - doc->last_edit_time, 1.06) < 0.62;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t line = doc->first_line + i;
    const uint32_t start = doc->lines[line];
    const uint32_t end = code_line_end(doc, line);
    VkrUiCodeSpan *line_spans = spans + i * CODE_SPAN_MAX;
    lines[i] = (VkrUiCodeLine){
        .text = {.str = doc->text + start, .length = end - start},
        .spans = line_spans,
        .span_count =
            code_tokenize(doc->text + start, end - start, doc->in_comment[line],
                          line_spans, CODE_SPAN_MAX),
        .number = line + 1u,
        .caret = UINT32_MAX,
        .current = line == caret_line,
    };
    if (selection_end > selection_begin && selection_begin <= end + 1u &&
        selection_end > start) {
      lines[i].selection_start =
          selection_begin > start ? selection_begin - start : 0u;
      lines[i].selection_end = selection_end - start;
    }
    if (line == caret_line && code->focused && blink) {
      lines[i].caret = doc->caret - start;
    }
    const VkrEditorScriptDiagnostic *diagnostic =
        code_diagnostic_on(editor, doc, line);
    if (diagnostic) {
      const Vec4 color = diagnostic->error ? theme->error : theme->warning;
      lines[i].marker_color = color;
      uint32_t from = diagnostic->column ? diagnostic->column - 1u : 0u;
      from = Min(from, end - start);
      uint32_t to = from;
      while (start + to < end && code_word_byte(doc->text[start + to])) {
        ++to;
      }
      lines[i].underline_start = from;
      lines[i].underline_end = Max(to, Min(from + 1u, end - start));
      lines[i].underline_color = color;
    }
  }
  const VkrUiCodeView view = {
      .lines = lines,
      .line_count = count,
      .font = editor->mono_font,
      .font_size_pt = CODE_FONT_PT,
      .line_height_pt = CODE_LINE_PT,
      .gutter_width_pt = gutter_pt,
      .scroll_x_pt = doc->scroll_x_pt,
      .text_color = (Vec4){0.84f, 0.85f, 0.87f, 1.0f},
      .gutter_text_color = theme->text_disabled,
      .selection_color = vkr_ui_color_alpha(theme->accent, 0.35f),
      .current_line_color = (Vec4){1.0f, 1.0f, 1.0f, 0.04f},
      .caret_color = theme->text,
  };
  VkrUiWidgetConfig config = vkr_ui_widget_config_default();
  config.placement.column = 0u;
  config.placement.row = row;
  config.style.background_color = (Vec4){0.105f, 0.11f, 0.12f, 1.0f};
  /* A requested focus holds until the view has a presented rectangle on
     the window's keyboard layer, which a click would otherwise select. */
  if (code->focus_request) {
    ui->focused_id =
        vkr_ui_id_stack_widget_label(&ui->id_stack, string8_lit("code.view"));
    ui->focused_is_text = true_v;
    (void)vkr_ui_keyboard_layer_set(ui, ui->input_layer);
  }
  code->focused = vkr_ui_code_view(ui, string8_lit("code.view"), &view, &config,
                                   &code->view_id);
  if (code->focused) {
    code->focus_request = false_v;
  }
  VkrUiRect rect;
  if (vkr_ui_widget_rect(ui, code->view_id, &rect)) {
    code->view_rect = rect;
  }
  if (mouse) {
    code_mouse(code, doc, ui,
               (Vec2){cell.x * ui->content_scale,
                      roundf(CODE_LINE_PT * ui->content_scale)},
               roundf(gutter_pt * ui->content_scale));
  }
}

/* `body` is the window body the list is placed in, in pixels; the list
 * opens below the caret, or above it where it would leave the body. */
static void code_build_completion(VkrEditorCode *code, VkrEditorUi *editor,
                                  CodeDocument *doc, VkrUiSystem *ui,
                                  VkrUiRect body) {
  const Vec2 origin = {body.x, body.y};
  CodeCompletion *completion = &code->completion;
  if (!completion->open || !code->focused) {
    return;
  }
  const VkrUiTheme *theme = vkr_ui_theme();
  const Vec2 cell = vkr_ui_code_cell_size(ui, editor->mono_font, CODE_FONT_PT);
  const uint32_t line = code_line_of(doc, completion->start);
  if (line < doc->first_line) {
    return;
  }
  const float32_t gutter_pt =
      cell.x *
          (float32_t)Max(3u, (uint32_t)log10((double)doc->line_count) + 1u) +
      22.0f;
  const float32_t scale = ui->content_scale;
  float32_t x_pt = (code->view_rect.x - origin.x) / scale + gutter_pt + 4.0f +
                   (float32_t)code_column(doc, completion->start) * cell.x -
                   doc->scroll_x_pt;
  float32_t y_pt = (code->view_rect.y - origin.y) / scale +
                   (float32_t)(line - doc->first_line + 1u) * CODE_LINE_PT;
  const char *detail = completion->items[completion->selected].detail;
  const uint32_t rows = completion->count + (detail ? 1u : 0u);
  VkrUiTrack tracks[CODE_COMPLETION_MAX + 1u];
  for (uint32_t i = 0; i < rows; ++i) {
    tracks[i] = (VkrUiTrack){CODE_ROW_PT, VKR_UI_TRACK_PX};
  }
  const VkrUiTrack column = {1, VKR_UI_TRACK_FR};
  VkrUiPanelConfig popup = vkr_ui_panel_config_default();
  popup.placement = (VkrUiPlacement){
      .column = 0u,
      .row = 0u,
      .column_span = 1u,
      .row_span = 1u,
      .justify = VKR_UI_ALIGN_START,
      .align = VKR_UI_ALIGN_START,
      .margin_pt = {.top = y_pt, .left = x_pt},
  };
  popup.columns = &column;
  popup.column_count = 1u;
  popup.rows = tracks;
  popup.row_count = rows;
  popup.style = vkr_editor_glass_style();
  /* Opaque, so the code below never reads through the list. */
  popup.style.background_color = theme->popup;
  popup.style.background_color.w = 1.0f;
  popup.style.padding_pt = (VkrUiEdges){5, 5, 5, 5};
  popup.style.gap_pt = 0.0f;
  /* Wide enough for the longest name or the signature, within bounds. */
  size_t longest = detail ? strlen(detail) : 0u;
  for (uint32_t i = 0; i < completion->count; ++i) {
    longest = Max(longest, strlen(completion->items[i].name));
  }
  const Vec2 size = {
      vkr_clamp_f32((float32_t)longest * cell.x + 58.0f, 220.0f, 520.0f),
      (float32_t)rows * CODE_ROW_PT + 10.0f};
  if (y_pt + size.y > body.height / scale - 24.0f &&
      y_pt - CODE_LINE_PT - size.y >= 0.0f) {
    y_pt -= CODE_LINE_PT + size.y;
  }
  x_pt = Max(0.0f, Min(x_pt, body.width / scale - size.x));
  popup.placement.margin_pt.top = y_pt;
  popup.placement.margin_pt.left = x_pt;
  popup.style.min_size_pt = popup.style.max_size_pt = size;
  popup.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("code.completion"), &popup)) {
    return;
  }
  code->popup_rect = (VkrUiRect){
      origin.x + x_pt * scale, origin.y + y_pt * scale,
      popup.style.min_size_pt.x * scale, popup.style.min_size_pt.y * scale};
  static const VkrUiIcon icons[] = {
      [CODE_SYMBOL_KEYWORD] = VKR_UI_ICON_KEYBOARD,
      [CODE_SYMBOL_TYPE] = VKR_UI_ICON_SHAPES,
      [CODE_SYMBOL_FUNCTION] = VKR_UI_ICON_LIGHTNING,
      [CODE_SYMBOL_MEMBER] = VKR_UI_ICON_DOT,
      [CODE_SYMBOL_CONSTANT] = VKR_UI_ICON_TAG,
      [CODE_SYMBOL_LOCAL] = VKR_UI_ICON_CODE,
  };
  for (uint32_t i = 0; i < completion->count; ++i) {
    (void)vkr_ui_push_id_u64(ui, i);
    /* Each kind keeps its highlighting color on the icon. */
    static const Vec4 kind_colors[] = {
        [CODE_SYMBOL_KEYWORD] = {0.80f, 0.55f, 0.95f, 1.0f},
        [CODE_SYMBOL_TYPE] = {0.40f, 0.78f, 0.95f, 1.0f},
        [CODE_SYMBOL_FUNCTION] = {0.95f, 0.80f, 0.45f, 1.0f},
        [CODE_SYMBOL_MEMBER] = {0.62f, 0.78f, 0.98f, 1.0f},
        [CODE_SYMBOL_CONSTANT] = {0.55f, 0.85f, 0.60f, 1.0f},
        [CODE_SYMBOL_LOCAL] = {0.80f, 0.82f, 0.86f, 1.0f},
    };
    VkrUiWidgetConfig row =
        vkr_editor_text_config(theme->font_body, theme->text);
    row.placement.column = 0u;
    row.placement.row = i;
    row.placement.justify = VKR_UI_ALIGN_STRETCH;
    row.placement.align = VKR_UI_ALIGN_STRETCH;
    row.fill = true_v;
    vkr_editor_ghost_style(&row);
    row.style.padding_pt = (VkrUiEdges){2, 8, 2, 6};
    row.style.text_color = theme->text;
    row.text.font = editor->mono_font;
    row.icon = icons[completion->items[i].kind];
    row.icon_size_pt = 11.0f;
    row.icon_color = kind_colors[completion->items[i].kind];
    if (i == completion->selected) {
      row.style.background_color = vkr_ui_color_alpha(theme->accent, 0.45f);
      row.style.hover_background_color = row.style.background_color;
    }
    /* The row takes the click; its name reads from the leading edge. */
    VkrUiWidgetConfig hit = row;
    hit.icon = VKR_UI_ICON_NONE;
    const bool8_t clicked =
        vkr_ui_button(ui, string8_lit("item"), (String8){0}, &hit);
    VkrUiWidgetConfig name = row;
    name.placement.justify = VKR_UI_ALIGN_START;
    name.placement.align = VKR_UI_ALIGN_CENTER;
    name.fill = false_v;
    name.style.background_color = (Vec4){0};
    name.style.hover_background_color = VKR_UI_COLOR_NONE;
    vkr_ui_label(
        ui, string8_lit("name"),
        string8_create_from_cstr((const uint8_t *)completion->items[i].name,
                                 strlen(completion->items[i].name)),
        &name);
    if (clicked) {
      completion->selected = i;
      code_accept_completion(code, doc);
      ui->focused_id = code->view_id;
    }
    (void)vkr_ui_pop_id(ui);
  }
  if (detail) {
    VkrUiWidgetConfig hint =
        vkr_editor_text_config(theme->font_caption, theme->text_secondary);
    hint.placement.column = 0u;
    hint.placement.row = completion->count;
    hint.text.font = editor->mono_font;
    hint.placement.align = VKR_UI_ALIGN_CENTER;
    hint.placement.margin_pt.left = 8.0f;
    vkr_ui_label(
        ui, string8_lit("detail"),
        string8_create_from_cstr((const uint8_t *)detail, strlen(detail)),
        &hint);
  }
  (void)vkr_ui_panel_end(ui);
}

/* Compiler messages of the active tab's module; a click jumps there. */
static void code_build_problems(VkrEditorCode *code, VkrEditorUi *editor,
                                VkrUiSystem *ui, uint32_t row, uint32_t shown) {
  const VkrUiTheme *theme = vkr_ui_theme();
  VkrUiTrack tracks[CODE_PROBLEM_ROWS];
  for (uint32_t i = 0; i < shown; ++i) {
    tracks[i] = (VkrUiTrack){CODE_ROW_PT, VKR_UI_TRACK_PX};
  }
  const VkrUiTrack column = {1, VKR_UI_TRACK_FR};
  VkrUiPanelConfig panel = vkr_ui_panel_config_default();
  panel.placement.column = 0u;
  panel.placement.row = row;
  panel.columns = &column;
  panel.column_count = 1u;
  panel.rows = tracks;
  panel.row_count = shown;
  panel.style.background_color = theme->panel;
  panel.style.padding_pt = (VkrUiEdges){4, 8, 4, 8};
  if (!vkr_ui_panel_begin(ui, string8_lit("code.problems"), &panel)) {
    return;
  }
  const uint32_t count = vkr_editor_scripts_diagnostic_count(editor->scripts);
  for (uint32_t i = 0, r = 0; i < count && r < shown; ++i) {
    const VkrEditorScriptDiagnostic *diagnostic =
        vkr_editor_scripts_diagnostic(editor->scripts, i);
    (void)vkr_ui_push_id_u64(ui, i);
    VkrUiWidgetConfig item = vkr_editor_text_config(
        theme->font_caption, diagnostic->error ? theme->error : theme->warning);
    item.placement.column = 0u;
    item.placement.row = r++;
    item.placement.justify = VKR_UI_ALIGN_STRETCH;
    item.placement.align = VKR_UI_ALIGN_STRETCH;
    item.fill = true_v;
    vkr_editor_ghost_style(&item);
    item.style.padding_pt = (VkrUiEdges){3, 8, 3, 8};
    item.style.text_color = diagnostic->error ? theme->error : theme->warning;
    item.icon = VKR_UI_ICON_WARNING_FILL;
    item.icon_size_pt = 11.0f;
    const char *name = strrchr(diagnostic->path, '/');
    const String8 text = string8_create_formatted(
        ui->frame_allocator, "%s:%u:%u  %s", name ? name + 1 : diagnostic->path,
        diagnostic->line, diagnostic->column, diagnostic->message);
    if (vkr_ui_button(ui, string8_lit("problem"), text, &item) &&
        vkr_editor_code_open(code, editor, diagnostic->path)) {
      CodeDocument *doc = code_active(code);
      const uint32_t line = Min(diagnostic->line ? diagnostic->line - 1u : 0u,
                                doc->line_count - 1u);
      doc->caret = doc->anchor =
          Min(doc->lines[line] +
                  (diagnostic->column ? diagnostic->column - 1u : 0u),
              code_line_end(doc, line));
      doc->reveal = true_v;
      ui->focused_id = code->view_id;
    }
    (void)vkr_ui_pop_id(ui);
  }
  (void)vkr_ui_panel_end(ui);
}

static void code_build_status(VkrEditorCode *code, VkrEditorUi *editor,
                              const CodeDocument *doc, VkrUiSystem *ui,
                              uint32_t row) {
  const VkrUiTheme *theme = vkr_ui_theme();
  const VkrEditorScriptModule *module =
      doc ? vkr_editor_scripts_module_of(editor->scripts, doc->path) : NULL;
  Vec4 color = theme->text_secondary;
  if (module && (module->status == VKR_EDITOR_SCRIPT_BUILD_FAILED ||
                 module->status == VKR_EDITOR_SCRIPT_LOAD_FAILED)) {
    color = theme->error;
  } else if (module && module->status == VKR_EDITOR_SCRIPT_LOADED) {
    color = theme->success;
  }
  String8 text = {0};
  if (doc) {
    const uint32_t line = code_line_of(doc, doc->caret);
    const VkrEditorScriptDiagnostic *here =
        code_diagnostic_on(editor, doc, line);
    text = string8_create_formatted(
        ui->frame_allocator, "Ln %u, Col %u   %s%s%s   %s", line + 1u,
        code_column(doc, doc->caret) + 1u, module ? module->name : "",
        module ? ": " : "", module ? module->message : "Not in a script module",
        here                   ? here->message
        : doc->changed_on_disk ? "Changed on disk; Save overwrites it"
                               : code->status);
    if (here) {
      color = here->error ? theme->error : theme->warning;
    }
  } else {
    text = string8_create_from_cstr((const uint8_t *)code->status,
                                    strlen(code->status));
  }
  VkrUiWidgetConfig status = vkr_editor_text_config(theme->font_caption, color);
  status.placement.column = 0u;
  status.placement.row = row;
  status.placement.align = VKR_UI_ALIGN_CENTER;
  status.placement.margin_pt = (VkrUiEdges){0, 10, 0, 10};
  vkr_ui_label(ui, string8_lit("code.status"), text, &status);
}

void vkr_editor_code_build(VkrEditorCode *code, VkrEditorUi *editor,
                           const VkrSampleUiFrame *frame, VkrUiRect bounds) {
  VkrUiSystem *ui = frame->ui;
  code_poll_files(code);
  const uint32_t problem_count = Min(
      CODE_PROBLEM_ROWS, vkr_editor_scripts_diagnostic_count(editor->scripts));
  VkrUiTrack rows[5];
  uint32_t row_count = 0u;
  rows[row_count++] = (VkrUiTrack){38, VKR_UI_TRACK_PX};
  const uint32_t naming_row = row_count;
  if (code->naming) {
    rows[row_count++] = (VkrUiTrack){40, VKR_UI_TRACK_PX};
  }
  const uint32_t view_row = row_count;
  rows[row_count++] = (VkrUiTrack){1, VKR_UI_TRACK_FR};
  const uint32_t problem_row = row_count;
  if (problem_count) {
    rows[row_count++] = (VkrUiTrack){
        (float32_t)problem_count * CODE_ROW_PT + 8.0f, VKR_UI_TRACK_PX};
  }
  const uint32_t status_row = row_count;
  rows[row_count++] = (VkrUiTrack){26, VKR_UI_TRACK_PX};
  const VkrUiTrack column = {1, VKR_UI_TRACK_FR};
  VkrUiPanelConfig layout = vkr_ui_panel_config_default();
  layout.placement.column = 0u;
  layout.placement.row = 0u;
  layout.columns = &column;
  layout.column_count = 1u;
  layout.rows = rows;
  layout.row_count = row_count;
  layout.style.padding_pt = (VkrUiEdges){0};
  layout.clip_children = true_v;
  if (!vkr_ui_panel_begin(ui, string8_lit("code.layout"), &layout)) {
    return;
  }
  code_build_tabs(code, editor, ui);
  CodeDocument *doc = code_active(code);
  if (code->naming) {
    (void)naming_row;
    code_build_naming(code, editor, ui);
  }
  if (doc) {
    if (code->focused) {
      /* Tab indents here; the UI keeps it from moving focus. */
      vkr_ui_keyboard_navigation_enabled(ui, false_v);
      ui->capture.keyboard = true_v;
      code_keys(code, editor, doc, frame->input);
    }
    code_build_view(code, editor, doc, ui, view_row,
                    !editor->windows[VKR_EDITOR_WINDOW_SCRIPT].resizing);
  } else {
    const VkrUiTheme *theme = vkr_ui_theme();
    VkrUiWidgetConfig empty =
        vkr_editor_text_config(theme->font_body, theme->text_secondary);
    empty.placement.column = 0u;
    empty.placement.row = view_row;
    empty.center = true_v;
    empty.placement.justify = VKR_UI_ALIGN_CENTER;
    empty.placement.align = VKR_UI_ALIGN_CENTER;
    vkr_ui_label(ui, string8_lit("code.empty"),
                 string8_lit("Double-click a script in Content, or create "
                             "one with New script."),
                 &empty);
  }
  if (problem_count) {
    code_build_problems(code, editor, ui, problem_row, problem_count);
  }
  code_build_status(code, editor, doc, ui, status_row);
  (void)vkr_ui_panel_end(ui);
  if (doc) {
    code_build_completion(code, editor, doc, ui, bounds);
  }
}

// =============================================================================
// Lifetime
// =============================================================================

VkrEditorCode *vkr_editor_code_create(VkrAllocator *allocator) {
  VkrEditorCode *code = vkr_allocator_alloc(allocator, sizeof(*code),
                                            VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (code) {
    MemZero(code, sizeof(*code));
    code->allocator = allocator;
  }
  return code;
}

void vkr_editor_code_close_saved(VkrEditorCode *code) {
  for (uint32_t i = code ? code->document_count : 0u; i-- > 0u;) {
    if (!code->documents[i]->dirty) {
      code_close(code, i);
    }
  }
}

void vkr_editor_code_destroy(VkrEditorCode *code) {
  if (!code) {
    return;
  }
  while (code->document_count) {
    code_close(code, code->document_count - 1u);
  }
  if (code->sdk.arena) {
    vkr_allocator_release_global_accounting(&code->sdk.allocator);
    arena_destroy(code->sdk.arena);
  }
  if (code->local.arena) {
    vkr_allocator_release_global_accounting(&code->local.allocator);
    arena_destroy(code->local.arena);
  }
  vkr_allocator_free(code->allocator, code, sizeof(*code),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

bool8_t vkr_editor_code_dirty(const VkrEditorCode *code) {
  for (uint32_t i = 0; code && i < code->document_count; ++i) {
    if (code->documents[i]->dirty) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_editor_code_open(VkrEditorCode *code, VkrEditorUi *editor,
                             const char *path) {
  if (!code || !path || !path[0]) {
    return false_v;
  }
  vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_SCRIPT, true_v);
  for (uint32_t i = 0; i < code->document_count; ++i) {
    if (file_path_equals(code->documents[i]->path, path)) {
      code->active = i;
      return true_v;
    }
  }
  if (code->document_count == CODE_DOCUMENT_MAX) {
    code_close(code, 0u);
  }
  CodeDocument *doc = vkr_allocator_alloc(code->allocator, sizeof(*doc),
                                          VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!doc) {
    return false_v;
  }
  MemZero(doc, sizeof(*doc));
  snprintf(doc->path, sizeof(doc->path), "%s", path);
  const char *name = strrchr(path, '/');
  snprintf(doc->name, sizeof(doc->name), "%s", name ? name + 1 : path);
  if (!code_reserve(code, doc, 0u) || !code_read_file(code, doc)) {
    snprintf(code->status, sizeof(code->status), "Cannot open %s", doc->name);
    code_document_free(code, doc);
    return false_v;
  }
  code->documents[code->document_count] = doc;
  code->active = code->document_count++;
  code->focus_request = true_v;
  code->completion.open = false_v;
  code->status[0] = '\0';
  return true_v;
}

bool8_t vkr_editor_code_goto(VkrEditorCode *code, uint32_t line) {
  CodeDocument *doc = code ? code_active(code) : NULL;
  if (!doc || !line) {
    return false_v;
  }
  const uint32_t index = Min(line - 1u, doc->line_count - 1u);
  doc->caret = doc->anchor = doc->lines[index] + code_indent_of(doc, index);
  doc->reveal = true_v;
  code->completion.open = false_v;
  code->focus_request = true_v;
  return true_v;
}

bool8_t vkr_editor_code_type(VkrEditorCode *code, const char *text) {
  CodeDocument *doc = code ? code_active(code) : NULL;
  if (!doc || !text) {
    return false_v;
  }
  uint32_t characters[256];
  uint32_t count = 0u;
  for (const char *at = text; *at && count < ArrayCount(characters); ++at) {
    characters[count++] = (uint8_t)*at;
  }
  code_type_text(code, doc, characters, count);
  code->focus_request = true_v;
  return true_v;
}

bool8_t vkr_editor_code_save_active(VkrEditorCode *code, VkrEditorUi *editor) {
  CodeDocument *doc = code ? code_active(code) : NULL;
  return doc && code_save(code, editor, doc);
}

void vkr_editor_code_new_script(VkrEditorCode *code, VkrEditorUi *editor) {
  vkr_editor_window_set_visible(editor, VKR_EDITOR_WINDOW_SCRIPT, true_v);
  code->naming = true_v;
  code->naming_focus = true_v;
}
