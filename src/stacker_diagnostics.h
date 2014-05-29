#pragma once

#include <cstdarg>

namespace stkr {

struct Document;
struct Node;
struct Box;
struct Paragraph;
struct ParagraphLine;

void dump_paragraph(Document *document, const Paragraph *p);
void dump_paragraph_lines(Document *document, const ParagraphLine *lines, unsigned count);
void dump_node(const Document *document, const Node *node, unsigned indent = 0);
void dump_inline_context(const Document *document, const Node *node);
void dump_all_inline_contexts(const Document *document, const Node *root);
void dump_boxes(const Document *document, const Box *box, unsigned indent = 0);
void dump_discard(void *data, const char *fmt, va_list args);
void dump_rule_table(const Document *document, bool global = false);
void dump_grid(Document *document);
void unit_test_box_grid(Document *document);

} // namespace stkr

