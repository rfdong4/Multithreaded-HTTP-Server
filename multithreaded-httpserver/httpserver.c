// Asgn 4: A simple HTTP server.
// By:  Andrew Quinn

#include "listener_socket.h"
#include "iowrapper.h"
#include "connection.h"
#include "response.h"
#include "request.h"
#include "queue.h"
#include "rwlock.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#define BUFFERSIZE 4096
#define DEFAULT_THREADS 4
#define LOCK_TIMEOUT_MS 3000

//Globals
queue_t *queue;
pthread_mutex_t lock_table_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t audit_mutex = PTHREAD_MUTEX_INITIALIZER;
void *worker_thread(void *arg);
void handle_connection(int);
void audit(conn_t *conn, const Response_t *res);
void handle_get(conn_t *);
void handle_put(conn_t *);
void handle_unsupported(conn_t *);
void free_resources(void);

#define LOCK_TABLE_SIZE 1024
#define LOCK_TIMEOUT_SECONDS 5

typedef struct {
    pthread_rwlock_t *lock;
    bool *lock_active;
} unlock_params_t;

typedef struct lock_entry {
    char *uri;
    pthread_rwlock_t rwlock;
    struct lock_entry *next;
} lock_entry_t;

lock_entry_t *lock_table[LOCK_TABLE_SIZE] = { NULL };



void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        // Flush stderr to ensure audit log is written
        fflush(stderr);
        free_resources();
        exit(EXIT_SUCCESS);
    }
}

void free_resources(void) {
    for (unsigned int i = 0; i < LOCK_TABLE_SIZE; i++) {
        lock_entry_t *entry = lock_table[i];
        while (entry != NULL) {
            lock_entry_t *next = entry->next;
            pthread_rwlock_destroy(&entry->rwlock);
            free(entry->uri);
            free(entry);
            entry = next;
        }
        lock_table[i] = NULL;
    }
    // Free queue
    if (queue != NULL) {
        queue_delete(&queue);
    }
    
    // Destroy mutexes
    pthread_mutex_destroy(&lock_table_mutex);
    pthread_mutex_destroy(&audit_mutex);
}

// Hash function for URI strings
unsigned int hash_uri(char *uri) {
    unsigned int hash = 0;
    for (int i = 0; uri[i] != '\0'; i++) {
        hash = (hash * 31) + uri[i];
    }
    return hash % LOCK_TABLE_SIZE;
}


// Get or create a reader-writer lock for a URI
pthread_rwlock_t *get_uri_lock(char *uri) {
    unsigned int index = hash_uri(uri);
    
    pthread_mutex_lock(&lock_table_mutex);
    
    // Check if lock already exists
    lock_entry_t *entry = lock_table[index];
    while (entry != NULL) {
        if (strcmp(entry->uri, uri) == 0) {
            pthread_rwlock_t *lock = &entry->rwlock;
            pthread_mutex_unlock(&lock_table_mutex);
            return lock;
        }
        entry = entry->next;
    }
    
    // Create new entry
    lock_entry_t *new_entry = malloc(sizeof(lock_entry_t));
    if (new_entry == NULL) {
        pthread_mutex_unlock(&lock_table_mutex);
        return NULL;
    }
    
    new_entry->uri = strdup(uri);
    if (new_entry->uri == NULL) {
        free(new_entry);
        pthread_mutex_unlock(&lock_table_mutex);
        return NULL;
    }
    
    // Initialize the pthread_rwlock
    if (pthread_rwlock_init(&new_entry->rwlock, NULL) != 0) {
        free(new_entry->uri);
        free(new_entry);
        pthread_mutex_unlock(&lock_table_mutex);
        return NULL;
    }
    
    new_entry->next = lock_table[index];
    lock_table[index] = new_entry;
    
    pthread_mutex_unlock(&lock_table_mutex);
    return &new_entry->rwlock;
}


int main(int argc, char **argv) {
    // Parse command-line options
    int threads = DEFAULT_THREADS;
    int opt;
    char *endptr = NULL;

    while ((opt = getopt(argc, argv, "t:")) != -1) {
        switch (opt) {
            case 't':
                threads = strtol(optarg, &endptr, 10);
                if (*endptr != '\0' || threads <= 0) {
                    fprintf(stderr, "Invalid thread count: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case '?':
                // Unknown option or missing argument
                return EXIT_FAILURE;;
                break;
        }
    }

    
    // Verify positional argument (port) exists
    if (optind != argc - 1) {
        fprintf(stderr, "Port number is required\n");
        return EXIT_FAILURE;
    }
    
    size_t port = (size_t) strtoull(argv[optind], &endptr, 10);
    if (endptr && *endptr != '\0') {
        warnx("invalid port number: %s", argv[1]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    queue = queue_new(threads * 3);

    pthread_t worker_threads[threads];
    
    for (int i = 0; i < threads; i++) {
        if (pthread_create(&worker_threads[i], NULL, worker_thread, NULL) != 0) {
            fprintf(stderr, "Failed to create worker thread %d\n", i);
            return EXIT_FAILURE;
        }
    }


    Listener_Socket_t *sock = ls_new(port);
    if (!sock) {
      warnx("cannot open socket");
      return EXIT_FAILURE;
    }

    while (1) {
        int connfd = ls_accept(sock);
        if (connfd < 0) {
            continue; // Skip invalid connections
        }
        
        // Allocate memory for the connection fd
        int *connfd_ptr = malloc(sizeof(int));
        if (connfd_ptr == NULL) {
            close(connfd);
            continue;
        }
        
        *connfd_ptr = connfd;
        
        // Push to queue - if full, this will block until a slot is available
        if (!queue_push(queue, connfd_ptr)) {
            free(connfd_ptr);
            close(connfd);
        }
    }
    ls_delete(&sock);
    free_resources();
    return EXIT_SUCCESS;
}

void audit(conn_t *conn, const Response_t *res) {
    const Request_t *req = conn_get_request(conn);
    const char *oper = request_get_str(req);
    char *uri = conn_get_uri(conn);
    uint16_t status_code = response_get_code(res);
    char *request_id = conn_get_header(conn, "Request-Id");
    
    pthread_mutex_lock(&audit_mutex);
    if (request_id != NULL) {
        fprintf(stderr, "%s,%s,%d,%s\n", oper, uri, status_code, request_id);
    } else {
        fprintf(stderr, "%s,%s,%d,0\n", oper, uri, status_code);
    }
    fflush(stderr);
    pthread_mutex_unlock(&audit_mutex);

}

void handle_connection(int connfd) {

    conn_t *conn = conn_new(connfd);

    const Response_t *res = conn_parse(conn);

    if (res != NULL) {
        conn_send_response(conn, res);
    } else {
    //   fprintf(stderr, "%s", conn_str(conn));
        const Request_t *req = conn_get_request(conn);
        if (req == &REQUEST_PUT) {
            handle_put(conn);
        }
        else if (req == &REQUEST_GET) {
            handle_get(conn);
        } else {
            handle_unsupported(conn);
        }
	// add cases for other types of requests here
    }

    conn_delete(&conn);
}

// Worker thread function
void *worker_thread(void *arg) {
    (void)arg; // Unused parameter
    
    while (1) {
        int *connfd_ptr = NULL;
        bool success = queue_pop(queue, (void **)&connfd_ptr);
        
        if (success && connfd_ptr != NULL) {
            int connfd = *connfd_ptr;
            free(connfd_ptr);
            
            handle_connection(connfd);
            close(connfd);
        }
    }
    
    return NULL;
}


void handle_put(conn_t *conn) {
    char *uri = conn_get_uri(conn);
    const Response_t *res = NULL;
    char file_temp[] = "/tmp/httpserver.XXXXXX";
    int temp;

    
    if ((temp = mkstemp(file_temp)) == -1) {
        res = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
        return;
    }

    // Receive entire file content into temporary file before taking lock
    res = conn_recv_file(conn, temp);
    if (res != NULL) {
        conn_send_response(conn, res);
        close(temp);
        unlink(file_temp); // Delete temp file
        return;
    }
    //get file size
    ssize_t file_size;
    if ((file_size = lseek(temp, 0, SEEK_END)) == -1) {
        res = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
        close(temp);
        unlink(file_temp);
        return;
    }

    
    // Reset file pointer to beginning
    if (lseek(temp, 0, SEEK_SET) == -1) {
        res = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
        close(temp);
        unlink(file_temp);
        return;
    }

    // Now get the file lock since we have the complete request body
    pthread_rwlock_t *uri_lock = get_uri_lock(uri);
    if (!uri_lock) {
        res = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
        close(temp);
        unlink(file_temp);
        return;
    }

    // Take write lock only after we have complete request body
    pthread_rwlock_wrlock(uri_lock);

    // Check file existence after getting lock
    bool file_existed = access(uri, F_OK) == 0;

    // Open destination file
    int fd = open(uri, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        res = (errno == EACCES || errno == EISDIR || errno == ENOENT)
                  ? &RESPONSE_FORBIDDEN
                  : &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
        pthread_rwlock_unlock(uri_lock);
        close(temp);
        unlink(file_temp);
        return;
    }


    ssize_t bytes_passed = pass_n_bytes(temp, fd, file_size);
    if (bytes_passed < 0) {
        res = &RESPONSE_INTERNAL_SERVER_ERROR;
        conn_send_response(conn, res);
        close(temp);
        unlink(file_temp);
        return;
    }

    // Set appropriate response
    res = file_existed ? &RESPONSE_OK : &RESPONSE_CREATED;

    // Send response and log
    conn_send_response(conn, res);
    audit(conn, res);

    // Clean up
    close(fd);
    pthread_rwlock_unlock(uri_lock);
    close(temp);
    unlink(file_temp);
}

void handle_get(conn_t *conn) {
    const Response_t *res = NULL;
    char *uri = conn_get_uri(conn);
    pthread_rwlock_t *uri_lock = get_uri_lock(uri);
    bool response_sent = false;
    
    pthread_rwlock_rdlock(uri_lock);

    int fd = open(uri, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES) {
            res = &RESPONSE_FORBIDDEN;
        } else if (errno == ENOENT) {
            res = &RESPONSE_NOT_FOUND;
        } else {
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) < 0) {
            close(fd);
            res = &RESPONSE_INTERNAL_SERVER_ERROR;
        } else {
            // Send file and get response status
            res = conn_send_file(conn, fd, st.st_size);
            close(fd);
            
            // If file was sent successfully
            if (res == NULL) {
                res = &RESPONSE_OK;
            }
            
            // conn_send_file already sends the response
            response_sent = true;
        }
    }
    
    // Only send response if we haven't already
    if (!response_sent) {
        conn_send_response(conn, res);
    }
    
    // Always audit the final response
    audit(conn, res);
    pthread_rwlock_unlock(uri_lock);
}


void handle_unsupported(conn_t *conn) {
    const Response_t *res = NULL;
    res = &RESPONSE_NOT_IMPLEMENTED;
    conn_send_response(conn, res);
    audit(conn, res);
}



