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

typedef enum request_type {
    START_ALARM,
    CHANGE_ALARM,
    CANCEL_ALARM,
    SUSPEND_ALARM,
    REACTIVATE_ALARM,
    VIEW_ALARMS
} request_type_t;

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
} alarm_request_t;

// Circular buffer
alarm_request_t buffer[BUFFER_SIZE];
int insert_idx = 0;
int remove_idx = 0;

// Synchronization
sem_t empty;
sem_t full;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread declarations
pthread_t main_thread;
pthread_t consumer_thread;
pthread_t start_alarm_thread;
pthread_t change_alarm_thread;
pthread_t suspend_reactivate_alarm_thread;
pthread_t remove_alarm_thread;
pthread_t view_alarm_thread;

// Linked lists
typedef struct alarm_node {
    alarm_request_t req;
    struct alarm_node* next;
} alarm_node_t;

alarm_node_t* alarm_list = NULL;
alarm_node_t* change_alarm_list = NULL;

// Mutex and condition variable for change alarm list
pthread_mutex_t change_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t change_alarm_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t view_alarm_cond = PTHREAD_COND_INITIALIZER;

void* consumer_thread_func(void* arg);
void* start_alarm_thread_func(void* arg);
void* change_alarm_thread_func(void* arg);
void* suspend_reactivate_thread_func(void* arg);
void* remove_alarm_thread_func(void* arg);
void* view_alarm_thread_func(void* arg);

void parse_and_insert_request(char* line);



// Insert into change alarm list (sorted by timestamp)
void insert_change_alarm(alarm_request_t req) {
    pthread_mutex_lock(&change_alarm_mutex);

    alarm_node_t* node = malloc(sizeof(alarm_node_t));
    node->req = req;
    node->next = NULL;

    if (!change_alarm_list || difftime(req.timestamp, change_alarm_list->req.timestamp) < 0) {
        node->next = change_alarm_list;
        change_alarm_list = node;
    } else {
        alarm_node_t* curr = change_alarm_list;
        while (curr->next && difftime(req.timestamp, curr->next->req.timestamp) >= 0)
            curr = curr->next;
        node->next = curr->next;
        curr->next = node;
    }

    pthread_cond_signal(&change_alarm_cond);
    pthread_mutex_unlock(&change_alarm_mutex);
}

// Find start alarm by ID in alarm list
alarm_node_t* find_start_alarm(int alarm_id) {
    alarm_node_t* curr = alarm_list;
    while (curr) {
        if (curr->req.alarm_id == alarm_id && curr->req.type == START_ALARM)
            return curr;
        curr = curr->next;
    }
    return NULL;
}

// Consumer thread function
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
            alarm_node_t* node = malloc(sizeof(alarm_node_t));
            node->req = req;
            node->next = NULL;
            pthread_mutex_lock(&buffer_mutex);
            node->next = alarm_list;
            alarm_list = node;
            pthread_mutex_unlock(&buffer_mutex);
        }
    }
}

// Change Alarm Thread
void* change_alarm_thread_func(void* arg) {
    while (1) {
        pthread_mutex_lock(&change_alarm_mutex);

        while (change_alarm_list == NULL) {
            pthread_cond_wait(&change_alarm_cond, &change_alarm_mutex);
        }

        alarm_node_t* change_node = change_alarm_list;
        change_alarm_list = change_alarm_list->next;

        pthread_mutex_unlock(&change_alarm_mutex);

        pthread_mutex_lock(&buffer_mutex);
        alarm_node_t* start_node = find_start_alarm(change_node->req.alarm_id);

        if (start_node && difftime(change_node->req.timestamp, start_node->req.timestamp) > 0) {
            start_node->req.group_id = change_node->req.group_id;
            start_node->req.interval = change_node->req.interval;
            start_node->req.timestamp = change_node->req.timestamp;
            strncpy(start_node->req.message, change_node->req.message, MAX_MSG_LEN);

            printf("Change Alarm Thread Has Changed Alarm(%d) at %ld: Group(%d) %ld %d %s\n",
                   change_node->req.alarm_id,
                   time(NULL),
                   change_node->req.group_id,
                   change_node->req.timestamp,
                   change_node->req.interval,
                   change_node->req.message);
        } else {
            printf("Invalid Change Alarm Request(%d) at %ld: Group(%d) %ld %d %s\n",
                   change_node->req.alarm_id,
                   time(NULL),
                   change_node->req.group_id,
                   change_node->req.timestamp,
                   change_node->req.interval,
                   change_node->req.message);
        }

        pthread_mutex_unlock(&buffer_mutex);
        free(change_node);
    }
}

// ========================= VIEW ALARMS THREAD =========================
void *view_alarm_thread_func(void *arg) {
  while (1) {
    // need to block till we have a view request
    int status = pthread_cond_wait(&view_alarm_cond, &buffer_mutex);
    if(status != 0) err_abort(status, "cond_wait in view_alarmas_thread");
    sleep(1);
  }
}

int main() {
    // Init semaphores
    sem_init(&empty, 0, BUFFER_SIZE);
    sem_init(&full, 0, 0);

    // Start consumer and change alarm threads
    pthread_create(&consumer_thread, NULL, consumer_thread_func, NULL);
    pthread_create(&start_alarm_thread, NULL, start_alarm_thread_func, NULL);
    pthread_create(&change_alarm_thread, NULL, change_alarm_thread_func, NULL);
    pthread_create(&suspend_reactivate_alarm_thread, NULL, suspend_reactivate_thread_func, NULL);
    pthread_create(&remove_alarm_thread, NULL, remove_alarm_thread_func, NULL);
    pthread_create(&view_alarm_thread, NULL, view_alarm_thread_func, NULL);

    // Main input loop
    char line[256];
    while (1) {
        printf("Alarm> ");
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        parse_and_insert_request(line);
    }

    pthread_join(consumer_thread, NULL);
    pthread_join(change_alarm_thread, NULL);
    return 0;
}

// Parse user input and push to circular buffer
void parse_and_insert_request(char* line) {
    alarm_request_t req;
    time(&req.timestamp);
    req.group_id = 0;
    req.interval = 0;

    if (strncmp(line, "Start_Alarm(", 12) == 0) {
        req.type = START_ALARM;
        sscanf(line, "Start_Alarm(%d): Group(%d) %d %[^\n]", &req.alarm_id, &req.group_id, &req.interval, req.message);
    } else if (strncmp(line, "Change_Alarm(", 13) == 0) {
        req.type = CHANGE_ALARM;
        sscanf(line, "Change_Alarm(%d): Group(%d) %d %[^\n]", &req.alarm_id, &req.group_id, &req.interval, req.message);
    } else if (strncmp(line, "Cancel_Alarm(", 13) == 0) {
        req.type = CANCEL_ALARM;
        sscanf(line, "Cancel_Alarm(%d)", &req.alarm_id);
    } else if (strncmp(line, "Suspend_Alarm(", 14) == 0) {
        req.type = SUSPEND_ALARM;
        sscanf(line, "Suspend_Alarm(%d)", &req.alarm_id);
    } else if (strncmp(line, "Reactivate_Alarm(", 17) == 0) {
        req.type = REACTIVATE_ALARM;
        sscanf(line, "Reactivate_Alarm(%d)", &req.alarm_id);
    } else if (strncmp(line, "View_Alarms", 11) == 0) {
        req.type = VIEW_ALARMS;
        pthread_cond_signal(&view_alarm_cond);

    } else {
        fprintf(stderr, "Invalid request format.\n");
        return;
    }

    sem_wait(&empty);
    pthread_mutex_lock(&buffer_mutex);

    buffer[insert_idx] = req;
    printf("Main Thread has Inserted Alarm_Request_Type <%s> Request (%d) at %ld: %ld into Circular_Buffer Index: %d\n",
           REQUEST_TYPE_LOOKUP[req.type], req.alarm_id, time(NULL), req.timestamp, insert_idx);
    insert_idx = (insert_idx + 1) % BUFFER_SIZE;

    pthread_mutex_unlock(&buffer_mutex);
    sem_post(&full);
}

/* TODO */
void* start_alarm_thread_func(void* arg) { return NULL; }
void* suspend_reactivate_thread_func(void* arg) { return NULL; }
void* remove_alarm_thread_func(void* arg) { return NULL; }
void* display_alarm_thread_func(void* arg) { return NULL; }
