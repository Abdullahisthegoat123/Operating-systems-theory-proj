# Multi-User Document Editor — Linux (GCC)
CC      := gcc
CFLAGS  := -Wall -Wextra -D_DEFAULT_SOURCE -O2
THREAD  := -lpthread -lrt
NCURSES := -lncurses

.PHONY: all clean

all: owner user gui myapp web_server

owner: owner.c shared.c doc_control.c registry.c shared.h doc_control.h registry.h
	$(CC) $(CFLAGS) -o owner owner.c shared.c doc_control.c registry.c $(THREAD)

user: user.c shared.c user_auth.c shared.h user_auth.h
	$(CC) $(CFLAGS) -o user user.c shared.c user_auth.c $(THREAD)

gui: gui.c shared.c doc_control.c registry.c user_auth.c shared.h doc_control.h registry.h user_auth.h
	$(CC) $(CFLAGS) -o gui gui.c shared.c doc_control.c registry.c user_auth.c $(THREAD) $(NCURSES)

myapp: myapp.c
	$(CC) $(CFLAGS) -o myapp myapp.c $(NCURSES)

web_server: web_server.c shared.c doc_control.c registry.c user_auth.c shared.h doc_control.h registry.h user_auth.h
	$(CC) $(CFLAGS) -o web_server web_server.c shared.c doc_control.c registry.c user_auth.c $(THREAD)

clean:
	rm -f owner user gui myapp web_server
