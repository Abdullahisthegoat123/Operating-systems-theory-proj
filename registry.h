#ifndef REGISTRY_H
#define REGISTRY_H

#include "shared.h"

enum {
    REG_OK = 0,
    REG_ERR_EXISTS = 1,
    REG_ERR_FULL = 2,
    REG_ERR_NOTFOUND = 3,
    REG_ERR_ADMIN = 4,
    REG_ERR_INVALID = 5
};

int registry_add_user(const char *name, int priority, int access_type);
int registry_remove_user(const char *name);
int registry_update_user(const char *name, int priority, int access_type);

#endif
