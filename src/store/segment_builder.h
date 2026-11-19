#ifndef STELOG_STORE_SEGMENT_BUILDER_H
#define STELOG_STORE_SEGMENT_BUILDER_H

#include "stelog.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Helper used by both builder and query/tests to recursively remove a directory */
void stelog_rmdir_recursive(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_STORE_SEGMENT_BUILDER_H */
