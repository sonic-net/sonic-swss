#include "producerstatetable.h"

using namespace std;

namespace swss
{

ProducerStateTable::ProducerStateTable(RedisPipeline *pipeline, const string &tableName, bool buffered)
    : TableBase(tableName, SonicDBConfig::getSeparator(pipeline->getDBConnector())), TableName_KeySet(tableName), m_buffered(buffered)
    , m_pipeowned(false)
    , m_tempViewActive(false)
    , m_pipe(pipeline) {}

/* (DBConnector*, string) constructor for callers like BfdLink that don't
 * already own a RedisPipeline. The real implementation calls
 * reloadRedisScript() here to upload Lua scripts via SCRIPT LOAD, which
 * mock_hiredis returns as REDIS_REPLY_INTEGER while the real code expects
 * REDIS_REPLY_STRING — this type-mismatch crashed the original
 * tests_bfdsyncd fixture and was the reason the test was deleted in
 * c3896459. Skipping the script load is safe in tests because
 * mock_table.cpp overrides ProducerStateTable::set() and del() with
 * in-memory implementations that don't go through Redis at all. */
ProducerStateTable::ProducerStateTable(DBConnector *db, const string &tableName)
    : TableBase(tableName, SonicDBConfig::getSeparator(db))
    , TableName_KeySet(tableName)
    , m_buffered(false)
    , m_pipeowned(true)
    , m_tempViewActive(false)
    , m_pipe(new RedisPipeline(db, 1)) {}

ProducerStateTable::~ProducerStateTable()
{
    if (m_pipeowned)
    {
        delete m_pipe;
    }
}

}
