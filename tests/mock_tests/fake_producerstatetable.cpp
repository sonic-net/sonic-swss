#include "producerstatetable.h"

using namespace std;

namespace swss
{

ProducerStateTable::ProducerStateTable(RedisPipeline *pipeline, const string &tableName, bool buffered)
    : TableBase(tableName, SonicDBConfig::getSeparator(pipeline->getDBConnector())), TableName_KeySet(tableName), m_buffered(buffered)
    , m_pipeowned(false)
    , m_tempViewActive(false)
    , m_pipe(pipeline) {}

ProducerStateTable::~ProducerStateTable() {}

}
