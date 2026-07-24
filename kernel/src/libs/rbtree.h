#pragma once

#include <libs/klibc.h>

#define KRB_RED 0
#define KRB_BLACK 1

typedef struct rb_node {
    uint64_t rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} rb_node_t;

typedef struct rb_root {
    struct rb_node *rb_node;
} rb_root_t;

typedef void (*rb_augment_f)(rb_node_t *node);

#define RB_ROOT_INIT ((rb_root_t){NULL})
#define rb_parent(r) ((struct rb_node *)((r)->rb_parent_color & ~3))
#define rb_color(r) ((r)->rb_parent_color & 1)
#define rb_is_red(r) (!rb_color(r))
#define rb_is_black(r) rb_color(r)
#define rb_set_red(r)                                                          \
    do {                                                                       \
        (r)->rb_parent_color &= ~1;                                            \
    } while (0)
#define rb_set_black(r)                                                        \
    do {                                                                       \
        (r)->rb_parent_color |= 1;                                             \
    } while (0)

static inline void rb_set_parent(rb_node_t *rb, rb_node_t *p) {
    rb->rb_parent_color = (rb->rb_parent_color & 3) | (uint64_t)p;
}

static inline void rb_set_color(rb_node_t *rb, int color) {
    rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

#define rb_entry(ptr, type, member) container_of_or_null(ptr, type, member)

void rb_erase(rb_node_t *node, rb_root_t *root);
void rb_erase_augmented(rb_node_t *node, rb_root_t *root, rb_augment_f augment);
void rb_insert_color(rb_node_t *node, rb_root_t *root);
void rb_insert_augmented(rb_node_t *node, rb_root_t *root,
                         rb_augment_f augment);
rb_node_t *rb_first(const rb_root_t *root);
rb_node_t *rb_next(const rb_node_t *node);
