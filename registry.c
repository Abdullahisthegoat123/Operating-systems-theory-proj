#include "registry.h"
#include "doc_control.h"

int registry_add_user(const char *name, int priority, int access_type) {
    User users[MAX_USERS];
    int user_count = read_control_file(users, MAX_USERS);

    if (user_count >= MAX_USERS) {
        return REG_ERR_FULL;
    }

    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0) {
            return REG_ERR_EXISTS;
        }
    }

    User new_user;
    strncpy(new_user.name, name, sizeof(new_user.name) - 1);
    new_user.name[sizeof(new_user.name) - 1] = '\0';

    if (priority != PRIORITY_HIGH && priority != PRIORITY_LOW) {
        new_user.priority = PRIORITY_LOW;
    } else {
        new_user.priority = priority;
    }

    if (access_type < ACCESS_READ_ONLY || access_type > ACCESS_BOTH) {
        new_user.access_type = ACCESS_READ_ONLY;
    } else {
        new_user.access_type = access_type;
    }

    new_user.pid = 0;
    users[user_count] = new_user;
    write_control_file(users, user_count + 1);
    return REG_OK;
}

int registry_remove_user(const char *name) {
    if (strcmp(name, "admin") == 0) {
        return REG_ERR_ADMIN;
    }

    User users[MAX_USERS];
    int user_count = read_control_file(users, MAX_USERS);

    int found = -1;
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0) {
            found = i;
            break;
        }
    }

    if (found == -1) {
        return REG_ERR_NOTFOUND;
    }

    if (process_exists(users[found].pid)) {
        kill(users[found].pid, SIGTERM);
    }

    for (int i = found; i < user_count - 1; i++) {
        users[i] = users[i + 1];
    }

    write_control_file(users, user_count - 1);
    return REG_OK;
}

int registry_update_user(const char *name, int priority, int access_type) {
    if (strcmp(name, "admin") == 0) {
        return REG_ERR_ADMIN;
    }

    User users[MAX_USERS];
    int user_count = read_control_file(users, MAX_USERS);

    int found = -1;
    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i].name, name) == 0) {
            found = i;
            break;
        }
    }

    if (found == -1) {
        return REG_ERR_NOTFOUND;
    }

    if (priority != PRIORITY_HIGH && priority != PRIORITY_LOW) {
        users[found].priority = PRIORITY_LOW;
    } else {
        users[found].priority = priority;
    }

    if (access_type < ACCESS_READ_ONLY || access_type > ACCESS_BOTH) {
        users[found].access_type = ACCESS_READ_ONLY;
    } else {
        users[found].access_type = access_type;
    }

    write_control_file(users, user_count);
    return REG_OK;
}
