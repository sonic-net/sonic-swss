#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iostream>
#include <vector>

#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "zmqclient.h"
#include "zmqproducerstatetable.h"
#include <nlohmann/json.hpp>

using namespace std;
using namespace swss;
using json = nlohmann::json;

const char* const op_name            = "OP";
const char* const name_delimiter     = ":";
const int el_count = 2;

const string SWSS_CONFIG_DIR    = "/etc/swss/config.d/";

void usage()
{
    cout << "Usage: swssconfig [FILE...]" << endl;
    cout << "       (default config folder is /etc/swss/config.d/)" << endl;
}

void dump_db_item(KeyOpFieldsValuesTuple &db_item)
{
    SWSS_LOG_DEBUG("db_item: [");
    SWSS_LOG_DEBUG("\toperation: %s", kfvOp(db_item).c_str());
    SWSS_LOG_DEBUG("\thash: %s", kfvKey(db_item).c_str());
    SWSS_LOG_DEBUG("\tfields: [");
    for (auto fv: kfvFieldsValues(db_item))
        SWSS_LOG_DEBUG("\t\tfield: %s value: %s", fvField(fv).c_str(), fvValue(fv).c_str());
    SWSS_LOG_DEBUG("\t]");
    SWSS_LOG_DEBUG("]");
}

bool write_db_data(vector<KeyOpFieldsValuesTuple> &db_items)
{
    DBConnector db("APPL_DB", 0, false);
    RedisPipeline pipeline(&db); // dtor of RedisPipeline will automatically flush data
    unordered_map<string, ProducerStateTable*> table_map;

    // [Hua] test code, need improve to a parameter
    ZmqClient zmqClient("tcp://localhost:8100");
    SWSS_LOG_WARN("[Hua] write_db_data start.");

    for (auto &db_item : db_items)
    {
        dump_db_item(db_item);

        string key = kfvKey(db_item);
        size_t pos = key.find(name_delimiter);
        if ((string::npos == pos) || ((key.size() - 1) == pos))
        {
            SWSS_LOG_ERROR("Invalid formatted hash:%s\n", key.c_str());
            return false;
        }
        string table_name = key.substr(0, pos);
        string key_name = key.substr(pos + 1);

        auto findResult = table_map.find(table_name);
        ProducerStateTable* p_table= nullptr;
        if (findResult == table_map.end())
        {
            if (table_name == APP_ROUTE_TABLE_NAME) {
                p_table = new ZmqProducerStateTable(&pipeline, table_name, zmqClient, true);
            }
            else {
                p_table = new ProducerStateTable(&pipeline, table_name, true);
            }

            table_map.emplace(table_name, p_table);
        }
        else
        {
            p_table = findResult->second;
        }


        if (kfvOp(db_item) == SET_COMMAND)
            p_table->set(key_name, kfvFieldsValues(db_item), SET_COMMAND);
        else if (kfvOp(db_item) == DEL_COMMAND)
            p_table->del(key_name, DEL_COMMAND);
        else
        {
            SWSS_LOG_ERROR("Invalid operation: %s\n", kfvOp(db_item).c_str());
            return false;
        }
    }

    
    SWSS_LOG_WARN("[Hua] write_db_data end.");
    // [Hua] test code, need improve to a parameter
    // release tables
    for (const auto& table_item : table_map)
    {
        delete table_item.second;
    }

    return true;
}

bool load_json_db_data(ifstream &fs, vector<KeyOpFieldsValuesTuple> &db_items)
{
    json json_array;
    fs >> json_array;

    if (!json_array.is_array())
    {
        SWSS_LOG_ERROR("Root element must be an array.");
        return false;
    }

    for (size_t i = 0; i < json_array.size(); i++)
    {
        auto &arr_item = json_array[i];

        if (arr_item.is_object())
        {
            if (el_count != arr_item.size())
            {
                SWSS_LOG_ERROR("Child elements must have both key and op entry. %s",
                               arr_item.dump().c_str());
                return false;
            }

            db_items.push_back(KeyOpFieldsValuesTuple());
            auto &cur_db_item = db_items[db_items.size() - 1];

            for (json::iterator child_it = arr_item.begin(); child_it != arr_item.end(); child_it++) {
                auto cur_obj_key = child_it.key();
                auto &cur_obj = child_it.value();

                string field_str;
                string value_str;

                if (cur_obj.is_object()) {
                    kfvKey(cur_db_item) = cur_obj_key;
                    for (json::iterator cur_obj_it = cur_obj.begin(); cur_obj_it != cur_obj.end(); cur_obj_it++)
                    {
                        string field_str = cur_obj_it.key();
                        string value_str;
                        if ((*cur_obj_it).is_number())
                            value_str = to_string((*cur_obj_it).get<int>());
                        else if ((*cur_obj_it).is_string())
                            value_str = (*cur_obj_it).get<string>();
                        kfvFieldsValues(cur_db_item).push_back(FieldValueTuple(field_str, value_str));
                    }
                }
                else
                {
                    if (op_name != child_it.key())
                    {
                        SWSS_LOG_ERROR("Invalid entry. %s", arr_item.dump().c_str());
                        return false;
                    }
                    kfvOp(cur_db_item) = cur_obj.get<string>();
                 }
            }
        }
        else
        {
            SWSS_LOG_ERROR("Child elements must be objects. element:%s", arr_item.dump().c_str());
            return false;
        }
    }
    return true;
}

vector<string> read_directory(const string &path)
{
    vector<string> ret;
    DIR *dir;
    dirent *entry;
    dir = opendir(path.c_str());
    if (dir)
    {
        while ((entry = readdir(dir)))
        {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            {
                ret.push_back(path + string(entry->d_name));
            }
        }
        closedir(dir);
    }
    sort(ret.begin(), ret.end());
    return ret;
}

int main(int argc, char **argv)
{
    vector<string> files;
    if (argc == 1)
    {
        files = read_directory(SWSS_CONFIG_DIR);
    }
    if (argc == 2 && !strcmp(argv[1], "-h"))
    {
        usage();
        exit(EXIT_SUCCESS);
    }
    else
    {
        for (auto i = 1; i < argc; i++)
        {
            files.push_back(string(argv[i]));
        }
    }

    for (auto i : files)
    {
        SWSS_LOG_NOTICE("Loading config from JSON file:%s...", i.c_str());

        vector<KeyOpFieldsValuesTuple> db_items;
        try
        {
            ifstream fs(i);
            if (!fs)
            {
                SWSS_LOG_ERROR("Failed to open file %s", i.c_str());
                cerr << "Failed to open file " << i.c_str() << endl;
                return EXIT_FAILURE;
            }

            if (!load_json_db_data(fs, db_items))
            {
                SWSS_LOG_ERROR("Failed loading data from JSON file %s", i.c_str());
                return EXIT_FAILURE;
            }

            if (!write_db_data(db_items))
            {
                SWSS_LOG_ERROR("Failed applying data from JSON file %s", i.c_str());
                return EXIT_FAILURE;
            }
        }
        catch(const exception &e)
        {
            SWSS_LOG_ERROR("Exception caught: %s", e.what());
            cout << "Exception caught: " << e.what() << endl;
                return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}
