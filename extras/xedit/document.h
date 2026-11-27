// SPDX-License-Identifier: MIT
#ifndef XEDIT_DOCUMENT_H
#define XEDIT_DOCUMENT_H

#include <stddef.h>
#include <sys/types.h>

#define DOC_PAGE 4096

/* Roots are immutable snapshots. Height zero is empty, height one is a
 * span of the original or additions file. Taller roots refer to disk nodes. */
struct root {
    off_t size, id;
    unsigned height;
};

int doc_open(const char *name, struct root *root);
/* An empty, untitled document: doc_path() is NULL until doc_save_as. */
int doc_new(struct root *root);
void doc_close(void);
const char *doc_path(void);
int doc_exists(void);
int doc_read(struct root root, off_t pos, void *buf, size_t len);
int doc_edit(struct root *root, off_t pos, off_t remove, const void *data, size_t len);
int doc_save(struct root root);
int doc_save_as(struct root root, const char *name, int overwrite);

#endif
