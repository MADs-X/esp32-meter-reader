#ifndef METER_SCHEDULER_H
#define METER_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>

// Scheduler Task Prototipleri
void meter_scheduler_task(void *pvParameters);

#endif // METER_SCHEDULER_H