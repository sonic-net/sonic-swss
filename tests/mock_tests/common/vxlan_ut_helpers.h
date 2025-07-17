#include <string>

#include "sai.h"

using namespace std;

namespace vxlan_ut_helpers
{
	void setUpVxlanPort(string vtep_ip_addr, sai_object_id_t vtep_obj_id);
	void setUpVxlanMember(string vtep_ip_addr, sai_object_id_t vtep_obj_id, string vlan);
}
