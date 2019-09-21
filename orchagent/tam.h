/*
 * Copyright 2019 Broadcom Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SWSS_TAM_H
#define SWSS_TAM_H

#include <string>
#include <map>
#include <unordered_map>
#include "orch.h"
#include "observer.h"
#include "portsorch.h"

class Tam 
{
public:
    Tam();
    ~Tam()
    {
    }

    /* SAI TAM handlers. */
    bool tamReportCreate(sai_tam_report_type_t report_type, sai_object_id_t *tam_report_obj);
    bool tamReportDelete(sai_object_id_t tam_report_obj);
    bool tamTelemetryTypeCreate(sai_object_id_t tam_report_obj,
                                sai_object_id_t *tam_telemetry_type_obj);   
    bool tamTelemetryTypeDelete(sai_object_id_t tam_telemetry_type_obj);
    bool tamTransportCreate(int dstPort, int srcPort, sai_object_id_t *tam_transport_obj);
    bool tamTransportDelete(sai_object_id_t tam_transport_obj);
    bool tamCollectorCreate(sai_ip_address_t src_ip_addr, sai_ip_address_t dst_ip_addr,
                         sai_object_id_t tam_transport_obj, sai_object_id_t *tam_collector_obj);
    bool tamCollectorDelete(sai_object_id_t tam_collector_obj);
    bool tamTelemetryCreate(int interval, sai_object_id_t tam_telemetry_type_obj,
                            sai_object_id_t tam_collector_obj, sai_object_id_t *tam_telemetry_obj);
    bool tamTelemetryDelete(sai_object_id_t tam_telemetry_obj);
    bool tamEventThresholdCreate(int threshold, sai_object_id_t *tam_event_threshold_obj);
    bool tamEventThresholdDelete(sai_object_id_t tam_event_threshold_obj);
    bool tamCreate(sai_tam_bind_point_type_t bind_point, sai_tam_attr_t attry_type,
                  sai_object_id_t tam_telemetry_event_obj, sai_object_id_t *tam_obj);
    bool tamDelete(sai_object_id_t tam_obj);
    bool tamEventActionCreate(sai_object_id_t tam_report_obj,
                                sai_object_id_t *tam_event_action_obj);
    bool tamEventActionDelete(sai_object_id_t tam_event_action_obj);
    bool tamEventCreate(sai_tam_event_type_t type, sai_object_id_t tam_event_action_obj, 
                        sai_object_id_t tam_collector_obj, sai_object_id_t tam_thd_obj, sai_object_id_t *tam_event_obj);
    bool tamEventDelete(sai_object_id_t tam_event_obj);
    bool tamIntCreate(uint32_t device_id, sai_object_id_t tam_report_obj,
                      bool sampling_enable, sai_object_id_t tam_sampling_obj,
                      bool collector_enable, sai_object_id_t tam_collector_obj,
                      sai_object_id_t *tam_int_obj);
    bool tamIntDelete(sai_object_id_t tam_int_obj);

private:

};
#endif /* SWSS_TAM_H */

