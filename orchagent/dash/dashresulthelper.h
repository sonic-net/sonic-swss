#pragma once

#include <memory>
#include <string>
#include "table.h"

void writeResultToDB(const std::unique_ptr<swss::Table>& table, const std::string& key,
                     uint32_t res, const std::string& version="");
void removeResultFromDB(const std::unique_ptr<swss::Table>& table, const std::string& key);
void flushResultsToDB(const std::unique_ptr<swss::Table>& table);
