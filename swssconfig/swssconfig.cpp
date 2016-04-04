#include <stdlib.h>
#include <iostream>
#include <vector>
#include "logger.h"
#include <fstream>
#include "dbconnector.h"
#include "producertable.h"
#include "json.hpp"

using namespace std;
using namespace swss;
using json = nlohmann::json;

int db_port                          = 6379;
const char* const hostname           = "localhost";
const char* const op_name            = "OP";
const char* const name_delimiter     = ":";
const int el_count = 2;

#define _in_
#define _out_
#define _inout_

typedef struct _sonic_db_item_t {
    string op_val;
    string hash_name;
    std::vector<FieldValueTuple> fvVector;
}sonic_db_item_t;

void usage(char **argv)
{
    cout <<"Usage: " << argv[0] << " json_file_path\n";
}

void dump_db_item_cout(_in_ sonic_db_item_t &db_item)
{
    cout << "db_item [\n";
    cout << "operation: " << db_item.op_val << "\n";
    cout << "hash: " << db_item.hash_name << "\n";
    cout << "[\n";
    for(auto &fv: db_item.fvVector) {
        cout << "field: " << fvField(fv);
        cout << "value: " << fvValue(fv) << "\n";
    }
    cout << "]\n";    
    cout << "]\n";    
}
void dump_db_item(_in_ sonic_db_item_t &db_item)
{
    SWSS_LOG_NOTICE("db_item: [\n");
    SWSS_LOG_NOTICE("operation: %s", db_item.op_val.c_str());
    SWSS_LOG_NOTICE("hash: %s\n", db_item.hash_name.c_str());
    SWSS_LOG_NOTICE("fields: [\n");
    for(auto &fv: db_item.fvVector) {
        SWSS_LOG_NOTICE("field: %s", fvField(fv).c_str());
        SWSS_LOG_NOTICE("value: %s\n", fvValue(fv).c_str());
    }
    SWSS_LOG_NOTICE("]\n");    
    SWSS_LOG_NOTICE("]\n");
}

bool write_db_data(_in_ std::vector<sonic_db_item_t> &db_items)
{
    DBConnector db(APPL_DB, hostname, db_port, 0);
#ifdef _DUMPT_TO_COUT_
    for (sonic_db_item_t &db_item : db_items) {
        dump_db_item_cout(db_item);
    }
#endif //_DUMPT_TO_COUT_    
    for (sonic_db_item_t &db_item : db_items) {
        dump_db_item(db_item);

        std::size_t pos = db_item.hash_name.find(name_delimiter);
        if((string::npos == pos) || ((db_item.hash_name.size() - 1) == pos)) {
            SWSS_LOG_ERROR("Invalid formatted hash:%s\n", db_item.hash_name.c_str());
            return false;
        }
        string table_name = db_item.hash_name.substr(0, pos);
        string key_name = db_item.hash_name.substr(pos + 1);
        ProducerTable producer(&db, table_name);

        if(db_item.op_val == SET_COMMAND) {
            producer.set(key_name, db_item.fvVector, SET_COMMAND);
        }
        if(db_item.op_val == DEL_COMMAND) {
            producer.del(key_name, DEL_COMMAND);
        }                
    }
    return true;
}

bool load_json_db_data(
    _in_     std::iostream                  &fs, 
    _out_    std::vector<sonic_db_item_t>   &db_items)
{
    json json_array;
    fs >> json_array;
    if(!json_array.is_array()) {
        SWSS_LOG_ERROR("root element must be an array\n");
        return false;
    }

    for (size_t i = 0; i < json_array.size(); i++) {

        auto &arr_item = json_array[i];
        
        if(arr_item.is_object()) {
            if(el_count != arr_item.size()) {
                SWSS_LOG_ERROR("root element must be an array\n");
                return false;
            }
            
            db_items.push_back(sonic_db_item_t());
            sonic_db_item_t &cur_db_item = db_items[db_items.size() - 1];

            //
            // iterate over array items
            // each item must have following structure:
            //     {
            //         "OP":"SET/DEL",
            //        db_key_name {
            //            1*Nfield:value
            //        }
            //    }
            // 
            //
            for (json::iterator child_it = arr_item.begin(); child_it != arr_item.end(); ++child_it) {
                auto cur_obj_key = child_it.key();
                auto &cur_obj = child_it.value();

                string field_str;
                int val;
                string value_str;
                
                if(cur_obj.is_object()) {
                    cur_db_item.hash_name = cur_obj_key;
                    for (json::iterator cur_obj_it = cur_obj.begin(); cur_obj_it != cur_obj.end(); ++cur_obj_it) {
                        
                        field_str = cur_obj_it.key();
                        if((*cur_obj_it).is_number()) {
                            val = (*cur_obj_it).get<int>();
                            value_str = std::to_string(val);
                        }
                        if((*cur_obj_it).is_string()) {
                            value_str = (*cur_obj_it).get<string>();
                        }
                        cur_db_item.fvVector.push_back(FieldValueTuple(field_str, value_str));
                    }
                }
                else {
                    if(op_name != child_it.key()) {
                        SWSS_LOG_ERROR("Invalid entry. expected item:%s\n", op_name);
                        return false;
                    }
                    cur_db_item.op_val = cur_obj.get<std::string>();
                 }
            }
        }
        else {
            SWSS_LOG_WARN("Skipping processing of an array item which is not an object\n:%s", arr_item.dump().c_str());
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    Logger::setMinPrio(Logger::SWSS_DEBUG);

    if (argc != 2)
    {
        usage(argv);
        exit(EXIT_FAILURE);
    }
    std::vector<sonic_db_item_t> db_items;
    std::fstream fs;
    try {
        fs.open (argv[1], std::fstream::in | std::fstream::out | std::fstream::app);
        if(!load_json_db_data(fs, db_items)) {
            SWSS_LOG_ERROR("Failed loading data from json file\n");
            fs.close();
            return EXIT_FAILURE;
        }
        fs.close();
    }
    catch(...) {
        cout << "Failed loading json file: " << argv[1] << " Please refer to logs\n";
        return EXIT_FAILURE;
    }
    try {
        if(!write_db_data(db_items)) {
            SWSS_LOG_ERROR("Failed writing data to db\n");
            return EXIT_FAILURE;
        }        
    }
    catch(...) {
        cout << "Failed applying settings from json file: " << argv[1] << " Please refer to logs\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
