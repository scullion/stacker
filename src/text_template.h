#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdarg>

#include <unordered_map>

namespace urlcache { 
	class UrlCache; 
	typedef void *UrlHandle;
	enum UrlNotification;
	enum UrlFetchState;
	typedef uint64_t UrlKey; 
}

namespace sstl {

/* Default number of decimal places to use when printing floats. Can be modified
 * with filters. */
const unsigned FLOAT_DISPLAY_PRECISION = 2;

/* Things that can go wrong with compiling or rendering a template. */
enum TemplateErrorCode {
	TE_OUT_OF_BOUNDS          = -14,
	TE_INVALID_ARGUMENT       = -13,
	TE_MISSING_PAREN          = -12,
	TE_STACK_OVERFLOW         = -11,
	TE_STACK_UNDERFLOW        = -10,
	TE_CYCLIC_INCLUDE         = -9,
	TE_TOO_MANY_CLAUSES       = -8,
	TE_INVALID_INPUT          = -7,
	TE_UNEXPECTED_TOKEN       = -6,
	TE_UNTERMINATED_STRING    = -5,
	TE_UNTERMINATED_DIRECTIVE = -4,
	TE_ERROR                  = -3,
	TE_TRUNCATED              = -2,
	TE_NOT_READY              = -1,
	TE_OK                     =  0
};

/* The type of an entry in a variable table. */
enum VariableType {
	TVT_NULL,
	TVT_BOOLEAN,
	TVT_INTEGER,
	TVT_FLOAT,
	TVT_STRING
};

/* A scope is a key value dictionary. Later scopes override earlier. */
enum Scope {
	VSCOPE_GLOBAL,
	VSCOPE_LOCAL,
	VSCOPE_COUNT
};

/* A template is complide with a "lookup chain", which is a bit mask that says
 * which scopes should be used while rendering it. Lookup chains are made
 * by ORing together these bits. */
enum ScopeBit {
	VSCOPE_BIT_GLOBAL = (1 << VSCOPE_GLOBAL),
	VSCOPE_BIT_LOCAL  = (1 << VSCOPE_LOCAL)
};

const unsigned DEFAULT_SCOPE_CHAIN = VSCOPE_BIT_GLOBAL;

/* Strongly typed entry in a variable table. */
struct TemplateVariable {
	VariableType type;
	union {
		struct { int value; } integer;
		struct { double value; int precision; } real;
		struct { char *data; unsigned length, capacity; } string;
	};
};

/* Internals. */
struct CompilerState;
struct RenderState;
struct TemplateOperation;
enum Token;

/* A bytecode program representing a template. */
class CompiledTemplate {
public:
	/* Renders the template. If no buffer is supplied or the buffer is too 
	 * small, a new heap buffer is allocated to store the text. The caller 
	 * should always check whether 'out_text' has been set to a buffer other
	 * than the one supplied, and if so, delete it. 
	 * 
	 * The caller may set 'out_text' to NULL to indicate that no heap buffer 
	 * should be allocated. In this case, if the text does not fit in the buffer 
	 * supplied, it is truncated. If the supplied buffer is also NULL, no output 
	 * at all is generated. This is useful for efficiently measuring the 
	 * length of the rendered template. 
	 * 
	 * In all cases, output text is guaranteed to be null terminated. The 
	 * reported length is the full length of the template, even if truncation
	 * occurs. The reported length excludes the null terminator. */
	int render(char **out_text, unsigned *out_length, char *buffer = 0, 
		unsigned buffer_size = 0, unsigned chain = DEFAULT_SCOPE_CHAIN) const;
	/* Retrieves the error code reported by the compiler. */
	TemplateErrorCode error_code(void) const;
	/* Retrieves the string message recorded during compilation, or NULL if
	 * no error occurred. */
	const char *error_message(void) const;
	/* Reports memory used by the object. */
	unsigned memory_usage(void) const;

private:

	friend class TemplateProcessor;

	/* Templates must be created and delete via a TemplateProcessor. */
	CompiledTemplate(TemplateProcessor *processor);
	~CompiledTemplate();

	TemplateProcessor *processor;
	TemplateOperation *program;
	unsigned num_operations;
	char *heap;
	unsigned heap_size;
	char compiler_error;
	unsigned short use_count;

	int set_error(CompilerState *cs, int code, ...);
	int set_error(RenderState *rs, int code, ...) const;
	int format_error(int code, va_list args, CompilerState *cs = 0, 
		RenderState *rs = 0);
	void clear_program(void);
	void clear_heap(void);
	void reallocate_heap(unsigned size);

	int next_token(CompilerState *cs);
	int compile(const char *input, unsigned length);
	void reset_compiler(CompilerState *cs, const char *input, unsigned length);

	TemplateOperation *add_operation(enum Opcode opcode);
	int parse_primary_expression(CompilerState *cs);
	int parse_unary_expression(CompilerState *cs);
	int parse_filter_specification(CompilerState *cs);
	int parse_pipe_expression(CompilerState *cs);
	int parse_multiplicative_expression(CompilerState *cs);
	int parse_additive_expression(CompilerState *cs);
	int parse_concatenative_expression(CompilerState *cs);
	int parse_relational_expression(CompilerState *cs);
	int parse_and_expression(CompilerState *cs);
	int parse_or_expression(CompilerState *cs);
	int parse_expression(CompilerState *cs);
	int parse_conditional_directive(CompilerState *cs);
	int parse_include_directive(CompilerState *cs);
	int parse_text(CompilerState *cs);
	int parse_template(CompilerState *cs);
	
	bool match_argument(CompilerState *cs, Token token, bool required, 
		const char *filter_name, const char *parameter);
	bool match_integer_argument(CompilerState *cs, bool required, 
		int *out_value, const char *filter_name, const char *parameter, 
		int min_value = INT_MIN, int max_value = INT_MAX);
	bool match_string_argument(CompilerState *cs, bool required, 
		const char **out_text, unsigned *out_length, 
		const char *filter_name, const char *parameter, 
		unsigned min_length = 0, unsigned max_length = UINT_MAX);


	void init_render_state(RenderState *rs, char *buffer, 
		unsigned buffer_size) const;
	void reallocate_render_buffer(RenderState *rs, unsigned size) const;
	int append(RenderState *rs, const void *data, unsigned size) const;
	int append_variable(RenderState *rs, const TemplateVariable *tv) const;
	int append_template(RenderState *rs, const CompiledTemplate *ct, 
		unsigned chain) const;
	void rewind(RenderState *rs) const;
	int execute(RenderState *rs, unsigned chain) const;
	int execute_op(RenderState *rs, const TemplateOperation *op, 
		unsigned chain) const;
	int push(RenderState *rs, const TemplateVariable *tv) const;
	int pop_insert(RenderState *rs) const;
	int look_up(unsigned chain, uint64_t key, 
		const TemplateVariable **tv) const;
	int insert_url(RenderState *rs, urlcache::UrlKey url_key, 
		unsigned chain) const;
};

/* Manages dictionaries of template variables. */
class TemplateProcessor
{
public:

	TemplateProcessor(urlcache::UrlCache *url_cache = 0);
	~TemplateProcessor();

	/* Deletes all variables in a scope. */
	void clear_scope(Scope scope);
	/* Sets a variable to a string value. */
	void set_string(Scope scope, const char *name, const char *value, 
		int length = -1);
	/* Sets a variable to an integer value. */
	void set_integer(Scope scope, const char *name, int value);
	/* Sets a variable to a floating point value. */
	void set_float(Scope scope, const char *name, double value, 
		int precision = -1);
	/* Sets a variable to a boolean value. */
	void set_boolean(Scope scope, const char *name, bool value);

	/* Attempts to compile a template. If compilation fails, an error code
	 * and message can be retrieved from the returned object. Regardless of
	 * whether compilation succeeded, the caller is obliged to call 
	 * destroy() on the compiled template. */
	CompiledTemplate *create(const char *source, int source_length = -1);
	/* Retrieves template source from the specified URL and compiles a 
	 * template. */
	CompiledTemplate *create_from_url(const char *url, int length = -1);
	CompiledTemplate *create_from_url(urlcache::UrlKey key);
	/* Destroys a template created with create(). */
	void destroy(CompiledTemplate *ct);
	/* Makes a copy of a template object. */
	CompiledTemplate *copy(const CompiledTemplate *ct);
	/* Returns a pointer to the URL cache used to process URL includes. */
	urlcache::UrlCache *url_cache(void) const;

private:

	friend class CompiledTemplate;

	static unsigned notify_callback(
		urlcache::UrlHandle handle, 
		urlcache::UrlNotification type, 
		urlcache::UrlKey key, 
		TemplateProcessor *processor, 
		CompiledTemplate *ct, 
		urlcache::UrlFetchState fetch_state);

	typedef std::unordered_map<uint64_t, TemplateVariable> VariableTable;

	VariableTable tables[VSCOPE_COUNT];
	urlcache::UrlCache *cache;
	int notify_sink_id;
	unsigned inclusion_depth;

	TemplateVariable *get_or_create_variable(Scope scope, const char *name);
	const TemplateVariable *look_up(uint64_t key, unsigned chain);
	CompiledTemplate *create_from_url_internal(urlcache::UrlHandle handle);
};

/*
 * Unit Tests
 */
void unit_test(FILE *os = 0);

} // namespace text_template
