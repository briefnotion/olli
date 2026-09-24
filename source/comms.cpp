#include "comms.h"

void comms_busy_inc(COMMS& Comms_var)
{
    if (Comms_var.busy < 1024)
        Comms_var.busy++;
}

void comms_busy_dec(COMMS& Comms_var)
{
    if (Comms_var.busy > 0)
        Comms_var.busy--;
}

bool comms_busy(COMMS& Comms_var)
{
    return (Comms_var.busy < 10);
}
