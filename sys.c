 /*
 * sys.c - Syscalls implementation
 */
#include <devices.h>
#include <utils.h>
#include <io.h>
#include <mm.h>
#include <mm_address.h>
#include <sched.h>
#include <p_stats.h>
#include <errno.h>
#include <piper.h>

#define LECTURA 0
#define ESCRIPTURA 1

void * get_ebp();

int check_fd(int fd, int permissions)
{
  if (fd>1) return -EBADF; 
  if (permissions!=ESCRIPTURA) return -EACCES; 
  return 0;
}

void user_to_system(void)
{
  update_stats(&(current()->p_stats.user_ticks), &(current()->p_stats.elapsed_total_ticks));
}

void system_to_user(void)
{
  update_stats(&(current()->p_stats.system_ticks), &(current()->p_stats.elapsed_total_ticks));
}

int sys_ni_syscall()
{
	return -ENOSYS; 
}

int sys_getpid()
{
	return current()->PID;
}

int global_PID=1000;

int ret_from_fork()
{
  return 0;
}

int sys_fork(void)
{
  struct list_head *lhcurrent = NULL;
  union task_union *uchild;
  
  /* Any free task_struct? */
  if (list_empty(&freequeue)) return -ENOMEM;

  lhcurrent=list_first(&freequeue);
  
  list_del(lhcurrent);
  
  uchild=(union task_union*)list_head_to_task_struct(lhcurrent);
  
  /* Copy the parent's task struct to child's */
  copy_data(current(), uchild, sizeof(union task_union));
  
  /* new pages dir */
  allocate_DIR((struct task_struct*)uchild);
  
  /* Allocate pages for DATA+STACK */
  int new_ph_pag, pag, i;
  page_table_entry *process_PT = get_PT(&uchild->task);
  for (pag=0; pag<NUM_PAG_DATA; pag++)
  {
    new_ph_pag=alloc_frame();
    if (new_ph_pag!=-1) /* One page allocated */
    {
      set_ss_pag(process_PT, PAG_LOG_INIT_DATA+pag, new_ph_pag);
    }
    else /* No more free pages left. Deallocate everything */
    {
      /* Deallocate allocated pages. Up to pag. */
      for (i=0; i<pag; i++)
      {
        free_frame(get_frame(process_PT, PAG_LOG_INIT_DATA+i));
        del_ss_pag(process_PT, PAG_LOG_INIT_DATA+i);
      }
      /* Deallocate task_struct */
      list_add_tail(lhcurrent, &freequeue);
      
      /* Return error */
      return -EAGAIN; 
    }
  }

  /* Copy parent's SYSTEM and CODE to child. */
  page_table_entry *parent_PT = get_PT(current());
  for (pag=0; pag<NUM_PAG_KERNEL; pag++)
  {
    set_ss_pag(process_PT, pag, get_frame(parent_PT, pag));
  }
  for (pag=0; pag<NUM_PAG_CODE; pag++)
  {
    set_ss_pag(process_PT, PAG_LOG_INIT_CODE+pag, get_frame(parent_PT, PAG_LOG_INIT_CODE+pag));
  }
  /* Copy parent's DATA to child. We will use TOTAL_PAGES-1 as a temp logical page to map to */
  for (pag=NUM_PAG_KERNEL+NUM_PAG_CODE; pag<NUM_PAG_KERNEL+NUM_PAG_CODE+NUM_PAG_DATA; pag++)
  {
    /* Map one child page to parent's address space. */
    set_ss_pag(parent_PT, pag+NUM_PAG_DATA, get_frame(process_PT, pag));
    copy_data((void*)(pag<<12), (void*)((pag+NUM_PAG_DATA)<<12), PAGE_SIZE);
    del_ss_pag(parent_PT, pag+NUM_PAG_DATA);
  }
  /* Deny access to the child's memory space */
  set_cr3(get_DIR(current()));

  uchild->task.PID=++global_PID;
  uchild->task.state=ST_READY;

  int register_ebp;		/* frame pointer */
  /* Map Parent's ebp to child's stack */
  register_ebp = (int) get_ebp();
  register_ebp=(register_ebp - (int)current()) + (int)(uchild);

  uchild->task.register_esp=register_ebp + sizeof(DWord);

  DWord temp_ebp=*(DWord*)register_ebp;
  /* Prepare child stack for context switch */
  uchild->task.register_esp-=sizeof(DWord);
  *(DWord*)(uchild->task.register_esp)=(DWord)&ret_from_fork;
  uchild->task.register_esp-=sizeof(DWord);
  *(DWord*)(uchild->task.register_esp)=temp_ebp;

  /* Set stats to 0 */
  init_stats(&(uchild->task.p_stats));

  /* Queue child process into readyqueue */
  uchild->task.state=ST_READY;
  list_add_tail(&(uchild->task.list), &readyqueue);
  
  return uchild->task.PID;
}

#define TAM_BUFFER 512
char pipe_buffer[4096];

int sys_write(int fd, char *buffer, int nbytes) 
{
  // Comprobamos si fd es valido
	if (fd < 0 || fd >= MAX_CHANNELS) return -EBADF;

  // Comprobamos que nbytes sea valido
	if (nbytes < 0) return -EINVAL;

  // Si el fd es un canal que no esta inicializado devolvemos -EBADF
  if (current()->table_channels[fd].m != WRITE) return -EBADF;

  // Comprobamos si tenemos acceso al buffer
	if (!access_ok(VERIFY_READ, buffer, nbytes)) return -EFAULT;
  
	int bytes_left;
  if (fd == 1) {
    char localbuffer [TAM_BUFFER];
    
    int ret;
    bytes_left = nbytes;
    while (bytes_left > TAM_BUFFER) {
      copy_from_user(buffer, localbuffer, TAM_BUFFER);
      ret = sys_write_console(localbuffer, TAM_BUFFER);
      bytes_left-=ret;
      buffer+=ret;
    }
    if (bytes_left > 0) {
      copy_from_user(buffer, localbuffer,bytes_left);
      ret = sys_write_console(localbuffer, bytes_left);
      bytes_left-=ret;
    }
    return (nbytes-bytes_left);
  }

  // Offset del buffer de la pipe
  int pwriter = current()->table_channels[fd].file->p_writer;
  // Puntero al buffer de la pipe
  unsigned char * buffer_pipe = current()->table_channels[fd].file->buffer_pipe;
  int bytes_disponibles;
  int bytes_por_leer = nbytes;

seguir_escribiendo: //esta tag por si no tenemos los suficientes bytes en la pipe
  printk("sys_writer");
  bytes_disponibles = min((current()->table_channels[fd].file->n_byte_available), bytes_por_leer);
  
  //leemos todo lo que hay en la pipe
  for (int i = 0; i < bytes_disponibles; ++i) {
    buffer_pipe[pwriter] = buffer[i];
    pwriter = (pwriter + 1) % PAGE_SIZE;
    --(current()->table_channels[fd].file->n_byte_available);
  }
  bytes_por_leer -= bytes_disponibles;

  //desbloqueamos todos los lectores
  while(bytes_disponibles > 0 && sem_signal(&(current()->table_channels[fd].file->sR)) >= 0);

  //si aun no hemos leido todo lo que queriamos y hay escritores disponibles iteramos
  if(bytes_por_leer > 0 && current()->table_channels[fd].file->n_readers > 0) {
    sem_wait(&(current()->table_channels[fd].file->sW));
    goto seguir_escribiendo;
  }
  printk(buffer_pipe);
  return (nbytes-bytes_por_leer);

}


extern int zeos_ticks;

int sys_gettime()
{
  return zeos_ticks;
}

void sys_exit()
{  
  int i;

  page_table_entry *process_PT = get_PT(current());

  // Deallocate all the propietary physical pages
  for (i=0; i<NUM_PAG_DATA; i++)
  {
    free_frame(get_frame(process_PT, PAG_LOG_INIT_DATA+i));
    del_ss_pag(process_PT, PAG_LOG_INIT_DATA+i);
  }
  
  /* Free task_struct */
  list_add_tail(&(current()->list), &freequeue);
  
  current()->PID=-1;
  
  /* Restarts execution of the next process */
  sched_next_rr();
}

/* System call to force a task switch */
int sys_yield()
{
  force_task_switch();
  return 0;
}

extern int remaining_quantum;

int sys_get_stats(int pid, struct stats *st)
{
  int i;
  
  if (!access_ok(VERIFY_WRITE, st, sizeof(struct stats))) return -EFAULT; 
  
  if (pid<0) return -EINVAL;
  for (i=0; i<NR_TASKS; i++)
  {
    if (task[i].task.PID==pid)
    {
      task[i].task.p_stats.remaining_ticks=remaining_quantum;
      copy_to_user(&(task[i].task.p_stats), st, sizeof(struct stats));
      return 0;
    }
  }
  return -ESRCH; /*ESRCH */
}

extern struct list_head opened_file_table;

int sys_pipe(int *pd)
{
  if (list_empty(&current()->free_channel)) return -EBUSY;
  
  struct list_head * c1 = list_first(&current()->free_channel);
  list_del(c1);
  struct list_head * c2 = list_first(&current()->free_channel);
  list_del(c2);
  struct channel * readC = (struct channel*)c1;
  struct channel * writeC = (struct channel*)c2;
  readC->m = READ;
  writeC->m = WRITE;
  pd[0] = readC->n_channel;
  pd[1] = writeC->n_channel;

  struct opened_file file;
  file.n_writers = file.n_readers = 1;
  file.n_byte_available = PAGE_SIZE;
  INIT_LIST_HEAD(&file.list);   //Inicializamos la lista si no, nos da un Page Fault

  //Esto es temporal
  //file.buffer_pipe = &pipe_buffer[0];
  file.p_writer = file.p_reader = 0;
  

  //Warning: falta por buscar paginas logicas libres
  int paginaL = 1023;
  page_table_entry * processPT = get_PT(current());
  while (processPT[paginaL].bits.present == 1 && paginaL > -1) --paginaL;
  unsigned int new_frame = get_frame(processPT, paginaL);
  set_ss_pag(processPT, paginaL, new_frame);
  file.buffer_pipe = (unsigned char*)new_frame;


  //file.p_reader = file.p_writer = new_frame;

  // Asigamos los semaforos a la pipe
  sem_init(&file.sR, 0);
  sem_init(&file.sW, 0);

  list_add_tail(&opened_file_table, &file.list);
  readC->file = &file;
  writeC->file = &file;

}

int sys_read(int fd, void *buf, int count)
{
  // Si fd esta fuera del rango de canales disponibles devolvemos -EBADF
  // En fd = 0 es el canal de lectura estandar, no esta implementado, por lo tanto restringimos el acceso.
  if (fd <= 0 || fd >= MAX_CHANNELS) return -EBADF;

  // Si el fd es un canal que no esta inicializado devolvemos -EBADF
  if (current()->table_channels[fd].m != READ) return -EBADF;

  // Si count es negativo devolvemos -EINVAL
  if (count < 0) return -EINVAL;

  // Si el buf no es correcto devolvemos -EFAULT
  if (!access_ok(VERIFY_WRITE, buf, count)) return -EFAULT; 

  if (current()->table_channels[fd].file->n_byte_available == PAGE_SIZE && current()->table_channels[fd].file->n_writers == 0) return -EIO;

  // Offset del buffer de la pipe
  int pread = current()->table_channels[fd].file->p_reader;
  // Puntero al buffer de la pipe
  unsigned char * buffer_pipe = current()->table_channels[fd].file->buffer_pipe;
  unsigned char * buff = (unsigned char*)buf;
  int bytes_disponibles = 0;
  int bytes_por_leer = count;
seguir_leyendo: //esta tag por si no tenemos los suficientes bytes en la pipe
  bytes_disponibles = min((current()->table_channels[fd].file->n_byte_available), bytes_por_leer);
  //leemos todo lo que hay en la pipe
  
  char a[1];
  itoa(bytes_disponibles, a);
  printk(a);

  for (int i = 0; i < bytes_disponibles; ++i) {
    buff[i] = current()->table_channels[fd].file->buffer_pipe[pread];
    printk("iteracion");
    pread = (pread + 1) % PAGE_SIZE;
    ++(current()->table_channels[fd].file->n_byte_available);
  }
  bytes_por_leer -= bytes_disponibles;

  //desbloqueamos todos los escritores
  while(bytes_disponibles > 0 && sem_signal(&(current()->table_channels[fd].file->sW)) >= 0);

  //si aun no hemos leido todo lo que queriamos y hay escritores disponibles iteramos
  if(bytes_por_leer > 0 && current()->table_channels[fd].file->n_writers > 0) {
    sem_wait(&(current()->table_channels[fd].file->sR));
    goto seguir_leyendo;
  }
  return (count-bytes_por_leer);
}

int sys_close(int fd)
{

}