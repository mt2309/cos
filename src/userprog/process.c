#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "vm/swap.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "userprog/exception.h"
#include "userprog/syscall.h"

#define STACK_BASE (((uint8_t*) PHYS_BASE) - PGSIZE)



static thread_func start_process NO_RETURN;
static bool load (char *command, void (**eip) (void), void **esp);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *command)
{
  tid_t tid;
  struct process* new_process;
  
  char* save_ptr;
  char* file_name = malloc(PGSIZE);

  new_process = malloc(sizeof(struct process));

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  new_process->command = malloc(PGSIZE);
  if (new_process->command == NULL)
    return TID_ERROR;
  strlcpy (new_process->command, command, PGSIZE);

  /* Initialise the new processes fields */
  new_process->load_success = false;
  new_process->exit_status = EXIT_FAILURE;
  sema_init(&new_process->load_complete, 0);
  sema_init(&new_process->exit_complete, 0);
  new_process->pid = PID_ERROR;
  list_init(&new_process->open_files);
  list_init(&new_process->mmaped_files);
  new_process->next_fd = 2;
  
  /* Initialise the process' Supplemental page table*/
  new_process->sup_table = malloc(sizeof(struct sup_table));
  page_table_init(new_process->sup_table);
  new_process->sup_table->process = new_process;


  /* Push this new process into the current(parent) process list of children */
  list_push_front(&thread_current()->children, &new_process->child_elem);
  
  /* Returns just the filename of the command to execute */
  strlcpy(file_name, command, PGSIZE);
  strtok_r(file_name, " ", &save_ptr);
  
  /*  If the thread's file name and the new threads file name is the same
      copy the executable file's page structs - ensures sharing of 
      data segments */
  if (thread_current()->process != NULL && (thread_current()->name == file_name))
    page_table_copy(thread_current()->process->sup_table, new_process->sup_table);

  
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (file_name, PRI_DEFAULT, start_process, new_process);
  if (tid == TID_ERROR) {
    /* If the thread was successfully created, start_process will free this */
    free(new_process->command);
  }

  /* Free file_name, as it is no longer used */
  free(file_name);

  /* Wait for load to complete  */
  sema_down(&new_process->load_complete);
  
  /* Check that the process has loaded successfully*/
  if(!new_process->load_success)
    return EXIT_FAILURE;

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *process_)
{
  struct process* process = process_;
  struct thread* thread = thread_current();
  struct intr_frame if_;
  bool success = false;

  /* Set current thread's process descriptor to the one passed to us */
  thread->process = process;
    
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (process->command, &if_.eip, &if_.esp);
  
  /*Free space allocated by process_execute */
  free(process->command);
  
  /* Process information is same as thread information */
  process->command = thread->name;
  process->pid = thread->tid;
  process->load_success = success;
  
  /* Signal load complete */
  sema_up(&process->load_complete);

  /* If load failed, quit. */
  if (!success)
    thread_exit();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct list* children = &thread_current()->children;
  struct list_elem* e;
  struct process* child;
  int exit_status = EXIT_FAILURE;

  for (e = list_begin(children); e != list_end(children);
       e = list_next (e))
  {
    child = list_entry(e, struct process, child_elem);
    if(child->pid == child_tid) {
      sema_down(&child->exit_complete); // Wait for child to exit
      exit_status = child->exit_status;
      list_remove(e); // Remove from our list of children
      free(child); // Free child process struct
      return exit_status;
    }
  }
  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  struct file* file;
  struct mmap_file* mmap_file;
  struct process* child;
  struct list_elem* e;
  
  printf ("%s: exit(%d)\n",cur->name, cur->process->exit_status);
  
  lock_acquire(&filesys_lock);
  
  file_close(cur->process->process_file);
  
  uint32_t *pd;
  
  e = list_begin (&cur->process->open_files);
  
  /* Closes this process's open files */
  while ( e != list_end (&cur->process->open_files))
  {
    file = list_entry(e, struct file, elem);
    e = list_next (e);
    if (!file->mmaped) {
      file_close(file); }
    else {
      list_remove(&file->elem);
      file->closed = true;
    }
  }
  lock_release(&filesys_lock);
  
  /* Frees all the memory used by the memory mapped files list */
  e = list_begin (&cur->process->mmaped_files);
  
  while ( e != list_end (&cur->process->mmaped_files))
  {
    mmap_file = list_entry(e, struct mmap_file, elem);
    e = list_next (e);
    if (mmap_file != NULL) {
      un_map_file(mmap_file, false);
    }
  }
  
  /* Wait for our children to die and free their memory - process_wait frees 
     the memory for the children's process structs */
  e = list_begin(&cur->children);
  while(e != list_end(&cur->children))
  {
    child = list_entry(e, struct process, child_elem);
    e = list_next(e);
    process_wait(child->pid);
  }
  
  /* Frees all the memory used by the hash table */
  page_table_destroy(cur->process->sup_table);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
  
  sema_up(&cur->process->exit_complete);
  
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELfault_addrF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X   1          /* Executable. */
#define PF_W_P 2          /* Writable flag in process - def also in exception.h */
#define PF_R   4          /* Readable. */

static bool setup_stack (void **esp, char *args);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);


/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (char* command, void (**eip) (void), void **esp) 
{
  struct thread* t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file and deny write access. This lock is held open for some time. */
  lock_acquire(&filesys_lock);
  
  file = filesys_open(t->name);

  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", t->name);
      goto done; 
    }

  t->process->process_file = file;
    
  file_deny_write(file);
  
  //debug_page_table(t->process->sup_table);
  if (page_table_empty(t->process->sup_table))
  {
    /* Read and verify executable header. */
    if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
        || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
        || ehdr.e_type != 2
        || ehdr.e_machine != 3
        || ehdr.e_version != 1
        || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
        || ehdr.e_phnum > 1024) 
      {
        printf ("load: %s: error loading executable\n", t->name);
        goto done; 
      }
      
    /* Read program headers. */
    file_ofs = ehdr.e_phoff;
    for (i = 0; i < ehdr.e_phnum; i++) 
      {
        struct Elf32_Phdr phdr;

        if (file_ofs < 0 || file_ofs > file_length (file))
          goto done;
        file_seek (file, file_ofs);

        if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
          goto done;
        file_ofs += sizeof phdr;
        switch (phdr.p_type) 
          {
          case PT_NULL:
          case PT_NOTE:
          case PT_PHDR:
          case PT_STACK:
          default:
            /* Ignore this segment. */
            break;
          case PT_DYNAMIC:
          case PT_INTERP:
          case PT_SHLIB:
            goto done;
          case PT_LOAD:
            if (validate_segment (&phdr, file)) 
              {
                bool writable = (phdr.p_flags & PF_W_P) != 0;
                uint32_t file_page = phdr.p_offset & ~PGMASK;
                uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
                uint32_t page_offset = phdr.p_vaddr & PGMASK;
                uint32_t read_bytes, zero_bytes;
                if (phdr.p_filesz > 0)
                  {
                    /* Normal segment.
                      Read initial part from disk and zero the rest. */
                    read_bytes = page_offset + phdr.p_filesz;
                    zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                  - read_bytes);
                  }
                else 
                  {
                    /* Entirely zero.
                      Don't read anything from disk. */
                    read_bytes = 0;
                    zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                  }
                if (!load_segment (file, file_page, (void *) mem_page,
                                  read_bytes, zero_bytes, writable))
                  goto done;
              }
            else
              goto done;
            break;
          }
      }
  }
  
    
  /* Set up stack. */
  if (!setup_stack (esp, command))
    goto done;
  
  
  //debug_page_table(thread_current()->process->sup_table);


  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. Unlock file system. */
  lock_release(&filesys_lock);
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 
  
  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;



  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Changed this to lazily load the process executable */
/* Conceptually loads a segment starting at offset OFS in FILE at address
   UPAGE by adding in total (READ_BYTES + ZERO_BYTES)/PGSIZE pages of virtual
   memory to the processes sup_table.
   Saving READ_BYTES, ZERO_BYTES, OFS, WRITABLE, to each page.*/
bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  struct page* page;
  off_t old_pos = file->pos;
  
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
      We will read PAGE_READ_BYTES bytes from FILE
      and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      
      page = page_create(upage, writable);
      page->ofs = ofs;
      page->read_bytes = page_read_bytes;
      page->zero_bytes = page_zero_bytes;
      page->file = file;

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs += PGSIZE;
    }
    
    /* Retain the file's old position offset */
    file_seek(file, old_pos);

  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, char *command) 
{
  struct list arguments;
  struct list_elem* e = NULL;
  struct arg_elem* this_arg = NULL;
  char* save_ptr, * token = NULL;
  uint8_t* ptr= NULL;
  int pointer_size = sizeof(void*);
  int num_args = 0;
  struct page* page;

  list_init(&arguments);
  
  page = page_allocate(STACK_BASE, PAL_ZERO, true);
  page->read_bytes = PGSIZE;

  ptr = PHYS_BASE;

  /* Add argument values and lengths to a list */
  for (token = strtok_r (command, " ", &save_ptr); token != NULL;
        token = strtok_r (NULL, " ", &save_ptr))
  {
    this_arg = malloc(sizeof(struct arg_elem));
    if(this_arg == NULL) {
      page_free(page);
      return false;
    }
    this_arg->length = strlen(token)+1;
    this_arg->argument = malloc(this_arg->length);
    if(this_arg->argument == NULL) {
      page_free(page);
      free(this_arg);
      return false;
    }

    strlcpy(this_arg->argument, token, this_arg->length);

    /* Push arguments onto stack and copy their address 
    to their list_elem */
    ptr -= this_arg->length;
    this_arg->location = ptr;
    strlcpy((char*)ptr, this_arg->argument, this_arg->length);

    list_push_front(&arguments, &this_arg->elem);
    num_args++;
  }

  /* Word Align
  We NAND the pointer with 0b11, setting the two least-significant bits 
  to 0. This effectively rounds down to the nearest 4 bytes. */
  ptr = (uint8_t*)((int)ptr & ~0b11);

  ptr -= pointer_size;
  *ptr = 0;

  /* Pushes argument address (string pointers) onto stack */
  e = list_begin(&arguments);
  while(e != list_end(&arguments))
  {
    this_arg = list_entry(e, struct arg_elem, elem);
    ptr -= pointer_size;
    *(uint32_t*)ptr = (uint32_t)this_arg->location;
    e = list_next(e);
    free(this_arg->argument);
    free(this_arg);
  }

  /* Pushes pointer to first string pointer */
  ptr -= pointer_size;
  *(uint32_t*)ptr = (uint32_t)ptr + pointer_size;

  /* Pushes number of arguments */
  ptr -= sizeof(uint32_t);
  *(uint32_t*)ptr = num_args;

  /* Pushes fake return address */
  ptr -= pointer_size;
  *(uint32_t*)ptr = 0;

  *esp = ptr;

  /*  Adds a page for the the very bottom of the stack - 
      ensures the stack can grow to max size and we don't enter the stack
      when doing memory mapping */
  page_create(MAX_STACK_ADDRESS, true);

  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}