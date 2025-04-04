#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>

#define BUFFER_SIZE 4
#define MAX_MSG_LEN 128

typedef enum {
    START_ALARM,
    CHANGE_ALARM,
    CANCEL_ALARM,
    SUSPEND_ALARM,
    REACTIVATE_ALARM,
    VIEW_ALARMS
} RequestType;

typedef struct {
    RequestType type;
    int alarm_id;
    int group_id;
    int interval;
    int time;
    time_t timestamp;
    char message[MAX_MSG_LEN];
} AlarmRequest;

// Circular buffer
AlarmRequest buffer[BUFFER_SIZE];
int insert_idx = 0;
int remove_idx = 0;

// Synchronization
sem_t empty;
sem_t full;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread declarations
pthread_t main_thread;
pthread_t consumer_thread;
pthread_t change_alarm_thread;

// Linked lists
typedef struct AlarmNode {
    AlarmRequest req;
    struct AlarmNode* next;
} AlarmNode;

AlarmNode* alarm_list = NULL;
AlarmNode* change_alarm_list = NULL;

// Mutex and condition variable for change alarm list
pthread_mutex_t change_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t change_alarm_cond = PTHREAD_COND_INITIALIZER;

// Utility function to convert RequestType to string
const char* request_type_to_str(RequestType type) {
    switch (type) {
        case START_ALARM: return "Start_Alarm";
        case CHANGE_ALARM: return "Change_Alarm";
        case CANCEL_ALARM: return "Cancel_Alarm";
        case SUSPEND_ALARM: return "Suspend_Alarm";
        case REACTIVATE_ALARM: return "Reactivate_Alarm";
        case VIEW_ALARMS: return "View_Alarms";
        default: return "Unknown";
    }
}

// Insert into change alarm list (sorted by timestamp)
void insert_change_alarm(AlarmRequest req) {
    pthread_mutex_lock(&change_alarm_mutex);

    AlarmNode* node = malloc(sizeof(AlarmNode));
    node->req = req;
    node->next = NULL;

    if (!change_alarm_list || difftime(req.timestamp, change_alarm_list->req.timestamp) < 0) {
        node->next = change_alarm_list;
        change_alarm_list = node;
    } else {
        AlarmNode* curr = change_alarm_list;
        while (curr->next && difftime(req.timestamp, curr->next->req.timestamp) >= 0)
            curr = curr->next;
        node->next = curr->next;
        curr->next = node;
    }

    pthread_cond_signal(&change_alarm_cond);
    pthread_mutex_unlock(&change_alarm_mutex);
}

// Find start alarm by ID in alarm list
AlarmNode* find_start_alarm(int alarm_id) {
    AlarmNode* curr = alarm_list;
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

        AlarmRequest req = buffer[remove_idx];
        remove_idx = (remove_idx + 1) % BUFFER_SIZE;

        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&empty);

        printf("Consumer Thread has Retrieved Alarm_Request_Type <%s> Request (%d) at %ld: %ld from Circular_Buffer Index: %d\n",
               request_type_to_str(req.type), req.alarm_id, time(NULL), req.timestamp, (remove_idx - 1 + BUFFER_SIZE) % BUFFER_SIZE);

        if (req.type == CHANGE_ALARM) {
            insert_change_alarm(req);
        } else {
            AlarmNode* node = malloc(sizeof(AlarmNode));
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

        AlarmNode* change_node = change_alarm_list;
        change_alarm_list = change_alarm_list->next;

        pthread_mutex_unlock(&change_alarm_mutex);

        pthread_mutex_lock(&buffer_mutex);
        AlarmNode* start_node = find_start_alarm(change_node->req.alarm_id);

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

// Parse user input and push to circular buffer
void parse_and_insert_request(char* line) {
    AlarmRequest req;
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
    } else {
        fprintf(stderr, "Invalid request format.\n");
        return;
    }

    sem_wait(&empty);
    pthread_mutex_lock(&buffer_mutex);

    buffer[insert_idx] = req;
    printf("Main Thread has Inserted Alarm_Request_Type <%s> Request (%d) at %ld: %ld into Circular_Buffer Index: %d\n",
           request_type_to_str(req.type), req.alarm_id, time(NULL), req.timestamp, insert_idx);
    insert_idx = (insert_idx + 1) % BUFFER_SIZE;

    pthread_mutex_unlock(&buffer_mutex);
    sem_post(&full);
}

int main() {
    // Init semaphores
    sem_init(&empty, 0, BUFFER_SIZE);
    sem_init(&full, 0, 0);

    // Start consumer and change alarm threads
    pthread_create(&consumer_thread, NULL, consumer_thread_func, NULL);
    pthread_create(&change_alarm_thread, NULL, change_alarm_thread_func, NULL);

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

/* TODO */
void* start_alarm_thread_func(void* arg) { return NULL; }
void* suspend_reactivate_thread_func(void* arg) { return NULL; }
void* remove_alarm_thread_func(void* arg) { return NULL; }
void* view_alarms_thread_func(void* arg) { return NULL; }
void* display_alarm_thread_func(void* arg) { return NULL; }
