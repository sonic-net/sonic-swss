extern "C" {
#include <sai.h>
#include <saistatus.h>
}

#include "orchdaemon.h"

/* Global variables */
sai_object_id_t gVirtualRouterId;
sai_object_id_t gUnderlayIfId;
sai_object_id_t gSwitchId = SAI_NULL_OBJECT_ID;
MacAddress gMacAddress;
MacAddress gVxlanMacAddress;

string gMySwitchType = "switch";
string gMySwitchSubType = "SmartSwitch";
int32_t gVoqMySwitchId = 0;
string gMyHostName = "Linecard1";
string gMyAsicName = "Asic0";
bool gTraditionalFlexCounter = false;
bool gSyncMode = false;
sai_redis_communication_mode_t gRedisCommunicationMode = SAI_REDIS_COMMUNICATION_MODE_REDIS_ASYNC;
bool gOrchUnhealthy = false;
string gSaiErrorString;

VRFOrch *gVrfOrch;

void syncd_apply_view() {}
