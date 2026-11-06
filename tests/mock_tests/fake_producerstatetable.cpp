#include "producerstatetable.h"

using namespace std;

namespace swss
{

ProducerStateTable::ProducerStateTable(DBConnector *db, const string &tableName)
    : TableBase(tableName, SonicDBConfig::getSeparator(db)), TableName_KeySet(tableName), m_buffered(false)
    , m_pipeowned(true)
    , m_tempViewActive(false)
    , m_pipe(new RedisPipeline(db, 1)) {}

ProducerStateTable::ProducerStateTable(RedisPipeline *pipeline, const string &tableName, bool buffered)
    : TableBase(tableName, SonicDBConfig::getSeparator(pipeline->getDBConnector())), TableName_KeySet(tableName), m_buffered(buffered)
    , m_pipeowned(false)
    , m_tempViewActive(false)
    , m_pipe(pipeline) {}

ProducerStateTable::~ProducerStateTable() {}

}
