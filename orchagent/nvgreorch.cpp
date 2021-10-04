#include "orch.h"
#include "sai.h"
#include "nvgreorch.h"
#include "request_parser.h"
#include "swssnet.h"

//extern Directory<Orch*> gDirectory;
extern PortsOrch*       gPortsOrch;
extern sai_object_id_t  gSwitchId;
extern sai_tunnel_api_t *sai_tunnel_api;

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
        throw std::runtime_error("Can't create tunnel map object");
        /*
        // FIXME: need to update SAI version in order to support the code bellow

        SWSS_LOG_ERROR("Failed to create NVGRE tunnel mapper = %u, SAI status = %d", sai_tunnel_map_type, status);
        task_process_status handle_status = handleSaiSetStatus(SAI_API_TUNNEL, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
        */
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
        throw std::runtime_error("Can't remove a tunnel map object");
    }
}


/** @brief Creates tunnel in SAI.
 *
 *  @param ids Pointer to structure where stored tunnel and tunnel mappers identifiers.
 *  @param src_ip Pointer to source IP address.
 *
 *  @return SAI tunnel identifier.
 */
sai_object_id_t NvgreTunnel::sai_create_tunnel(struct tunnel_sai_ids_t* ids, sai_ip_address_t *src_ip)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    // # FIXME: CHANGE type
    attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
    tunnel_attrs.push_back(attr);

    sai_object_id_t decap_map_list[MAP_SIZE+1];
    uint8_t num_decap_map = 0;

    for (auto map_id : ids->tunnel_decap_id)
    {
        if (map_id != SAI_NULL_OBJECT_ID)
        {
            decap_map_list[num_decap_map] = map_id;
            num_decap_map++;
        }
    }

    attr.id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
    attr.value.objlist.count = num_decap_map;
    attr.value.objlist.list = decap_map_list;
    tunnel_attrs.push_back(attr);

    sai_object_id_t encap_map_list[MAP_SIZE+1];
    uint8_t num_encap_map = 0;

    for (auto map_id : ids->tunnel_encap_id)
    {
        if (map_id != SAI_NULL_OBJECT_ID)
        {
            encap_map_list[num_decap_map] = map_id;
            num_encap_map++;
        }
    }

    attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
    attr.value.objlist.count = num_encap_map;
    attr.value.objlist.list = encap_map_list;
    tunnel_attrs.push_back(attr);

    attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
    attr.value.ipaddr = *src_ip;
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
        throw std::runtime_error("Can't create a tunnel object");
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
        throw std::runtime_error("Can't remove a tunnel object");
    }
}

void NvgreTunnel::createTunnelMappers()
{
    for (auto map_value : nvgreEncapTunnelMap)
        tunnel_ids_.tunnel_encap_id.push_back(sai_create_tunnel_map(map_value));

    for (auto map_value : nvgreDecapTunnelMap)
        tunnel_ids_.tunnel_decap_id.push_back(sai_create_tunnel_map(map_value));
}

void NvgreTunnel::removeTunnelMappers()
{
    for (auto map_id : tunnel_ids_.tunnel_encap_id)
        sai_remove_tunnel_map(map_id);

    for (auto map_id : tunnel_ids_.tunnel_decap_id)
        sai_remove_tunnel_map(map_id);
}

void NvgreTunnel::createTunnel()
{
    try {
        sai_ip_address_t ip_addr;
        swss::copy(ip_addr, src_ip_);

        tunnel_ids_.tunnel_id = sai_create_tunnel(&tunnel_ids_, &ip_addr);
    }
    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error while creating tunnel %s: %s", tunnel_name_.c_str(), error.what());
    }

    SWSS_LOG_INFO("NVGRE tunnel '%s' was created", tunnel_name_.c_str());
}

void NvgreTunnel::removeTunnel()
{
    try
    {
        sai_remove_tunnel(tunnel_ids_.tunnel_id);
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error while removing tunnel entry. Tunnel: %s. Error: %s", tunnel_name_.c_str(), error.what());
    }

    SWSS_LOG_INFO("NVGRE tunnel '%s' was removed", tunnel_name_.c_str());
}

NvgreTunnel::NvgreTunnel(std::string tunnelName, IpAddress srcIp) :
                         tunnel_name_(tunnelName),
                         src_ip_(srcIp)
{
    createTunnelMappers();
    createTunnel();
}

NvgreTunnel::~NvgreTunnel()
{
    removeTunnel();
    removeTunnelMappers();

    fill(tunnel_ids_.tunnel_decap_id.begin(), tunnel_ids_.tunnel_decap_id.end(), SAI_NULL_OBJECT_ID);
    fill(tunnel_ids_.tunnel_encap_id.begin(), tunnel_ids_.tunnel_encap_id.end(), SAI_NULL_OBJECT_ID);
    tunnel_ids_.tunnel_id = SAI_NULL_OBJECT_ID;
}

bool NvgreTunnelOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    auto src_ip = request.getAttrIP("src_ip");
    const auto& tunnel_name = request.getKeyString(0);

    if (isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel '%s' is already exists", tunnel_name.c_str());
        return true;
    }

    nvgre_tunnel_table_[tunnel_name] = std::unique_ptr<NvgreTunnel>(new NvgreTunnel(tunnel_name, src_ip));

    SWSS_LOG_INFO("NVGRE tunnel '%s' was added", tunnel_name.c_str());

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

bool NvgreTunnelMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    /*
    auto tunnel_name = request.getKeyString(0);
    NvgreTunnelOrch* tunnel_orch = gDirectory.get<NvgreTunnelOrch*>();

    if (!tunnel_orch->isTunnelExists(tunnel_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel '%s' doesn't exist", tunnel_name.c_str());
        return false;
    }

    auto tunnel_obj = tunnel_orch->getNvgreTunnel(tunnel_name);
    const auto full_tunnel_map_entry_name = request.getFullKey();

    if (tunnel_obj->isTunnelMapExists(full_tunnel_map_entry_name))
    {
        SWSS_LOG_WARN("NVGRE tunnel map '%s' already exist",
                      full_tunnel_map_entry_name.c_str());
        return true;
    }

    sai_vlan_id_t vlan_id = (sai_vlan_id_t)request.getAttrVlan("vlan");
    Port tempPort;

    if (!gPortsOrch->getVlanByVlanId(vlan_id, tempPort))
    {
        SWSS_LOG_WARN("VLAN ID doesn't exist: %d", vlan_id);
        return false;
    }

    // check if it can be assigned by garbage value
    auto vsid = static_cast<sai_uint32_t>(request.getAttrUint("vsid"));
    if (vsid >= 1<<24)
    {
        SWSS_LOG_WARN("VSID is too big: %d", vsid);
        return true;
    }

    // create inside NvgreTunnel class sai methon and usual method to add tunnel_map_entry
    */

    return true;
}

bool NvgreTunnelMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_WARN("DEL operation is not implemented");

    return true;
}