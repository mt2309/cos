#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

#define EXIT_SUCCESS 0
#define EXIT_FAILURE -1

tid_t process_execute (const char *command);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

/* Process identifier - same as in lib/user/syscall.h */
typedef int pid_t;
#define PID_ERROR ((pid_t) -1)

struct arg_elem
{
  char argument[128];
  int length;
  char* location;
  struct list_elem elem;
};

struct process
{
  char* command;                  /* Command for use when loading process/thread */
  bool load_success;              /* Used with load_complete */
  struct semaphore load_complete; /* Ensures load is complete before process_execute() finishes */
  int exit_status;                /* Process exit status initialised to EXIT_FAILURE */
  struct semaphore exit_complete; /* Used in process_wait() */
  pid_t pid;                      /* Process pid */
  struct list_elem child_elem;    /* So it can be made a child of another processes thread*/
  struct list open_files;         /* List of files the process has open */
  int next_fd;                    /* Used for generating file descriptors*/
 
  /* Members below here are only initialised upon successful thread creation */
  struct thread* thread;          /* The thread associated with this process */
  struct file* process_file;      /* The filename of the process's executable */
};


#endif /* userprog/process.h */
