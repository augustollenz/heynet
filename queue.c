#include <stdio.h>
#include <stdlib.h>

#include "queue.h"

#if !QUEUE_DEBUG
static int printf_null(const char *format, ...)
{
    (void) format;
    return 0;
}
#define printf  printf_null
#endif

extern queue_t queue_init(void)
{
    static struct queue queue;
    queue_t this = &queue;

    this->top = 0;
    this->bot = 0;

    return this;
}

extern int queue_push(queue_t this, int data)
{
    if (queue_is_full(this))
        return -1;

    this->data[this->top] = data;
    printf("%s -> %03d,%03d,%03zu:%03d\n", __FUNCTION__,
                                           this->bot,
                                           this->top,
                                           queue_length(this) + 1,
                                           data);
    this->top = (this->top + 1) % QUEUE_SIZE;

    return 0;
}

extern int queue_pop(queue_t this, int *data)
{
    if (queue_is_empty(this))
        return -1;

    *data = this->data[this->bot];
    printf("%s  -> %03d,%03d,%03zu:%03d\n", __FUNCTION__,
                                           this->bot,
                                           this->top,
                                           queue_length(this) - 1,
                                           *data);
    this->bot = (this->bot + 1) % QUEUE_SIZE;

    return 0;
}

extern int queue_top(queue_t this, int *data)
{
    if (queue_is_empty(this))
        return -1;

    *data = this->data[this->bot];
    printf("%s  -> %03d,%03d,%03zu:%03d\n", __FUNCTION__,
                                            this->bot,
                                            this->top,
                                            queue_length(this),
                                            *data);

    return 0;
}

extern size_t queue_length(queue_t this)
{
    size_t len;

    if (this->top >= this->bot)
        len = this->top - this->bot;
    else
        len = this->top + QUEUE_SIZE - this->bot;

    return len;
}

extern int queue_is_full(queue_t this)
{
    return (queue_length(this) == (QUEUE_SIZE - 1));
}

extern int queue_is_empty(queue_t this)
{
    return (this->top == this->bot);
}

