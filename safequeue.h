#ifndef SAFEQUEUE_H
#define SAFEQUEUE_H
#define MAX_PATHSIZE 128

typedef struct {
    int priority;
    int data;
    int delay;
    char path[MAX_PATHSIZE];
} work_item_t;


typedef struct {
    work_item_t *array;
    int capacity;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} priority_queue_t;

priority_queue_t *create_queue(int capacity);
int add_work(priority_queue_t *queue, int priority, int client_fd, int delay, const char *path);
work_item_t get_work(priority_queue_t *queue);
int get_work_nonblocking(priority_queue_t *queue, work_item_t *item);
void cleanup_queue(priority_queue_t *queue);

#endif // SAFEQUEUE_H
