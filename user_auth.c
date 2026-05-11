#include "user_auth.h"

bool find_user(const char *name, User *user) {
    FILE *file = fopen(CONTROL_FILE, "r");
    if (file == NULL) {
        perror("Error opening control file");
        exit(EXIT_FAILURE);
    }

    char line[MAX_LINE];
    char doc_path[MAX_LINE];

    if (!fgets(doc_path, MAX_LINE, file)) {
        fclose(file);
        return false;
    }
    (void)doc_path;

    User admin;
    if (!fgets(line, MAX_LINE, file)) {
        fclose(file);
        return false;
    }
    sscanf(line, "%s %d %d %d", admin.name, &admin.priority, &admin.access_type, &admin.pid);
    admin.priority = PRIORITY_OWNER;

    if (strcmp(name, admin.name) == 0) {
        *user = admin;
        fclose(file);
        return true;
    }

    int user_count;
    if (!fgets(line, MAX_LINE, file)) {
        fclose(file);
        return false;
    }
    sscanf(line, "%d", &user_count);

    bool found = false;
    for (int i = 0; i < user_count; i++) {
        if (fgets(line, MAX_LINE, file) != NULL) {
            User current;
            sscanf(line, "%s %d %d %d", current.name, &current.priority, &current.access_type,
                   &current.pid);

            if (strcmp(name, current.name) == 0) {
                *user = current;
                found = true;
                break;
            }
        }
    }

    fclose(file);
    return found;
}
