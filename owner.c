#include "shared.h"
#include "doc_control.h"
#include "registry.h"

void display_menu(void);
void view_document(User *user);
void edit_document(User *user);
void add_user(void);
void remove_user(void);
void update_user(void);
void list_users(void);

User owner_user;

static void send_priority_signal(pid_t pid) {
    if (process_exists(pid)) {
        printf("Sending priority signal to process %d\n", (int)pid);
        kill(pid, PRIORITY_SIGNAL);
    }
}

int main(void) {
    int choice;

    create_shared_doc_if_not_exists();
    initialize_control_file();
    initialize_synchronization(true);

    strcpy(owner_user.name, "admin");
    owner_user.priority = PRIORITY_OWNER;
    owner_user.access_type = ACCESS_BOTH;
    owner_user.pid = getpid();

    printf("Owner process started with PID: %d\n", (int)getpid());

    while (1) {
        display_menu();
        if (scanf("%d", &choice) != 1) {
            choice = -1;
        }
        getchar();

        switch (choice) {
            case 1:
                view_document(&owner_user);
                break;
            case 2:
                edit_document(&owner_user);
                break;
            case 3:
                add_user();
                break;
            case 4:
                remove_user();
                break;
            case 5:
                update_user();
                break;
            case 6:
                list_users();
                break;
            case 7:
                append_to_history();
                break;
            case 8:
                pop_last_snapshot();
                break;
            case 9:
                print_history();
                break;
            case 10:
                printf("Exiting owner program.\n");
                cleanup_synchronization(true);
                exit(0);
            default:
                printf("Invalid choice. Please try again.\n");
        }
    }

    return 0;
}

void display_menu(void) {
    printf("\n=== Document Sharing System (Owner/Admin) ===\n");
    printf("1. View document (read)\n");
    printf("2. Edit document (write)\n");
    printf("3. Add user\n");
    printf("4. Remove user\n");
    printf("5. Update user access\n");
    printf("6. List all users\n");
    printf("7. Push History\n");
    printf("8. POP History\n");
    printf("9. View History Log\n");
    printf("10. Exit\n");
    printf("Enter your choice: ");
}

void view_document(User *user) {
    lock_info->owner_waiting = true;

    int fd = open(SHARED_DOC, O_RDONLY);
    if (fd == -1) {
        perror("Error opening document for reading");
        lock_info->owner_waiting = false;
        return;
    }

    if (lock_info->lock_type == 2 && lock_info->holding_pid > 0) {
        printf("Document is currently locked by process %d, sending priority signal\n",
               (int)lock_info->holding_pid);
        send_priority_signal(lock_info->holding_pid);
        sleep(1);
    }

    if (!acquire_read_lock(fd, user)) {
        close(fd);
        lock_info->owner_waiting = false;
        return;
    }

    lock_info->owner_waiting = false;

    printf("\n--- Document Content ---\n");
    char buffer[1024];
    ssize_t bytes_read;

    while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        printf("%s", buffer);
    }

    printf("\n--- End of Document ---\n");

    release_read_lock(fd, user);
    close(fd);
}

void edit_document(User *user) {
    lock_info->owner_waiting = true;
    lock_info->forced_lock = true;

    int fd = open(SHARED_DOC, O_RDWR);
    if (fd == -1) {
        perror("Error opening document for editing");
        lock_info->owner_waiting = false;
        lock_info->forced_lock = false;
        return;
    }

    if ((lock_info->lock_type == 1 || lock_info->lock_type == 2) && lock_info->holding_pid > 0) {
        printf("Document is currently locked by process %d\n", (int)lock_info->holding_pid);

        if (lock_info->time_limit_active) {
            time_t current_time = time(NULL);
            int elapsed_time = (int)(current_time - lock_info->edit_start_time);
            int remaining_time = lock_info->time_allocation - elapsed_time;

            if (remaining_time > 5) {
                printf("Current user has %d seconds remaining in their time allocation.\n", remaining_time);
                printf("Starting 5-second countdown for owner priority access...\n");
            } else {
                printf("Current user's time is almost up (%d seconds left). Waiting briefly...\n",
                       remaining_time);
                sleep(remaining_time > 0 ? (unsigned)remaining_time : 1U);
                printf("Proceeding to take over document...\n");
            }
        } else {
            printf("Starting 5-second countdown for owner priority access...\n");
        }

        lock_info->countdown_active = true;
        lock_info->countdown_value = 5;

        for (int i = 5; i >= 0; i--) {
            lock_info->countdown_value = i;
            printf("Owner taking over in %d seconds...\n", i);

            if (lock_info->editor_pid > 0) {
                if (i == 0) {
                    printf("Forcing editor to close and taking control...\n");
                    kill(lock_info->editor_pid, SIGTERM);
                } else if (i <= 2) {
                    printf("Sending save signal to editor...\n");
                    kill(lock_info->editor_pid, SIGUSR1);
                }
            }

            sleep(1);
        }

        lock_info->countdown_active = false;

        sem_wait(access_sem);
        lock_info->holding_pid = 0;
        lock_info->lock_type = 0;
        sem_post(access_sem);

        sleep(1);
    }

    if (!acquire_write_lock(fd, user)) {
        close(fd);
        lock_info->owner_waiting = false;
        lock_info->forced_lock = false;
        return;
    }

    lock_info->owner_waiting = false;

    int time_allocation = 30;
    lock_info->edit_start_time = time(NULL);
    lock_info->time_allocation = time_allocation;
    lock_info->time_limit_active = true;

    printf("Opening editor for owner (Time allocation: %d seconds)...\n", time_allocation);

    pid_t pid = fork();

    if (pid == 0) {
        execlp("nano", "nano", "-B", SHARED_DOC, NULL);
        perror("Failed to open editor");
        exit(EXIT_FAILURE);
    } else if (pid > 0) {
        lock_info->editor_pid = pid;

        int status;
        pid_t result;
        time_t start_time = time(NULL);
        int elapsed_time = 0;
        int time_remaining = time_allocation;

        while ((result = waitpid(pid, &status, WNOHANG)) == 0) {
            elapsed_time = (int)(time(NULL) - start_time);
            time_remaining = time_allocation - elapsed_time;

            if (time_remaining <= 0) {
                printf("\n[!] Time allocation (%d seconds) has expired.\n", time_allocation);
                printf("Saving and closing editor...\n");
                sleep(1);
                kill(pid, SIGTERM);
                waitpid(pid, &status, 0);
                break;
            }

            usleep(100000);
        }

        lock_info->editor_pid = 0;
        lock_info->time_limit_active = false;

        if (result > 0 && time_remaining > 0) {
            printf("\nDocument editing completed by owner.\n");
        } else {
            printf("\nEditor closed due to time limit expiration.\n");
        }
    } else {
        perror("Fork failed");
    }

    release_write_lock(fd, user);
    lock_info->forced_lock = false;
    close(fd);
}

void add_user(void) {
    char name[50];
    int priority;
    int access_type;

    printf("Enter new user name: ");
    if (scanf("%49s", name) != 1) {
        return;
    }
    getchar();

    printf("Enter priority (0 for high, 1 for low): ");
    if (scanf("%d", &priority) != 1) {
        return;
    }
    getchar();

    if (priority != PRIORITY_HIGH && priority != PRIORITY_LOW) {
        printf("Invalid priority. Setting to default (low priority).\n");
        priority = PRIORITY_LOW;
    }

    printf("Enter access type (1: read-only, 2: write-only, 3: both): ");
    if (scanf("%d", &access_type) != 1) {
        return;
    }
    getchar();

    if (access_type < ACCESS_READ_ONLY || access_type > ACCESS_BOTH) {
        printf("Invalid access type. Setting to default (read-only).\n");
        access_type = ACCESS_READ_ONLY;
    }

    int rc = registry_add_user(name, priority, access_type);
    if (rc == REG_OK) {
        printf("User '%s' added successfully.\n", name);
    } else if (rc == REG_ERR_EXISTS) {
        printf("User '%s' already exists.\n", name);
    } else if (rc == REG_ERR_FULL) {
        printf("Maximum number of users reached.\n");
    } else {
        printf("Could not add user (error %d).\n", rc);
    }
}

void remove_user(void) {
    char name[50];
    printf("Enter user name to remove: ");
    if (scanf("%49s", name) != 1) {
        return;
    }
    getchar();

    int rc = registry_remove_user(name);
    if (rc == REG_OK) {
        printf("User '%s' removed successfully.\n", name);
    } else if (rc == REG_ERR_ADMIN) {
        printf("Cannot remove admin user.\n");
    } else if (rc == REG_ERR_NOTFOUND) {
        printf("User '%s' not found.\n", name);
    } else {
        printf("Remove failed (error %d).\n", rc);
    }
}

void update_user(void) {
    char name[50];
    printf("Enter user name to update: ");
    if (scanf("%49s", name) != 1) {
        return;
    }
    getchar();

    if (strcmp(name, "admin") == 0) {
        printf("Cannot modify admin user's properties.\n");
        return;
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
        printf("User '%s' not found.\n", name);
        return;
    }

    printf("Current priority: %d, access type: %d\n", users[found].priority, users[found].access_type);

    int new_pri, new_acc;
    printf("Enter new priority (0 for high, 1 for low): ");
    if (scanf("%d", &new_pri) != 1) {
        return;
    }
    getchar();

    printf("Enter new access type (1: read-only, 2: write-only, 3: both): ");
    if (scanf("%d", &new_acc) != 1) {
        return;
    }
    getchar();

    pid_t prev_pid = users[found].pid;
    int rc = registry_update_user(name, new_pri, new_acc);
    if (rc == REG_OK) {
        if (process_exists(prev_pid)) {
            printf("User '%s' is currently running (PID: %d). Changes apply on next login.\n", name,
                   (int)prev_pid);
        }
        printf("User '%s' updated successfully.\n", name);
    } else if (rc == REG_ERR_ADMIN) {
        printf("Cannot modify admin user's properties.\n");
    } else if (rc == REG_ERR_NOTFOUND) {
        printf("User '%s' not found.\n", name);
    } else {
        printf("Update failed (error %d).\n", rc);
    }
}

void list_users(void) {
    User users[MAX_USERS];
    int user_count = read_control_file(users, MAX_USERS);

    printf("\n--- User List ---\n");
    printf("%-20s %-10s %-15s %-10s %-10s\n", "Name", "Priority", "Access Type", "PID", "Status");
    printf("----------------------------------------------------------------\n");

    for (int i = 0; i < user_count; i++) {
        const char *priority;
        if (users[i].priority == PRIORITY_OWNER)
            priority = "Owner";
        else if (users[i].priority == PRIORITY_HIGH)
            priority = "High";
        else
            priority = "Low";

        const char *access;
        switch (users[i].access_type) {
            case ACCESS_READ_ONLY:
                access = "Read-only";
                break;
            case ACCESS_WRITE_ONLY:
                access = "Write-only";
                break;
            case ACCESS_BOTH:
                access = "Read-Write";
                break;
            default:
                access = "Unknown";
        }

        const char *status = (users[i].pid > 0 && process_exists(users[i].pid)) ? "Active" : "Inactive";

        printf("%-20s %-10s %-15s %-10d %-10s\n", users[i].name, priority, access, (int)users[i].pid,
               status);
    }

    printf("--- End of User List ---\n");
}
