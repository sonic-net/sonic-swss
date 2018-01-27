#include <fstream>
#include <iostream>
#include <string.h>
#include "logger.h"
#include "dbconnector.h"
#include "producerstatetable.h"
#include "tokenize.h"
#include "ipprefix.h"
#include "buffermgr.h"
#include "exec.h"
#include "shellcmd.h"

using namespace std;
using namespace swss;

BufferMgr::BufferMgr(DBConnector *cfgDb, DBConnector *stateDb, string pg_lookup_file, const vector<string> &tableNames) :
        Orch(cfgDb, tableNames),
        m_statePortTable(stateDb, STATE_PORT_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgPortTable(cfgDb, CFG_PORT_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgCableLenTable(cfgDb, CFG_PORT_CABLE_LEN_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgBufferProfileTable(cfgDb, CFG_BUFFER_PROFILE_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgBufferPgTable(cfgDb, CFG_BUFFER_PG_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR),
        m_cfgLosslessPgPoolTable(cfgDb, CFG_BUFFER_POOL_TABLE_NAME, CONFIGDB_TABLE_NAME_SEPARATOR)
{
    readPgProfileLookupFile(pg_lookup_file);
}

//# speed, cable, size,    xon,  xoff, threshold
//  40000   5m    34816   18432  16384   1
void BufferMgr::readPgProfileLookupFile(string file)
{
    SWSS_LOG_NOTICE("Read lookup configuration file...");

    ifstream infile(file);
    if (!infile.is_open())
    {
        return;
    }

    string line;
    while (getline(infile, line))
    {
        if (line.empty() || (line.at(0) == '#'))
        {
            continue;
        }

        istringstream iss(line);
        string speed, cable;

        iss >> speed;
        iss >> cable;
        iss >> m_pgProfileLookup[speed][cable].size;
        iss >> m_pgProfileLookup[speed][cable].xon;
        iss >> m_pgProfileLookup[speed][cable].xoff;
        iss >> m_pgProfileLookup[speed][cable].threshold;

        SWSS_LOG_NOTICE("PG profile for speed %s and cable %s is: size:%s, xon:%s xoff:%s th:%s",
                       speed.c_str(), cable.c_str(),
                       m_pgProfileLookup[speed][cable].size.c_str(),
                       m_pgProfileLookup[speed][cable].xon.c_str(),
                       m_pgProfileLookup[speed][cable].xoff.c_str(),
                       m_pgProfileLookup[speed][cable].threshold.c_str()
                       );
    }

    infile.close();
}

void BufferMgr::doCableTask(string port, string cable_length)
{
    m_cableLenLookup[port] = cable_length;
}

string BufferMgr::getPgPoolMode()
{
    vector<FieldValueTuple> pool_properties;
    m_cfgLosslessPgPoolTable.get(INGRESS_LOSSLESS_PG_POOL_NAME, pool_properties);
    for (auto& prop : pool_properties)
    {
        if (fvField(prop) == "mode")
            return fvValue(prop);
    }
    return "";
}

/*
Create/update two tables: profile (in m_cfgBufferProfileTable) and port buffer (in m_cfgBufferPgTable):

    "BUFFER_PROFILE": {
        "pg_lossless_100G_300m_profile": {
            "pool":"[BUFFER_POOL_TABLE:ingress_lossless_pool]",
            "xon":"18432",
            "xoff":"165888",
            "size":"184320",
            "dynamic_th":"1"
        }
    }
    "BUFFER_PG" :{
        Ethernet44|3-4": {
            "profile" : "[BUFFER_PROFILE:pg_lossless_100000_300m_profile]"
        }
    }
*/
void BufferMgr::doSpeedUpdateTask(string port, string speed)
{
    vector<FieldValueTuple> fvVector;
    string cable;

    if (m_cableLenLookup.count(port) == 0)
    {
        SWSS_LOG_ERROR("Unable to create/update PG profile for port %s. Cable length is not set", port.c_str());
        return;
    }

    cable = m_cableLenLookup[port];

    if (m_pgProfileLookup.count(speed) == 0 || m_pgProfileLookup[speed].count(cable) == 0)
    {
        SWSS_LOG_ERROR("Unable to create/update PG profile for port %s. No PG profile configured for speed %s and cable length %s",
                       port.c_str(), speed.c_str(), cable.c_str());
        return;
    }

    // Crete record in BUFFER_PROFILE table
    // key format is pg_lossless_<speed>_<cable>_profile
    string buffer_pg_key = port + CONFIGDB_TABLE_NAME_SEPARATOR + LOSSLESS_PGS;
    string buffer_profile_key = "pg_lossless_" + speed + "_" + cable + "_profile";

    // check if profile already exists - if yes - skip creation
    m_cfgBufferProfileTable.get(buffer_profile_key, fvVector);
    if (fvVector.size() == 0)
    {
        SWSS_LOG_NOTICE("Creating new profile '%s'", buffer_profile_key.c_str());

        string mode = getPgPoolMode();
        if (mode.empty())
        {
            // this should never happen if switch initialized properly
            SWSS_LOG_ERROR("PG lossless pool is not yet created");
            return;
        }

        // profile threshold field name
        mode += "_th";
        string pg_pool_reference = string(CFG_BUFFER_POOL_TABLE_NAME) + CONFIGDB_TABLE_NAME_SEPARATOR + INGRESS_LOSSLESS_PG_POOL_NAME;
        fvVector.push_back(make_pair("pool", "[" + pg_pool_reference + "]"));
        fvVector.push_back(make_pair("xon", m_pgProfileLookup[speed][cable].xon));
        fvVector.push_back(make_pair("xoff", m_pgProfileLookup[speed][cable].xoff));
        fvVector.push_back(make_pair("size", m_pgProfileLookup[speed][cable].size));
        fvVector.push_back(make_pair(mode, m_pgProfileLookup[speed][cable].threshold));
        m_cfgBufferProfileTable.set(buffer_profile_key, fvVector);
    }
    else
    {
        SWSS_LOG_NOTICE("Reusing existing profile '%s'", buffer_profile_key.c_str());
    }

    fvVector.clear();
    string profile_ref = string("[") + CFG_BUFFER_PROFILE_TABLE_NAME + CONFIGDB_TABLE_NAME_SEPARATOR + buffer_profile_key + "]";
    fvVector.push_back(make_pair("profile", profile_ref));
    m_cfgBufferPgTable.set(buffer_pg_key, fvVector);
}

void BufferMgr::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    string table_name = consumer.getTableName();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string keySeparator = CONFIGDB_KEY_SEPARATOR;
        vector<string> keys = tokenize(kfvKey(t), keySeparator[0]);
        string port(keys[0]);

        string op = kfvOp(t);
        if (op == SET_COMMAND)
        {
            for (auto i : kfvFieldsValues(t))
            {
                if (table_name == CFG_PORT_CABLE_LEN_TABLE_NAME)
                {
                    // receive and cache cable length table
                    doCableTask(fvField(i), fvValue(i));
                }
                // In case of PORT table update, Buffer Manager is interested in speed update only
                if (table_name == CFG_PORT_TABLE_NAME && fvField(i) == "speed")
                {
                    // create/update profile for port
                    doSpeedUpdateTask(port, fvValue(i));
                }
            }
        }

        it = consumer.m_toSync.erase(it);
        continue;
    }
}
