#ifndef SCHEDULER_H
#define SCHEDULER_H

#define USE_SCHEDULER

bool scheduler_init(void);
bool scheduler_step(void);
void scheduler_run(void);
void scheduler_yield(void);

#endif
