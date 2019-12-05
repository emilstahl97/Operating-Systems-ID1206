#include <stdlib.h>
#include <ucontext.h>
#include <assert.h>
#include <signal.h>
#include <sys/time.h>
#include "green.h"

#define PERIOD 100
#define FALSE 0
#define TRUE 1
#define STACK_SIZE 4096

static sigset_t block;

static ucontext_t main_cntx = {0};
static green_t main_green = {&main_cntx, NULL, NULL, NULL, NULL, FALSE};

static green_t *running = &main_green;

static void init() __attribute__((constructor));

void timer_handler(int sig);

void init()
{
    sigemptyset(&block);
    sigaddset(&block, SIGVTALRM);

    struct sigaction act = {0};
    struct timeval interval;
    struct itimerval period;

    act.sa_handler = timer_handler;
    assert(sigaction(SIGVTALRM, &act, NULL) == 0);

    interval.tv_sec = 0;
    interval.tv_usec = PERIOD;
    period.it_interval = interval;
    period.it_value = interval;
    setitimer(ITIMER_VIRTUAL, &period, NULL);

    getcontext(&main_cntx);
}

void timer_handler(int sig)
{
    //printf("TIMER INTERRUPT\n");
    green_yield();
}

int green_create(green_t *new, void *(*fun)(void *), void *arg)
{

    ucontext_t *cntx = (ucontext_t *)malloc(sizeof(ucontext_t));
    getcontext(cntx);

    void *stack = malloc(STACK_SIZE);

    cntx->uc_stack.ss_sp = stack;
    cntx->uc_stack.ss_size = STACK_SIZE;
    makecontext(cntx, green_thread, 0);

    new->context = cntx;
    new->fun = fun;
    new->arg = NULL;
    new->next = NULL;
    new->join = NULL;
    new->zombie = FALSE;

    add_to_ready_queue(new);
}

void add_to_ready_queue(green_t *ready)
{
    add_to_queue(&running, ready);
}

void add_to_queue(green_t **queue, green_t *thread_to_add)
{
    green_t *current = queue;
    if (current == NULL)
    {
        *queue = thread_to_add;
    }
    else
    {
        while (current->next != NULL)
            current = current->next;

        current = thread_to_add;
    }
}

void green_thread()
{

    sigprocmask(SIG_UNBLOCK, &block, NULL);

    green_t *this = running;

    (*this->fun)(this->arg);

    sigprocmask(SIG_BLOCK, &block, NULL);

    //add joining thread to ready queue

    if (this->join != NULL)
        add_to_ready_queue(this->join);

    //free allocated memory structures

    free(this->context->uc_stack.ss_sp);
    free(this->context);

    //we're a zombie
    this->zombie = TRUE;

    //find the next thread to run
    set_next_running();

    setcontext(running->context); //Threads life ends here
}

int green_yield()
{
    sigprocmask(SIG_BLOCK, &block, NULL);

    green_t *susp = running;

    //add susp to ready queue
    add_to_ready_queue(susp);

    //select the next thread for execution

    set_next_running();
    swapcontext(susp->context, running->context);
    sigprocmask(SIG_UNBLOCK, &block, NULL);

    return 0;
}

int green_join(green_t *thread)
{

    if (thread->zombie)
        return 0;

    green_t *susp = running;
    sigprocmask(SIG_BLOCK, &block, NULL);
    //add to waiting threads
    if (thread->join == NULL)
        thread->join = susp; // If no thread joining, just put it in join field
    else
    {
        green_t *current = thread->join; // Otherwise, find tail of queue, and add last
        while (current->next != NULL)
            current = current->next;

        current->next = susp;
    }
    //select the next thread for execution
    set_next_running();

    swapcontext(susp->context, running->context);
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

int green_mutex_int(green_mutex_t *mutex)
{
    mutex->taken = FALSE;
    mutex->susp = NULL;
    return 0;
}

int green_mutex_lock(green_mutex_t *mutex)
{

    sigprocmask(SIG_BLOCK, &block, NULL);

    green_t *susp = running;

    // Unlock moves all suspended threads to rdy Q, so here we just
    // check lock, and add us back to suspended queue and yield
    // All threads unblock first thing, so this should work

    while (mutex->taken)
    {
        add_to_queue(&mutex->susp, susp);
        set_next_running();
        swapcontext(susp->context, running->context);
    }

    mutex->taken = TRUE;
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

int green_mutex_unlock(green_mutex_t *mutex)
{
    sigprocmask(SIG_BLOCK, &block, NULL);
    add_to_ready_queue(mutex->susp);
    mutex->susp = NULL;
    mutex->taken = FALSE;
    sigprocmask(SIG_UNBLOCK, &block, NULL);
    return 0;
}

void set_next_running()
{

    pop_from_queue(&running);
    if (running == NULL)
        printf("Deadlock, no thread ready to run!\n");
}

green_t *pop_from_queue(green_t **queue)
{
    green_t *popped = *queue;

    if (popped != NULL)
    {
        *queue = popped->next;
        popped->next = NULL;
    }
    return popped;
}

//For debugging
int queue_length(green_t *queue)
{

    green_t *current = queue;
    int counter = 1;
    while (current->next != NULL)
    {
        current = current->next;
        counter++;
    }
    return counter;
}