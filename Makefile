# Date: 2025-03-31
# Description: Makefile to build new_alarm_cond.c
############################################

all:
	gcc New_Alarm_Cond.c -D_POSIX_PTHREAD_SEMANTICS -lpthread -o a.out
	./a.out