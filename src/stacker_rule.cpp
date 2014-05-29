#include "stacker_rule.h"

#include <cstdint>

#include <algorithm>
#include <unordered_map>

#include "stacker_shared.h"
#include "stacker_util.h"
#include "stacker_attribute.h"
#include "stacker_node.h"
#include "stacker_document.h"
#include "stacker_system.h"

namespace stkr {

/* True if the supplied attribute can be part of a rule. */
inline bool is_rule_attribute(int token)
{
	return token != TOKEN_MATCH && token != TOKEN_GLOBAL;
}

/* Builds a lookup key for a rule table by combining a rule key with a level
 * number. The resulting values matches a rule with the specified key 'level' 
 * places from the end of its selector. */
static uint64_t make_rule_lookup_key(uint64_t key, unsigned level)
{
	static const unsigned LEVEL_SHIFT = 60;
	static const uint64_t NAME_MASK = (1ull << LEVEL_SHIFT) - 1ull;
	assertb(level == (level & 7));
	return (key & NAME_MASK) + ((uint64_t)level << LEVEL_SHIFT);
}

/* Parses a selector, converting it into an array of rule keys. */
int parse_selector(ParsedSelector *ps, const char *s, int length)
{
	ps->total_keys = 0;
	ps->num_clauses = 0;

	if (length < 0)
		length = (int)strlen(s);
	unsigned depth = 0, i = unsigned(length - 1), ul = (unsigned)length;
	for (;;) {
		/* Skip white space. */
		while (i + 1 != 0 && isspace(s[i]))
			--i;

		/* Is this the end of the current clause? */
		if (i + 1 == 0 || s[i] == ',') {
			if (depth == 0) {
				return (ps->num_clauses == 0) ? STKR_SELECTOR_EMPTY : 
					STKR_SELECTOR_ILL_FORMED;
			}
			ps->keys_per_clause[ps->num_clauses++] = depth;
			depth = 0;
			if (i + 1 == 0)
				break;
			--i; // Skip the comma.
		} else {
			/* Too many parts in this clause? */
			if (depth == MAX_SELECTOR_DEPTH)
				return STKR_SELECTOR_TOO_LONG;

			/* Scan to the start of the part. */
			while (i + 1 != 0 && !isspace(s[i]) && s[i] != ',')
				--i;
			unsigned before_part = i++;
			
			/* Read the node type to make the first token in the token buffer.
			 * If there isn't a node type, the first token is "*". */
			uint64_t tokens[MAX_RULE_CLASSES + 1];
			unsigned num_tokens = 0;
			if (isidentfirst(s[i]) || s[i] == '*') {
				unsigned start = i;
				do { ++i; } while (i != ul && isident(s[i]));
				tokens[num_tokens++] = murmur3_64(s + start, i - start);
			} else {
				tokens[num_tokens++] = murmur3_64_cstr("*");
			}
			
			/* Read a sequence of zero or more ".class" or ":state" 
			 * qualifiers. */
			while (i != ul && (s[i] == '.' || s[i] == ':') && 
				num_tokens != MAX_RULE_CLASSES + 1) {
				unsigned start = (s[i] == ':') ? i++ : ++i;
				if (i == ul || !isidentfirst(s[i]))
					return STKR_SELECTOR_MISSING_CLASS;
				do { ++i; } while (i != ul && isident(s[i]));
				tokens[num_tokens++] = murmur3_64(s + start, i - start);
			}
			if (i != ul && !isspace(s[i]) && s[i] != ',')
				return STKR_SELECTOR_INVALID_CHAR;
			i = before_part;

			/* Compute the combined key for this part by hashing the token 
			 * keys. */
			std::sort(tokens + 1, tokens + num_tokens);
			uint64_t rule_key = murmur3_64(tokens, num_tokens * sizeof(uint64_t));
			ps->keys[ps->total_keys++] = make_rule_lookup_key(rule_key, depth);
			++depth;
		}
	}

	return STKR_OK;
}

static int create_rule(
	Rule **result, 
	System *system, 
	Document *document, 
	const ParsedSelector *ps,
	const AttributeAssignment *attributes, 
	unsigned num_attributes, 
	unsigned flags, 
	int priority_key)
{
	/* Allocate the rule object, its selectors, and a static attribute buffer 
	 * big enough to hold the supplied attributes. */
	unsigned attribute_block_size = 0;
	for (unsigned i = 0; i < num_attributes; ++i) {
		if (is_rule_attribute(attributes[i].name)) {
			int rc = abuf_set(NULL, attributes[i].name, &attributes[i].value);
			if (rc < 0)
				return rc;
			attribute_block_size += (unsigned)rc;
			if (attributes[i].name == TOKEN_CLASS)
				flags |= RFLAG_MODIFIES_CLASS;
		}
	} 
	unsigned bytes_required = sizeof(Rule);
	bytes_required += ps->total_keys * sizeof(uint64_t);
	bytes_required += ps->num_clauses * sizeof(Selector);
	bytes_required += attribute_block_size;
	char *block = new char[bytes_required];
	Rule *rule = (Rule *)block;
	block += sizeof(Rule);
	rule->keys = (uint64_t *)block;
	block += ps->total_keys * sizeof(uint64_t);
	rule->selectors = (Selector *)block;
	block += ps->num_clauses * sizeof(Selector);
	abuf_init(&rule->attributes, block, attribute_block_size);
	memcpy(rule->keys, ps->keys, ps->total_keys * sizeof(uint64_t));
	rule->total_keys = (unsigned short)ps->total_keys;
	rule->num_selectors = (unsigned char)ps->num_clauses;
	rule->priority = priority_key;
	rule->flags = (unsigned char)flags;
	rule->revision = 0;
	if (document != NULL)
		rule->document = document;
	else
		rule->system = system;

	/* Make a selector object for each clause. */
	unsigned key_offset = 0;
	for (unsigned i = 0; i < ps->num_clauses; ++i) {
		Selector *selector = rule->selectors + i;
		selector->rule = rule;
		selector->key_offset = (unsigned short)key_offset;
		selector->num_keys = (unsigned short)ps->keys_per_clause[i];
		key_offset += ps->keys_per_clause[i];
	}

	/* Store the supplied attributes in the buffer. */
	for (unsigned i = 0; i < num_attributes; ++i) {
		if (is_rule_attribute(attributes[i].name))
			abuf_set(&rule->attributes, attributes[i].name, 
				&attributes[i].value, attributes[i].op);
	}

	*result = rule;
	return STKR_OK;
}

static void destroy_rule_internal(Rule *rule)
{
	abuf_clear(&rule->attributes);
	delete [] (char *)rule;
}

/* Inserts a rule into a rule table. The table takes ownership of the rule. */
static void add_rule_to_table(RuleTable *table, Rule *rule)
{
	for (unsigned i = 0; i < rule->num_selectors; ++i) {
		Selector *selector = rule->selectors + i;
		for (unsigned j = 0; j < selector->num_keys; ++j) {
			uint64_t key = rule->keys[selector->key_offset + j];
			table->insert(RuleTable::value_type(key, selector));
		}
	}
}

/* Removes a rule from a rule table. */
static void remove_rule_from_table(RuleTable *table, Rule *rule)
{
	typedef RuleTable::iterator I;
	for (unsigned i = 0; i < rule->num_selectors; ++i) {
		Selector *selector = rule->selectors + i;
		for (unsigned j = 0; j < selector->num_keys; ++j) {
			uint64_t key = rule->keys[selector->key_offset + j];
			std::pair<I, I> range = table->equal_range(key);
			for (I iter = range.first; iter != range.second; ++iter) {
				if (iter->second == selector) {
					table->erase(iter);
					break;
				}
			}
		}
	}
}

/* Empties a rule table and frees all rules in contains. */
void clear_rule_table(RuleTable *table)
{
	RuleTable::iterator iter = table->begin();
	while (iter != table->end()) {
		Rule *rule = iter->second->rule;
		if (--rule->total_keys == 0)
			destroy_rule_internal(rule);
		++iter;
	}
	table->clear();
}

/* Makes a key used to sort rules. The "priority" is a user supplied value.
 * Rules of equal priority are order by the "order" value, which represents
 * document position. */
static int make_rule_priority_key(int priority, int order)
{
	return (order | (0xFF << RULE_PRIORITY_SHIFT)) + 
		(priority << RULE_PRIORITY_SHIFT);
}

/* Retrieves a System pointer from a document or system rule. */
inline System *rule_get_system(const Rule *rule)
{
	return ((rule->flags & RFLAG_IN_DOCUMENT_TABLE) != 0) ? 
		rule->document->system : rule->system;
}

/* Creates a new rule and adds it to the document or system rule table. */
int add_rule(
	Rule **result, 
	System *system, 
	Document *document, 
	const ParsedSelector *ps,
	const AttributeAssignment *attributes, 
	unsigned num_attributes,
	unsigned flags, 
	int priority)
{
	/* If this is a global rule, add it to the system's rule table, otherwise
	 * to the document's. */
	RuleTable *table;
	if (document == NULL || (flags & RFLAG_GLOBAL) != 0) {
		table = &system->global_rules;
		system->rule_table_revision++;
		flags |= RFLAG_IN_SYSTEM_TABLE;
	} else {
		table = &document->rules;
		document->flags |= DOCFLAG_RULE_TABLE_CHANGED;
		flags |= RFLAG_IN_DOCUMENT_TABLE;
	}

	/* Parse the 'match' attribute and create the rule.*/
	Rule *rule = NULL;
	if (result != NULL)
		*result = NULL;
	int order = -(1 + int(table->size()));
	int priority_key = make_rule_priority_key(priority, order);
	int rc = create_rule(
		&rule, 
		system,
		document,
		ps,
		attributes, 
		num_attributes, 
		flags,
		priority_key);
	if (rc < 0)
		return rc;

	/* Initialize the rule's revision counter using the system counter. This
	 * ensures, modulo wrap-around, that the set of revision numbers exposed by 
	 * a rule over its lifetime is disjoint from that of any rule created after
	 * its destruction. We can then assume that if a newly matched rule has the 
	 * same pointer and revision number as one already in a rule slot, then 
	 * it's the same rule, not a different rule that happens to have the same 
	 * pointer (the one previously occupying the slot having been destroyed). */
	rule->revision = system->rule_revision_counter++;

	/* Add the rule. */
	add_rule_to_table(table, rule);
	
	if (result != NULL)
		*result = rule;
	return STKR_OK;
}

/* Creates a rule using a selector string. */
int add_rule(
	Rule **result, 
	System *system, 
	Document *document, 
	const char *selector, 
	int selector_length,
	const AttributeAssignment *attributes, 
	unsigned num_attributes,
	unsigned flags, 
	int priority)
{
	ParsedSelector ps;
	int rc = parse_selector(&ps, selector, selector_length);
	if (rc < 0)
		return rc;
	return add_rule(
		result,
		system,
		document, 
		&ps,
		attributes,
		num_attributes,
		flags,
		priority);
}

/* Extracts the "match" and "global" attributes from an attribute list and
 * uses them to create a rule. */
int add_rule_from_attributes(
	Rule **result, 
	System *system, 
	Document *document, 
	const AttributeAssignment *attributes, 
	unsigned num_attributes,
	unsigned flags, 
	int priority)
{
	const AttributeAssignment *match = NULL, *global = NULL;
	for (unsigned i = 0; i < num_attributes; ++i) {
		const AttributeAssignment *a = attributes + i;
		if (a->name == TOKEN_MATCH)
			match = a;
		else if (a->name == TOKEN_GLOBAL)
			global = a;
	}
	if (match == NULL)
		return STKR_MISSING_SELECTOR;
	
	const char *selector = match->value.string.data;
	int selector_length = (int)match->value.string.length;
	if (global != NULL && global->value.integer != 0)
		flags |= RFLAG_GLOBAL;
	
	return add_rule(
		result,
		system,
		document,
		selector,
		selector_length,
		attributes,
		num_attributes,
		flags, priority);
}

/* Removes a rule from any rule tables that contain it and destroys the rule. */
void destroy_rule(Rule *rule)
{
	System *system = rule_get_system(rule);
	Document *document = NULL;
	if ((rule->flags & RFLAG_IN_DOCUMENT_TABLE) != 0) {
		document = rule->document;
		remove_rule_from_table(&document->rules, rule);
		document->flags |= DOCFLAG_RULE_TABLE_CHANGED;
	} else if ((rule->flags & RFLAG_IN_SYSTEM_TABLE) != 0) {
		remove_rule_from_table(&system->global_rules, rule);
		system->rule_table_revision++;
	}
	system->rule_revision_counter++;
	destroy_rule_internal(rule);
}

unsigned get_rule_flags(const Rule *rule)
{
	return rule->flags;
}

/* A rule has been changed in some way. Nodes using the rule must update their
 * styles. */
static void rule_revised(Rule *rule)
{
	rule->revision++;
	rule_get_system(rule)->rule_revision_counter++;
}

/* Sets a mask of rule flags to true or false and marks any tables containing
 * the rule as changed. */
void set_rule_flags(Rule *rule, unsigned mask, bool value)
{
	unsigned new_flags = set_or_clear(rule->flags, mask, value);
	if (new_flags != rule->flags) {
		rule->flags = (unsigned char)new_flags;
		rule_revised(rule);
	}
}

int set_integer_attribute(Rule *rule, int name, 
	ValueSemantic vs, int value)
{
	int rc = abuf_set_integer(&rule->attributes, name, vs, value);
	if (rc == 1)
		rule_revised(rule);
	return rc;
}

int set_float_attribute(Rule *rule, int name, 
	ValueSemantic vs, float value)
{
	int rc = abuf_set_float(&rule->attributes, name, vs, value);
	if (rc == 1)
		rule_revised(rule);
	return rc;
}

int set_string_attribute(Rule *rule, int name, 
	ValueSemantic vs, const char *value, int length)
{
	int rc = abuf_set_string(&rule->attributes, name, vs, value, length);
	if (rc < 0)
		return rc;
	if (name == TOKEN_CLASS && (rule->flags & RFLAG_MODIFIES_CLASS) == 0) {
		rule->flags |= RFLAG_MODIFIES_CLASS;
		rc = 1;
	}
	if (rc == 1)
		rule_revised(rule);
	return rc;
}

/* Builds an array of rule keys representing all selectors a node can match. */
unsigned make_node_rule_keys(const System *system, int node_token, 
	unsigned node_flags, const char *cls, unsigned cls_length, 
	uint64_t *keys, unsigned max_keys)
{
	/* Hash each class name. */
	uint64_t class_names[MAX_RULE_CLASSES + 1];
	unsigned num_classes = 0;
	if (cls != NULL) {
		unsigned part_length;
		const char *end = cls + cls_length;
		for (const char *part = cls; part != end; part += part_length + 1) {
			if (num_classes == MAX_RULE_CLASSES)
				break;
			part_length = strlen(part);
			class_names[num_classes++] = murmur3_64(part, part_length);
		}
	}

	/* Append a pseudo-class based on the current interation state. */
	if ((node_flags & NFLAG_INTERACTION_HIGHLIGHTED) != 0)
		class_names[num_classes++] = system->rule_name_highlighted;
	else if ((node_flags & NFLAG_INTERACTION_ACTIVE) != 0)
		class_names[num_classes++] = system->rule_name_active;

	/* Order the classes and pseudo-classes by their hashed names. */
	std::sort(class_names, class_names + num_classes);

	/* Output keys that match "*.<classes>" and "<tag>.<clasess>" selectors for 
	 * each subset in the power set of the class names. */
	uint64_t hashbuf[MAX_RULE_CLASSES + 2];
	unsigned num_combinations = 1 << num_classes;
	unsigned num_keys = 0;
	for (unsigned n = 0; n < num_combinations && num_keys != max_keys; ++n) {
		/* Build the n-th combination. */
		unsigned count = 1;
		for (unsigned j = 0; j < num_classes; ++j) {
			hashbuf[count] = class_names[j];
			count += (n >> j) & 1;	
		}

		/* Make a key for *.<classes>. */
		hashbuf[0] = system->rule_name_all;
		keys[num_keys++] = murmur3_64(hashbuf, count * sizeof(uint64_t));

		/* Make a key for <tag>.<classes>. */
		if (node_token != TOKEN_INVALID && num_keys != max_keys) {
			hashbuf[0] = system->token_rule_names[
				node_token - TOKEN_KEYWORD_FIRST];
			keys[num_keys++] = murmur3_64(hashbuf, count * sizeof(uint64_t));
		}
	}

	return num_keys;
}

int node_matches_selector(const Document *document, const Node *node, 
	const ParsedSelector *ps)
{
	document;
	/* For each clause, walk up the parent chain from 'node'. The clause matches
	 * if its key at each level is found in the rule key buffer of the 
	 * corresponding node. */
	unsigned offset = 0, i, j;
	for (i = 0; i < ps->num_clauses; ++i) {
		const Node *n = node;
		bool clause_match = true;
		for (unsigned depth = 0; depth < ps->keys_per_clause[i]; ++depth) {
			if (n == NULL) {
				clause_match = false;
				break;
			}
			uint64_t key = ps->keys[offset + depth];
			for (j = 0; j < n->num_rule_keys; ++j)
				if (key == make_rule_lookup_key(n->rule_keys[j], depth))
					break;
			if (j == n->num_rule_keys) {
				clause_match = false;
				break;
			}
			n = n->parent;
		}
		if (clause_match)
			break;
		offset += ps->keys_per_clause[i];
	}
	return int(i != ps->num_clauses);
}

/* Returns 1 if a node matches the supplied rule selector, 0 if it doesn't,
 * or a negative number if there's an error parsing the selector. */
int node_matches_selector(const Document *document, const Node *node, 
	const char *selector, int length)
{
	ParsedSelector ps;
	int rc = parse_selector(&ps, selector, length);
	if (rc < 0)
		return rc;
	return node_matches_selector(document, node, &ps);
}

/* Recursively matches nodes against a selector. */
int match_nodes(const Document *document, const Node *root, 
	const ParsedSelector *ps, const Node **matched_nodes, 
	unsigned max_matched, int max_depth)
{
	if (root == NULL)
		root = document->root;

	int depth = 0;
	unsigned num_matched = 0;
	for (const Node *node = root; node != NULL; ) {
		int rc = node_matches_selector(document, node, ps);
		if (rc == 1) {
			if (num_matched != max_matched)
				matched_nodes[num_matched] = node;
			num_matched++;
		}
		node = ((unsigned)depth < (unsigned)max_depth) ? 
			tree_next(document, root, node) : 
			tree_next_up(document, root, node);
	}
	return (int)num_matched;
}

/* Recursively matches nodes against a selector. Returns the number of nodes
 * matched, or a negative error code if the selector is not well formed. */
int match_nodes(const Document *document, const Node *root, 
	const char *selector, int selector_length, 
	const Node **matched_nodes, unsigned max_matched,
	int max_depth)
{
	ParsedSelector ps;
	int rc = parse_selector(&ps, selector, selector_length);
	if (rc < 0)
		return rc;
	return match_nodes(document, root, &ps, matched_nodes, max_matched, max_depth);
}

/* Updates the array of matched rules for a node by looking up its rule keys
 * in global and local rule tables. */
unsigned match_rules(Document *document, Node *node, 
	const Rule **matched, unsigned max_rules,
	const RuleTable *local_table, 
	const RuleTable *global_table)
{
	static const unsigned MAX_MATCH_KEYS = 256;
	static const unsigned LEVEL_MAX = 32;

	document;

	/* Starting at the node, walk up the parent chain, refining the set of
	 * matched selectors at each step. */
	const Selector *buffers[3][LEVEL_MAX];
	const Selector **a = buffers[0], **b = buffers[1], **c = buffers[2];
	const Rule *matched_set[LEVEL_MAX];
	unsigned len_a = 0, len_b = 0;
	unsigned match_count = 0;
	unsigned depth = 0;
	Node *n = node;
	do {
		/* Look up each of the node's keys in the table, appending the 
		 * discovered selectors to the level's match buffer 'A'. */
		len_a = 0;
		for (unsigned i = 0; i < (unsigned)n->num_rule_keys; ++i) {
			uint64_t key = make_rule_lookup_key(n->rule_keys[i], depth);
			typedef RuleTable::const_iterator I;
			std::pair<I, I> range;
			I iter;
			if (local_table != NULL) {
				range = local_table->equal_range(key);
				for (iter = range.first; iter != range.second && 
					len_a != LEVEL_MAX; ++iter) {
					a[len_a++] = iter->second;
				}
			}
			if (global_table != NULL) {
				range = global_table->equal_range(key);
				for (iter = range.first; iter != range.second && 
					len_a != LEVEL_MAX; ++iter) {
						a[len_a++] = iter->second;
				}
			}
			if (len_a == LEVEL_MAX)
				break;
		}

		/* Eliminate duplicates in A. */
		std::sort(a, a + len_a);
		std::unique(a, a + len_a);

		/* Intersect set A with the working result B. */
		if (depth != 0) {
			std::sort(b, b + len_b);
			len_b = std::set_intersection(a, a + len_a, b, b + len_b, c) - c;
			std::swap(b, c);
		} else {
			std::swap(a, b);
			len_b = len_a;
		}

		/* Move any selectors that have fully matched from B to the result 
		 * list. This invalidates the order in B.*/
		for (unsigned i = 0; i < len_b && match_count != MAX_MATCH_KEYS; ++i) {
			if (b[i]->num_keys == depth + 1) {
				matched_set[match_count++] = b[i]->rule;
				b[i--] = b[--len_b];
			}	
		}
		
		/* Move up the tree. */
		n = n->parent;
		++depth;
	} while (n != NULL && depth != MAX_SELECTOR_DEPTH && 
		match_count != MAX_MATCH_KEYS && len_b != 0);
		
	/* Copy the matched rules to the output buffer, most important first.
	 * Lower priority numbers indicate higher priority. */
	unsigned result_count = std::min(max_rules, match_count);
	struct {
		bool operator () (const Rule *a, const Rule *b) const 
			{ return a->priority < b->priority; }
	} priority_less;
	std::partial_sort_copy(matched_set, matched_set + match_count, matched, 
		matched + result_count, priority_less);
	return result_count;
}

void dump_rule_table(const Document *document, bool global)
{
	const RuleTable *table = global ? &document->system->global_rules : 
		&document->rules;
	RuleTable::const_iterator iter;
	dmsg("RULE TABLE %x, %u entries\n", table, table->size());
	for (iter = table->begin(); iter != table->end(); ++iter) {
		const Selector *selector = iter->second;
		const Rule *rule = selector->rule;
		dmsg("\t%llXh => selector [", iter->first);
		for (unsigned i = 0; i < selector->num_keys; ++i) {
			if (i != 0)
				dmsg(", ");
			dmsg("%llXh", rule->keys[selector->key_offset + i]);
		}
		dmsg("] for rule %Xh: num_selectors=%u total_keys=%u priority=%d\n", 
			rule, rule->num_selectors, rule->total_keys, rule->priority);
	}
	dmsg("END RULE TABLE\n");
}

} // namespace stkr