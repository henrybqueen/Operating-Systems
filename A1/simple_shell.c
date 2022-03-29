#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>


/* Id is the PID of the process. Serial Id is the number used to forground the process. */
struct Node {
    int Id;
    int serialId;
    struct Node* next;
};

// built in commands
char *commands[5];
int num_commands;

// head/tail pointers for linked list
struct Node *head = NULL;
struct Node *tail;

int numJobs;

// pid of the process running in the foreground
int fg_process = 0;

/* Remove any processes that have terminated from the linked list. */
void clean_jobs() {

    struct Node *current = head;
    struct Node *last_visited = NULL;

    while (current != NULL) {

        int pid = current->Id;

        // if current child is terminated
        if (waitpid(pid, NULL, WNOHANG) !=0) {

            if (last_visited != NULL) {
                last_visited->next = current->next;
            }
            else {
                head = current->next;
            }

            //free(current);

        } else {
            last_visited = current;

        }

        current = current->next;
    }

    tail = last_visited;
    
}

void exitShell() {

    printf("Exiting the shell...\n");
    struct Node *current = head;
    struct Node *next;

    // terminate all child processes
    while (current != NULL) {
        kill(current->Id, SIGTERM);
        current = current->next;
    }

    exit(0);
}

// fetches next command, returns the number of tokens recieved
int getcmd(char *args[], int *bg) {

    int length;
    char *loc, *tok;
    int i = 0;
    size_t linecap = 1024;
    char *line = NULL;

    //line = (char *)malloc(linecap * sizeof(char));

    printf("%s", ">> ");
    length = getline(&line, &linecap, stdin);

    while ((tok = strsep(&line, " \t\n")) != NULL) {

        
        for (int j=0; j < strlen(tok); j++) {
            if (tok[j] <= 32) {
                tok[j] = '\0';
            }
        }

        if (strlen(tok) > 0) {
            args[i] = tok;
            i++;
        }
    }

    // If an '&' ended the line, then remove it from args and set bg to 1
    if (i > 0 && strcmp(args[i-1], "&") == 0) {
        *bg = 1;
        i--;
    }
    else {
        *bg = 0;
    }

    // This line is super suss
    args[i] = NULL;
    return i;
}


// Add a new job to the linked list
void addJob(int pid) {

    numJobs++;

    struct Node *newjob = (struct Node*)malloc(sizeof(struct Node));
    newjob->Id = pid;
    newjob->serialId = numJobs;

    if (head == NULL) {
        head = newjob;
        tail = newjob;
    }
    else {
        tail->next = newjob;
        tail = newjob;
    }
}

int builtInCmdHandler(char **args) {

    // Check if args[0] is a built in command
    int cmd = -1;
    for (int j=0; j< num_commands; j++) {
        if (strcmp(commands[j], args[0])==0) {
            cmd = j;
            break;
        } 
    }

    // 0 : exit
    // 1 : cd
    // 2 : pwd
    // 3 : jobs
    // 4 : fg

    char path_name[100];   

    switch (cmd) {
        case -1: return 0; break;
        case 0: exitShell(); break;
        case 1: chdir(args[1]); break;
        case 2: 
            
            getcwd(path_name, 50);
            printf("%s\n", path_name);
            break;
        case 3:

            clean_jobs();

            if (head == NULL) {printf("There are no jobs running.\n");}
            else {
                struct Node *current;
                current = head;

                printf("--------- Jobs ---------\n");
                while (current != NULL) {
                    printf("[%d]: %d\n", current->serialId, current->Id);
                    current = current->next;
                }
                printf("------------------------\n");
            }    
            break;
        case 4:

            // Job is the serial Id, so to obtain PID of job we must use current->Id
            
            // check that job was passed
            if (args[1] == NULL) {
                break;
            }

            int job = atoi(args[1]);
            if (job == 0) {
                break;
            }

            struct Node *current;
            current = head;

            while (current != NULL) {
                if (current->serialId == job) {
                    fg_process = current->Id;
                    waitpid(current->Id, NULL, 0);
                    fg_process = 0;

                    break;
                }
                current = current->next;
            }
            
    }

    return 1;

}

/*
Given two args arrays, this function creates a pipe and forks so that the output of the child
is given as input to the parent. This function assumes that we are in a child of the shell.
*/
void execCommandPipe(char *args1[], char *args2[]) {

    int pipefd[2];
    int pid;

    pipe(pipefd);

    pid = fork();

    if (pid == 0) {

        // redirect output to pipefd[1]
        close(1);
        dup2(pipefd[1], 1);
        close(pipefd[0]);
        close(pipefd[1]);

        if (execvp(args1[0], args1) == -1) {
            exit(-1);
        }

    } else {

        waitpid(pid, NULL, 0);

        // redirect input to pipefd[0]
        close(0);
        dup2(pipefd[0], 0);
        close(pipefd[0]);
        close(pipefd[1]);

        if (execvp(args2[0], args2) == -1) {
            exit(-1);
        }

    }
}

/*
Given the args array, determines if there is a redirect or pipe and whether to run 
process in background. In the case of a pipe, we call execCommandPipe(), which causes
a second fork from the child process created in this method.
*/
void execCommand(char *args[], int numArgs, int bg) {

    int redirect = -1;
    int pipe_ = -1;
    
    for (int i = 0; i<numArgs; i++) {
        if (strcmp(args[i], ">") == 0) {
            redirect = i;
        }   
        else if (strcmp(args[i], "|") == 0) {
            pipe_ = i;
        }
    }

    if (!builtInCmdHandler(args)) {
        int pid = fork();

        if (pid == 0) {

            // Make child ignore SIGINT
            signal(SIGINT, SIG_IGN);

            // Check if pipe was indicated
            if (pipe_ >= 0) {
                args[pipe_] = NULL;
                execCommandPipe(args, args + pipe_ + 1);
            }

            // redirect output if indicated
            if (redirect >= 0) {
                args[redirect] = NULL;
                close(1);
                int fd = open(args[redirect+1], O_CREAT | O_WRONLY | O_APPEND, 0777);
            }

            if (execvp(args[0], args) == -1) {
                exit(-1);                    
            } 
        }

        else {
            if (bg == 0) {
                fg_process = pid;
                waitpid(pid, NULL, 0);
                fg_process = 0;
            } 
            else {
                addJob(pid);
            }   
        }
    }
}

// Control C signal handler
void sigHandler(int sig) {
    if (fg_process > 0) {
        kill(fg_process, SIGKILL);
    } 
}

int main() {

    // initialize built in commands
    commands[0] = "exit";
    commands[1] = "cd";
    commands[2] = "pwd";
    commands[3] = "jobs";
    commands[4] = "fg";
    num_commands = 5;

    char *args[20];
    int numArgs;

    numJobs = 0;

    int bg;

    if (signal(SIGINT, sigHandler) == SIG_ERR) {
        printf("Error: Could not bind signal handler.\n");
        exit(1);
    }
    
    // ignore command z
    signal(SIGTSTP, SIG_IGN);

    while (1) {
        numArgs = getcmd(args, &bg);

        if (numArgs > 0) {
            execCommand(args, numArgs, bg);
        }    
    }

}