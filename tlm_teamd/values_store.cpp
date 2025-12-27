#include <jansson.h>

#include <logger.h>
#include <table.h>

#include "values_store.h"

///
/// Extract port names from teamd status json dump.
/// @return vector of LAG member port names from the dump
///
std::vector<std::string> ValuesStore::get_ports(json_t * root)
{
    std::vector<std::string> result;
    json_t * ports = nullptr;
    int err = json_unpack(root, "{s:o}", "ports", &ports);
    if (err != 0)
    {
        return result;
    }

    const char * key;
    json_t * value;
    json_object_foreach(ports, key, value)
    {
        result.emplace_back(std::string(key));
    }

    return result;
}

///
/// Split the json path in format "key1.key2.key3" to the internal format
/// where all elements except last are put to the vector, and last key
/// extract separately.
/// @return a pair. The first element in the pair is the vector of the path elements
///         The second element in the pair is the last element of the path elements
///
std::pair<std::vector<std::string>, std::string> ValuesStore::convert_path(const std::string & path)
{
    size_t last = 0, next = 0;
    std::vector<std::string> result;
    while ((next = path.find('.', last)) != std::string::npos)
    {
        result.emplace_back(path.substr(last, next-last));
        last = next + 1;
    }
    return { result,  path.substr(last) };
}

///
/// Traverse parsed json structure to find correct object to extract the value
/// @param root parsed json structure
/// @param path_array path array as a first element from ValuesStore::convert_path()
/// @param path canonical path
///
json_t * ValuesStore::traverse(json_t * root, const std::vector<std::string> & path_array, const std::string & path)
{
    json_t * cur_root = root;
    for (const auto & key: path_array)
    {
        json_t * cur = nullptr;
        int err = json_unpack(cur_root, "{s:o}", key.c_str(), &cur);
        if (err != 0)
        {
            throw std::runtime_error("Can't traverse through the path '" + path + "'. not found key = " + key);
        }
        cur_root = cur;
    }

    return cur_root;
}

///
/// Extract json string value from json structure
/// @return extracted string
///
std::string ValuesStore::unpack_string(json_t * root, const std::string & key, const std::string & path)
{
    const auto & c_key = key.c_str();
    const char * str = nullptr;
    int err = json_unpack(root, "{s:s}", c_key, &str);
    if (err != 0)
    {
        throw std::runtime_error("Can't unpack a string. key='" + path + "' json='" + json_dumps(root, 0) + "'");
    }
    return std::string(str);
}

///
/// Extract json boolean value from json structure
/// @return extracted boolean as a string
///
std::string ValuesStore::unpack_boolean(json_t * root, const std::string & key, const std::string & path)
{
    const auto & c_key = key.c_str();
    int value;
    int err = json_unpack(root, "{s:b}", c_key, &value);
    if (err != 0)
    {
        throw std::runtime_error("Can't unpack a boolean. key='" + path + "' json='" + json_dumps(root, 0) + "'");
    }
    return std::string(value ? "true" : "false");
}

///
/// Extract json integer value from json structure
/// @return extracted integer as a string
///
std::string ValuesStore::unpack_integer(json_t * root, const std::string & key, const std::string & path)
{
    const auto & c_key = key.c_str();
    int value;
    int err = json_unpack(root, "{s:i}", c_key, &value);
    if (err != 0)
    {
        throw std::runtime_error("Can't unpack an integer. key='" + path + "' json='" + json_dumps(root, 0) + "'");
    }
    return std::to_string(value);
}

///
/// Extract a value from the parsed json. Path to the value is defined by path, and type of the value
/// is defined by type.
/// @param root a pointer to parsed json structure
/// @param path a canonical path to the value
/// @param type a type of the value
///
std::string ValuesStore::get_value(json_t * root, const std::string & path, ValuesStore::json_type type)
{
    auto path_pair = convert_path(path);

    json_t * found_object = traverse(root, path_pair.first, path);

    const auto & key = path_pair.second;

    switch (type)
    {
        case ValuesStore::json_type::string:  return unpack_string(found_object, key, path);
        case ValuesStore::json_type::boolean: return unpack_boolean(found_object, key, path);
        case ValuesStore::json_type::integer: return unpack_integer(found_object, key, path);
    }

    throw std::runtime_error("Reach the end of the ValuesStore::get_value. Path=" + path);
}

///
/// Extract values for LAG with name lag_name, from the parsed json tree with root, to the temporary storage
/// @param lag_name a name of the LAG
/// @param root a pointer to the parsed json tree
/// @param storage a reference to the temporary storage
///
//
void ValuesStore::extract_values_static(const std::string & lag_name, json_t * root, HashOfRecords & storage)
{

    const std::string key = "LAG_TABLE|" + lag_name;
    Records lag_values;

    for (const auto & p: m_lag_static_paths)
    {
        const auto & path = p.first;
        const auto & type = p.second;
        const auto & value = get_value(root, path, type);
        lag_values.emplace(path, value);
    }
    storage.emplace(key, lag_values);

    const auto & ports = get_ports(root);
    for (const auto & port: ports)
    {
        const std::string key = "LAG_MEMBER_TABLE|" + lag_name + "|" + port;
        Records member_values;
        for (const auto & p: m_member_static_paths)
        {
            const auto & path = p.first;
            const auto & type = p.second;
            const std::string full_path = "ports." + port + "." + path;
            const auto & value = get_value(root, full_path, type);
            member_values.emplace(path, value);
        }
        storage.emplace(key, member_values);
    }

    return;
}

void ValuesStore::extract_values(const std::string & lag_name, json_t * root, HashOfRecords & storage)
{

    const std::string key = "LAG_TABLE|" + lag_name;
    Records lag_values;

    bool is_static = false;

    for (const auto & p: m_lag_paths)
    {
        const auto & path = p.first;
	const auto & type = p.second;
	if (path  == "setup.runner_name") {
	    const auto & value = get_value(root, path, type);
	    if(value == "loadbalance" ||
               value == "roundrobin")
               is_static = true;
            break;	    
	}
    }

    if (is_static) {
        extract_values_static(lag_name, root, storage);
	return;    
    }

    for (const auto & p: m_lag_paths)
    {
        const auto & path = p.first;
        const auto & type = p.second;
        const auto & value = get_value(root, path, type);
        lag_values.emplace(path, value);
    }
    storage.emplace(key, lag_values);

    const auto & ports = get_ports(root);
    for (const auto & port: ports)
    {
        const std::string key = "LAG_MEMBER_TABLE|" + lag_name + "|" + port;
        Records member_values;
        for (const auto & p: m_member_paths)
        {
            const auto & path = p.first;
            const auto & type = p.second;
            const std::string full_path = "ports." + port + "." + path;
            const auto & value = get_value(root, full_path, type);
            member_values.emplace(path, value);
        }
        storage.emplace(key, member_values);
    }

    return;
}

///
/// Parse json from the data
/// @return a pointer to the parsed json tree
///
json_t * ValuesStore::load_json(const std::string & data)
{
    json_t * root;
    json_error_t error;
    root = json_loads(data.c_str(), 0, &error);
    if (!root)
    {
        throw std::runtime_error("Can't parse json dump = '" + data + "'");
    }

    return root;
}


///
/// Convert json input from all teamds to the temporary storage
/// @param dumps dumps from all teamds. It is a vector of pairs. Each pair
///              has a first element - name of the LAG and a second element
///              - json dump
/// @return temporary storage
///
HashOfRecords ValuesStore::from_json(const std::vector<StringPair> & dumps)
{
    HashOfRecords storage;
    for (const auto & p: dumps)
    {
        const auto & lag_name = p.first;
        const auto & json_dump = p.second;
        json_t * root = load_json(json_dump);
        extract_values(lag_name, root, storage);
        json_decref(root);
    }

    return storage;
}

///
/// Extract a list of stale keys from the storage.
/// The stale key is a key which a presented in the storage, but not presented
/// in the temporary storage. That means that the key must be removed
/// @param storage a reference to the temporary storage
/// @return list of stale keys
///
std::vector<std::string> ValuesStore::get_old_keys(const HashOfRecords & storage)
{
    std::vector<std::string> old_keys;
    for (const auto & p: m_storage)
    {
        const auto & db_key = p.first;
        if (storage.find(db_key) == storage.end())
        {
            old_keys.push_back(db_key);
        }
    }

    return old_keys;
}

///
/// Remove keys from vector keys from the storage
/// @param keys a list of keys to remove from the storage
///
void ValuesStore::remove_keys_storage(const std::vector<std::string> & keys)
{
    for (const auto & key: keys)
    {
        m_storage.erase(key);
    }
}

///
/// Split a full key to the database key and entry key
/// For example" TABLE|entry_key would return { "TABLE", "entry_key" }
/// @param key a database key.
/// @return a pair for keys
///
StringPair ValuesStore::split_key(const std::string & key)
{
    auto sep_pos = key.find('|');
    assert(sep_pos != std::string::npos);
    return std::make_pair(key.substr(0, sep_pos), key.substr(sep_pos + 1));
}

///
/// Remove keys from the db
/// @param keys a list of keys to remove
///
void ValuesStore::remove_keys_db(const std::vector<std::string> & keys)
{
    for (const auto & key: keys)
    {
        const auto & p = split_key(key);
        const auto & table_name = p.first;
        const auto & table_key = p.second;
        // Do not delete te key from State Db for table LAB_TABLE. LAB_TABLE entry is created/deleted
        // from teamsyncd on detecting netlink of teamd dev as up/down. For some reason 
        // if we do not get state dump from teamdctl it might be transient issue. If it is 
        // persistent issue then teamsyncd might be able to catch it and delete state db entry
        // or we can keep entry in it's current state as best effort. Similar to try_add_lag which is best effort
        // to connect to teamdctl and if it fails we do not delete State Db entry.
        if (table_name == "LAG_TABLE")
            continue;
        swss::Table table(m_db, table_name);
        table.del(table_key);
    }
}

///
/// Update the storage with values from the temporary storage
/// The update is the following:
/// 1. For each key in the temporary storage we check that we have that key in the storage
/// 2. if not, we insert the key and value to the storage
/// 3. if yes, we check that value of the key is not changed. If the value is changed we
///    replace that value with the value from the temporary storage
/// This method returns a list of keys which should be updated in the database
/// @param storage the temporary storage
/// @retorun a list of keys which must be updated in the storage
///
std::vector<std::string> ValuesStore::update_storage(const HashOfRecords & storage)
{
    std::vector<std::string> to_update;

    for (const auto & entry_pair: storage)
    {
        const auto & entry_key    = entry_pair.first;
        const auto & entry_values = entry_pair.second;
        if (m_storage.find(entry_key) == m_storage.end())
        {
            m_storage.emplace(entry_pair);
            to_update.emplace_back(entry_key);
        }
        else
        {
            bool is_changed = false;
            for (const auto & row_pair: entry_values)
            {
                const auto & row_key   = row_pair.first;
                const auto & row_value = row_pair.second;
                if (m_storage[entry_key][row_key] != row_value)
                {
                    is_changed = true;
                    break;
                }
            }

            if (is_changed)
            {
                m_storage.erase(entry_key);
                m_storage.emplace(entry_pair);
                to_update.emplace_back(entry_key);
            }
        }
    }

    return to_update;
}

///
/// Update values in the db with values from the temporary storage
/// @param storage a reference to the temporary storage
/// @param keys_to_refresh a list of keys which must be refreshed in the db
///
void ValuesStore::update_db(const HashOfRecords & storage, const std::vector<std::string> & keys_to_refresh)
{
    for (const auto & key: keys_to_refresh)
    {
        std::vector<swss::FieldValueTuple> fvp;
        for (const auto & row_pair: storage.at(key))
        {
            fvp.emplace_back(row_pair);
        }
        const auto & table_pair = split_key(key);
        swss::Table table(m_db, table_pair.first);
        table.set(table_pair.second, fvp);
    }
}


///
/// Update the storage with json dumps for every registered LAG interface.
///
void ValuesStore::update(const std::vector<StringPair> & dumps)
{
    try
    {
        const auto & storage = from_json(dumps);
        const auto & old_keys = get_old_keys(storage);
        remove_keys_db(old_keys);
        remove_keys_storage(old_keys);
        const auto & keys_to_refresh = update_storage(storage);
        update_db(storage, keys_to_refresh);
    }
    catch (const std::exception & e)
    {
        SWSS_LOG_WARN("Exception '%s' had been thrown in ValuesStore", e.what());
    }
}
