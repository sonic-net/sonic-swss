#ifndef MOCK_SAI_STP_H
#define MOCK_SAI_STP_H

#include <gmock/gmock.h>
extern "C"
{
#include "sai.h"
}
    
// Mock class for SAI STP APIs
class MockSaiStp {
public:
    // Mock method for creating an STP instance
    MOCK_METHOD4(create_stp,
                sai_status_t(_Out_ sai_object_id_t *stp_instance_id, 
                             _In_ sai_object_id_t switch_id, 
                             _In_ uint32_t attr_count, 
                             _In_ const sai_attribute_t *attr_list));

    // Mock method for removing an STP instance
    MOCK_METHOD1(remove_stp, sai_status_t(_In_ sai_object_id_t stp_instance_id));

    // Mock method for setting STP instance attributes
    MOCK_METHOD2(set_stp_attribute,
                sai_status_t(_In_ sai_object_id_t stp_instance_id, 
                             _In_ const sai_attribute_t *attr));

    // Mock method for getting STP instance attributes
    MOCK_METHOD3(get_stp_attribute,
                sai_status_t(_Out_ sai_object_id_t stp_instance_id, 
                             _In_ uint32_t attr_count, 
                             _In_ sai_attribute_t *attr_list));

    // Mock method for creating an STP port
    MOCK_METHOD4(create_stp_port,
                sai_status_t(_Out_ sai_object_id_t *stp_port_id, 
                             _In_ sai_object_id_t switch_id, 
                             _In_ uint32_t attr_count, 
                             _In_ const sai_attribute_t *attr_list));

    // Mock method for removing an STP port
    MOCK_METHOD1(remove_stp_port,
                sai_status_t(_In_ sai_object_id_t stp_port_id));

    // Mock method for setting STP port attributes
    MOCK_METHOD2(set_stp_port_attribute,
                sai_status_t(_Out_ sai_object_id_t stp_port_id, 
                             _In_ const sai_attribute_t *attr));

    // Mock method for getting STP port attributes
    MOCK_METHOD3(get_stp_port_attribute,
                sai_status_t(_Out_ sai_object_id_t stp_port_id, 
                            _In_ uint32_t attr_count,
                            _In_ sai_attribute_t *attr_list));
};

// Global mock object for SAI STP APIs
MockSaiStp *mock_sai_stp;

sai_status_t mock_create_stp(_Out_ sai_object_id_t *stp_instance_id, 
                        _In_ sai_object_id_t switch_id, 
                        _In_ uint32_t attr_count, 
                        _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_stp->create_stp(stp_instance_id, switch_id, attr_count, attr_list);
}

sai_status_t mock_remove_stp(_In_ sai_object_id_t stp_instance_id)
{
    return mock_sai_stp->remove_stp(stp_instance_id);
}

sai_status_t mock_set_stp_attribute(_In_ sai_object_id_t stp_instance_id, _In_ const sai_attribute_t *attr)
{
    return mock_sai_stp->set_stp_attribute(stp_instance_id, attr);
}

sai_status_t mock_get_stp_attribute(_Out_ sai_object_id_t stp_instance_id, 
                            _In_ uint32_t attr_count, _Inout_ sai_attribute_t *attr_list)
{
    return mock_sai_stp->get_stp_attribute(stp_instance_id, attr_count, attr_list);
}
sai_status_t mock_create_stp_port(_Out_ sai_object_id_t *stp_port_id, 
                             _In_ sai_object_id_t switch_id, 
                             _In_ uint32_t attr_count, 
                             _In_ const sai_attribute_t *attr_list)
{
    return mock_sai_stp->create_stp_port(stp_port_id, switch_id,attr_count, attr_list);
}

sai_status_t mock_remove_stp_port(_In_ sai_object_id_t stp_port_id)
{
    return mock_sai_stp->remove_stp_port(stp_port_id);
}

sai_status_t mock_set_stp_port_attribute(_In_ sai_object_id_t stp_port_id, 
                             _In_ const sai_attribute_t *attr)
{
    return mock_sai_stp->set_stp_port_attribute(stp_port_id, attr);
}

sai_status_t mock_get_stp_port_attribute(_Out_ sai_object_id_t stp_port_id, 
                                    _In_ uint32_t attr_count,
                                    _Inout_ sai_attribute_t *attr_list)            
{
    return mock_sai_stp->get_stp_port_attribute(stp_port_id, attr_count, attr_list);
}

#endif // MOCK_SAI_STP_H

