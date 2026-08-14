#include "mesh/mesh_retry.h"

#include "mesh/tx_scheduler.h"

void mesh_retry_init(void)
{
    tx_scheduler_init();
}

void mesh_retry_track(uint32_t id, const char *source, const char *destination, const char *priority)
{
    (void)id;
    (void)source;
    (void)destination;
    (void)priority;
}
