#include "logger.h"

// Define static members
clog_manager::null_buffer clog_manager::nb;
std::wostream clog_manager::nullb(&clog_manager::nb);
