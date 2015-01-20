#include "stacker_parser.h"

#include <cctype>
#include <cstring>
#include <cstdarg>

#include <algorithm>

#include "stacker.h"
#include "stacker_token.h"
#include "stacker_attribute.h"
#include "stacker_util.h"
#include "stacker_node.h"
#include "stacker_document.h"
#include "stacker_encoding.h"
#include "stacker_system.h"

namespace stkr {

const int STKR_OK_HALT     = 1; /* No error, but don't parse any more. */
const int STKR_OK_NO_SCOPE = 2; /* No error, but this is a non-document tag (a rule). Don't try to create a node from it. */
const int STKR_SKIP_TAG    = 3; /* No error, but the tag should be ignored. */

static int read_url_literal(Parser *parser);
static int read_color_literal(Parser *parser, int keyword);

/* True if 'ch' is one of the characters that can be backslash-escaped in
 * free text. */
inline bool is_escapeable(uint32_t ch)
{
	return ch == '<' || ch == '>' || ch == '\\';
}

/* Copies a UTF-8 string, replacing any backslash escape sequences with the 
 * corresponding unescaped character. */
static unsigned unescape(const char *s, unsigned length, char *dest)
{
	unsigned j = 0;
	for (unsigned i = 0; i < length; ++i) {
		if (is_escapeable((uint32_t)s[i]) && i != 0 && 
			s[i - 1] == (uint8_t)'\\')
			j--;
		dest[j++] = s[i];
	}
	return j;
}

inline uint32_t next_character(Parser *s)
{
	s->ch0 = s->ch1;
	s->pos_ch0 = s->pos_ch1;
	s->pos_ch1 = s->pos;
	s->pos += utf8_decode(s->input + s->pos, 
		s->input + s->input_size, &s->ch1);
	return s->ch0;
}

inline uint32_t skip_two_characters(Parser *s)
{
	next_character(s);
	return next_character(s);
}

inline uint32_t rewind(Parser *s, unsigned saved_pos_ch0)
{
	s->pos = saved_pos_ch0;
	return skip_two_characters(s);
}

/* Reads and cleans up a small amount of text surrounding the current token for 
 * use in error messages. */
static int read_context(const Parser *parser, char *buffer, unsigned buffer_size)
{
	const char *s = parser->input + parser->token_start;
	const char *end = parser->input + parser->input_size;
	unsigned length = 0;
	bool drop_spaces = true;
	bool is_space = false;
	for (;;) {
		uint32_t code_point;
		s += utf8_decode(s, end, &code_point);
		if (code_point == END_OF_STREAM)
			break;
		is_space = unicode_isspace(code_point);
		if (!(is_space && drop_spaces)) {
			if (is_space)
				code_point = ' '; /* Convert newlines to spaces. */
			if (length + utf8_encoded_length(code_point) >= buffer_size)
				break;
			length += utf8_encode(buffer + length, code_point);
		}
		drop_spaces = is_space;
	}
	length -= (length != 0 && is_space); /* Remove trailing space. */
	buffer[length] = 0;
	return length;
}

static int parser_message(Parser *parser, int code, 
	const char *fmt, va_list args)
{
	/* Format the main message. */
	char message_utf8[MAX_MESSAGE_SIZE + 1];
	int length = vsnprintf((char *)message_utf8, sizeof(message_utf8), 
		fmt, args);
	if (length < 0)
		length = MAX_MESSAGE_SIZE;

	/* Add a suffix giving some indication of the location of the error. */
	char context[ERROR_CONTEXT_CHARS + 1];
	int context_length = read_context(parser, context, ERROR_CONTEXT_CHARS + 1);
	int suffix_length, remaining = sizeof(message_utf8) - length;
	if (context_length != 0) {
		suffix_length = snprintf((char *)message_utf8 + length, 
			remaining, " near \"%s\" on line %u", 
			context, parser->line);
	} else {
		suffix_length = snprintf((char *)message_utf8 + length, remaining, 
			" at end of input");
	}
	if (suffix_length < 0)
		suffix_length = remaining;
	length = std::min(unsigned(length + suffix_length), MAX_MESSAGE_SIZE);

	/* Convert the message into the system encoding. */
	if (parser->message.utf8 != NULL)
		delete [] parser->message.utf8;
	parser->message_length = utf8_transcode_heap(message_utf8, length, 
		&parser->message.data, parser->system->message_encoding);

	parser->code = code;
	return code;
}

static void clear_message(Parser *parser)
{
	if (parser->message.utf8 != NULL)
		delete [] parser->message.utf8;
	parser->message.utf8 = NULL;
	parser->message_length = 0;
}

static int parser_error(Parser *parser, int code, ...)
{
	if (parser->code < 0)
		return parser->code;
	va_list args;
	va_start(args, code);
	switch (code) {
		case STKR_OK:
			break;
		case STKR_ERROR:
			parser_message(parser, code, "internal error", args);
			break;
		case STKR_INVALID_TOKEN:
			parser_message(parser, code, "invalid token", args);
			break;
		case STKR_TOO_MANY_ATTRIBUTES:
			parser_message(parser, code, "too many attributes", args);
			break;
		case STKR_UNEXPECTED_TOKEN:
			parser_message(parser, code, "expected %s", args);
			break;
		case STKR_ATTRIBUTE_VALUE_TYPE_MISMATCH:
			parser_message(parser, code, "unsuitable value type for attribute \"%s\"", args);
			break;
		case STKR_ATTRIBUTE_VALUE_OUT_OF_BOUNDS:
			parser_message(parser, code, "value for attribute \"%s\" out of bounds", args);
			break;
		case STKR_INVALID_NUMERIC_LITERAL:
			parser_message(parser, code, "invalid numeric literal \"%s\"", args);
			break;
		case STKR_UNTERMINATED_STRING:
			parser_message(parser, code, "unterminated string literal", args);
			break;
		case STKR_MISMATCHED_TAGS:
			parser_message(parser, code, "mismatched tags - expected </%s>", args);
			break;
		case STKR_INVALID_KEYWORD:
			parser_message(parser, code, "invalid keyword \"%s\"", args);
			break;
		case STKR_INVALID_INPUT:
			parser_message(parser, code, "nonsense", args);
			break;
		case STKR_INVALID_TAG:
			parser_message(parser, code, "invalid tag \"%s\"", args);
			break;
		case STKR_COLOR_COMPONENT_OUT_OF_RANGE:
			parser_message(parser, code, "color component %u out of range", args);
			break;
		case STKR_SELECTOR_ILL_FORMED:
			parser_message(parser, code, "ill-formed selector", args);
			break;
		case STKR_SELECTOR_EMPTY:
			parser_message(parser, code, "empty selector clause", args);
			break;
		case STKR_SELECTOR_INVALID_CHAR:
			parser_message(parser, code, "invalid character in selector", args);
			break;
		case STKR_SELECTOR_MISSING_CLASS:
			parser_message(parser, code, "missing class in selector", args);
			break;
		case STKR_SELECTOR_TOO_LONG:
			parser_message(parser, code, "selector too long", args);
			break;
		case STKR_MISSING_SELECTOR:
			parser_message(parser, code, "rule missing \"match\" attribute", args);
			break;
		case STKR_INCORRECT_CONTEXT:
			parser_message(parser, code, "non-rule tag encountered outside document context", args);
			break;
		case STKR_TYPE_MISMATCH:
			parser_message(parser, code, "type mismatch", args);
			break;
		case STKR_INVALID_OPERATION:
			parser_message(parser, code, "operator %s cannot be applied to attribute \"%s\"", args);
			break;
		case STKR_INVALID_SET_LITERAL:
			parser_message(parser, code, "invalid set literal", args);
		default:
			ensure(false);
	}
	va_end(args);
	return code;
}

static void push_scope(Parser *parser, Node *node)
{
	if (parser->scope != NULL)
		append_child(parser->document, parser->scope, node);
	parser->scope = node;
}

static int pop_scope(Parser *parser)
{
	Node *popped = parser->scope;
	if (popped == NULL)
		return STKR_MISMATCHED_TAGS;
	parser->scope = popped->t.parent.node;
	if (parser->scope == parser->root) {
		if (parser->first_parsed == NULL)
			parser->first_parsed = popped;
		parser->last_parsed = popped;
		if ((parser->flags & PARSEFLAG_SINGLE_NODE) != 0)
			return STKR_OK_HALT;
	}
	return STKR_OK;
}

/* Reads a numeric literal token. */
static int read_number(Parser *parser)
{
	/* Read any prefixes. */
	char buf[64], *end;
	unsigned length = 0;
	uint32_t ch = parser->ch0;
	if (ch == '-') {
		buf[length++] = '-';
		buf[length] = 0;
		ch = next_character(parser);
		if (ch == END_OF_STREAM)
			return parser_error(parser, STKR_INVALID_NUMERIC_LITERAL, buf);
	}

	/* Read the digits. */
	bool is_float = false;
	do {
		buf[length++] = (char)ch;
		ch = next_character(parser);
		if (ch == END_OF_STREAM)
			break;
		if (ch == '.' || ch == 'e')
			is_float = true;
	} while ((unicode_isdigit(ch) || ch == '.' || ch == 'e') && 
		length + 1 != sizeof(buf));
	buf[length] = 0;

	/* Convert the number. */
	if (is_float) {
		variant_set_float(&parser->token_value, (float)strtod(buf, &end));
	} else {
		variant_set_integer(&parser->token_value, (int)strtol(buf, &end, 0));
	}
	if (end != buf + length)
		return parser_error(parser, STKR_INVALID_NUMERIC_LITERAL, buf);		

	/* A percentage literal? */
	if (ch == '%') {
		if (!is_float) {
			variant_set_float(&parser->token_value, 
				(float)parser->token_value.integer);
		}
		parser->token = TOKEN_PERCENTAGE;
		next_character(parser); /* Skip percentage sign. */
	} else {
		parser->token = is_float ? TOKEN_FLOAT : TOKEN_INTEGER;
	}
	return parser->token;
}

static int next_token(Parser *parser)
{
	uint32_t ch = parser->ch0;
	if (ch == END_OF_STREAM) {
		parser->token = TOKEN_EOF;
		return TOKEN_EOF;
	}
	if (parser->in_tag) {
		/* Skip white space. */
		while (unicode_isspace(ch)) {
			parser->line += (ch == '\n');
			ch = next_character(parser);
			if (ch == END_OF_STREAM) {
				parser->token = TOKEN_EOF;
				return TOKEN_EOF;
			}
		}
		/* Read the token. */
		uint32_t ch2 = parser->ch1;
		parser->token_start = parser->pos;
		if (ch == '>') {
			parser->token = TOKEN_CLOSE_ANGLE;
			parser->in_tag = false;
			next_character(parser);
		} else if (ch == '/' && ch2 == '>') {
			parser->token = TOKEN_SLASH_CLOSE_ANGLE;
			parser->in_tag = false;
			skip_two_characters(parser);
		} else if (ch == '=') {
			parser->token = TOKEN_EQUALS;
			next_character(parser);
		} else if (ch2 == '=' && (ch == ':' || ch == '+' || ch == '-' || 
			ch == '*' || ch == '/')) {
			switch (ch) {
				case ':':
					parser->token = TOKEN_COLON_EQUALS;
					break;
				case '+': 
					parser->token = TOKEN_PLUS_EQUALS;
					break;
				case '-':
					parser->token = TOKEN_DASH_EQUALS;
					break;
				case '*':
					parser->token = TOKEN_STAR_EQUALS;
					break;
				case '/':
					parser->token = TOKEN_SLASH_EQUALS;
					break;
			}
			skip_two_characters(parser);
		} else if (ch == '(') {
			parser->token = TOKEN_OPEN_PARENTHESIS;
			next_character(parser);
		} else if (ch == ')') {
			parser->token = TOKEN_CLOSE_PARENTHESIS;
			next_character(parser);
		} else if (ch == ',') {
			parser->token = TOKEN_COMMA;
			next_character(parser);
		} else if (unicode_isdigit(ch) || ch == '-') {
			if ((parser->token = read_number(parser)) < 0)
				return parser->token;
		} else if (ch == '"') {
			/* A quoted string literal. */
			parser->token = TOKEN_STRING;
			next_character(parser); /* Consume the opening quote. */
			unsigned start = parser->pos_ch0;
			variant_set_string(&parser->token_value, parser->input + start, 0);
			do {
				ch = next_character(parser);
				if (ch == END_OF_STREAM) {
					parser->token = parser_error(parser, 
						STKR_UNTERMINATED_STRING);
					return parser->token;
				}
			} while (ch != '"');
			parser->token_value.string.length = parser->pos_ch0 - start;
			next_character(parser); /* Consume the closing quote. */
		} else if (unicode_isidentfirst(ch)) {
			/* A keyword, or something we don't understand. */
			unsigned start = parser->pos_ch0;
			do {
				ch = next_character(parser);
				if (ch == '-' && !unicode_isalnum(parser->ch1))
					break;
			} while (unicode_isident(ch));
			int keyword = find_keyword(parser->input + start, 
				parser->pos_ch0 - start);
			
			/* Handle keywords like "false" and "true" that become non-keyword
			 * tokens, and simplify compound tokens like url(...) and 
			 * rgb(...). */
			if (keyword == TOKEN_FALSE || keyword == TOKEN_TRUE) {
				parser->token = TOKEN_BOOLEAN;
				variant_set_integer(&parser->token_value, 
					int(keyword == TOKEN_TRUE));
			} else if (keyword == TOKEN_RGB || keyword == TOKEN_RGBA || 
				keyword == TOKEN_ALPHA) {
				parser->token = keyword;
				variant_set_integer(&parser->token_value, parser->token);
				parser->token = read_color_literal(parser, keyword);
			} else if (keyword == TOKEN_URL) {
				parser->token = keyword;
				variant_set_integer(&parser->token_value, parser->token);
				parser->token = read_url_literal(parser);
			} else if (is_keyword(keyword)) {
				/* A keyword token. */
				parser->token = keyword;
				variant_set_integer(&parser->token_value, parser->token);
			} else {
				parser->token = STKR_INVALID_INPUT;
				return parser_error(parser, STKR_INVALID_INPUT);
			}
		} else {
			/* Something bogus. */
			parser->token = STKR_INVALID_INPUT;
			return parser_error(parser, STKR_INVALID_INPUT);
		}
	} else {
		parser->token_start = parser->pos_ch0;

		/* Return a break token if the last character was a line break. */
		if (parser->emit_break) {
			parser->emit_break = false;
			parser->token = TOKEN_BREAK;
			return parser->token;
		}

		/* Freeform input consists of text and breaks, and is terminated by an 
		 * unescaped '<'. */
		if (ch == '<') {
			ch = next_character(parser);
			if (ch == '/') {
				parser->token = TOKEN_OPEN_ANGLE_SLASH;
				next_character(parser);
			} else {
				parser->token = TOKEN_OPEN_ANGLE;
			}
			parser->in_tag = true;
			return parser->token;
		}

		/* A text token. */
		parser->token = TOKEN_TEXT_BLANK;
		parser->token_escape_count = 0;
		variant_set_string(&parser->token_value, 
			parser->input + parser->pos_ch0, 0);
		bool seen_newline = false;
		unsigned start = parser->pos_ch0;
		do {
			if (unicode_isspace(ch)) {
				if (ch == '\n') {
					parser->emit_break = seen_newline;
					seen_newline = true;
				}
			} else {
				seen_newline = false;
				parser->token = TOKEN_TEXT;
			}
			if (parser->ch0 == '\\' && is_escapeable(parser->ch1)) {
				parser->token_escape_count++;
				ch = skip_two_characters(parser);
			} else {
				ch = next_character(parser);
			}
		} while (ch != '<' && ch != END_OF_STREAM && !parser->emit_break);
		parser->token_value.string.length = parser->pos_ch0 - start;
	}
	return parser->token;
}

/* Parses a url(...) literal. */
static int read_url_literal(Parser *parser)
{
	/* Look ahead to decide whether this is a url '(' ... ')' literal or 
	 * the keyword 'url' alone. */
	unsigned saved_pos = parser->pos_ch0;
	if (next_token(parser) != TOKEN_OPEN_PARENTHESIS) {
		rewind(parser, saved_pos);
		parser->token = TOKEN_URL;
		variant_set_integer(&parser->token_value, TOKEN_URL);
		return TOKEN_URL;
	}

	/* Read the url ( ... ) literal. */
	int value_token = next_token(parser);
	Variant string_value;
	if (value_token == TOKEN_STRING) {
		string_value = parser->token_value;
	} else {
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, "string");
	}
	if (next_token(parser) != TOKEN_CLOSE_PARENTHESIS)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, ")");
	parser->token = TOKEN_URL_LITERAL;
	parser->token_value = string_value;
	return TOKEN_URL_LITERAL;
}

/* Parses an rgb(r, g, b), rgba(r, g, b, a) or alpha(a) literal. */
static int read_color_literal(Parser *parser, int keyword)
{
	if (next_token(parser) != TOKEN_OPEN_PARENTHESIS)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, "(");
	unsigned num_components = 0, offset = 0;
	uint32_t components[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	switch (keyword) {
		case TOKEN_RGB:
			num_components = 3;
			break;
		case TOKEN_RGBA:
			num_components = 4;
			break;
		case TOKEN_ALPHA:
			num_components = 1;
			offset = 3;
			break;
	}
	for (unsigned i = 0; i < num_components; ++i) {
		if (i != 0 && next_token(parser) != TOKEN_COMMA)
			return parser_error(parser, STKR_UNEXPECTED_TOKEN, "comma");
		int value_token = next_token(parser);
		if (value_token == TOKEN_INTEGER) {
			components[offset + i] = uint32_t(parser->token_value.integer);
		} else if (value_token == TOKEN_FLOAT) {
			components[offset + i] = uint32_t(parser->token_value.real * 255.0f + 0.5f);
		} else {
			return parser_error(parser, STKR_UNEXPECTED_TOKEN, "number");
		}
		if (components[offset + i] > 255) {
			return parser_error(parser, STKR_COLOR_COMPONENT_OUT_OF_RANGE, 
				components[offset + i]);
		}
	}
	if (next_token(parser) != TOKEN_CLOSE_PARENTHESIS)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, ")");
	parser->token = TOKEN_COLOR_LITERAL;
	uint32_t color = (components[0] <<  0) + 
	                 (components[1] <<  8) + 
	                 (components[2] << 16) + 
	                 (components[3] << 24);
	variant_set_integer(&parser->token_value, (int)color);
	return TOKEN_COLOR_LITERAL;
}

static int parse_tag(Parser *parser);


/* Appends a text node to the current scope. */
static int add_text_node(Parser *parser)
{
	if (parser->scope == NULL)
		return parser_error(parser, STKR_INCORRECT_CONTEXT);
	unsigned unescaped_length = parser->token_value.string.length - 
		parser->token_escape_count;
	Node *node = NULL;
	int rc = create_node(
		&node, 
		parser->document, 
		LNODE_TEXT, 
		TOKEN_INVALID, 
		NULL, 0, 
		NULL, 
		unescaped_length);
	if (rc >= 0) {
		unescape(
			parser->token_value.string.data, 
			parser->token_value.string.length, 
			node->text);
		set_node_debug_string(node, "text (%u characters)", 
			unescaped_length);
		append_child(parser->document, parser->scope, node);
	} else {
		parser_error(parser, STKR_ERROR);
	}
	return rc;
}

static bool should_skip_tag(const Parser *, int tag_name)
{
	return tag_name != TOKEN_RULE && 
		node_type_for_tag(tag_name) == LNODE_INVALID;
}

static int maybe_skip_opening_tag(Parser *parser)
{
	if (parser->token != TOKEN_OPEN_ANGLE)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, "<");	
	int tag_name = next_token(parser);
	if (!should_skip_tag(parser, tag_name))
		return STKR_OK;
	return parse_tag(parser);
}

static int maybe_skip_closing_tag(Parser *parser)
{
	if (parser->token != TOKEN_OPEN_ANGLE_SLASH)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, "</");
	int tag_name = next_token(parser);
	if (!should_skip_tag(parser, tag_name))
		return STKR_OK;
	if (next_token(parser) != TOKEN_CLOSE_ANGLE)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, ">");
	next_token(parser); /* Consume the closer. */
	return STKR_SKIP_TAG;
}

static int parse_text(Parser *parser, bool in_block = true)
{
	int rc = STKR_OK;
	bool done = false, have_paragraph = false;

	do {
		bool open_paragraph = false;
		bool close_paragraph = false;

		int token = parser->token;
		if (token == TOKEN_TEXT) {
			open_paragraph = in_block;
		} else if (token == TOKEN_TEXT_BLANK) {
			if (in_block) {
				next_token(parser);
				continue;
			}
		} else if (token == TOKEN_OPEN_ANGLE) {
			rc = maybe_skip_opening_tag(parser);
			if (rc == STKR_SKIP_TAG)
				continue;
			if (rc != STKR_OK)
				return rc;
			Layout layout = token_natural_layout(parser->token);
			if (in_block && layout != LAYOUT_NONE) {
				if (layout != LAYOUT_INLINE)
					close_paragraph = true; /* Block is sibling to <p>. */
				else
					open_paragraph =  true; /* Non-block goes inside <p>. */
			}
		}  else if (token == TOKEN_OPEN_ANGLE_SLASH) {
			rc = maybe_skip_closing_tag(parser);
			if (rc == STKR_SKIP_TAG)
				continue;
			if (rc != STKR_OK)
				return rc;
			/* A closer for a tag we're not skipping terminates the text 
			 * scope. */
			done = true; 
		} else if (token == TOKEN_BREAK) {
			next_token(parser);
			close_paragraph = true;
		} else if (token == TOKEN_EOF) {
			done = true;
		} else {
			return parser_error(parser, STKR_INVALID_INPUT);
		}

		/* Start a new paragraph if required. */
		if (open_paragraph && !have_paragraph) {
			Node *paragraph = NULL;
			rc = create_node(&paragraph, parser->document, LNODE_PARAGRAPH, 
				TOKEN_PARAGRAPH);
			if (rc < 0)
				return parser_error(parser, STKR_ERROR);
			push_scope(parser, paragraph);
			have_paragraph = true;
		}

		/* Append a new text node to the scope. */
		if (token == TOKEN_TEXT || token == TOKEN_TEXT_BLANK) {
			rc = add_text_node(parser);
			if (rc < 0)
				return rc;
			next_token(parser);
		}

		/* Close any open paragraph before reading the tag, if requested. */
		if (close_paragraph && have_paragraph) {
			have_paragraph = false;
			rc = pop_scope(parser);
			if (rc != STKR_OK)
				return rc;
		}

		/* If we've encountered a tag, parse it. */
		if (token == TOKEN_OPEN_ANGLE) 
			if ((rc = parse_tag(parser)) != STKR_OK)
				return rc;
	} while (!done);

	return have_paragraph ? pop_scope(parser) : STKR_OK;
}

static int interpret_tag(Parser *parser, int tag_name, 
	const AttributeAssignment *attributes, unsigned num_attributes)
{
	if (tag_name == TOKEN_RULE) {
		int rc = add_rule_from_attributes(
			NULL,
			parser->system, 
			parser->document,
			attributes, 
			num_attributes);
		if (rc < 0)
			return parser_error(parser, STKR_ERROR);
		return STKR_OK_NO_SCOPE;
	} else {
		/* Only rules can be parsed outside a document context. */
		if (parser->document == NULL)
			return parser_error(parser, STKR_INCORRECT_CONTEXT);
		NodeType node_type = node_type_for_tag(tag_name);
		if (node_type == LNODE_INVALID)
			return STKR_SKIP_TAG;
		Node *node = NULL;
		int rc = create_node(&node, parser->document, node_type, tag_name, 
			attributes, num_attributes);
		if (rc < 0)
			return parser_error(parser, STKR_ERROR);
		push_scope(parser, node);
	}
	return STKR_OK;
}

static int match_closing_tag(Parser *parser, int tag_name)
{
	/* Does the closing tag match the opening tag? */
	if (parser->token != tag_name)
		return parser_error(parser, STKR_MISMATCHED_TAGS, 
			TOKEN_STRINGS[tag_name]);
	if (next_token(parser) != TOKEN_CLOSE_ANGLE)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, ">");
	next_token(parser); // Consume '>'.
	return pop_scope(parser);
}

/* Parses a list of attribute assignments inside a tag. */
static int parse_attribute_list(Parser *parser, AttributeAssignment *assignments)
{
	int rc = STKR_OK;

	unsigned num_attributes = 0;
	while (is_keyword(parser->token)) {
		if (num_attributes == MAX_ATTRIBUTES)
			return parser_error(parser, STKR_TOO_MANY_ATTRIBUTES);

		/* Read an assigment. */
		int name_token = parser->token;
		int value_token = TOKEN_INVALID;
		int op_token = next_token(parser);
		int op = token_to_attribute_operator(op_token);
		Variant value;
		bool synthetic = false;
		if (op < 0) {
			/* An attribute with no "=value" suffix. If it's a boolean 
			 * attribute, treat this as shorthand for "attribute = true". */
			AttributeSemantic as = attribute_semantic(name_token);
			if (as == ASEM_FLAG) {
				value_token = TOKEN_BOOLEAN;
				variant_set_integer(&value, true, VSEM_BOOLEAN);
				op = AOP_SET;
				synthetic = true;
			} else if (num_attributes == 0) {
				continue;
			} else {
				return parser_error(parser, STKR_UNEXPECTED_TOKEN, 
					"assignment operator");
			}
		} else {
			next_token(parser); /* Skip the assignment operator. */
			value_token = parser->token;
			value = parser->token_value;
		}

		/* Add a semantic to the value based on its token. */
		value.semantic = value_semantic(value_token);
		if (value.semantic == VSEM_INVALID)
			return STKR_TYPE_MISMATCH;

		/* Is the assignment valid? */
		rc = abuf_set(NULL, (Token)name_token, &value, (AttributeOperator)op);
		if (rc < 0) {
			switch (rc) {
				case STKR_NO_SUCH_ATTRIBUTE:
					return parser_error(parser, 
						STKR_INVALID_TOKEN, "attribute name");
				case STKR_TYPE_MISMATCH:
					return parser_error(parser, 
						STKR_ATTRIBUTE_VALUE_TYPE_MISMATCH, 
						TOKEN_STRINGS[name_token]);
				case STKR_OUT_OF_BOUNDS:
					return parser_error(parser, 
						STKR_ATTRIBUTE_VALUE_OUT_OF_BOUNDS, 
						TOKEN_STRINGS[name_token]);
				case STKR_INVALID_OPERATION:
					return parser_error(parser, STKR_INVALID_OPERATION, 
						TOKEN_STRINGS[op_token], TOKEN_STRINGS[name_token]);
			}
			return parser_error(parser, STKR_ERROR);
		}
		assignments[num_attributes].name = (Token)name_token;
		assignments[num_attributes].op = (AttributeOperator)op;
		assignments[num_attributes].value = value;
		num_attributes++;

		/* Consume the value token, if there was one. */
		if (!synthetic)
			next_token(parser); 
	}

	return (int)num_attributes;
}

static int parse_tag(Parser *parser)
{
	int rc = STKR_OK;

	/* Read the tag name. */
	int tag_name = parser->token;
	if (!is_keyword(tag_name))
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, "tag name");

	/* Read the attribute assignments. */
	AttributeAssignment assignments[MAX_ATTRIBUTES];
	rc = parse_attribute_list(parser, assignments);
	if (rc < 0)
		return rc;
	unsigned num_attributes = (unsigned)rc;

	/* Read the terminator. */
	bool self_terminating = (parser->token == TOKEN_SLASH_CLOSE_ANGLE);
	if (parser->token != TOKEN_CLOSE_ANGLE && !self_terminating)
		return parser_error(parser, STKR_UNEXPECTED_TOKEN, ">");
	next_token(parser);

	/* Create a node for the tag. */
	rc = interpret_tag(parser, tag_name, assignments, num_attributes);
	if (rc < 0)
		return rc;

	/* We don't ever read the contents of skipped tags, since they will be
	 * flattened into the enclosing text scope. */
	if (rc == STKR_SKIP_TAG)
		return STKR_SKIP_TAG;

	/* Self terminating tags have no content and we don't expect a closer. */
	if (self_terminating)
		return (rc == STKR_OK_NO_SCOPE) ? STKR_OK : pop_scope(parser);

	/* Parse the contents. */
	rc = parse_text(parser, token_natural_layout(tag_name) == LAYOUT_BLOCK);
	if (rc != STKR_OK)
		return rc;

	/* Match the closing tag. Note than parse_text() has consumed the "</"
	 * because it needed to decide whether to skip the tag. */
	return match_closing_tag(parser, tag_name);
}

int parse_document(Parser *parser)
{
	bool in_block = parser->scope == NULL || 
		natural_layout((NodeType)parser->scope->type) == LAYOUT_BLOCK;
	int rc = parse_text(parser, in_block);
	if (rc != STKR_OK)
		return rc;
	if (parser->token != TOKEN_EOF) {
		if (parser->token == TOKEN_OPEN_ANGLE_SLASH) {
			int tag_name = next_token(parser);
			rc = parser_error(parser, STKR_MISMATCHED_TAGS, 
				TOKEN_STRINGS[tag_name]);
		} else {
			rc = parser_error(parser, STKR_UNEXPECTED_TOKEN, 
				"end of stream");
		}
	}
	return rc;
}

void init_parser(Parser *parser, System *system, Document *document, 
	unsigned flags)
{
	parser->system = system;
	parser->document = document;
	parser->input = NULL;
	parser->input_size = 0;
	parser->pos = 0;
	parser->token = STKR_INVALID_TOKEN;
	parser->message.utf8 = NULL;
	parser->message_length = 0;
	parser->code = STKR_OK;
	parser->flags = flags;
}

void deinit_parser(Parser *parser)
{
	clear_message(parser);
}

static void reset_parser(Parser *parser, Node *root, 
	const char *input, unsigned length)
{
	parser->root = root;
	parser->first_parsed = NULL;
	parser->last_parsed = NULL;
	parser->scope = root;
	parser->input = input;
	parser->input_size = length;
	parser->pos = 0;
	parser->line = 1;
	parser->token = STKR_INVALID_TOKEN;
	parser->token_start = 0;
	parser->in_tag = false;
	parser->emit_break = false;
	parser->code = STKR_OK;
	clear_message(parser);
	if (skip_two_characters(parser) == UNICODE_BOM)
		next_character(parser);
	next_token(parser);
}

int parse(Parser *parser, Node *root, const char *input, unsigned length)
{
	/* If we have a root node, make sure it's from the parser's document. */
	if (root != NULL && (parser->document == NULL ||
		root->document != parser->document))
		return parser_error(parser, STKR_ERROR);
	
	/* Pass the source to the document so it can make a copy if desired. */
	if (parser->document != NULL)
		document_store_source(parser->document, input, length);
	
	/* Reset parsing state and parse the input. */
	reset_parser(parser, root, input, length);
	int rc = parse_document(parser);
	if (rc == STKR_OK_HALT)
		rc = STKR_OK;
	return rc;
}

static int parse_helper(
	System *system, 
	Document *document, 
	Node *root, 
	const char *input, 
	unsigned length,
	unsigned flags, 
	Node **first_parsed,
	Node **last_parsed,
	void *error_buffer, 
	unsigned error_buffer_size)
{
	Parser parser;
	init_parser(&parser, system, document, flags);
	int rc = parse(&parser, root, input, length);
	if (error_buffer != NULL)
		strcpy_encoding(parser.message.data, parser.message_length, 
			error_buffer, error_buffer_size, system->message_encoding);
	if (first_parsed != NULL)
		*first_parsed = parser.first_parsed;
	if (last_parsed != NULL)
		*last_parsed = parser.last_parsed;
	deinit_parser(&parser);
	return rc;
}

/* Public parsing function. */
int parse(
	System *system, 
	Document *document, 
	Node *root, 
	const char *input, 
	unsigned length, 
	void *error_buffer, 
	unsigned error_buffer_size)
{
	return parse_helper(system, document, root, input, length, 0, 
		NULL, NULL, error_buffer, error_buffer_size);
}

/* Attempts to create a single tree of nodes from a markup fragment. The parser
 * halts as soon as the markup for one node has been read, ignoring the 
 * remainder of the input. */
int create_node_from_markup(
	Node **out_node, 
	Document *document, 
	const char *input, 
	unsigned length, 
	char *error_buffer, 
	unsigned error_buffer_size)
{
	return parse_helper(document->system, document, NULL, input, length, 
		PARSEFLAG_SINGLE_NODE, out_node, NULL, error_buffer, error_buffer_size);
}

} // namespace stkr
