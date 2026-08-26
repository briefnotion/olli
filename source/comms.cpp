#include "comms.h"

void COMMS::log(const std::string& text)
{
    std::lock_guard<std::mutex> lock(output_buffer_mutex);
    log_buffer += text;
}
