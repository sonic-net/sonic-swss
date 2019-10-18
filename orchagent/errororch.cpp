/*
 * Copyright 2019 Broadcom Inc.
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

#include <assert.h>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <utility>

#include "logger.h"
#include "tokenize.h"
#include "errororch.h"
#include "errormap.h"
#include "notifier.h"
#include "sai_serialize.h"
#include "swss/json.hpp"
using json = nlohmann::json;

/* Error handling framework currently handling only the following
 * tables and can be extended to other tables */
static map<string, string> m_ObjTableMap = {
    {"SAI_OBJECT_TYPE_ROUTE_ENTRY",       APP_ROUTE_TABLE_NAME},
    {"SAI_OBJECT_TYPE_NEIGHBOR_ENTRY",    APP_NEIGH_TABLE_NAME},
};

ErrorOrch::ErrorOrch(DBConnector *asicDb, DBConnector *errorDb, vector<string> &tableNames) :
    Orch(asicDb, tableNames)
{
    SWSS_LOG_ENTER();

    m_errorDb = shared_ptr<DBConnector>(errorDb);
    /* Create Table objects for the requested tables */
    for(auto it : tableNames)
    {
        createTableObject(it);
    }

    /* Clear ERROR DB entries request from user interface */
    m_errorFlushNotificationConsumer = new swss::NotificationConsumer(asicDb, "FLUSH_ERROR_DB");
    auto errorNotifier = new Notifier(m_errorFlushNotificationConsumer, this, "FLUSH_ERROR_DB");
    Orch::addExecutor(errorNotifier);

    /* Add SAI API status error notifications support from Syncd */
    m_errorNotificationConsumer = new swss::NotificationConsumer(asicDb, "ERROR_NOTIFICATIONS");
    errorNotifier = new Notifier(m_errorNotificationConsumer, this, "SYNCD_ERROR_NOTIFICATIONS");
    Orch::addExecutor(errorNotifier);

    /* Create notification channels through which errors are sent to
     * the applications via error listerner */
    std::shared_ptr<swss::NotificationProducer> errorNotifications;
    for(auto objTable = m_ObjTableMap.begin(); objTable!= m_ObjTableMap.end(); objTable++)
    {
        string strChannel = getErrorListenerChannelName(objTable->second);
        errorNotifications = std::make_shared<swss::NotificationProducer>(errorDb, strChannel);
        m_TableChannel[objTable->second] = errorNotifications;
        SWSS_LOG_INFO("Notification channel %s is created for %s", strChannel.c_str(), objTable->second.c_str());
    }
    SWSS_LOG_INFO("EHF is ready to receive status reports from Syncd");
}

ErrorOrch::~ErrorOrch()
{
    SWSS_LOG_ENTER();
    delete m_errorNotificationConsumer;
    delete m_errorFlushNotificationConsumer;
}

void ErrorOrch::doTask(NotificationConsumer& consumer)
{
    SWSS_LOG_ENTER();

    string op;
    string data;
    std::vector<swss::FieldValueTuple> values;
    Orch *orch;
    std::shared_ptr<Table> table;

    consumer.pop(op, data, values);

    SWSS_LOG_DEBUG("EHF received operation: %s data : %s", op.c_str(), data.c_str());
    if (&consumer == m_errorNotificationConsumer && op == "saiapi_status")
    {
        /*
         * The following steps are performed:
         * Extract object type from the received data
         * Map the object type to registrant
         * Invoke mapper function to convert the data to application friendly format
         * Update DB if the notification is about failure
         * Send notification to error listener class
         */

        json j = json::parse(data);

        string asicKey = j["key"];
        const string &str_object_type = asicKey.substr(0, asicKey.find(":"));

        sai_object_type_t object_type;
        sai_deserialize_object_type(str_object_type, object_type);

        if(m_ObjTableMap.find(str_object_type) == m_ObjTableMap.end())
        {
            SWSS_LOG_INFO("EHF does not support SAI object type %s",
                    str_object_type.c_str());
            return;
        }
        string tableName = m_ObjTableMap[str_object_type];

        if(m_TableOrchMap.find(tableName) == m_TableOrchMap.end())
        {
            SWSS_LOG_INFO("No registrants for %s mapping", tableName.c_str());
            return;
        }

        orch = m_TableOrchMap[tableName];
        if(orch == NULL)
        {
            SWSS_LOG_INFO("Invalid Orch agent mapper object for %s", tableName.c_str());
            return;
        }

        std::vector<FieldValueTuple> asicValues;
        std::vector<FieldValueTuple> appValues;
        for (json::iterator it = j.begin(); it != j.end(); ++it)
        {
            asicValues.emplace_back(it.key(), it.value());
        }

        if(orch->mapToErrorDbFormat(object_type, asicValues, appValues) == false)
        {
            SWSS_LOG_ERROR("Mapping for object type %s is failed", str_object_type.c_str());
            return;
        }

        SWSS_LOG_DEBUG("Field values for %s after mapping: ", str_object_type.c_str());
        for (size_t i = 0; i < appValues.size(); i++)
        {
            SWSS_LOG_DEBUG("%s -> %s", fvField(appValues[i]).c_str(), fvValue(appValues[i]).c_str());
        }

        /* Convert SAI error code to SWSS error code */
        string swssRCStr = ErrorMap::getSwssRCStr(j["rc"]);
        appValues.emplace_back("rc", swssRCStr);

        /* SAI operation could be create/remove/set */
        appValues.emplace_back("operation", j["operation"]);

        string errKey;
        auto dbValues = appValues;

        /* Remove key from FV as it's passed explicitly while updating DB */
        auto iu = dbValues.begin();
        while (iu != dbValues.end())
        {
            if(fvField(*iu) == "key")
            {
                errKey = fvValue(*iu);
                iu = dbValues.erase(iu);
                break;
            }
            else
            {
                iu++;
            }
        }

        /* Update Error Database */
        updateErrorDb(tableName, errKey, dbValues);

        if (applNotificationEnabled(object_type))
        {
            /* Write the entry into Notification channel */
            json js;
            for (const auto &v: appValues)
            {
                js[fvField(v)] = fvValue(v);
            }
            string s = js.dump();
            string strOp = "oper_" + tableName;
            sendNotification(tableName, strOp, s);
        }
    }
    else if (&consumer == m_errorFlushNotificationConsumer)
    {
        if (op == "ALL" || op == "TABLE")
        {
            flushErrorDb(op, data);
        }
        else
        {
            SWSS_LOG_ERROR("Received unknown flush ERROR DB request");
            return;
        }
    }
}

int ErrorOrch::flushErrorDb(const string &op, const string &data)
{
    std::shared_ptr<Table> table;
    vector<string> keys;
    SWSS_LOG_DEBUG("ERROR DB flush request received, op %s, data %s", op.c_str(), data.c_str());

    string errTableName = data;
    for(auto iter = m_TableNameObjMap.begin(); iter!= m_TableNameObjMap.end(); iter++)
    {
        if((op != "ALL") && (errTableName != iter->first))
        {
            SWSS_LOG_INFO("Skipping flushing of entries for %s", errTableName.c_str());
            continue;
        }
        table = iter->second;
        if(table == NULL)
        {
            SWSS_LOG_INFO("Invalid EHF Table object found for %s", iter->first.c_str());
            continue;
        }

        table->getKeys(keys);
        for (auto& key : keys)
        {
            table->del(key);
        }
        SWSS_LOG_DEBUG("Flushed ERROR DB entries for %s", iter->first.c_str());
    }
    return 0;
}

bool ErrorOrch::applNotificationEnabled(_In_ sai_object_type_t object_type)
{
    if(object_type == SAI_OBJECT_TYPE_ROUTE_ENTRY
            || object_type == SAI_OBJECT_TYPE_NEIGHBOR_ENTRY)
    {
        return true;
    }
    return false;
}
void ErrorOrch::sendNotification(
        _In_ string& tableName,
        _In_ string& op,
        _In_ string& data,
        _In_ std::vector<swss::FieldValueTuple> &entry)
{
    int64_t rv = 0;
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("%s %s", op.c_str(), data.c_str());

    auto tableChannel = m_TableChannel.find(tableName);
    if(tableChannel == m_TableChannel.end())
    {
        SWSS_LOG_ERROR("Failed to find the notification channel for %s", tableName.c_str());
        return;
    }
    if(tableChannel->second == NULL)
    {
        SWSS_LOG_ERROR("Invalid notification channel found for %s", tableName.c_str());
        return;
    }

    std::shared_ptr<swss::NotificationProducer> notifications = tableChannel->second;

    rv = notifications->send(op, data, entry);

    SWSS_LOG_DEBUG("notification send successful, subscribers count %ld", rv);
}

void ErrorOrch::sendNotification(
        _In_ string& tableName,
        _In_ string& op,
        _In_ string& data)
{
    SWSS_LOG_ENTER();

    std::vector<swss::FieldValueTuple> entry;

    sendNotification(tableName, op, data, entry);
}

void ErrorOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();
}

string ErrorOrch::getErrorListenerChannelName(string &appDbTableName)
{
    string errorChnl = "ERROR_";
    errorChnl += appDbTableName + "_CHANNEL";

    return errorChnl;
}

string ErrorOrch::getErrorTableName(string &appDbTableName)
{
    string errorTableName = "ERROR_";
    errorTableName += appDbTableName;

    return errorTableName;
}

string ErrorOrch::getAppTableName(string &errDbTableName)
{
    string appTableName = errDbTableName;
    appTableName.erase(0, strlen("ERROR_"));

    return appTableName;
}

bool ErrorOrch::mappingHandlerRegister(string tableName, Orch* orch)
{
    SWSS_LOG_ENTER();
    if(m_TableOrchMap.find(tableName) != m_TableOrchMap.end())
    {
        SWSS_LOG_ERROR("Mapper for %s already exists in EHF", tableName.c_str());
        return false;
    }
    m_TableOrchMap[tableName] = orch;

    SWSS_LOG_INFO("Mapper for %s is registered with EHF", tableName.c_str());
    return true;
}

bool ErrorOrch::mappingHandlerDeRegister(string tableName)
{
    SWSS_LOG_ENTER();

    if(m_TableOrchMap.find(tableName) == m_TableOrchMap.end())
    {
        SWSS_LOG_ERROR("De-registration with Error Handling Framework failed for %s",
                tableName.c_str());
        return false;
    }

    m_TableOrchMap.erase(m_ObjTableMap[tableName]);
    SWSS_LOG_INFO("De-registration with Error Handling Framework for %s is successful",
            tableName.c_str());

    return true;
}

bool ErrorOrch::createTableObject(string &tableName)
{
    SWSS_LOG_ENTER();
    if(m_TableNameObjMap.find(tableName) != m_TableNameObjMap.end())
    {
        SWSS_LOG_ERROR("EHF Table object for %s already exists", tableName.c_str());
        return false;
    }
    m_TableNameObjMap[tableName] = shared_ptr<Table>(new Table(m_errorDb.get(), tableName));

    SWSS_LOG_DEBUG("Created EHF Table object for %s", tableName.c_str());
    return true;
}

bool ErrorOrch::deleteTableObject(string &tableName)
{
    SWSS_LOG_ENTER();

    string appTableName = getAppTableName(tableName);
    if(m_TableNameObjMap.find(appTableName) == m_TableNameObjMap.end())
    {
        SWSS_LOG_ERROR("Failed to remove EHF Table object for %s", tableName.c_str());
        return false;
    }

    m_TableNameObjMap.erase(tableName);
    SWSS_LOG_DEBUG("Removed EHF Table object for %s", tableName.c_str());

    return true;
}

void ErrorOrch::updateErrorDb(string &tableName, const string &key,
         std::vector<FieldValueTuple> &values)
 {
     SWSS_LOG_ENTER();
     string strOp, strRc;
     bool entryFound = false;
     bool addEntry = false;
     bool removeEntry = false;
     bool updateEntry = false;

     /* Extract current return code and operation */
     for (size_t i = 0; i < values.size(); i++)
     {
         if(fvField(values[i]) == "operation")
         {
             strOp = fvValue(values[i]);
         }
         else if(fvField(values[i]) == "rc")
         {
             strRc = fvValue(values[i]);
         }
     }

     /* Get ERROR DB table object */
     std::shared_ptr<Table> table;
     string errTableName = getErrorTableName(tableName);
     if(m_TableNameObjMap.find(errTableName) == m_TableNameObjMap.end())
     {
         SWSS_LOG_INFO("Failed to find EHF Table object for %s", errTableName.c_str());
         return;
     }

     table = m_TableNameObjMap[errTableName];
     if(table == NULL)
     {
         SWSS_LOG_INFO("Invalid EHF Table object found for %s", errTableName.c_str());
         return;
     }

     /* Check if the entry with the key is present in ERROR DB */
     std::vector<FieldValueTuple> ovalues;
     if(table->get(key, ovalues))
     {
         entryFound = true;
     }

     if(strRc == "SWSS_RC_SUCCESS")
     {
         /* Remove the entry if present in error database */
         if(entryFound == true)
         {
             removeEntry = true;
         }
     }
     else
     {
         if(strOp == "create")
         {
             addEntry = true;
         }
         else if(strOp == "remove")
         {
             if(entryFound == true)
             {
                 removeEntry = true;
             }
             else
             {
                 addEntry = true;
             }
         }
         else if(strOp == "set")
         {
             if(entryFound == true)
             {
                 updateEntry = true;
             }
             else
             {
                 addEntry = true;
             }
         }
     }
     SWSS_LOG_DEBUG("key %s operation %s RC %s Entry in DB %d add:remove:update %d:%d:%d ",
             key.c_str(), strOp.c_str(), strRc.c_str(),
             entryFound, addEntry, removeEntry, updateEntry);
     if(addEntry == true || updateEntry == true)
     {
         /* Create/Update the database entry */
         table->set(key, values);
         SWSS_LOG_INFO("Publish %s(ok) to error db", key.c_str());
     }
     else if (removeEntry == true)
     {
         /* Remove the entry from database */
         table->del(key);
         SWSS_LOG_INFO("Removed %s(ok) from error db", key.c_str());
     }

     return;
 }

/* The following function is used by other Orch Agents to add the entry into *
 * error database and optionally send a notification to the applications     */
void ErrorOrch::addErrorEntry(sai_object_type_t object_type,
        std::vector<FieldValueTuple> &appValues, uint32_t flags)
{
    SWSS_LOG_ENTER();

    std::shared_ptr<Table> table;

    /*
     * The following steps are performed:
     * Find out the table object based on the object type
     * Update DB if the notification is about failure
     * Send notification to error listener class if the flag is set
     */

    std::string str_object_type = sai_serialize_object_type(object_type);
    SWSS_LOG_DEBUG("Field values received for %s: ", str_object_type.c_str());
    for (size_t i = 0; i < appValues.size(); i++)
    {
        SWSS_LOG_DEBUG("%s -> %s", fvField(appValues[i]).c_str(),
                fvValue(appValues[i]).c_str());
    }

    if(m_ObjTableMap.find(str_object_type) == m_ObjTableMap.end())
    {
        SWSS_LOG_INFO("EHF does not support SAI object type %s",
                str_object_type.c_str());
        return;
    }

    string tableName = m_ObjTableMap[str_object_type];
    if(m_TableOrchMap.find(tableName) == m_TableOrchMap.end())
    {
        SWSS_LOG_INFO("EHF does not support error handling for %s",
                tableName.c_str());
        return;
    }

    string errKey;
    auto dbValues = appValues;

    /* Remove key from FV as it's passed explicitly while updating DB */
    auto iu = dbValues.begin();
    while (iu != dbValues.end())
    {
        if(fvField(*iu) == "key")
        {
            errKey = fvValue(*iu);
            iu = dbValues.erase(iu);
            break;
        }
        else
        {
            iu++;
        }
    }

    /* Update Error Database */
    updateErrorDb(tableName, errKey, dbValues);

    /* Write the entry into Notificaition channel */
    if ((flags & ERRORORCH_FLAGS_NOTIF_SEND) && (applNotificationEnabled(object_type)))
    {
        json js;
        for (const auto &v: appValues)
        {
            js[fvField(v)] = fvValue(v);
        }
        string s = js.dump();
        string strOp = "oper_" + tableName;
        sendNotification(tableName, strOp, s);
    }
}
