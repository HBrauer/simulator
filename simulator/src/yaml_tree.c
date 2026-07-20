#include "yaml_tree.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yaml.h>

struct yaml_tree_node {
    yaml_tree_kind_t kind;
    char *scalar;                  /* SCALAR only */
    bool quoted;                   /* SCALAR only: single/double quoted in the source */
    char **keys;                   /* MAPPING only, parallel to children */
    yaml_tree_node_t **children;   /* SEQUENCE and MAPPING */
    size_t count;
    size_t capacity;
};

static yaml_tree_node_t *node_new(yaml_tree_kind_t kind)
{
    yaml_tree_node_t *node = calloc(1, sizeof(*node));
    if (node != NULL) {
        node->kind = kind;
    }
    return node;
}

/* Grow the children (and, for mappings, keys) arrays so one more entry fits. */
static bool node_reserve(yaml_tree_node_t *node)
{
    if (node->count < node->capacity) {
        return true;
    }
    const size_t new_capacity = node->capacity == 0 ? 4 : node->capacity * 2;
    yaml_tree_node_t **children = realloc(node->children, new_capacity * sizeof(*children));
    if (children == NULL) {
        return false;
    }
    node->children = children;
    if (node->kind == YAML_TREE_MAPPING) {
        char **keys = realloc(node->keys, new_capacity * sizeof(*keys));
        if (keys == NULL) {
            return false;
        }
        node->keys = keys;
    }
    node->capacity = new_capacity;
    return true;
}

void yaml_tree_free(yaml_tree_node_t *node)
{
    if (node == NULL) {
        return;
    }
    free(node->scalar);
    for (size_t i = 0; i < node->count; i++) {
        if (node->keys != NULL) {
            free(node->keys[i]);
        }
        yaml_tree_free(node->children[i]);
    }
    free(node->keys);
    free(node->children);
    free(node);
}

static char *dup_scalar(const yaml_event_t *event)
{
    const char *value = (const char *)event->data.scalar.value;
    const size_t length = event->data.scalar.length;
    char *out = malloc(length + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, value, length);
    out[length] = '\0';
    return out;
}

static bool event_is_quoted(const yaml_event_t *event)
{
    return event->data.scalar.style == YAML_SINGLE_QUOTED_SCALAR_STYLE ||
           event->data.scalar.style == YAML_DOUBLE_QUOTED_SCALAR_STYLE;
}

static void set_parse_error(char *error, size_t error_size, yaml_parser_t *parser)
{
    snprintf(error, error_size, "scenario_invalid:%s", parser->problem != NULL ? parser->problem : "parse_error");
}

/* Build the node whose opening event is already in `event`. For sequences and mappings this
 * consumes further events through the matching END. Returns NULL (and sets `error`) on failure. */
static yaml_tree_node_t *build_from_event(yaml_parser_t *parser, yaml_event_t *event, char *error, size_t error_size)
{
    switch (event->type) {
    case YAML_SCALAR_EVENT: {
        yaml_tree_node_t *node = node_new(YAML_TREE_SCALAR);
        if (node == NULL) {
            snprintf(error, error_size, "scenario_invalid:out_of_memory");
            return NULL;
        }
        node->scalar = dup_scalar(event);
        node->quoted = event_is_quoted(event);
        if (node->scalar == NULL) {
            yaml_tree_free(node);
            snprintf(error, error_size, "scenario_invalid:out_of_memory");
            return NULL;
        }
        return node;
    }
    case YAML_SEQUENCE_START_EVENT: {
        yaml_tree_node_t *seq = node_new(YAML_TREE_SEQUENCE);
        if (seq == NULL) {
            snprintf(error, error_size, "scenario_invalid:out_of_memory");
            return NULL;
        }
        for (;;) {
            yaml_event_t child;
            if (!yaml_parser_parse(parser, &child)) {
                set_parse_error(error, error_size, parser);
                yaml_tree_free(seq);
                return NULL;
            }
            if (child.type == YAML_SEQUENCE_END_EVENT) {
                yaml_event_delete(&child);
                break;
            }
            yaml_tree_node_t *item = build_from_event(parser, &child, error, error_size);
            yaml_event_delete(&child);
            if (item == NULL || !node_reserve(seq)) {
                yaml_tree_free(item);
                yaml_tree_free(seq);
                if (item != NULL) {
                    snprintf(error, error_size, "scenario_invalid:out_of_memory");
                }
                return NULL;
            }
            seq->children[seq->count++] = item;
        }
        return seq;
    }
    case YAML_MAPPING_START_EVENT: {
        yaml_tree_node_t *map = node_new(YAML_TREE_MAPPING);
        if (map == NULL) {
            snprintf(error, error_size, "scenario_invalid:out_of_memory");
            return NULL;
        }
        for (;;) {
            yaml_event_t key_event;
            if (!yaml_parser_parse(parser, &key_event)) {
                set_parse_error(error, error_size, parser);
                yaml_tree_free(map);
                return NULL;
            }
            if (key_event.type == YAML_MAPPING_END_EVENT) {
                yaml_event_delete(&key_event);
                break;
            }
            if (key_event.type != YAML_SCALAR_EVENT) {
                yaml_event_delete(&key_event);
                yaml_tree_free(map);
                snprintf(error, error_size, "scenario_invalid:non_scalar_key");
                return NULL;
            }
            char *key = dup_scalar(&key_event);
            yaml_event_delete(&key_event);
            if (key == NULL) {
                yaml_tree_free(map);
                snprintf(error, error_size, "scenario_invalid:out_of_memory");
                return NULL;
            }
            yaml_event_t value_event;
            if (!yaml_parser_parse(parser, &value_event)) {
                set_parse_error(error, error_size, parser);
                free(key);
                yaml_tree_free(map);
                return NULL;
            }
            yaml_tree_node_t *value = build_from_event(parser, &value_event, error, error_size);
            yaml_event_delete(&value_event);
            if (value == NULL || !node_reserve(map)) {
                free(key);
                yaml_tree_free(value);
                yaml_tree_free(map);
                if (value != NULL) {
                    snprintf(error, error_size, "scenario_invalid:out_of_memory");
                }
                return NULL;
            }
            map->keys[map->count] = key;
            map->children[map->count] = value;
            map->count++;
        }
        return map;
    }
    default:
        snprintf(error, error_size, "scenario_invalid:unsupported_yaml");
        return NULL;
    }
}

yaml_tree_node_t *yaml_tree_load_file(const char *path, char *error, size_t error_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "scenario_invalid:cannot_open_file");
        return NULL;
    }
    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(file);
        snprintf(error, error_size, "scenario_invalid:yaml_init_failed");
        return NULL;
    }
    yaml_parser_set_input_file(&parser, file);

    yaml_tree_node_t *root = NULL;
    bool done = false;
    bool failed = false;
    while (!done) {
        yaml_event_t event;
        if (!yaml_parser_parse(&parser, &event)) {
            set_parse_error(error, error_size, &parser);
            failed = true;
            break;
        }
        switch (event.type) {
        case YAML_STREAM_START_EVENT:
        case YAML_DOCUMENT_START_EVENT:
            yaml_event_delete(&event);
            break;
        case YAML_DOCUMENT_END_EVENT:
        case YAML_STREAM_END_EVENT:
            yaml_event_delete(&event);
            done = true;
            break;
        default:
            /* First content event is the document root. */
            root = build_from_event(&parser, &event, error, error_size);
            yaml_event_delete(&event);
            failed = root == NULL;
            done = true;
            break;
        }
    }

    yaml_parser_delete(&parser);
    fclose(file);
    if (failed) {
        yaml_tree_free(root);
        return NULL;
    }
    if (root == NULL) {
        snprintf(error, error_size, "scenario_invalid:empty_document");
        return NULL;
    }
    return root;
}

yaml_tree_kind_t yaml_tree_kind(const yaml_tree_node_t *node)
{
    return node->kind;
}

bool yaml_tree_is_scalar(const yaml_tree_node_t *node)
{
    return node != NULL && node->kind == YAML_TREE_SCALAR;
}

const yaml_tree_node_t *yaml_tree_get(const yaml_tree_node_t *node, const char *key)
{
    if (node == NULL || node->kind != YAML_TREE_MAPPING) {
        return NULL;
    }
    for (size_t i = 0; i < node->count; i++) {
        if (strcmp(node->keys[i], key) == 0) {
            return node->children[i];
        }
    }
    return NULL;
}

size_t yaml_tree_count(const yaml_tree_node_t *node)
{
    if (node == NULL || node->kind != YAML_TREE_SEQUENCE) {
        return 0;
    }
    return node->count;
}

const yaml_tree_node_t *yaml_tree_at(const yaml_tree_node_t *node, size_t index)
{
    if (node == NULL || node->kind != YAML_TREE_SEQUENCE || index >= node->count) {
        return NULL;
    }
    return node->children[index];
}

static const char *scalar_text(const yaml_tree_node_t *node)
{
    if (node == NULL || node->kind != YAML_TREE_SCALAR) {
        return NULL;
    }
    return node->scalar;
}

bool yaml_tree_get_string(const yaml_tree_node_t *node, const char *key, char *dst, size_t dst_size)
{
    const char *value = scalar_text(yaml_tree_get(node, key));
    if (value == NULL) {
        return false;
    }
    snprintf(dst, dst_size, "%s", value);
    return true;
}

bool yaml_tree_get_u64(const yaml_tree_node_t *node, const char *key, uint64_t *out)
{
    const char *value = scalar_text(yaml_tree_get(node, key));
    if (value == NULL || !isdigit((unsigned char)value[0])) {
        return false; /* rejects empty, sign, and leading whitespace */
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = (uint64_t)parsed;
    return true;
}

bool yaml_tree_get_u32(const yaml_tree_node_t *node, const char *key, uint32_t *out)
{
    uint64_t value = 0;
    if (!yaml_tree_get_u64(node, key, &value) || value > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

bool yaml_tree_get_i64(const yaml_tree_node_t *node, const char *key, int64_t *out)
{
    const char *value = scalar_text(yaml_tree_get(node, key));
    if (value == NULL || value[0] == '\0' || isspace((unsigned char)value[0])) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const long long parsed = strtoll(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return false;
    }
    *out = (int64_t)parsed;
    return true;
}

bool yaml_tree_get_double(const yaml_tree_node_t *node, const char *key, double *out)
{
    const char *value = scalar_text(yaml_tree_get(node, key));
    if (value == NULL || value[0] == '\0' || isspace((unsigned char)value[0])) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const double parsed = strtod(value, &end);
    if (end == value || *end != '\0') {
        return false;
    }
    *out = parsed;
    return true;
}

static bool matches_token(const yaml_tree_node_t *node, const char *lower, const char *upper)
{
    if (node == NULL || node->kind != YAML_TREE_SCALAR || node->quoted) {
        return false;
    }
    return strcmp(node->scalar, lower) == 0 || strcmp(node->scalar, upper) == 0;
}

bool yaml_tree_is_true(const yaml_tree_node_t *node)
{
    return matches_token(node, "true", "True") ||
           (node != NULL && !node->quoted && node->kind == YAML_TREE_SCALAR && strcmp(node->scalar, "TRUE") == 0);
}

bool yaml_tree_is_bool(const yaml_tree_node_t *node)
{
    return yaml_tree_is_true(node) || matches_token(node, "false", "False") ||
           (node != NULL && !node->quoted && node->kind == YAML_TREE_SCALAR && strcmp(node->scalar, "FALSE") == 0);
}
