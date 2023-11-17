#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "safequeue.h"

// Create a new priority queue
priority_queue_t *create_queue(int capacity) {
    priority_queue_t *queue = (priority_queue_t *)malloc(sizeof(priority_queue_t));
    if (!queue) {
        perror("Failed to create priority queue");
        exit(EXIT_FAILURE);
    }

    queue->array = (work_item_t *)malloc(capacity * sizeof(work_item_t));
    if (!queue->array) {
        perror("Failed to allocate memory for the priority queue array");
        exit(EXIT_FAILURE);
    }

    queue->capacity = capacity;
    queue->size = 0;

    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);

    return queue;
}

// Add work to the priority queue
void add_work(priority_queue_t *queue, int priority, int data) {
    pthread_mutex_lock(&queue->mutex);

    if (queue->size == queue->capacity) {
        // Handle queue full condition (You can adjust this based on your error handling logic)
        perror("Priority queue is full");
        exit(EXIT_FAILURE);
    }

    // Find the position to insert the new work item based on priority
    int index = 0;
    while (index < queue->size && priority <= queue->array[index].priority) {
        index++;
    }

    // Shift elements to make space for the new work item
    for (int i = queue->size; i > index; i--) {
        queue->array[i] = queue->array[i - 1];
    }

    // Insert the new work item
    queue->array[index].priority = priority;
    queue->array[index].data = data;
    queue->size++;

    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

// Get the job with the highest priority
work_item_t get_work(priority_queue_t *queue) {
    pthread_mutex_lock(&queue->mutex);

    while (queue->size == 0) {
        // Wait for the queue to be non-empty
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }

    // Remove and return the work item with the highest priority
    work_item_t item = queue->array[0];
    for (int i = 0; i < queue->size - 1; i++) {
        queue->array[i] = queue->array[i + 1];
    }
    queue->size--;

    pthread_mutex_unlock(&queue->mutex);

    return item;
}

// Get the job with the highest priority (non-blocking version)
int get_work_nonblocking(priority_queue_t *queue, work_item_t *item) {
    pthread_mutex_lock(&queue->mutex);

    if (queue->size == 0) {
        // Queue is empty
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }

    // Remove and return the work item with the highest priority
    *item = queue->array[0];
    for (int i = 0; i < queue->size - 1; i++) {
        queue->array[i] = queue->array[i + 1];
    }
    queue->size--;

    pthread_mutex_unlock(&queue->mutex);

    return 1;
}

// Clean up resources
void cleanup_queue(priority_queue_t *queue) {
    free(queue->array);
    free(queue);
}
