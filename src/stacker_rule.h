#pragma once

#include <cstdint>
#include <unordered_map>

#include "stacker.h"
#include "stacker_attribute_buffer.h"

namespace stkr {

struct System;
struct Document;
struct Node;

const unsigned MAX_RULE_CLASSES   = 4;
const unsigned MAX_NODE_RULE_KEYS = 256;

struct Selector {
	struct Rule *rule;
	unsigned short key_offset;
	unsigned short num_keys;
};

struct Rule {
	Selector *selectors;
	uint64_t *keys;
	unsigned short total_keys;
	unsigned char num_selectors;
	unsigned char flags;
	int priority;
	unsigned revision;
	union {
		System *system;
		Document *document;
	};
	AttributeBuffer attributes;
};

typedef std::unordered_multimap<uint64_t, Selector *, 
	std::identity<uint64_t>> RuleTable;

int add_rule_from_attributes(
	Rule **result, 
	System *system, 
	Document *document, 
	const AttributeAssignment *attributes, 
	unsigned num_attributes,
	unsigned flags = RFLAG_ENABLED, 
	int priority = 0);
void clear_rule_table(RuleTable *table);
unsigned make_node_rule_keys(const System *system, 
	int node_token, unsigned node_flags, const char *cls, 
	unsigned cls_length, uint64_t *keys, unsigned max_keys);
unsigned match_rules(Document *document, Node *node, 
	const Rule **matched, unsigned max_rules,
	const RuleTable *local_table = NULL, 
	const RuleTable *global_table = NULL);

} // namespace stkr

