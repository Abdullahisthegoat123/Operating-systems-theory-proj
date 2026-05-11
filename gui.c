/*
 * Integrated ncurses GUI for the multi-user document editor.
 * Uses the same POSIX IPC, control file, and reader-writer locks as owner/user.
 *
 * Usage:
 *   ./gui --owner          Start administrator GUI (run first; initializes IPC)
 *   ./gui <username>      Start client GUI for a registered user
 */

#include <ncurses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#include "shared.h"
#include "doc_control.h"
#include "registry.h"
#include "user_auth.h"

#define MAX_GUI_DOC (256 * 1024)
#define OWNER_ARG "--owner"

static User g_owner_user;
static User g_current_user;
static int g_is_owner_gui;

static void gui_send_priority_signal(pid_t pid) {
    if (process_exists(pid)) {
        kill(pid, PRIORITY_SIGNAL);
    }
}

static void gui_status_msg(const char *fmt, ...) {
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    attron(A_REVERSE);
    mvprintw(LINES - 1, 0, "%-*s", COLS - 1, buf);
    attroff(A_REVERSE);
    refresh();
}

static void gui_draw_owner_menu(void) {
    clear();
    attron(A_BOLD);
    mvprintw(0, 0, "Multi-User Document Editor — Owner / Administrator");
    attroff(A_BOLD);
    mvprintw(2, 0, "1) View document   2) Edit (nano)   3) Add user   4) Remove user");
    mvprintw(3, 0, "5) Update user     6) List users     7) Push history   8) Pop history");
    mvprintw(4, 0, "9) View history    0) Exit");
    if (lock_info) {
        mvprintw(6, 0, "Lock: type=%d holder_pid=%d readers=%d owner_wait=%d countdown=%d",
                 lock_info->lock_type, (int)lock_info->holding_pid, lock_info->reader_count,
                 lock_info->owner_waiting ? 1 : 0, lock_info->countdown_value);
    }
    mvprintw(LINES - 2, 0, "Choose option (0-9), then Enter.");
    refresh();
}

static void gui_draw_user_menu(User *u) {
    clear();
    attron(A_BOLD);
    mvprintw(0, 0, "Document access — %s", u->name);
    attroff(A_BOLD);
    int row = 2;
    if (u->access_type == ACCESS_READ_ONLY || u->access_type == ACCESS_BOTH) {
        mvprintw(row++, 0, "1) View document");
    }
    if (u->access_type == ACCESS_WRITE_ONLY || u->access_type == ACCESS_BOTH) {
        mvprintw(row++, 0, "2) Edit document (nano)");
    }
    mvprintw(row++, 0, "3) Exit");
    if (lock_info) {
        mvprintw(row + 1, 0, "Lock: type=%d holder_pid=%d readers=%d",
                 lock_info->lock_type, (int)lock_info->holding_pid, lock_info->reader_count);
    }
    mvprintw(LINES - 2, 0, "Enter choice.");
    refresh();
}

static void gui_paged_text(const char *title, const char *body, size_t body_len) {
    size_t i = 0;

    for (;;) {
        clear();
        mvprintw(0, 0, "%s (q: close, space: next page)", title);
        int row = 2;

        while (row < LINES - 2 && i < body_len) {
            size_t start = i;
            while (i < body_len && body[i] != '\n' && (int)(i - start) < COLS - 1) {
                i++;
            }
            char linebuf[512];
            size_t n = i - start;
            if (n >= sizeof(linebuf)) {
                n = sizeof(linebuf) - 1;
            }
            memcpy(linebuf, body + start, n);
            linebuf[n] = '\0';
            mvprintw(row++, 0, "%s", linebuf);
            if (i < body_len && body[i] == '\n') {
                i++;
            }
        }

        if (i >= body_len) {
            mvprintw(LINES - 2, 0, "End. Press any key to return.");
        } else {
            mvprintw(LINES - 2, 0, "Space: next page   q: return");
        }
        refresh();

        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            break;
        }
        if (i >= body_len) {
            break;
        }
        if (ch != ' ') {
            continue;
        }
    }
}

static void gui_owner_view(void) {
    lock_info->owner_waiting = true;
    int fd = open(SHARED_DOC, O_RDONLY);
    if (fd == -1) {
        gui_status_msg("Cannot open document: %s", strerror(errno));
        lock_info->owner_waiting = false;
        return;
    }

    if (lock_info->lock_type == 2 && lock_info->holding_pid > 0) {
        gui_send_priority_signal(lock_info->holding_pid);
        sleep(1);
    }

    if (!acquire_read_lock(fd, &g_owner_user)) {
        close(fd);
        lock_info->owner_waiting = false;
        gui_status_msg("Could not acquire read lock.");
        return;
    }

    lock_info->owner_waiting = false;

    char *buf = malloc(MAX_GUI_DOC);
    if (!buf) {
        release_read_lock(fd, &g_owner_user);
        close(fd);
        gui_status_msg("Out of memory.");
        return;
    }

    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, MAX_GUI_DOC - 1 - (size_t)total)) > 0) {
        total += n;
        if ((size_t)total >= MAX_GUI_DOC - 1) {
            break;
        }
    }
    buf[total] = '\0';

    release_read_lock(fd, &g_owner_user);
    close(fd);

    gui_paged_text("Document", buf, (size_t)total);
    free(buf);
}

static void gui_user_view(User *user) {
    if (access(SHARED_DOC, F_OK) == -1) {
        gui_status_msg("Shared document missing.");
        return;
    }

    int fd = open(SHARED_DOC, O_RDWR);
    if (fd == -1) {
        gui_status_msg("Open failed: %s", strerror(errno));
        return;
    }

    if (!acquire_read_lock(fd, user)) {
        close(fd);
        gui_status_msg("Read lock not granted.");
        return;
    }

    char *buf = malloc(MAX_GUI_DOC);
    if (!buf) {
        release_read_lock(fd, user);
        close(fd);
        gui_status_msg("Out of memory.");
        return;
    }

    ssize_t total = 0;
    ssize_t n;
    while ((n = read(fd, buf + total, MAX_GUI_DOC - 1 - (size_t)total)) > 0) {
        total += n;
        if ((size_t)total >= MAX_GUI_DOC - 1) {
            break;
        }
    }
    buf[total] = '\0';

    release_read_lock(fd, user);
    close(fd);

    gui_paged_text("Document", buf, (size_t)total);
    free(buf);
}

static void gui_owner_countdown_and_force_release(void) {
    if (!((lock_info->lock_type == 1 || lock_info->lock_type == 2) && lock_info->holding_pid > 0)) {
        return;
    }

    lock_info->countdown_active = true;
    lock_info->countdown_value = 5;

    for (int i = 5; i >= 0; i--) {
        lock_info->countdown_value = i;
        gui_status_msg("Owner takeover in %d...", i);
        if (lock_info->editor_pid > 0) {
            if (i == 0) {
                kill(lock_info->editor_pid, SIGTERM);
            } else if (i <= 2) {
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

static void gui_run_nano_with_write_lock(User *user, int time_allocation) {
    int fd = open(SHARED_DOC, O_RDWR);
    if (fd == -1) {
        gui_status_msg("Open for write failed.");
        return;
    }

    if (!acquire_write_lock(fd, user)) {
        close(fd);
        gui_status_msg("Write lock not granted.");
        return;
    }

    lock_info->edit_start_time = time(NULL);
    lock_info->time_allocation = time_allocation;
    lock_info->time_limit_active = true;

    def_prog_mode();
    endwin();

    pid_t pid = fork();
    if (pid == 0) {
        signal(PRIORITY_SIGNAL, SIG_IGN);
        execlp("nano", "nano", "-B", SHARED_DOC, NULL);
        _exit(127);
    }

    if (pid < 0) {
        reset_prog_mode();
        refresh();
        release_write_lock(fd, user);
        close(fd);
        lock_info->time_limit_active = false;
        gui_status_msg("Fork failed.");
        return;
    }

    lock_info->editor_pid = pid;

    int status;
    time_t start_time = time(NULL);
    while (waitpid(pid, &status, WNOHANG) == 0) {
        int elapsed = (int)(time(NULL) - start_time);
        int remaining = time_allocation - elapsed;
        if (remaining <= 0) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(200000);
    }

    lock_info->editor_pid = 0;
    lock_info->time_limit_active = false;

    reset_prog_mode();
    refresh();

    release_write_lock(fd, user);
    close(fd);
}

static void gui_owner_edit(void) {
    lock_info->owner_waiting = true;
    lock_info->forced_lock = true;

    int fd = open(SHARED_DOC, O_RDWR);
    if (fd == -1) {
        lock_info->owner_waiting = false;
        lock_info->forced_lock = false;
        gui_status_msg("Open failed.");
        return;
    }

    if ((lock_info->lock_type == 1 || lock_info->lock_type == 2) && lock_info->holding_pid > 0) {
        gui_owner_countdown_and_force_release();
    }

    if (!acquire_write_lock(fd, &g_owner_user)) {
        close(fd);
        lock_info->owner_waiting = false;
        lock_info->forced_lock = false;
        gui_status_msg("Write lock failed.");
        return;
    }

    lock_info->owner_waiting = false;

    int time_allocation = 30;
    lock_info->edit_start_time = time(NULL);
    lock_info->time_allocation = time_allocation;
    lock_info->time_limit_active = true;

    def_prog_mode();
    endwin();

    pid_t pid = fork();
    if (pid == 0) {
        execlp("nano", "nano", "-B", SHARED_DOC, NULL);
        _exit(127);
    }

    if (pid < 0) {
        reset_prog_mode();
        refresh();
        release_write_lock(fd, &g_owner_user);
        close(fd);
        lock_info->forced_lock = false;
        lock_info->time_limit_active = false;
        gui_status_msg("Fork failed.");
        return;
    }

    lock_info->editor_pid = pid;

    int status;
    time_t start_time = time(NULL);
    while (waitpid(pid, &status, WNOHANG) == 0) {
        int elapsed = (int)(time(NULL) - start_time);
        int remaining = 30 - elapsed;
        if (remaining <= 0) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(200000);
    }

    lock_info->editor_pid = 0;
    lock_info->time_limit_active = false;

    reset_prog_mode();
    refresh();

    release_write_lock(fd, &g_owner_user);
    lock_info->forced_lock = false;
    close(fd);
}

static void gui_user_edit(User *user) {
    if (lock_info->forced_lock) {
        gui_status_msg("Owner is taking over; try again later.");
        return;
    }

    int time_allocation;
    if (user->priority == PRIORITY_HIGH) {
        time_allocation = 15;
    } else {
        time_allocation = 10;
    }

    gui_run_nano_with_write_lock(user, time_allocation);
}

static void gui_prompt_string(WINDOW *w, int y, int x, const char *label, char *out, size_t outsz) {
    echo();
    curs_set(1);
    mvwprintw(w, y, x, "%s", label);
    wmove(w, y, (int)(x + strlen(label)));
    wgetnstr(w, out, (int)(outsz - 1));
    noecho();
    curs_set(0);
}

static void gui_prompt_int(WINDOW *w, int y, int x, const char *label, int *out) {
    char tmp[32];
    gui_prompt_string(w, y, x, label, tmp, sizeof(tmp));
    *out = atoi(tmp);
}

static void gui_owner_add_user(void) {
    WINDOW *w = newwin(12, 60, (LINES - 12) / 2, (COLS - 60) / 2);
    box(w, 0, 0);
    mvwprintw(w, 1, 2, "Add user");
    char name[50] = {0};
    int pri = PRIORITY_LOW;
    int acc = ACCESS_READ_ONLY;
    gui_prompt_string(w, 3, 2, "Username: ", name, sizeof(name));
    gui_prompt_int(w, 4, 2, "Priority (0 high, 1 low): ", &pri);
    gui_prompt_int(w, 5, 2, "Access (1 ro, 2 wo, 3 rw): ", &acc);
    if (pri != PRIORITY_HIGH && pri != PRIORITY_LOW) {
        pri = PRIORITY_LOW;
    }
    if (acc < ACCESS_READ_ONLY || acc > ACCESS_BOTH) {
        acc = ACCESS_READ_ONLY;
    }
    int rc = registry_add_user(name, pri, acc);
    mvwprintw(w, 7, 2, rc == REG_OK ? "Saved." : "Error.");
    wrefresh(w);
    wgetch(w);
    delwin(w);
    touchwin(stdscr);
}

static void gui_owner_remove_user(void) {
    WINDOW *w = newwin(8, 50, (LINES - 8) / 2, (COLS - 50) / 2);
    box(w, 0, 0);
    char name[50] = {0};
    gui_prompt_string(w, 2, 2, "Remove: ", name, sizeof(name));
    int rc = registry_remove_user(name);
    mvwprintw(w, 4, 2, rc == REG_OK ? "Removed." : "Failed.");
    wrefresh(w);
    wgetch(w);
    delwin(w);
    touchwin(stdscr);
}

static void gui_owner_update_user(void) {
    WINDOW *w = newwin(10, 60, (LINES - 10) / 2, (COLS - 60) / 2);
    box(w, 0, 0);
    char name[50] = {0};
    int pri, acc;
    gui_prompt_string(w, 2, 2, "Username: ", name, sizeof(name));
    gui_prompt_int(w, 3, 2, "Priority (0/1): ", &pri);
    gui_prompt_int(w, 4, 2, "Access (1-3): ", &acc);
    int rc = registry_update_user(name, pri, acc);
    mvwprintw(w, 6, 2, rc == REG_OK ? "Updated." : "Failed.");
    wrefresh(w);
    wgetch(w);
    delwin(w);
    touchwin(stdscr);
}

static void gui_owner_list_users(void) {
    User users[MAX_USERS];
    int n = read_control_file(users, MAX_USERS);
    clear();
    mvprintw(0, 0, "%-16s %-8s %-12s %-8s %-8s", "Name", "Priority", "Access", "PID", "Active");
    int row = 2;
    for (int i = 0; i < n && row < LINES - 2; i++) {
        const char *pr = users[i].priority == PRIORITY_OWNER ? "owner"
                         : users[i].priority == PRIORITY_HIGH ? "high"
                                                              : "low";
        const char *ac = users[i].access_type == ACCESS_READ_ONLY   ? "read"
                         : users[i].access_type == ACCESS_WRITE_ONLY ? "write"
                                                                     : "rw";
        mvprintw(row++, 0, "%-16s %-8s %-12s %-8d %-8s", users[i].name, pr, ac, (int)users[i].pid,
                 (users[i].pid > 0 && process_exists(users[i].pid)) ? "yes" : "no");
    }
    mvprintw(LINES - 2, 0, "Press any key.");
    refresh();
    getch();
}

static void gui_owner_history_view(void) {
    FILE *hf = fopen("history.txt", "r");
    if (!hf) {
        gui_status_msg("No history file.");
        return;
    }
    fseek(hf, 0, SEEK_END);
    long sz = ftell(hf);
    if (sz < 0 || sz > (long)MAX_GUI_DOC) {
        fclose(hf);
        gui_status_msg("History too large.");
        return;
    }
    rewind(hf);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(hf);
        gui_status_msg("Out of memory.");
        return;
    }
    size_t got = fread(buf, 1, (size_t)sz, hf);
    fclose(hf);
    buf[got] = '\0';
    gui_paged_text("history.txt", buf, got);
    free(buf);
}

static void gui_owner_loop(void) {
    int ch;
    for (;;) {
        gui_draw_owner_menu();
        ch = getch();
        if (ch == '0') {
            break;
        }
        switch (ch) {
            case '1':
                gui_owner_view();
                break;
            case '2':
                gui_owner_edit();
                break;
            case '3':
                gui_owner_add_user();
                break;
            case '4':
                gui_owner_remove_user();
                break;
            case '5':
                gui_owner_update_user();
                break;
            case '6':
                gui_owner_list_users();
                break;
            case '7':
                append_to_history();
                gui_status_msg("Snapshot pushed to history.txt");
                getch();
                break;
            case '8':
                pop_last_snapshot();
                gui_status_msg("Last snapshot popped.");
                getch();
                break;
            case '9':
                gui_owner_history_view();
                break;
            default:
                break;
        }
    }
}

static void gui_user_loop(User *user) {
    int ch;
    for (;;) {
        gui_draw_user_menu(user);
        ch = getch();
        if (ch == '3') {
            break;
        }
        if (ch == '1' && (user->access_type == ACCESS_READ_ONLY || user->access_type == ACCESS_BOTH)) {
            gui_user_view(user);
        } else if (ch == '2' &&
                   (user->access_type == ACCESS_WRITE_ONLY || user->access_type == ACCESS_BOTH)) {
            gui_user_edit(user);
        }
    }
}

static void print_usage(const char *argv0) {
    fprintf(stderr, "Usage:\n  %s --owner\n  %s <username>\n", argv0, argv0);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    g_is_owner_gui = (strcmp(argv[1], OWNER_ARG) == 0 || strcmp(argv[1], "-o") == 0);

    if (g_is_owner_gui) {
        create_shared_doc_if_not_exists();
        initialize_control_file();
        initialize_synchronization(true);
        strcpy(g_owner_user.name, "admin");
        g_owner_user.priority = PRIORITY_OWNER;
        g_owner_user.access_type = ACCESS_BOTH;
        g_owner_user.pid = getpid();
    } else {
        initialize_synchronization(false);
        if (!find_user(argv[1], &g_current_user)) {
            fprintf(stderr, "User '%s' not found. Start with: %s --owner\n", argv[1], argv[0]);
            cleanup_synchronization(false);
            return 1;
        }
        g_current_user.pid = getpid();
    }

    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
    }

    if (g_is_owner_gui) {
        gui_owner_loop();
        cleanup_synchronization(true);
    } else {
        gui_user_loop(&g_current_user);
        cleanup_synchronization(false);
    }

    endwin();
    return 0;
}
