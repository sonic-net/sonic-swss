#include "mock_sai_switch.h"

MockSaiSwitch *mock_sai_switch;

sai_status_t mock_get_switch_attribute(_In_ sai_object_id_t switch_id, _In_ uint32_t attr_count,
                                       _Inout_ sai_attribute_t *attr_list)
{
    return mock_sai_switch->get_switch_attribute(switch_id, attr_count, attr_list);
}

sai_status_t mock_set_switch_attribute(_In_ sai_object_id_t switch_id, _In_ const sai_attribute_t *attr)
{
    return mock_sai_switch->set_switch_attribute(switch_id, attr);
}
