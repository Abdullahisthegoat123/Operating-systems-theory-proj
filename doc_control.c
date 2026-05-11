#include "doc_control.h"

void create_shared_doc_if_not_exists(void) {
    FILE *file = fopen(SHARED_DOC, "a+");
    if (file == NULL) {
        perror("Error creating shared document");
        exit(EXIT_FAILURE);
    }
    fclose(file);
    printf("Shared document verified or created.\n");
}

void initialize_control_file(void) {
    FILE *file = fopen(CONTROL_FILE, "r");

    if (file == NULL) {
        file = fopen(CONTROL_FILE, "w");
        if (file == NULL) {
            perror("Error creating control file");
            exit(EXIT_FAILURE);
        }

        fprintf(file, "%s\n", SHARED_DOC);
        fprintf(file, "admin %d %d %d\n", PRIORITY_OWNER, ACCESS_BOTH, (int)getpid());
        fprintf(file, "0\n");

        fclose(file);
        printf("Control file initialized with admin user.\n");
    } else {
        fclose(file);

        User users[MAX_USERS];
        int user_count = read_control_file(users, MAX_USERS);
        users[0].pid = getpid();
        write_control_file(users, user_count);
    }
}

int read_control_file(User users[], int max_users) {
    FILE *file = fopen(CONTROL_FILE, "r");
    if (file == NULL) {
        perror("Error opening control file");
        exit(EXIT_FAILURE);
    }

    char line[MAX_LINE];
    if (!fgets(line, MAX_LINE, file)) {
        fclose(file);
        fprintf(stderr, "Error: control file is empty or unreadable.\n");
        exit(EXIT_FAILURE);
    }

    if (!fgets(line, MAX_LINE, file)) {
        fclose(file);
        fprintf(stderr, "Error: control file missing admin line.\n");
        exit(EXIT_FAILURE);
    }
    sscanf(line, "%s %d %d %d", users[0].name, &users[0].priority, &users[0].access_type, &users[0].pid);

    users[0].priority = PRIORITY_OWNER;

    int user_count;
    if (!fgets(line, MAX_LINE, file)) {
        fclose(file);
        fprintf(stderr, "Error: control file missing user count.\n");
        exit(EXIT_FAILURE);
    }
    sscanf(line, "%d", &user_count);

    if (user_count > max_users - 1) {
        user_count = max_users - 1;
    }

    for (int i = 0; i < user_count; i++) {
        if (fgets(line, MAX_LINE, file) != NULL) {
            sscanf(line, "%s %d %d %d", users[i + 1].name, &users[i + 1].priority,
                   &users[i + 1].access_type, &users[i + 1].pid);
        }
    }

    fclose(file);
    return user_count + 1;
}

void write_control_file(User users[], int user_count) {
    FILE *file = fopen(CONTROL_FILE, "w");
    if (file == NULL) {
        perror("Error opening control file for writing");
        exit(EXIT_FAILURE);
    }

    fprintf(file, "%s\n", SHARED_DOC);

    users[0].priority = PRIORITY_OWNER;
    fprintf(file, "%s %d %d %d\n", users[0].name, users[0].priority, users[0].access_type, users[0].pid);

    fprintf(file, "%d\n", user_count - 1);

    for (int i = 1; i < user_count; i++) {
        fprintf(file, "%s %d %d %d\n", users[i].name, users[i].priority, users[i].access_type, users[i].pid);
    }

    fclose(file);
}

bool process_exists(pid_t pid) {
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) {
        return true;
    }
    return false;
}
