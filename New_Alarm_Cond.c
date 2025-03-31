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

    // Start consumer thread
    pthread_create(&consumer_thread, NULL, consumer_thread_func, NULL);

    // Main input loop
    char line[256];
    while (1) {
        printf("Alarm> ");
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        parse_and_insert_request(line);
    }

    pthread_join(consumer_thread, NULL);
    return 0;
}

/* TODO */
void* start_alarm_thread_func(void* arg) { return NULL; }
void* change_alarm_thread_func(void* arg) { return NULL; }
void* suspend_reactivate_thread_func(void* arg) { return NULL; }
void* remove_alarm_thread_func(void* arg) { return NULL; }
void* view_alarms_thread_func(void* arg) { return NULL; }
void* display_alarm_thread_func(void* arg) { return NULL; }