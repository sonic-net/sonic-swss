/*
 * Copyright 2019 Broadcom.  The term Broadcom refers to Broadcom Inc. and/or
 * its subsidiaries.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SWSS_ERRORORCH_H
#define SWSS_ERRORORCH_H

#include "orch.h"
#include "notificationproducer.h"
#include "notificationconsumer.h"
#include <map>

typedef enum {
    ERRORORCH_FLAGS_NOTIF_SEND = 0x1,
    ERRORORCH_FLAGS_LAST
} errororch_flags_t;

class ErrorOrch: public Orch
{
    public:
        ErrorOrch(swss::DBConnector *asicDb, swss::DBConnector *errorDb, std::vector<std::string> &tableNames);
        std::string getOperation(std::string &tableName);
        std::string getErrorListenerChannelName(std::string &appDbTableName);
        std::string getErrorTableName(std::string &appDbTableName);
        std::string getAppTableName(std::string &errDbTableName);
        bool mappingHandlerRegister(std::string tableName, Orch* orch);
        bool mappingHandlerDeRegister(std::string tableName);
        bool createTableObject(std::string &errTableName);
        bool deleteTableObject(std::string &errTableName);
        bool applNotificationEnabled(_In_ sai_object_type_t object_type);
        void sendNotification(_In_ std::string& tableName, _In_ std::string& op,
                _In_ std::string& data, _In_ std::vector<swss::FieldValueTuple> &entry);
        void sendNotification(_In_ std::string& tableName,
                _In_ std::string& op, _In_ std::string& data);
        void addErrorEntry(sai_object_type_t object_type,
                std::vector<swss::FieldValueTuple> &appValues, uint32_t flags);

    private:
        std::shared_ptr<swss::DBConnector> m_errorDb;
        std::unique_ptr<swss::NotificationConsumer> m_errorNotificationConsumer;
        std::unique_ptr<swss::NotificationConsumer> m_errorFlushNotificationConsumer;
        /* Table ID to Orchestration agent object map */
        std::map<std::string, Orch*>  m_TableOrchMap;
        std::map<std::string, std::shared_ptr<swss::Table>> m_TableNameObjMap;
        std::unordered_map<std::string, std::shared_ptr<swss::NotificationProducer>> m_TableChannel;

        void doTask(Consumer &consumer);
        void doTask(swss::NotificationConsumer& consumer);
        void extractEntry(std::vector<swss::FieldValueTuple> &values,
                const std::string &field, std::string &value);
        int flushErrorDb(const std::string &op, const std::string &tableName);
        void updateErrorDb(std::string &tableName, const std::string &key,
                std::vector<swss::FieldValueTuple> &values);

};

#endif /* SWSS_ERRORORCH_H */
