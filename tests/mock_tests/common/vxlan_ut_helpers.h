#pragma once

#include <string>

#include "sai.h"

namespace vxlan_ut_helpers
{
	void setUpVxlanPort(std::string vtep_ip_addr, sai_object_id_t vtep_obj_id);
	void setUpVxlanMember(std::string vtep_ip_addr, sai_object_id_t vtep_obj_id, std::string vlan);
}
