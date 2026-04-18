#pragma once

extern "C" {
#include "sai.h"
#include "saiextensions.h"
}

void on_fdb_event(uint32_t count, sai_fdb_event_notification_data_t *data);
void on_port_state_change(uint32_t count, sai_port_oper_status_notification_t *data);
void on_bfd_session_state_change(uint32_t count, sai_bfd_session_state_notification_t *data);
void on_twamp_session_event(uint32_t count, sai_twamp_session_event_notification_data_t *data);
void on_ha_set_event(uint32_t count, sai_ha_set_event_data_t *data);
void on_ha_scope_event(uint32_t count, sai_ha_scope_event_data_t *data);
void on_flow_bulk_get_session_event(sai_object_id_t flow_bulk_session_id, uint32_t count, sai_flow_bulk_get_session_event_data_t *data);

// The function prototype information can be found here:
//      https://github.com/sonic-net/sonic-sairedis/blob/master/meta/NotificationSwitchShutdownRequest.cpp#L49
void on_switch_shutdown_request(sai_object_id_t switch_id);

void on_port_host_tx_ready(sai_object_id_t switch_id, sai_object_id_t port_id, sai_port_host_tx_ready_status_t m_portHostTxReadyStatus);

void on_switch_asic_sdk_health_event(sai_object_id_t switch_id,
                                     sai_switch_asic_sdk_health_severity_t severity,
                                     sai_timespec_t timestamp,
                                     sai_switch_asic_sdk_health_category_t category,
                                     sai_switch_health_data_t data,
                                     const sai_u8_list_t description);

void on_tam_tel_type_config_change(sai_object_id_t tam_tel_id);

void on_switch_macsec_post_status_notify(sai_object_id_t switch_id,
                                         sai_switch_macsec_post_status_t switch_macsec_post_status);
void on_macsec_post_status_notify(sai_object_id_t macsec_id,
                                  sai_macsec_post_status_t macsec_post_status);
