#ifndef SWSS_LAGID_H
#define SWSS_LAGID_H

#include "dbconnector.h"
#include "sal.h"
#include "schema.h"
#include "redisapi.h"

using namespace swss;
using namespace std;

#define LAG_ID_ALLOCATOR_ERROR_DELETE_ENTRY_NOT_FOUND  0
#define LAG_ID_ALLOCATOR_ERROR_TABLE_FULL             -1
#define LAG_ID_ALLOCATOR_ERROR_GET_ENTRY_NOT_FOUND    -2
#define LAG_ID_ALLOCATOR_ERROR_INVALID_OP             -3
#define LAG_ID_ALLOCATOR_ERROR_DB_ERROR               -4

class LagIdAllocator
{
public:

    LagIdAllocator(
            _In_ DBConnector* chassis_app_db);

public:

    int32_t lagIdAdd(
            _In_ const string &pcname,
            _In_ int32_t lag_id);

    int32_t lagIdDel(
            _In_ const string &pcname);

    int32_t lagIdGet(
            _In_ const string &pcname);

private:

    DBConnector* m_dbConnector;

    string m_shaLagId;
};

#endif // SWSS_LAGID_H
