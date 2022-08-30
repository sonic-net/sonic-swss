#include "orch.h"
#include "nvgreorch.h"
#include "request_parser.h"
#include "swssnet.h"
#include "directory.h"

#define NVGRE_VSID_MAX_VALUE 16777214

extern Directory<Orch*> gDirectory;
extern PortsOrch*       gPortsOrch;
extern sai_object_id_t  gSwitchId;
extern sai_object_id_t  gUnderlayIfId;
extern sai_object_id_t  gVirtualRouterId;
extern sai_tunnel_api_t *sai_tunnel_api;

static const std::vector<map_type_t> nvgreMapTypes = {
    MAP_T_VLAN,
    MAP_T_BRIDGE
};

static const std::map<map_type_t, sai_tunnel_map_type_t> nvgreEncapTunnelMap = {
    { MAP_T_VLAN, SAI_TUNNEL_MAP_TYPE_VLAN_ID_TO_VSID },
    { MAP_T_BRIDGE, SAI_TUNNEL_MAP_TYPE_BRIDGE_IF_TO_VSID }
};

static inline sai_tunnel_map_type_t get_encap_nvgre_mapper(map_type_t map)
{
    return nvgreEncapTunnelMap.at(map);
}

static const std::map<map_type_t, sai_tunnel_map_type_t> nvgreDecapTunnelMap = {
    { MAP_T_VLAN, SAI_TUNNEL_MAP_TYPE_VSID_TO_VLAN_ID },
    { MAP_T_BRIDGE, SAI_TUNNEL_MAP_TYPE_VSID_TO_BRIDGE_IF }
};

static inline sai_tunnel_map_type_t get_decap_nvgre_mapper(map_type_t map)
{
    return nvgreDecapTunnelMap.at(map);
}

static const map<map_type_t, std::pair<sai_tunnel_map_entry_attr_t, sai_tunnel_map_entry_attr_t>> nvgreEncapTunnelMapKeyVal =
{
    { MAP_T_VLAN,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VSID_ID_VALUE }
    },
    { MAP_T_BRIDGE,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_BRIDGE_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VSID_ID_VALUE }
    }
};

static inline sai_tunnel_map_entry_attr_t get_encap_nvgre_map_key(map_type_t map)
{
    return nvgreEncapTunnelMapKeyVal.at(map).first;
}

static inline sai_tunnel_map_entry_attr_t get_encap_nvgre_map_val(map_type_t map)
{
    return nvgreEncapTunnelMapKeyVal.at(map).second;
}

static const map<map_type_t, std::pair<sai_tunnel_map_entry_attr_t, sai_tunnel_map_entry_attr_t>> nvgreDecapTunnelMapKeyVal =
{
    { MAP_T_VLAN,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VSID_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_VLAN_ID_VALUE }
    },
    { MAP_T_BRIDGE,
        { SAI_TUNNEL_MAP_ENTRY_ATTR_VSID_ID_KEY, SAI_TUNNEL_MAP_ENTRY_ATTR_BRIDGE_ID_VALUE }
    }
};

static inline sai_tunnel_map_entry_attr_t get_decap_nvgre_map_key(map_type_t map)
{
    return nvgreDecapTunnelMapKeyVal.at(map).first;
}

static inline sai_tunnel_map_entry_attr_t get_decap_nvgre_map_val(map_type_t map)
{
    return nvgreDecapTunnelMapKeyVal.at(map).second;
}

/** @brief Creates tunnel mapper in SAI.
 *
 *  @param sai_tunnel_map_type SAI tunnel map type e.g. VSID_TO_VLAN
 *
 *  @return Tunnel map SAI identifier.
 */
sai_object_id_t NvgreTunnel::sai_create_tunnel_map(sai_tunnel_map_type_t sai_tunnel_map_type)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_attrs;

    attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
    attr.value.u32 = sai_tunnel_map_type;

    tunnel_map_attrs.push_back(attr);

    sai_object_id_t tunnel_map_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_map(
                                &tunnel_map_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_map_attrs.size()),
                                tunnel_map_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create the NVGRE tunnel map object");
    }

    return tunnel_map_id;
}

/** @brief Removes tunnel mapper in SAI.
 *
 *  @param sai_tunnel_map_type SAI tunnel map identifier.
 *
 *  @return void.
 */
void NvgreTunnel::sai_remove_tunnel_map(sai_object_id_t tunnel_map_id)
{
    sai_status_t status = sai_tunnel_api->remove_tunnel_map(tunnel_map_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't remove the NVGRE tunnel map object");
    }
}


/** @brief Creates tunnel in SAI.
 *
 *  @param ids Pointer to structure where stored tunnel and tunnel mappers identifiers.
 *  @param src_ip Pointer to source IP address.
 *
 *  @return SAI tunnel identifier.
 */
sai_object_id_t NvgreTunnel::sai_create_tunnel(struct tunnel_sai_ids_t &ids, const sai_ip_address_t &src_ip, sai_object_id_t underlay_rif)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_NVGRE;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_UNDERLAY_INTERFACE;
    attr.value.oid = underlay_rif;
    tunnel_attrs.push_back(attr);

    sai_object_id_t decap_map_list[MAP_T_MAX];
    uint8_t num_decap_map = 0;

    for (auto map_type : nvgreMapTypes)
    {
        decap_map_list[num_decap_map] = ids.tunnel_decap_id.at(map_type);
        num_decap_map++;
    }

    attr.id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
    attr.value.objlist.count = num_decap_map;
    attr.value.objlist.list = decap_map_list;
    tunnel_attrs.push_back(attr);

    sai_object_id_t encap_map_list[MAP_T_MAX];
    uint8_t num_encap_map = 0;

    for (auto map_type : nvgreMapTypes)
    {
        encap_map_list[num_encap_map] = ids.tunnel_encap_id.at(map_type);
        num_encap_map++;
    }

    attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
    attr.value.objlist.count = num_encap_map;
    attr.value.objlist.list = encap_map_list;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    attr.value.ipaddr = src_ip;
    tunnel_attrs.push_back(attr);

    sai_object_id_t tunnel_id;
    sai_status_t status = sai_tunnel_api->create_tunnel(
                                &tunnel_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_attrs.size()),
                                tunnel_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create the NVGRE tunnel object");
    }

    return tunnel_id;
}

/** @brief Removes tunnel in SAI.
 *
 *  @param tunnel_id Pointer to tunnel identifier.
 *
 *  @return void.
 */
void NvgreTunnel::sai_remove_tunnel(sai_object_id_t tunnel_id)
{
    sai_status_t status = sai_tunnel_api->remove_tunnel(tunnel_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't remove the NVGRE tunnel object");
    }
}

/** @brief Creates tunnel termination in SAI.
 *
 *  @param tunnel_id Tunnel identifier.
 *  @param src_ip Pointer to source IP address.
 *  @param default_vrid Virtual router identifier.
 *
 *  @return SAI tunnel termination identifier.
 */
sai_object_id_t NvgreTunnel::sai_create_tunnel_termination(sai_object_id_t tunnel_id, const sai_ip_address_t &src_ip, sai_object_id_t default_vrid)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TYPE;
    attr.value.s32 = SAI_TUNNEL_TERM_TABLE_ENTRY_TYPE_P2MP;
    tunnel_attrs.push_back(attr);
   
    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_VR_ID;
    attr.value.oid = default_vrid;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_DST_IP;
    attr.value.ipaddr = src_ip;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_TUNNEL_TYPE;
    attr.value.s32 = SAI_TUNNEL_TYPE_NVGRE;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_TERM_TABLE_ENTRY_ATTR_ACTION_TUNNEL_ID;
    attr.value.oid = tunnel_id;
    tunnel_attrs.push_back(attr);

    sai_object_id_t term_table_id;
    sai_status_t status = sai_tunnel_api->create_tunnel_term_table_entry(
                                &term_table_id,
                                gSwitchId,
                                static_cast<uint32_t>(tunnel_attrs.size()),
                                tunnel_attrs.data()
                          );
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create a tunnel term table object");
    }

    return term_table_id;
}

/** @brief Removes tunnel termination in SAI.
 *
 *  @param tunnel_id Pointer to tunnel termination identifier.
 *
 *  @return void.
 */
void NvgreTunnel::sai_remove_tunnel_termination(sai_object_id_t tunnel_term_id)
{
    sai_status_t status = sai_tunnel_api->remove_tunnel_term_table_entry(tunnel_term_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't remove a tunnel term object");
    }
}

void NvgreTunnel::createNvgreMappers()
{
    for (auto map_type : nvgreMapTypes)
    {
        tunnel_ids_.tunnel_encap_id.insert(
            make_pair(map_type, sai_create_tunnel_map(get_encap_nvgre_mapper(map_type)))
        );
    }

    for (auto map_type : nvgreMapTypes)
    {
        tunnel_ids_.tunnel_decap_id.insert(
            make_pair(map_type, sai_create_tunnel_map(get_decap_nvgre_mapper(map_type)))
        );
    }
}

void NvgreTunnel::removeNvgreMappers()
{
    for (auto map_type : nvgreMapTypes)
    {
        sai_remove_tunnel_map(getEncapMapId(map_type));
    }

    for (auto map_type : nvgreMapTypes)
    {
        sai_remove_tunnel_map(getDecapMapId(map_type));
    }

    tunnel_ids_.tunnel_encap_id.clear();
    tunnel_ids_.tunnel_decap_id.clear();
}

void NvgreTunnel::createNvgreTunnel()
{
    sai_ip_address_t ip_addr;
    swss::copy(ip_addr, src_ip_);

    tunnel_ids_.tunnel_id = sai_create_tunnel(tunnel_ids_, ip_addr, gUnderlayIfId);
    tunnel_ids_.tunnel_term_id = sai_create_tunnel_termination(tunnel_ids_.tunnel_id, ip_addr, gVirtualRouterId);

    SWSS_LOG_INFO("NVGRE tunnel '%s' was created", tunnel_name_.c_str());
}

void NvgreTunnel::removeNvgreTunnel()
{
    try
    {
        sai_remove_tunnel_termination(tunnel_ids_.tunnel_term_id);
        sai_remove_tunnel(tunnel_ids_.tunnel_id);
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error while removing tunnel entry. Tunnel: %s. Error: %s", tunnel_name_.c_str(), error.what());
    }

    SWSS_LOG_INFO("NVGRE tunnel '%s' was removed", tunnel_name_.c_str());

    tunnel_ids_.tunnel_id = SAI_NULL_OBJECT_ID;
    tunnel_ids_.tunnel_term_id = SAI_NULL_OBJECT_ID;
}

NvgreTunnel::NvgreTunnel(std::string tunnelName, IpAddress srcIp) :
                         tunnel_name_(tunnelName),
                         src_ip_(srcIp)
{
    createNvgreMappers();
    createNvgreTunnel();
}

NvgreTunnel::~NvgreTunnel()
{
    removeNvgreTunnel();
    removeNvgreMappers();
}

bool NvgreTunnelOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto src_ip = request.getAttrIP("src_ip");
    const auto& tunnel_name = request.getKeyString(0);

    if (isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel '%s' already exists", tunnel_name.c_str());
        return true;
    }

    nvgre_tunnel_table_[tunnel_name] = std::unique_ptr<NvgreTunnel>(new NvgreTunnel(tunnel_name, src_ip));

    return true;
}

bool NvgreTunnelOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    const auto& tunnel_name = request.getKeyString(0);

    if (!isTunnelExists(tunnel_name))
    {
        SWSS_LOG_ERROR("NVGRE tunnel '%s' doesn't exist", tunnel_name.c_str());
        return true;
    }

    nvgre_tunnel_table_.erase(tunnel_name);

    SWSS_LOG_INFO("NVGRE tunnel '%s' was removed", tunnel_name.c_str());

    return true;
}

/** @brief Creates tunnel map entry in SAI.
 *
 *  @param map_type      map type - VLAN or BRIDGE.
 *  @param vsid          Virtual Subnet ID value.
 *  @param vlan_id       VLAN ID value.
 *  @param bridge_obj_id SAI bridge object.
 *  @param encap         encapsulation flag.
 *
 *  @return SAI tunnel map entry ID.
 */
sai_object_id_t NvgreTunnel::sai_create_tunnel_map_entry(
    map_type_t map_type,
    sai_uint32_t vsid,
    sai_vlan_id_t vlan_id,
    sai_object_id_t bridge_obj_id,
    bool encap)
{
    sai_attribute_t attr;
    sai_object_id_t tunnel_map_entry_id;
    std::vector<sai_attribute_t> tunnel_map_entry_attrs;

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP_TYPE;
    attr.value.u32 = (encap) ? get_encap_nvgre_mapper(map_type) : get_decap_nvgre_mapper(map_type);
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_MAP_ENTRY_ATTR_TUNNEL_MAP;
    attr.value.oid = (encap) ? getEncapMapId(map_type) : getDecapMapId(map_type);
    tunnel_map_entry_attrs.push_back(attr);

    attr.id = (encap) ? get_encap_nvgre_map_key(map_type) : get_decap_nvgre_map_val(map_type);
    if (bridge_obj_id != SAI_NULL_OBJECT_ID)
    {
        attr.value.oid = bridge_obj_id;
    }
    else
    {
        attr.value.u16 = vlan_id;
    }

    tunnel_map_entry_attrs.push_back(attr);

    attr.id = (encap) ? get_encap_nvgre_map_val(map_type) : get_decap_nvgre_map_key(map_type);
    attr.value.u32 = vsid;
    tunnel_map_entry_attrs.push_back(attr);

    sai_status_t status = sai_tunnel_api->create_tunnel_map_entry(&tunnel_map_entry_id, gSwitchId,
                                            static_cast<uint32_t> (tunnel_map_entry_attrs.size()),
                                            tunnel_map_entry_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't create the NVGRE tunnel map entry object");
    }

    return tunnel_map_entry_id;
}


bool NvgreTunnel::addDecapMapperEntry(
    map_type_t map_type,
    uint32_t vsid,
    sai_vlan_id_t vlan_id,
    std::string tunnel_map_entry_name,
    sai_object_id_t bridge_obj)
{
    auto tunnel_map_entry_id = sai_create_tunnel_map_entry(map_type, vsid, vlan_id, bridge_obj);

    nvgre_tunnel_map_table_[tunnel_map_entry_name].map_entry_id = tunnel_map_entry_id;
    nvgre_tunnel_map_table_[tunnel_map_entry_name].vlan_id = vlan_id;
    nvgre_tunnel_map_table_[tunnel_map_entry_name].vsid = vsid;

    SWSS_LOG_INFO("NVGRE decap tunnel map entry '%s' for tunnel '%s' was created",
        tunnel_map_entry_name.c_str(), tunnel_name_.c_str());

    return true;
}

bool NvgreTunnelMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto tunnel_name = request.getKeyString(0);
    NvgreTunnelOrch* tunnel_orch = gDirectory.get<NvgreTunnelOrch*>();

    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel '%s' doesn't exist", tunnel_name.c_str());
        return true;
    }

    auto tunnel_obj = tunnel_orch->getNvgreTunnel(tunnel_name);
    const auto full_tunnel_map_entry_name = request.getFullKey();

    if (tunnel_obj->isTunnelMapExists(full_tunnel_map_entry_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel map '%s' already exist", full_tunnel_map_entry_name.c_str());
        return true;
    }

    sai_vlan_id_t vlan_id = (sai_vlan_id_t) request.getAttrVlan("vlan_id");
    Port port;

    if (!gPortsOrch->getVlanByVlanId(vlan_id, port))
    {
        SWSS_LOG_WARN("VLAN ID doesn't exist: %d", vlan_id);
        return true;
    }

    auto vsid = static_cast<sai_uint32_t>(request.getAttrUint("vsid"));
    if (vsid > NVGRE_VSID_MAX_VALUE)
    {
        SWSS_LOG_WARN("VSID is invalid: %d", vsid);
        return true;
    }

    if (!tunnel_obj->addDecapMapperEntry(MAP_T_VLAN, vsid, vlan_id, full_tunnel_map_entry_name))
    {
        return true;
    }

    return true;
}

/** @brief Removes tunnel map entry in SAI.
 *
 *  @param obj_id SAI tunnel map identifier.
 *
 *  @return void.
 */
void NvgreTunnel::sai_remove_tunnel_map_entry(sai_object_id_t obj_id)
{
    sai_status_t status = SAI_STATUS_SUCCESS;

    if (obj_id != SAI_NULL_OBJECT_ID)
    {
        status = sai_tunnel_api->remove_tunnel_map_entry(obj_id);
    }

    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't delete the NVGRE tunnel map entry object");
    }
}

bool NvgreTunnel::delMapperEntry(std::string tunnel_map_entry_name)
{
    auto tunnel_map_entry_id = getMapEntryId(tunnel_map_entry_name);

    try
    {
        sai_remove_tunnel_map_entry(tunnel_map_entry_id);
    }
    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error while removing decap tunnel map %s: %s",
            tunnel_map_entry_name.c_str(), error.what());
        return false;
    }

    nvgre_tunnel_map_table_.erase(tunnel_map_entry_name);

    SWSS_LOG_INFO("NVGRE tunnel map entry '%s' for tunnel '%s' was removed",
        tunnel_map_entry_name.c_str(), tunnel_name_.c_str());

    return true;
}

bool NvgreTunnelMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    const auto& tunnel_name = request.getKeyString(0);
    NvgreTunnelOrch* tunnel_orch = gDirectory.get<NvgreTunnelOrch*>();
    auto tunnel_obj = tunnel_orch->getNvgreTunnel(tunnel_name);
    const auto& full_tunnel_map_entry_name = request.getFullKey();

    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel '%s' does not exist", tunnel_name.c_str());
        return true;
    }

    if (!tunnel_obj->isTunnelMapExists(full_tunnel_map_entry_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel map '%s' does not exist",
            full_tunnel_map_entry_name.c_str());
        return true;
    }

    if (!tunnel_obj->delMapperEntry(full_tunnel_map_entry_name))
    {
        return true;
    }

    return true;
}
