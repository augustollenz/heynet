#ifndef QUEUE_H_
#define QUEUE_H_

#define QUEUE_SIZE      16

struct queue {
    int top, bot;
    int data[QUEUE_SIZE];
};

typedef struct queue *queue_t;

extern queue_t queue_init(void);
extern int     queue_push(queue_t this, int data);
extern int     queue_pop(queue_t this, int *data);
extern int     queue_top(queue_t this, int *data);
extern size_t  queue_length(queue_t);
extern int     queue_is_full(queue_t this);
extern int     queue_is_empty(queue_t this);

#endif
