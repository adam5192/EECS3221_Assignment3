#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include "errors.h"

#define BUFFER_SIZE 4
#define MAX_MSG_LEN 128
#define MAX_DISPLAY_THREADS 8

typedef enum request_type {
    START_ALARM,
    CHANGE_ALARM,
    CANCEL_ALARM,
    SUSPEND_ALARM,
    REACTIVATE_ALARM,
    VIEW_ALARMS
} request_type_t;

typedef enum {
    STATUS_ACTIVE,
    STATUS_SUSPENDED,
    STATUS_CANCELLED,
    STATUS_EXPIRED,
    STATUS_GROUP_CHANGED,
    STATUS_MESSAGE_CHANGED,
    STATUS_INTERVAL_CHANGED,
    STATUS_REACTIVATED
} alarm_status_t;

char* REQUEST_TYPE_LOOKUP[] = {
    "START_ALARM",
    "CHANGE_ALARM",
    "CANCEL_ALARM",
    "SUSPEND_ALARM",
    "REACTIVATE_ALARM",
    "VIEW_ALARMS"
};

typedef struct alarm_request {
    request_type_t type;
    int alarm_id;
    int group_id;
    int interval;
    int time;
    time_t timestamp;
    char message[MAX_MSG_LEN];
    pthread_t display_thread;

    alarm_status_t status;
    char last_displayed_message[MAX_MSG_LEN];
    int last_interval;
} alarm_request_t;

typedef struct alarm_node {
    alarm_request_t req;
    struct alarm_node* next;
} alarm_node_t;

typedef struct {
    pthread_t display_alarm_thread;
    int alarm_count;
    int group_id;
} display_thread_t;

// Globals
alarm_request_t buffer[BUFFER_SIZE];
int insert_idx = 0, remove_idx = 0;

sem_t empty, full;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t alarm_list_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t start_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t start_alarm_cond = PTHREAD_COND_INITIALIZER;

pthread_mutex_t change_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t change_alarm_cond = PTHREAD_COND_INITIALIZER;

alarm_node_t* alarm_list = NULL;
alarm_node_t* change_alarm_list = NULL;

display_thread_t display_thread_pool[MAX_DISPLAY_THREADS];

// Display thread
void* display_alarm_thread_func(void* arg) {
    display_thread_t* self = (display_thread_t*)arg;
    int group_id = self->group_id;

    if (group_id == -1) {
        printf("Display thread could not determine its group ID. Exiting.\n");
        pthread_exit(NULL);
    }

    printf("Display thread for Group(%d) started. Thread ID: %lu\n",
           group_id, (unsigned long)pthread_self());

    while (1) {
        pthread_mutex_lock(&alarm_list_mutex);
        alarm_node_t* current = alarm_list;
        int found_alarm = 0;

        while (current) {
            alarm_request_t* req = &current->req;

            if (req->type == START_ALARM && req->group_id == group_id) {
                time_t now = time(NULL);
                int expired = difftime(now, req->timestamp) >= req->interval;

                switch (req->status) {
                    case STATUS_ACTIVE:
                    case STATUS_REACTIVATED:
                        if (!expired) {
                            printf("Alarm (%d) Printed by Alarm Display Thread %lu at %ld: Group(%d) %ld %d %s\n",
                                   req->alarm_id,
                                   (unsigned long)pthread_self(),
                                   now,
                                   req->group_id,
                                   req->timestamp,
                                   req->interval,
                                   req->message);
                            found_alarm = 1;
                        } else {
                            req->status = STATUS_EXPIRED;
                            printf("Display Alarm Thread %lu Stopped Printing Expired Alarm(%d) at %ld: %ld %d %s\n",
                                   (unsigned long)pthread_self(),
                                   req->alarm_id,
                                   now,
                                   req->timestamp,
                                   req->interval,
                                   req->message);
                        }
                        break;
                    case STATUS_GROUP_CHANGED:
                    case STATUS_CANCELLED:
                        printf("Display Thread %lu Has Stopped Printing Message of Alarm(%d) at %ld: Group(%d) %ld %d %s\n",
                               (unsigned long)pthread_self(),
                               req->alarm_id,
                               now,
                               req->group_id,
                               req->timestamp,
                               req->interval,
                               req->message);
                        req->display_thread = 0;
                        break;
                    case STATUS_MESSAGE_CHANGED:
                        printf("Display Thread %lu Starts to Print Changed Message Alarm(%d) at %ld: Group(%d) %ld %d %s\n",
                               (unsigned long)pthread_self(),
                               req->alarm_id,
                               now,
                               req->group_id,
                               req->timestamp,
                               req->interval,
                               req->message);
                        req->status = STATUS_ACTIVE;
                        found_alarm = 1;
                        break;
                    case STATUS_INTERVAL_CHANGED:
                        printf("Display Thread %lu Starts to Print Changed Interval Value Alarm(%d) at %ld: Group(%d) %ld %d %s\n",
                               (unsigned long)pthread_self(),
                               req->alarm_id,
                               now,
                               req->group_id,
                               req->timestamp,
                               req->interval,
                               req->message);
                        req->status = STATUS_ACTIVE;
                        found_alarm = 1;
                        break;
                    case STATUS_SUSPENDED:
                        break;
                    default:
                        break;
                }
            }
            current = current->next;
        }

        // Exit if no active alarms
        int any_left = 0;
        current = alarm_list;
        while (current) {
            if (current->req.group_id == group_id &&
                current->req.status != STATUS_EXPIRED &&
                current->req.status != STATUS_CANCELLED &&
                current->req.status != STATUS_GROUP_CHANGED) {
                any_left = 1;
                break;
            }
            current = current->next;
        }

        pthread_mutex_unlock(&alarm_list_mutex);

        if (!any_left) {
            printf("No More Alarms in Group(%d): Display Thread %lu exiting at %ld\n",
                   group_id, (unsigned long)pthread_self(), time(NULL));
            pthread_exit(NULL);
        }

        sleep(5);
    }
}

// Insert change
void insert_change_alarm(alarm_request_t req) {
    pthread_mutex_lock(&change_alarm_mutex);
    alarm_node_t* node = malloc(sizeof(alarm_node_t));
    node->req = req;
    node->next = change_alarm_list;
    change_alarm_list = node;
    pthread_cond_signal(&change_alarm_cond);
    pthread_mutex_unlock(&change_alarm_mutex);
}

// Change alarm thread
void* change_alarm_thread_func(void* arg) {
    while (1) {
        pthread_mutex_lock(&change_alarm_mutex);
        while (!change_alarm_list)
            pthread_cond_wait(&change_alarm_cond, &change_alarm_mutex);

        alarm_node_t* node = change_alarm_list;
        change_alarm_list = node->next;
        pthread_mutex_unlock(&change_alarm_mutex);

        pthread_mutex_lock(&alarm_list_mutex);
        alarm_node_t* current = alarm_list;

        while (current) {
            if (current->req.alarm_id == node->req.alarm_id &&
                difftime(node->req.timestamp, current->req.timestamp) > 0) {
                int group_changed = current->req.group_id != node->req.group_id;
                int msg_changed = strcmp(current->req.message, node->req.message) != 0;
                int interval_changed = current->req.interval != node->req.interval;

                current->req.group_id = node->req.group_id;
                current->req.interval = node->req.interval;
                current->req.timestamp = node->req.timestamp;
                strncpy(current->req.message, node->req.message, MAX_MSG_LEN);

                if (group_changed)
                    current->req.status = STATUS_GROUP_CHANGED;
                else if (msg_changed)
                    current->req.status = STATUS_MESSAGE_CHANGED;
                else if (interval_changed)
                    current->req.status = STATUS_INTERVAL_CHANGED;
                else
                    current->req.status = STATUS_ACTIVE;

                printf("Change Alarm Thread Has Changed Alarm(%d) at %ld: Group(%d) %ld %d %s\n",
                       current->req.alarm_id,
                       time(NULL),
                       current->req.group_id,
                       current->req.timestamp,
                       current->req.interval,
                       current->req.message);
                break;
            }
            current = current->next;
        }

        pthread_mutex_unlock(&alarm_list_mutex);
        free(node);
    }
}

// Start alarm thread
void* start_alarm_thread_func(void* arg) {
    for (int i = 0; i < MAX_DISPLAY_THREADS; ++i) {
        display_thread_pool[i].alarm_count = 0;
        display_thread_pool[i].group_id = -1;
    }

    while (1) {
        pthread_mutex_lock(&start_alarm_mutex);
        pthread_cond_wait(&start_alarm_cond, &start_alarm_mutex);
        pthread_mutex_unlock(&start_alarm_mutex);

        pthread_mutex_lock(&alarm_list_mutex);
        alarm_node_t* current = alarm_list;

        while (current) {
            if (current->req.type == START_ALARM &&
                current->req.display_thread == 0 &&
                (current->req.status == STATUS_ACTIVE ||
                 current->req.status == STATUS_REACTIVATED ||
                 current->req.status == STATUS_GROUP_CHANGED ||
                 current->req.status == STATUS_MESSAGE_CHANGED ||
                 current->req.status == STATUS_INTERVAL_CHANGED)) {

                int assigned = 0;

                for (int i = 0; i < MAX_DISPLAY_THREADS; ++i) {
                    if (display_thread_pool[i].group_id == current->req.group_id &&
                        display_thread_pool[i].alarm_count < 2) {
                        current->req.display_thread = display_thread_pool[i].display_alarm_thread;
                        display_thread_pool[i].alarm_count++;
                        printf("Alarm (%d) Assigned to Display Thread(<%lu>) at %ld: Group(%d) %ld %d %s\n",
                               current->req.alarm_id,
                               (unsigned long)display_thread_pool[i].display_alarm_thread,
                               time(NULL),
                               current->req.group_id,
                               current->req.timestamp,
                               current->req.interval,
                               current->req.message);
                        assigned = 1;
                        break;
                    }
                }

                if (!assigned) {
                    for (int i = 0; i < MAX_DISPLAY_THREADS; ++i) {
                        if (display_thread_pool[i].alarm_count == 0) {
                            display_thread_pool[i].group_id = current->req.group_id;
                            pthread_create(&display_thread_pool[i].display_alarm_thread, NULL,
                                           display_alarm_thread_func, &display_thread_pool[i]);
                            display_thread_pool[i].alarm_count = 1;
                            current->req.display_thread = display_thread_pool[i].display_alarm_thread;
                            printf("Start Alarm Thread Created New Display Alarm Thread <%lu> For Alarm(%d) at %ld: Group(%d) %ld %d %s\n",
                                   (unsigned long)display_thread_pool[i].display_alarm_thread,
                                   current->req.alarm_id,
                                   time(NULL),
                                   current->req.group_id,
                                   current->req.timestamp,
                                   current->req.interval,
                                   current->req.message);
                            break;
                        }
                    }
                }
            }
            current = current->next;
        }

        pthread_mutex_unlock(&alarm_list_mutex);
    }
}

// Consumer thread
void* consumer_thread_func(void* arg) {
    while (1) {
        sem_wait(&full);
        pthread_mutex_lock(&buffer_mutex);
        alarm_request_t req = buffer[remove_idx];
        remove_idx = (remove_idx + 1) % BUFFER_SIZE;
        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&empty);

        printf("Consumer Thread has Retrieved Alarm_Request_Type <%s> Request (%d) at %ld: %ld from Circular_Buffer Index: %d\n",
               REQUEST_TYPE_LOOKUP[req.type], req.alarm_id, time(NULL), req.timestamp, (remove_idx - 1 + BUFFER_SIZE) % BUFFER_SIZE);

        if (req.type == CHANGE_ALARM) {
            insert_change_alarm(req);
        } else {
            pthread_mutex_lock(&alarm_list_mutex);
            alarm_node_t* node = malloc(sizeof(alarm_node_t));
            node->req = req;
            node->next = alarm_list;
            alarm_list = node;
            pthread_mutex_unlock(&alarm_list_mutex);

            if (req.type == START_ALARM) {
                pthread_cond_signal(&start_alarm_cond);
            }
        }
    }
}

// Parse user input
void parse_and_insert_request(char* line) {
    alarm_request_t req;
    time(&req.timestamp);
    req.display_thread = 0;
    memset(req.message, 0, MAX_MSG_LEN);

    if (strncmp(line, "Start_Alarm(", 12) == 0) {
        req.type = START_ALARM;
        sscanf(line, "Start_Alarm(%d): Group(%d) %d %[^\n]",
               &req.alarm_id, &req.group_id, &req.interval, req.message);
        req.status = STATUS_ACTIVE;
        strncpy(req.last_displayed_message, req.message, MAX_MSG_LEN);
        req.last_interval = req.interval;
    } else if (strncmp(line, "Change_Alarm(", 13) == 0) {
        req.type = CHANGE_ALARM;
        sscanf(line, "Change_Alarm(%d): Group(%d) %d %[^\n]",
               &req.alarm_id, &req.group_id, &req.interval, req.message);
    } else {
        fprintf(stderr, "Invalid request.\n");
        return;
    }

    sem_wait(&empty);
    pthread_mutex_lock(&buffer_mutex);
    buffer[insert_idx] = req;
    insert_idx = (insert_idx + 1) % BUFFER_SIZE;
    pthread_mutex_unlock(&buffer_mutex);
    sem_post(&full);

    printf("Main Thread has Inserted Alarm_Request_Type <%s> Request (%d) at %ld: %ld into Circular_Buffer Index: %d\n",
           REQUEST_TYPE_LOOKUP[req.type], req.alarm_id, time(NULL), req.timestamp, (insert_idx - 1 + BUFFER_SIZE) % BUFFER_SIZE);
}

int main() {
    sem_init(&empty, 0, BUFFER_SIZE);
    sem_init(&full, 0, 0);

    pthread_t cons, changer, starter;
    pthread_create(&cons, NULL, consumer_thread_func, NULL);
    pthread_create(&changer, NULL, change_alarm_thread_func, NULL);
    pthread_create(&starter, NULL, start_alarm_thread_func, NULL);

    char line[256];
    while (1) {
        printf("Alarm> ");
        if (fgets(line, sizeof(line), stdin)) {
            parse_and_insert_request(line);
        } else {
            break;
        }
    }

    pthread_join(cons, NULL);
    pthread_join(changer, NULL);
    pthread_join(starter, NULL);

    return 0;
}

/* TODO */
void* suspend_reactivate_thread_func(void* arg) { return NULL; }
void* remove_alarm_thread_func(void* arg) { return NULL; }
void* view_alarms_thread_func(void* arg) { return NULL; }
