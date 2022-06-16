#ifndef __PIPER_H__
#define __PIPER_H__

#include <list.h>


#define MAX_SEMPHORES 10

// opened file table

struct semaphore {
	struct list_head blocked;
	int count;
};

struct opened_file {
	struct list_head list;
	unsigned char * buffer_pipe;
	int p_writer;
	int p_reader;
	int n_byte_available;
	int n_writers;
	int n_readers;
	struct semaphore sW;
	struct semaphore sR;
};

enum mode {WRITE, READ, UNUSED};

struct channel {
	struct list_head list;
	int n_channel;
	enum mode m;
	struct opened_file * file;
};

void init_pipes();
int sem_init(struct semaphore * p, unsigned int value);
int sem_wait(struct semaphore * p);
int sem_signal(struct semaphore * p);
int sem_destroy(struct semaphore * p);

#endif