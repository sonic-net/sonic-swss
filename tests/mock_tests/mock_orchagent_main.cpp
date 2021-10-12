extern "C" {
#include "sai.h"
#include "saistatus.h"
}

#include "orchdaemon.h"

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
MacAddress gMacAddress;
MacAddress gVxlanMacAddress;

#define DEFAULT_BATCH_SIZE 128
int gBatchSize = DEFAULT_BATCH_SIZE;

bool gSairedisRecord = true;
bool gSwssRecord = true;
bool gLogRotate = false;
bool gSaiRedisLogRotate = false;
ofstream gRecordOfs;
string gRecordFile;
string gMySwitchType = "switch";
int32_t gVoqMySwitchId = 0;
string gMyHostName = "Linecard1";
string gMyAsicName = "Asic0";

VRFOrch *gVrfOrch;

void syncd_apply_view() {}
