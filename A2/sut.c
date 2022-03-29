
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "queue.h"
#include "sut.h"
#include <ucontext.h>
#include <fcntl.h>
#include <string.h>

typedef struct threaddesc
{
	int threadid;
	char *threadstack;
	void *threadfunc;
	ucontext_t threadcontext;
} threaddesc;

#define OPEN 0
#define CLOSE 1
#define READ 2
#define WRITE 3

typedef struct IOrequest
{
    int action;
    char *file;
    char *buffer;
    int buffer_size;
    int file_descriptor;
    struct queue_entry * task;
} IOrequest;


typedef struct IOresult
{
    int action;
    int result;
} IOresult;

int numthreads;

struct queue readyqueue;
struct queue IOqueue;
struct queue IOoutqueue;

//threaddesc *current_descriptor;

// kernel threads
pthread_t* cexec;
pthread_t* cexec2;
pthread_t* iexec;

// cexec and iexec mutex
pthread_mutex_t cexec_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t iexec_mutex = PTHREAD_MUTEX_INITIALIZER;

/* pthread variable to keep track of cexec contexts */
pthread_key_t context_key;

/* pthread variable to keep track of current context running on a kernel thread */
pthread_key_t current_descriptor_key;

#define MAX_THREADS                        32
#define THREAD_STACK_SIZE                  1024*64

// the number of cexec threads to run (can be either 1 or 2)
#define num_c_execs 1

bool shutdown;
bool readyqueue_empty;
bool IOqueue_empty;

/* tell the kernel threads that shutdown has been called and wait for them to terminate */
void sut_shutdown() {

    shutdown = true;

    pthread_join(*cexec, NULL);

    if (num_c_execs == 2) {
        pthread_join(*cexec2, NULL);
    }

    pthread_join(*iexec, NULL);
}

void sut_yield() {

    struct threaddesc * current_descriptor = pthread_getspecific(current_descriptor_key);
    getcontext(&current_descriptor->threadcontext);

    struct queue_entry* task = queue_new_node(current_descriptor);

    pthread_mutex_lock(&cexec_mutex);
    queue_insert_tail(&readyqueue, task);
    pthread_mutex_unlock(&cexec_mutex);

    swapcontext(&current_descriptor->threadcontext, pthread_getspecific(context_key));

}

void sut_exit() {

    struct threaddesc * current_descriptor = pthread_getspecific(current_descriptor_key);
    getcontext(&current_descriptor->threadcontext);

    // deallocate memory to avoid memory leak
    free(current_descriptor);
    numthreads--;
    
    swapcontext(&current_descriptor->threadcontext, pthread_getspecific(context_key));
}

/* adds a request to the IO queue, C-executer will deal with the request */
int sut_open(char *dest) {

    struct threaddesc * current_descriptor = pthread_getspecific(current_descriptor_key);
    getcontext(&current_descriptor->threadcontext);

    struct queue_entry* task = queue_new_node(current_descriptor);

    /* create IO request, add it to IOwaitqueue */
    struct IOrequest *request = (IOrequest *) malloc(sizeof(IOrequest));
    request->task = task;
    request->action = OPEN;
    request->file = dest;

    struct queue_entry *entry = queue_new_node(request);

    pthread_mutex_lock(&iexec_mutex);
    queue_insert_tail(&IOqueue, entry);
    pthread_mutex_unlock(&iexec_mutex);

    swapcontext(&current_descriptor->threadcontext, pthread_getspecific(context_key));

    /* task resumes here, IO result should be head of IOoutqueue */
    /* retrieve and return result form IO request */
    struct queue_entry* IOentry = queue_pop_head(&IOoutqueue);
    struct IOresult * res = (IOresult *) IOentry->data;
    return res->result;
}

char *sut_read(int fd, char *buf, int size) {

    struct threaddesc * current_descriptor = pthread_getspecific(current_descriptor_key);
    getcontext(&current_descriptor->threadcontext);

    struct queue_entry* task = queue_new_node(current_descriptor);

    /* create IO request, add it to IOwaitqueue */
    struct IOrequest *request = (IOrequest *) malloc(sizeof(IOrequest));
    request->task = task;
    request->action = READ;
    request->file_descriptor = fd;
    request->buffer = buf;
    request->buffer_size = size;

    struct queue_entry *entry = queue_new_node(request);

    pthread_mutex_lock(&iexec_mutex);
    queue_insert_tail(&IOqueue, entry);
    pthread_mutex_unlock(&iexec_mutex);

    swapcontext(&current_descriptor->threadcontext, pthread_getspecific(context_key));

    return buf;
}

void sut_write(int fd, char *buf, int size) {

    struct threaddesc * current_descriptor = pthread_getspecific(current_descriptor_key);
    getcontext(&current_descriptor->threadcontext);

    struct queue_entry* task = queue_new_node(current_descriptor);

    /* create IO request, add it to IOwaitqueue */
    struct IOrequest *request = (IOrequest *) malloc(sizeof(IOrequest));
    request->task = task;
    request->action = WRITE;
    request->file_descriptor = fd;
    request->buffer = buf;
    request->buffer_size = size;

    struct queue_entry *entry = queue_new_node(request);

    pthread_mutex_lock(&iexec_mutex);
    queue_insert_tail(&IOqueue, entry);
    pthread_mutex_unlock(&iexec_mutex);

    swapcontext(&current_descriptor->threadcontext, pthread_getspecific(context_key));

    memset(buf, 0, size);
}

void sut_close(int fd) {

    struct threaddesc * current_descriptor = pthread_getspecific(current_descriptor_key);
    getcontext(&current_descriptor->threadcontext);

    struct queue_entry* task = queue_new_node(current_descriptor);

    /* create IO request, add it to IOwaitqueue */
    struct IOrequest *request = (IOrequest *) malloc(sizeof(IOrequest));
    request->task = task;
    request->action = CLOSE;
    request->file_descriptor = fd;

    struct queue_entry *entry = queue_new_node(request);

    pthread_mutex_lock(&iexec_mutex);
    queue_insert_tail(&IOqueue, entry);
    pthread_mutex_unlock(&iexec_mutex);

    swapcontext(&current_descriptor->threadcontext, pthread_getspecific(context_key));
}

void *iexec_scheduler() {

    struct queue_entry *ptr;
    IOrequest *request;

    int fd;
    int fd2;
    int file_size;
    FILE *fp;

    while (!shutdown || !readyqueue_empty || !IOqueue_empty || numthreads > 0) {

        pthread_mutex_lock(&iexec_mutex);
        ptr = queue_pop_head(&IOqueue);
        pthread_mutex_unlock(&iexec_mutex);

        if (ptr) {

            IOqueue_empty = false;

            request = (IOrequest *) ptr->data;
            switch (request->action) {
                case OPEN:

                    /* open file and insert result into IOoutqueue */
                    fd = open(request->file, O_RDWR | O_APPEND | O_CREAT, 0777);

                    struct IOresult *res = (IOresult *) malloc(sizeof(IOresult));
                    res->result=fd;
                    res->action=OPEN;

                    struct queue_entry * entry = queue_new_node(res);
                    pthread_mutex_lock(&iexec_mutex);
                    queue_insert_tail(&IOoutqueue, entry);
                    pthread_mutex_unlock(&iexec_mutex);

                    /* add job that requested this IO service back into the ready queue */
                    pthread_mutex_lock(&cexec_mutex);
                    queue_insert_tail(&readyqueue, request->task);
                    pthread_mutex_unlock(&cexec_mutex);

                    break;

                case READ:

                    read(request->file_descriptor, request->buffer, request->buffer_size);

                    /* add job that requested this IO service back into the ready queue */
                    pthread_mutex_lock(&cexec_mutex);
                    queue_insert_tail(&readyqueue, request->task);
                    pthread_mutex_unlock(&cexec_mutex);

                    break;

                case WRITE:

                    write(request->file_descriptor, request->buffer, request->buffer_size);

                    /* add job that requested this IO service back into the ready queue */
                    pthread_mutex_lock(&cexec_mutex);
                    queue_insert_tail(&readyqueue, request->task);
                    pthread_mutex_unlock(&cexec_mutex);

                    break;

                case CLOSE:

                    close(request->file_descriptor);

                    /* add job that requested this IO service back into the ready queue */
                    pthread_mutex_lock(&cexec_mutex);
                    queue_insert_tail(&readyqueue, request->task);
                    pthread_mutex_unlock(&cexec_mutex);

                    break;
            }


        } else {
            IOqueue_empty = true;
            usleep(100);
        }
    }
}

void *cexec_scheduler() {

    struct queue_entry *ptr;
    ucontext_t *current_context;
    threaddesc *current_descriptor;
    ucontext_t *p;

    ucontext_t parent_context;
    pthread_setspecific(context_key, &parent_context);

    while (!shutdown || !readyqueue_empty || !IOqueue_empty || numthreads > 0) {

        pthread_mutex_lock(&cexec_mutex);
        ptr = queue_pop_head(&readyqueue);
        pthread_mutex_unlock(&cexec_mutex);


        if (ptr) {

            readyqueue_empty = false;
            current_descriptor = (threaddesc *) ptr->data;
            pthread_setspecific(current_descriptor_key, current_descriptor);

            swapcontext(pthread_getspecific(context_key), &current_descriptor->threadcontext);
        
        } else {
            readyqueue_empty = true;
            usleep(100);
        }

    }

}

bool sut_create(sut_task_f fn) {

    ucontext_t threadcontext;
    threaddesc *descriptor = (threaddesc *) malloc(sizeof(threaddesc));
    
    getcontext(&threadcontext);

    // update relevant parts of context
	threadcontext.uc_stack.ss_sp = (char *)malloc(THREAD_STACK_SIZE);
	threadcontext.uc_stack.ss_size = THREAD_STACK_SIZE;

    threadcontext.uc_link = 0;
	threadcontext.uc_stack.ss_flags = 0;

    // when this context is switched to, fn will be called, 0 denotes that fn takes 0 args
	makecontext(&threadcontext, fn, 0);

    descriptor->threadcontext = threadcontext;
    descriptor->threadstack = threadcontext.uc_stack.ss_sp;
    descriptor->threadfunc = fn;
    descriptor->threadid = numthreads;

    struct queue_entry *new_entry = queue_new_node(descriptor);

    pthread_mutex_lock(&cexec_mutex);
    queue_insert_tail(&readyqueue, new_entry);
    pthread_mutex_unlock(&cexec_mutex);

    numthreads++;

    return 1;

}

void sut_init() {

    // initialize keys
    pthread_key_create(&context_key, NULL);
    pthread_key_create(&current_descriptor_key, NULL);

    numthreads = 0;

    shutdown = false;
    readyqueue_empty = false;
    IOqueue_empty = false;

    /* initialize queues */
    readyqueue = queue_create();
    queue_init(&readyqueue);

    IOqueue = queue_create();
    queue_init(&IOqueue);

    IOoutqueue = queue_create();
    queue_init(&IOoutqueue);

    // create kernel threads
    cexec = (pthread_t *) malloc(sizeof(pthread_t));
    pthread_create(cexec, NULL, cexec_scheduler, NULL);

    if (num_c_execs == 2) {
        cexec2 = (pthread_t *) malloc(sizeof(pthread_t));
        pthread_create(cexec2, NULL, cexec_scheduler, NULL);
    }

    iexec = (pthread_t *) malloc(sizeof(pthread_t));
    pthread_create(iexec, NULL, iexec_scheduler, NULL);

}
