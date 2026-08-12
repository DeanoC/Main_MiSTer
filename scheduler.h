#ifndef SCHEDULER_H
#define SCHEDULER_H

#define USE_SCHEDULER

enum SchedulerMode
{
	SCHEDULER_MODE_LEGACY,
	SCHEDULER_MODE_NATIVE_HEADLESS
};

bool scheduler_init(void);
bool scheduler_init_mode(SchedulerMode mode);
bool scheduler_step(void);
void scheduler_run(void);
void scheduler_yield(void);
bool scheduler_stop(void);
bool scheduler_failed(void);

#endif
