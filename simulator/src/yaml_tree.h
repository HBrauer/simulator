#ifndef YAML_TREE_H
#define YAML_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A minimal in-memory tree for a parsed YAML document -- just enough to navigate a document by
 * key and read typed scalars, built directly from libyaml's event stream. There is no JSON and
 * no external DOM library involved: scalars are stored as raw text and interpreted on access (as
 * string / unsigned / signed / double / bool) by whichever field reads them.
 *
 * A tree (rather than reacting to the raw event stream inline) is used because YAML mapping keys
 * are unordered, and the scenario loader reads fields by name in an order of its own choosing. */

typedef enum {
    YAML_TREE_SCALAR,
    YAML_TREE_SEQUENCE,
    YAML_TREE_MAPPING,
} yaml_tree_kind_t;

typedef struct yaml_tree_node yaml_tree_node_t;

/* Parse a YAML file into a tree. Returns a heap tree the caller frees with yaml_tree_free(), or
 * NULL on error (with a "scenario_invalid:<reason>" message written to `error`). Anchors,
 * aliases and tags are not supported. */
yaml_tree_node_t *yaml_tree_load_file(const char *path, char *error, size_t error_size);
void yaml_tree_free(yaml_tree_node_t *node);

/* Structure. yaml_tree_kind must not be called on NULL. */
yaml_tree_kind_t yaml_tree_kind(const yaml_tree_node_t *node);
bool yaml_tree_is_scalar(const yaml_tree_node_t *node); /* NULL-safe */

/* Mapping access: the value node for `key`, or NULL if absent or `node` is not a mapping. */
const yaml_tree_node_t *yaml_tree_get(const yaml_tree_node_t *node, const char *key);

/* Sequence access. yaml_tree_count returns 0 unless `node` is a sequence. */
size_t yaml_tree_count(const yaml_tree_node_t *node);
const yaml_tree_node_t *yaml_tree_at(const yaml_tree_node_t *node, size_t index);

/* Typed reads of a mapping's scalar child `key`. Return false if the key is missing, is not a
 * scalar, or does not parse cleanly as the requested type. */
bool yaml_tree_get_string(const yaml_tree_node_t *node, const char *key, char *dst, size_t dst_size);
bool yaml_tree_get_u64(const yaml_tree_node_t *node, const char *key, uint64_t *out);
bool yaml_tree_get_u32(const yaml_tree_node_t *node, const char *key, uint32_t *out);
bool yaml_tree_get_i64(const yaml_tree_node_t *node, const char *key, int64_t *out);
bool yaml_tree_get_double(const yaml_tree_node_t *node, const char *key, double *out);

/* Boolean interpretation of a scalar node (NULL-safe). A quoted scalar such as "true" is a
 * string, not a boolean, matching the usual YAML/JSON distinction. */
bool yaml_tree_is_bool(const yaml_tree_node_t *node);
bool yaml_tree_is_true(const yaml_tree_node_t *node);

#endif
