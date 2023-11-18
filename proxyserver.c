#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "safequeue.h"
#include "proxyserver.h"


/*
 * Constants
 */
#define RESPONSE_BUFSIZE 10000

/*
 * Global configuration variables.
 * Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int num_listener;
int *listener_ports;
int num_workers;
char *fileserver_ipaddr;
int fileserver_port;
int max_queue_size;

void send_error_response(int client_fd, status_code_t err_code, char *err_msg) {
    http_start_response(client_fd, err_code);
    http_send_header(client_fd, "Content-Type", "text/html");
    http_end_headers(client_fd);
    char *buf = malloc(strlen(err_msg) + 2);
    sprintf(buf, "%s\n", err_msg);
    http_send_string(client_fd, buf);
    return;
}

/*
 * The priority queue to hold the work items.
 */
priority_queue_t *work_queue;

void serve_request(int client_fd) {

    // create a fileserver socket
    int fileserver_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fileserver_fd == -1) {
        fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
        exit(errno);
    }

    // create the full fileserver address
    struct sockaddr_in fileserver_address;
    fileserver_address.sin_addr.s_addr = inet_addr(fileserver_ipaddr);
    fileserver_address.sin_family = AF_INET;
    fileserver_address.sin_port = htons(fileserver_port);

    // connect to the fileserver
    int connection_status = connect(fileserver_fd, (struct sockaddr *)&fileserver_address,
                                    sizeof(fileserver_address));
    if (connection_status < 0) {
        // failed to connect to the fileserver
        printf("Failed to connect to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");
        return;
    }

    // successfully connected to the file server
    char *buffer = (char *)malloc(RESPONSE_BUFSIZE * sizeof(char));

    // forward the client request to the fileserver
    int bytes_read = read(client_fd, buffer, RESPONSE_BUFSIZE);
    int ret = http_send_data(fileserver_fd, buffer, bytes_read);
    if (ret < 0) {
        printf("Failed to send request to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");

    } else {
        // forward the fileserver response to the client
        while (1) {
            int bytes_read = recv(fileserver_fd, buffer, RESPONSE_BUFSIZE - 1, 0);
            if (bytes_read <= 0) // fileserver_fd has been closed, break
                break;
            ret = http_send_data(client_fd, buffer, bytes_read);
            if (ret < 0) { // write failed, client_fd has been closed
                break;
            }
        }
    }

    // close the connection to the fileserver
    shutdown(fileserver_fd, SHUT_WR);
    close(fileserver_fd);

    // Free resources and exit
    free(buffer);
}

void handle_get_job_request(int client_fd) {
    work_item_t work_item;

    if (!get_work_nonblocking(work_queue, &work_item)) {
        // Queue is empty, send an error response to the client
        send_error_response(client_fd, QUEUE_EMPTY, "Queue is empty");
    } else {
        // Queue is not empty, send the path in the response body
        http_start_response(client_fd, OK);
        http_send_header(client_fd, "Content-Type", "text/plain");
        http_end_headers(client_fd);
        http_send_string(client_fd, work_item.path);
    }
    // Close the connection to the client
    shutdown(client_fd, SHUT_WR);
    close(client_fd);
}

/*
 * Listener Thread Modifications to original implementation:
 * Remove the serve_request function and replace it with queuing logic:
 * enqueue(client_fd, priority). Do not perform shutdown or close in the listener thread.
 */
void *listener_thread(void *arg) {
    int listener_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (listener_fd == -1) {
        perror("Failed to create a new listener socket");
        exit(errno);
    }

    // manipulate options for the socket
    int socket_option = 1;
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) == -1) {
        perror("Failed to set listener socket options");
        exit(errno);
    }

    int listener_port = *((int *)arg);

    // create the full address for this listener
    struct sockaddr_in listener_address;
    memset(&listener_address, 0, sizeof(listener_address));
    listener_address.sin_family = AF_INET;
    listener_address.sin_addr.s_addr = INADDR_ANY;
    listener_address.sin_port = htons(listener_port);

    // bind the listener socket to the address and port number specified
    if (bind(listener_fd, (struct sockaddr *)&listener_address,
             sizeof(listener_address)) == -1) {
        perror("Failed to bind listener socket");
        exit(errno);
    }

    // starts waiting for the client to request a connection
    if (listen(listener_fd, 1024) == -1) {
        perror("Failed to listen on listener socket");
        exit(errno);
    }

    struct sockaddr_in client_address;
    size_t client_address_length = sizeof(client_address);

    while (1) {
        int client_fd = accept(listener_fd,
                               (struct sockaddr *)&client_address,
                               (socklen_t *)&client_address_length);
        if (client_fd < 0) {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n",
               inet_ntoa(client_address.sin_addr),
               client_address.sin_port);
       
        // Use recv with MSG_PEEK to inspect incoming data without consuming it
        char peek_buffer[RESPONSE_BUFSIZE];
        int bytes_read = recv(client_fd, peek_buffer, RESPONSE_BUFSIZE - 1, MSG_PEEK);
        peek_buffer[bytes_read] = '\0';

        // Check if the request contains the GetJob command
        if (strstr(peek_buffer, GETJOBCMD) != NULL) {
            handle_get_job_request(client_fd);
            continue;
        }

        // Extract the priority, delay, and path from the request
        int priority = 0, delay = 0;
        char path[MAX_PATHSIZE];

        // Extract the path from the request
        sscanf(peek_buffer, "GET %s", path);

        // Extract priority from the request path
        char *priority_start = strstr(peek_buffer, "GET /");
        if (priority_start != NULL) {
            sscanf(priority_start, "GET /%d/", &priority);
        }

        // Extract delay from the request
        char *delay_start = strstr(peek_buffer, "Delay: ");
        if (delay_start != NULL) {
            sscanf(delay_start, "Delay: %d", &delay);
        }

        // Try to enqueue the work item into the priority queue
        if (add_work(work_queue, priority, client_fd, delay, path) == -1) {
            // Queue is full, send an error response to the client
            send_error_response(client_fd, QUEUE_FULL, "Queue is full");
            close(client_fd);
        }
    }

    close(listener_fd);
    pthread_exit(NULL);
}

/*
 * Worker Thread Modifications to original implementation:
 * Modify serve_request to accept no arguments (void).
 * Inside the implementation, dequeue a work item (work_item = dequeue())
 * and obtain the client file descriptor (int client_fd = work_item->data).
 */
void *worker_thread(void *arg) {
    while (1) {
        // Call http_send_data without worrying about its implementation
        printf("Waiting for work...\n");
        work_item_t work_item = get_work(work_queue);
        int client_fd = work_item.data;
        int delay = work_item.delay;

        // Sleep for the specified delay if it's greater than 0
        if (delay > 0) {
            printf("Delaying for %d seconds...\n", delay);
            sleep(delay);
        }

        serve_request(client_fd);

        // Close the connection to the client
        shutdown(client_fd, SHUT_WR);
        close(client_fd);
    }

    pthread_exit(NULL);
}

int server_fd;
/*
 * opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *server_fd) {
    // Create the work queue
    work_queue = create_queue(max_queue_size);

    // Create listener threads
    pthread_t listener_threads[num_listener];
    for (int i = 0; i < num_listener; i++) {
        int *listener_port = malloc(sizeof(int));
        *listener_port = listener_ports[i];
        pthread_create(&listener_threads[i], NULL, listener_thread, (void *)listener_port);
    }

    // Create worker threads
    pthread_t worker_threads[num_workers];
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&worker_threads[i], NULL, worker_thread, NULL);
    }

    // Join listener threads and free allocated memory
    for (int i = 0; i < num_listener; i++) {
        pthread_join(listener_threads[i], NULL);
    }
    free(listener_ports);

    // Cleanup work queue and join worker threads
    cleanup_queue(work_queue);
    for (int i = 0; i < num_workers; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    shutdown(*server_fd, SHUT_RDWR);
    close(*server_fd);
}

/*
 * Default settings for in the global configuration variables
 */
void default_settings() {
    num_listener = 1;
    listener_ports = (int *)malloc(num_listener * sizeof(int));
    listener_ports[0] = 8000;

    num_workers = 1;

    fileserver_ipaddr = "127.0.0.1";
    fileserver_port = 3333;

    max_queue_size = 100;
}

void print_settings() {
    printf("\t---- Setting ----\n");
    printf("\t%d listeners [", num_listener);
    for (int i = 0; i < num_listener; i++)
        printf(" %d", listener_ports[i]);
    printf(" ]\n");
    printf("\t%d workers\n", num_listener);
    printf("\tfileserver ipaddr %s port %d\n", fileserver_ipaddr, fileserver_port);
    printf("\tmax queue size  %d\n", max_queue_size);
    printf("\t  ----\t----\t\n");
}

void signal_callback_handler(int signum) {
    printf("Caught signal %d: %s\n", signum, strsignal(signum));
    for (int i = 0; i < num_listener; i++) {
        if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
    }
    free(listener_ports);
    exit(0);
}

char *USAGE =
    "Usage: ./proxyserver [-l 1 8000] [-n 1] [-i 127.0.0.1 -p 3333] [-q 100]\n";

void exit_with_usage() {
    fprintf(stderr, "%s", USAGE);
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
    signal(SIGINT, signal_callback_handler);

    /* Default settings */
    default_settings();

    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp("-l", argv[i]) == 0) {
            num_listener = atoi(argv[++i]);
            free(listener_ports);
            listener_ports = (int *)malloc(num_listener * sizeof(int));
            for (int j = 0; j < num_listener; j++) {
                listener_ports[j] = atoi(argv[++i]);
            }
        } else if (strcmp("-w", argv[i]) == 0) {
            num_workers = atoi(argv[++i]);
        } else if (strcmp("-q", argv[i]) == 0) {
            max_queue_size = atoi(argv[++i]);
        } else if (strcmp("-i", argv[i]) == 0) {
            fileserver_ipaddr = argv[++i];
        } else if (strcmp("-p", argv[i]) == 0) {
            fileserver_port = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
            exit_with_usage();
        }
    }
    print_settings();

    serve_forever(&server_fd);

    return EXIT_SUCCESS;
}
