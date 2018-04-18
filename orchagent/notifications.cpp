extern "C" {
#include "sai.h"
}

#include "logger.h"
#include "notifications.h"

void on_switch_shutdown_request()
{
    SWSS_LOG_ENTER();

    /* TODO: Later a better restart story will be told here */
    SWSS_LOG_ERROR("Syncd stopped");

    exit(EXIT_FAILURE);
}
