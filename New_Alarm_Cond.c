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

typedef struct alarm_tag {
    request_type_t type;
    int alarm_id;
    int group_id;
    int interval;
    time_t timestamp;
    char message[MAX_MSG_LEN];
    int size;
    struct alarm_tag* link;
} alarm_t;

typedef struct change_alarm_tag {
    request_type_t type;
    int alarm_id;
    int group_id;
    int interval;
    time_t timestamp;
    char message[MAX_MSG_LEN];
    int size;
    struct alarm_tag* link;
} change_alarm_t;

// Circular buffer
alarm_t buffer[BUFFER_SIZE];
int insert_idx = 0;
int remove_idx = 0;

// Synchronization
sem_t empty;
sem_t full;
sem_t writing;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread declarations
pthread_t main_thread;
pthread_t consumer_thread;
pthread_t start_alarm_thread;
pthread_t change_alarm_thread;
pthread_t suspend_reactivate_alarm_thread;
pthread_t remove_alarm_thread;
pthread_t view_alarm_thread;

alarm_t* alarm_list = NULL;
alarm_t* change_alarm_list = NULL;

// Mutex and condition variable for change alarm list
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t change_alarm_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t view_alarm_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t insert_buffer_cond = PTHREAD_COND_INITIALIZER;

void* consumer_thread_func(void* arg);
void* start_alarm_thread_func(void* arg);
void* change_alarm_thread_func(void* arg);
void* suspend_reactivate_thread_func(void* arg);
void* remove_alarm_thread_func(void* arg);
void* view_alarm_thread_func(void* arg);

void parse_and_insert_request(char* line);

alarm_t* init_alarm_node(int alarm_id, int group_id, int interval, char* msg);
void print_alarm_list(alarm_t* node);

int reader_count = 0;

//request_type_t type;
//int alarm_id;
//int group_id;
//int interval;
//time_t timestamp;
//char message[max_msg_len];
//struct alarm_tag* link;
alarm_t* init_alarm_node(int alarm_id, int group_id, int interval, char* msg) {
   time_t now = time(NULL);
   alarm_t* node = (alarm_t*)malloc(sizeof(alarm_t));
   // init default fields
   node->link = NULL;
   node->type = 0;
   node->size = 0;
   // set data
   node->alarm_id = alarm_id;
   node->interval = interval;
   node->group_id = group_id;
   strcpy(node->message, msg);
   // return pointer to the node
   return node;
 }
 
 // assumption is that caller will always lock the list
 alarm_t* get_alarm_by_id(alarm_t** list, int alarm_id) {
   alarm_t *current = *list;
   while (current != NULL) {
       if (current->alarm_id == alarm_id) return current;
       current = current->link;
   }
   return NULL;
 }

void insert_alarm(alarm_t** list, int alarm_id, int group_id, int interval, char* msg) {
  pthread_mutex_lock(&alarm_mutex);

  // Check for duplicate alarm ID
  alarm_t *current = *list;
  while (current != NULL) {
      if (current->alarm_id == alarm_id) {
          printf("Error: Alarm ID %d already exists. Use Change_Alarm instead.\n", alarm_id);
          pthread_mutex_unlock(&alarm_mutex);
          return;
      }
      current = current->link;
  }

  time_t now = time(NULL);
  alarm_t* head = *list;

  // For empty list
  // add the first node and increment the size
  if(head == NULL) {
    head = init_alarm_node(alarm_id, group_id, interval, msg);
    head->timestamp = now;
    ++head->size;
    *list = head;
  } else {
    // if we have one node, sorted by timestamp there are two cases
    // insert after or insert before...
    // Also if it's sorted in order of timestamps and timestamp is assigned at insertion.. we don't really need to sort it because it's how *time* works?
    alarm_t* new_node = init_alarm_node(alarm_id, group_id, interval, msg);
    new_node->timestamp = now;

    if(head->size == 1) {
      // create a new node
      head->link = new_node;
    } else {
      // go to the end and add it there
      alarm_t* temp = head;
      while(temp->link != NULL) temp = temp->link;
      temp->link = new_node;
    }
    ++head->size;
  }

  pthread_mutex_unlock(&alarm_mutex);
}


// Find start alarm by ID in alarm list
//alarm_node_t* find_start_alarm(int alarm_id) {
//    alarm_node_t* curr = alarm_list;
//    while (curr) {
//        if (curr->req.alarm_id == alarm_id && curr->req.type == START_ALARM)
//            return curr;
//        curr = curr->next;
//    }
//    return NULL;
//}

// Consumer thread function
void* consumer_thread_func(void* arg) {
    while (1) {
        pthread_cond_wait(&insert_buffer_cond, &buffer_mutex);  // Signal the waiting thread
        reader_count++;
        // first reader needs to lock out the insert thread
        if(reader_count == 1) {
          sem_wait(&writing);
        }

        alarm_t req = buffer[remove_idx];
        pthread_mutex_unlock(&buffer_mutex);
        const char* req_type = REQUEST_TYPE_LOOKUP[req.type];

        switch(req.type) {
          case START_ALARM:
            printf("Start_Alarm( <alarm_id>) Inserted by Consumer Thread <thread-id> Into Alarm List: Group(<group_id>) <Time_Stamp interval time message>\n");
            insert_alarm(&alarm_list, req.alarm_id, req.group_id, req.interval, req.message);
          break;
          case CHANGE_ALARM:
            insert_alarm(&change_alarm_list, req.alarm_id, req.group_id, req.interval, req.message);
            printf("Change Alarm (<alarm_id>) Inserted by Consumer Thread<thread-id> into Separate Change Alarm Request List: Group(<group_id>) <Time_Stamp interval time message>\n");
          break;
          case CANCEL_ALARM:
            printf("Cancel Alarm( <alarm_id>) Inserted by Consumer Thread <thread-id> Into Alarm List: Group(<group_id>) <Time_Stamp interval time message>\n");
            insert_alarm(&alarm_list, req.alarm_id, req.group_id, req.interval, req.message);
          case SUSPEND_ALARM:
            printf("Cancel Alarm( <alarm_id>) Inserted by Consumer Thread <thread-id> Into Alarm List: Group(<group_id>) <Time_Stamp interval time message>\n");
            insert_alarm(&alarm_list, req.alarm_id, req.group_id, req.interval, req.message);
          break;
          case REACTIVATE_ALARM:
            printf("Cancel Alarm( <alarm_id>) Inserted by Consumer Thread <thread-id> Into Alarm List: Group(<group_id>) <Time_Stamp interval time message>\n");
            insert_alarm(&alarm_list, req.alarm_id, req.group_id, req.interval, req.message);
          break;
          case VIEW_ALARMS:
            printf("Cancel Alarm( <alarm_id>) Inserted by Consumer Thread <thread-id> Into Alarm List: Group(<group_id>) <Time_Stamp interval time message>\n");
            insert_alarm(&alarm_list, req.alarm_id, req.group_id, req.interval, req.message);
          break;
        }

        print_alarm_list(alarm_list);
        printf("CHANGE ALARM LIST\n");
        print_alarm_list(change_alarm_list);

        printf("Consumer Thread has Retrieved Alarm_Request_Type <%s> Request (%d) at %ld: %ld from Circular_Buffer Index: %d\n",
           req_type, 
           req.alarm_id, 
           time(NULL), 
           req.timestamp, 
           remove_idx
        );
        printf("Above <%d> is the Circular-Buffer array index from which the consumer thread retrieved the alarm request. <%s> can be either “Start_Alarm”, or “Change_Alarm”, or “Cancel_Alarm”, or “Suspend_Alarm”, or “Reactivate_Alarm”, or “View Alarms\n", remove_idx, req_type);

        remove_idx = (remove_idx + 1) % BUFFER_SIZE;

        reader_count--;
        if (reader_count == 0) {
            sem_post(&writing); 
        }
    }
}

//// Change Alarm Thread
void* change_alarm_thread_func(void* arg) {
  int i = 3;
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
    sem_init(&writing, 0, 1);
    pthread_mutex_init(&mutex, NULL);

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

        sleep(1); // let other threads finish
        
    }

    pthread_join(consumer_thread, NULL);
    pthread_join(change_alarm_thread, NULL);
    return 0;
}

// Parse user input and push to circular buffer
void parse_and_insert_request(char* line) {
    alarm_t req;
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

    sem_wait(&writing);
    pthread_mutex_lock(&buffer_mutex);

    buffer[insert_idx] = req;
    time_t now = time(NULL);
    printf("Main Thread has Inserted Alarm_Request_Type <%s> Request (%d) at %s: %s into Circular_Buffer Index: %d\n",
           REQUEST_TYPE_LOOKUP[req.type], 
           req.alarm_id, 
           ctime(&now), 
           ctime(&req.timestamp), 
           insert_idx
    );

    insert_idx = (insert_idx + 1) % BUFFER_SIZE;

    pthread_mutex_unlock(&buffer_mutex);
    pthread_cond_signal(&insert_buffer_cond);
    sem_post(&writing);
}

//request_type_t type;
//int alarm_id;
//int group_id;
//int interval;
//time_t timestamp;
//char message[MAX_MSG_LEN];
void print_alarm_list(alarm_t* list) {
   alarm_t* temp = list;
   int count = 1;
 
   printf("BEGIN\n");
   while(temp != NULL) {
     printf("node #: %d, alarm_id: %d, group_id: %d, interval: %d, message: %s, pointer: %p\n", 
         count,
         temp->alarm_id, 
         temp->group_id, 
         temp->interval, 
         ctime(&temp->timestamp), 
         temp->message, 
         temp
     );
     temp = temp->link;
     ++count;
   }
   printf("END\n");
}

/* TODO */
void* start_alarm_thread_func(void* arg) { return NULL; }
void* suspend_reactivate_thread_func(void* arg) { return NULL; }
void* remove_alarm_thread_func(void* arg) { return NULL; }
void* display_alarm_thread_func(void* arg) { return NULL; }
