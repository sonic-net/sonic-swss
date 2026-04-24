#pragma once
#include <vector>
#include <string>
#include "orch.h"

// Weak declaration
__attribute__((weak)) int send_ipc_to_teamd(const std::string &method, const std::vector<std::string> &args);
