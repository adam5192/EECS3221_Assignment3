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

// Circular buffer
alarm_t buffer[BUFFER_SIZE];
int insert_idx = 0;
int remove_idx = 0;

// Synchronization
sem_t empty;
sem_t full;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t view_alarm_cond = PTHREAD_COND_INITIALIZER;

// Thread declarations
pthread_t main_thread;
pthread_t consumer_thread;
pthread_t start_alarm_thread;
pthread_t change_alarm_thread;
pthread_t suspend_reactivate_alarm_thread;
pthread_t remove_alarm_thread;
pthread_t view_alarm_thread;

alarm_t* alarm_list = NULL;

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

alarm_t* init_alarm_node(int alarm_id, int group_id, int interval, char* msg);
void print_alarm_list(alarm_t* node);


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


//// Insert into change alarm list (sorted by timestamp)
//void insert_change_alarm(alarm_request_t req) {
//    pthread_mutex_lock(&change_alarm_mutex);
//
//    alarm_node_t* node = malloc(sizeof(alarm_node_t));
//    node->req = req;
//    node->next = NULL;
//
//    if (!change_alarm_list || difftime(req.timestamp, change_alarm_list->req.timestamp) < 0) {
//        node->next = change_alarm_list;
//        change_alarm_list = node;
//    } else {
//        alarm_node_t* curr = change_alarm_list;
//        while (curr->next && difftime(req.timestamp, curr->next->req.timestamp) >= 0)
//            curr = curr->next;
//        node->next = curr->next;
//        curr->next = node;
//    }
//
//    pthread_cond_signal(&change_alarm_cond);
//    pthread_mutex_unlock(&change_alarm_mutex);
//}

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

        alarm_t req = buffer[remove_idx];
        remove_idx = (remove_idx + 1) % BUFFER_SIZE;

        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&empty);

        printf("Consumer Thread has Retrieved Alarm_Request_Type <%s> Request (%d) at %ld: %ld from Circular_Buffer Index: %d\n",
               REQUEST_TYPE_LOOKUP[req.type], req.alarm_id, time(NULL), req.timestamp, (remove_idx - 1 + BUFFER_SIZE) % BUFFER_SIZE);
    }
}

//// Change Alarm Thread
void* change_alarm_thread_func(void* arg) {
    while (1) {

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

// ========================= REMOVE ALARMS THREAD =========================
void* remove_alarm_thread_func(void* arg) 
{
    while (1) 
    {
        sleep(1);  
        const time_t current_time = time(NULL);
        
        pthread_mutex_lock(&alarm_mutex);
        
        alarm_t** current = &alarm_list;
        
        // process all alarms in the list
        while (*current != NULL) 
        {
            alarm_t* this_alarm = *current;
            bool alarm_removed = false;
            
            // process cancellation requests
            if (this_alarm->type == CANCEL_ALARM) 
            {
                alarm_t** search_ptr = &alarm_list;
                
                // look for matching start alarm 
                while (*search_ptr != NULL) 
                {
                    alarm_t* candidate = *search_ptr;
                    
                    // if alarm is matching and has earlier time stamp remove 
                    if (candidate->type == START_ALARM &&
                        candidate->alarm_id == this_alarm->alarm_id &&
                        difftime(candidate->timestamp, this_alarm->timestamp) < 0) 
                    {
                        
                        *search_ptr = candidate->link;
                        
                        printf("Alarm(%d) Cancelled at %ld | Group(%d) | Interval: %ds\n",
                              candidate->alarm_id, current_time, 
                              candidate->group_id, candidate->interval);
                              
                        free(candidate);
                        break;
                    }
                    search_ptr = &(*search_ptr)->link;
                }
                
                *current = this_alarm->link;
                free(this_alarm);
                alarm_removed = true;
            }
            // check for expired alarms
            else if (this_alarm->type == START_ALARM && 
                    difftime(current_time, this_alarm->timestamp) >= this_alarm->interval) 
            {
                *current = this_alarm->link;
                
                printf("Alarm(%d) Expired at %ld | Group(%d) | Interval: %ds\n",
                      this_alarm->alarm_id, current_time,
                      this_alarm->group_id, this_alarm->interval);
                      
                free(this_alarm);
                alarm_removed = true;
            }
            
            if (!alarm_removed) {
                current = &(*current)->link;
            }
        }
        
        pthread_mutex_unlock(&alarm_mutex);
    }
    
    return NULL;
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

    sem_wait(&empty);
    pthread_mutex_lock(&buffer_mutex);

    buffer[insert_idx] = req;
    printf("Main Thread has Inserted Alarm_Request_Type <%s> Request (%d) at %ld: %ld into Circular_Buffer Index: %d\n",
           REQUEST_TYPE_LOOKUP[req.type], req.alarm_id, time(NULL), req.timestamp, insert_idx);
    insert_idx = (insert_idx + 1) % BUFFER_SIZE;

    pthread_mutex_unlock(&buffer_mutex);
    sem_post(&full);
}

int main() {
    // Init semaphores
    sem_init(&empty, 0, BUFFER_SIZE);
    sem_init(&full, 0, 0);

    pthread_t view_thread;
    // Start consumer thread
    pthread_create(&consumer_thread, NULL, consumer_thread_func, NULL);
    if (pthread_create(&view_thread, NULL, view_alarms_thread_func, NULL) != 0) {
        perror("Failed to create View Alarms Thread");
        return 1;
    }

    // Main input loop
    char line[256];
    while (1) {
        printf("Alarm> ");
        if (fgets(line, sizeof(line), stdin) == NULL) break;
        parse_and_insert_request(line);
    }

    pthread_join(consumer_thread, NULL);
    pthread_join(view_thread, NULL);
    return 0;
}

/* TODO */
void* start_alarm_thread_func(void* arg) { return NULL; }
void* suspend_reactivate_thread_func(void* arg) { return NULL; }
void* display_alarm_thread_func(void* arg) { return NULL; }

