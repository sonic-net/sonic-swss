/*
 * Copyright 2019-2020 Broadcom Inc.
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

#include "phyparser.h"

bool
PhyParser::parse()
{
  json root;

  try {
    root = getJSONRoot();
  } catch (const std::exception& e) {
    SWSS_LOG_ERROR("JSON root not parseable");
    return false;
  }

  if (false) {
    return false;
  }

  std::vector<swss::FieldValueTuple> attrs;
  swss::FieldValueTuple attr;
  json val, vals, ports, port, lanes, lane;
  std::string valsStr;

  try {
    lanes = root["lanes"];
    if (lanes.size() == 0) {
      SWSS_LOG_ERROR("zero-sized 'lanes' field in phy configuration");
      return false;
    }
  } catch (const std::exception& e) {
      SWSS_LOG_ERROR("unable to read phy configuration (invalid format)");
      return false;
  }
  for (uint32_t iter = 0; iter < lanes.size(); iter++) {
    lane = lanes[iter];
    try {
      attrs.clear();

       /* index */
       if (lane.find("index") == lane.end()) {
         SWSS_LOG_ERROR("missing 'index' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["index"];
       int id = val.get<int>();
       attr = std::make_pair("index", std::to_string(id));
       attrs.push_back(attr);

       /* system_side */
       if (lane.find("system_side") == lane.end()) {
         SWSS_LOG_ERROR("missing 'system_side' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["system_side"];
       if (val.get<bool>() != false && val.get<bool>() != true) {
         SWSS_LOG_ERROR("not a boolean: 'system_side' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       std::string systemSideStr = val.get<bool>() == true ? "true" : "false";
       attr = std::make_pair("system_side", systemSideStr);
       attrs.push_back(attr);

       /* local_lane_id */
       if (lane.find("local_lane_id") == lane.end()) {
         SWSS_LOG_ERROR("missing 'local_lane_id' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["local_lane_id"];
       attr = std::make_pair("local_lane_id", std::to_string(val.get<int>()));
       attrs.push_back(attr);

       /* tx_polarity */
       if (lane.find("tx_polarity") == lane.end()) {
         SWSS_LOG_ERROR("missing 'tx_polarity' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["tx_polarity"];
       attr = std::make_pair("tx_polarity", std::to_string(val.get<int>()));
       attrs.push_back(attr);

       /* rx_polarity */
       if (lane.find("rx_polarity") == lane.end()) {
         SWSS_LOG_ERROR("missing 'rx_polarity' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["rx_polarity"];
       attr = std::make_pair("rx_polarity", std::to_string(val.get<int>()));
       attrs.push_back(attr);

       /* line_tx_lanemap */
       if (lane.find("line_tx_lanemap") == lane.end()) {
         SWSS_LOG_ERROR("missing 'line_tx_lanemap' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["line_tx_lanemap"];
       attr = std::make_pair("line_tx_lanemap", std::to_string(val.get<int>()));
       attrs.push_back(attr);

       /* line_rx_lanemap */
       if (lane.find("line_rx_lanemap") == lane.end()) {
         SWSS_LOG_ERROR("missing 'line_rx_lanemap' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["line_rx_lanemap"];
       attr = std::make_pair("line_rx_lanemap", std::to_string(val.get<int>()));
       attrs.push_back(attr);

       /* line_to_system_lanemap */
       if (lane.find("line_to_system_lanemap") == lane.end()) {
         SWSS_LOG_ERROR("missing 'line_to_system_lanemap' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["line_to_system_lanemap"];
       attr = std::make_pair("line_to_system_lanemap", std::to_string(val.get<int>()));
       attrs.push_back(attr);

       /* mdio_addr */
       if (lane.find("mdio_addr") == lane.end()) {
         SWSS_LOG_ERROR("missing 'mdio_addr' field of item %d of 'lanes' field in phy configuration", iter);
         return false;
       }
       val = lane["mdio_addr"];
       attr = std::make_pair("mdio_addr", val.get<std::string>());
       attrs.push_back(attr);

       std::string key;
       key = "phy:" + std::to_string(getPhyId()) + ":lanes:" + std::to_string(id);
       if (getWriteToDb() == true) {
         writeToDb(key, attrs);
       }
     } catch (const std::exception& e) {
       SWSS_LOG_ERROR("unable to read lanes configuration item %d (invalid format)", iter);
       return false;
     }
  }
  if (root.find("ports") != root.end()) {
    ports = root["ports"];
    if (ports.size() == 0) {
      SWSS_LOG_ERROR("zero-sized 'ports' field in phy configuration");
      return false;
    }
    for (uint32_t iter = 0; iter < ports.size(); iter++) {
      port = ports[iter];
      try {
         attrs.clear();
         swss::FieldValueTuple attr;

         /* index */
         if (port.find("index") == port.end()) {
           SWSS_LOG_ERROR("missing 'index' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["index"];
         int id = val.get<int>();
         attr = std::make_pair("index", std::to_string(id));
         attrs.push_back(attr);

         /* mdio_addr */
         if (port.find("mdio_addr") == port.end()) {
           SWSS_LOG_ERROR("missing 'mdio_addr' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["mdio_addr"];
         attr = std::make_pair("mdio_addr", val.get<std::string>());
         attrs.push_back(attr);

         /* system_speed */
         if (port.find("system_speed") == port.end()) {
           SWSS_LOG_ERROR("missing 'system_speed' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["system_speed"];
         attr = std::make_pair("system_speed", std::to_string(val.get<int>()));
         attrs.push_back(attr);

         /* system_fec */
         if (port.find("system_fec") == port.end()) {
           SWSS_LOG_ERROR("missing 'system_fec' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["system_fec"];
         attr = std::make_pair("system_fec", val.get<std::string>());
         attrs.push_back(attr);

         /* system_auto_neg */
         if (port.find("system_auto_neg") == port.end()) {
           SWSS_LOG_ERROR("missing 'system_auto_neg' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["system_auto_neg"];
         if (val.get<bool>() != false && val.get<bool>() != true) {
       	  SWSS_LOG_ERROR("not a boolean: 'system_auto_neg' field of item %d of 'ports' field in phy configuration", iter);
       	  return false;
         }
         std::string systemAutoNegStr = val.get<bool>() == true ? "true" : "false";
         attr = std::make_pair("system_auto_neg", systemAutoNegStr);
         attrs.push_back(attr);

         /* system_loopback */
         if (port.find("system_loopback") == port.end()) {
           SWSS_LOG_ERROR("missing 'system_loopback' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["system_loopback"];
         attr = std::make_pair("system_loopback", val.get<std::string>());
         attrs.push_back(attr);

         /* system_training */
         if (port.find("system_training") == port.end()) {
           SWSS_LOG_ERROR("missing 'system_training' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["system_training"];
         if (val.get<bool>() != false && val.get<bool>() != true) {
       	  SWSS_LOG_ERROR("not a boolean: 'system_training' field of item %d of 'ports' field in phy configuration", iter);
       	  return false;
         }
         std::string systemTrainingStr = val.get<bool>() == true ? "true" : "false";
         attr = std::make_pair("system_training", systemTrainingStr);
         attrs.push_back(attr);

         /* line_speed */
         if (port.find("line_speed") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_speed' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_speed"];
         attr = std::make_pair("line_speed", std::to_string(val.get<int>()));
         attrs.push_back(attr);

         /* line_fec */
         if (port.find("line_fec") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_fec' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_fec"];
         attr = std::make_pair("line_fec", val.get<std::string>());
         attrs.push_back(attr);

         /* line_auto_neg */
         if (port.find("line_auto_neg") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_auto_neg' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_auto_neg"];
         if (val.get<bool>() != false && val.get<bool>() != true) {
       	  SWSS_LOG_ERROR("not a boolean: 'line_auto_neg' field of item %d of 'ports' field in phy configuration", iter);
       	  return false;
         }
         std::string lineAutoNegStr = val.get<bool>() == true ? "true" : "false";
         attr = std::make_pair("line_auto_neg", lineAutoNegStr);
         attrs.push_back(attr);

         /* line_media_type */
         if (port.find("line_media_type") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_media_type' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_media_type"];
         attr = std::make_pair("line_media_type", val.get<std::string>());
         attrs.push_back(attr);

         /* line_intf_type */
         if (port.find("line_intf_type") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_intf_type' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_intf_type"];
         attr = std::make_pair("line_intf_type", val.get<std::string>());
         attrs.push_back(attr);

         /* line_loopback */
         if (port.find("line_loopback") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_loopback' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_loopback"];
         attr = std::make_pair("line_loopback", val.get<std::string>());
         attrs.push_back(attr);

         /* line_training */
         if (port.find("line_training") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_training' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_training"];
         if (val.get<bool>() != false && val.get<bool>() != true) {
       	  SWSS_LOG_ERROR("not a boolean: 'line_training' field of item %d of 'ports' field in phy configuration", iter);
       	  return false;
         }
         std::string lineTrainingStr = val.get<bool>() == true ? "true" : "false";
         attr = std::make_pair("line_training", lineTrainingStr);
         attrs.push_back(attr);

         /* line_adver_speed */
         if (port.find("line_adver_speed") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_adver_speed' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         vals = port["line_adver_speed"]; // vec
         valsStr = "";
         for (uint32_t iter1 = 0; iter1 < vals.size(); iter1++) {
       	  val = vals[iter1];
       	  if (valsStr.length() > 0) {
       		  valsStr += ",";
          }
       	  valsStr += std::to_string(val.get<int>());
         }
         attr = std::make_pair("line_adver_speed", valsStr);
         attrs.push_back(attr);

         /* line_adver_fec */
         if (port.find("line_adver_fec") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_adver_fec' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         vals = port["line_adver_fec"]; // vec
         valsStr = "";
         for (uint32_t iter1 = 0; iter1 < vals.size(); iter1++) {
       	  val = vals[iter1];
       	  if (valsStr.length() > 0) {
       		  valsStr += ",";
       	  }
       	  valsStr += std::to_string(val.get<int>());
         }
         attr = std::make_pair("line_adver_fec", valsStr);
         attrs.push_back(attr);

         /* line_adver_auto_neg */
         if (port.find("line_adver_auto_neg") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_adver_auto_neg' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_adver_auto_neg"];
         if (val.get<bool>() != false && val.get<bool>() != true) {
       	  SWSS_LOG_ERROR("not a boolean: 'line_adver_auto_neg' field of item %d of 'ports' field in phy configuration", iter);
       	  return false;
         }
         std::string lineAdverAutoNegStr = val.get<bool>() == true ? "true" : "false";
         attr = std::make_pair("line_adver_auto_neg", lineAdverAutoNegStr);
         attrs.push_back(attr);

         /* line_adver_asym_pause */
         if (port.find("line_adver_asym_pause") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_adver_asym_pause' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_adver_asym_pause"];
         if (val.get<bool>() != false && val.get<bool>() != true) {
       	  SWSS_LOG_ERROR("not a boolean: 'line_adver_asym_pause' field of item %d of 'ports' field in phy configuration", iter);
       	  return false;
         }
         std::string lineAdverAsymPauseStr = val.get<bool>() == true ? "true" : "false";
         attr = std::make_pair("line_adver_asym_pause", lineAdverAsymPauseStr);
         attrs.push_back(attr);

         /* line_adver_media_type */
         if (port.find("line_adver_media_type") == port.end()) {
           SWSS_LOG_ERROR("missing 'line_adver_media_type' field of item %d of 'ports' field in phy configuration", iter);
           return false;
         }
         val = port["line_adver_media_type"];
         attr = std::make_pair("line_adver_media_type", val.get<std::string>());
         attrs.push_back(attr);

         std::string key;
         int phyId = getPhyId();
         key = "phy:" + std::to_string(phyId) + ":ports:" + std::to_string(id);
         if (getWriteToDb() == true) {
           writeToDb(key, attrs);
         }
       } catch (const std::exception& e) {
           SWSS_LOG_ERROR("unable to read ports configuration item %d (invalid format): %s", iter, e.what());
           return false;
       }
    }
  } else {
    SWSS_LOG_ERROR("missing 'ports' field in phy configuration");
    return false;
  }
  return true;
}
