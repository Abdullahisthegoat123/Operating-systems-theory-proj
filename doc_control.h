#ifndef DOC_CONTROL_H
#define DOC_CONTROL_H

#include "shared.h"

void create_shared_doc_if_not_exists(void);
void initialize_control_file(void);
int read_control_file(User users[], int max_users);
void write_control_file(User users[], int user_count);
bool process_exists(pid_t pid);

#endif
