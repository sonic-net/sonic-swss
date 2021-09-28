#include "orch.h"
#include "sai.h"
#include "nvgreorch.h"
#include "request_parser.h"
#include "swssnet.h"

extern sai_object_id_t gSwitchId;
extern sai_tunnel_api_t *sai_tunnel_api;

static sai_object_id_t create_tunnel_map(uint32_t sai_map)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_map_attrs;

    attr.id = SAI_TUNNEL_MAP_ATTR_TYPE;
    attr.value.s32 = sai_map;

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
    }

    return tunnel_map_id;
}

static void remove_tunnel_map(sai_object_id_t tunnel_map_id)
{
    sai_status_t status = sai_tunnel_api->remove_tunnel_map(tunnel_map_id);

    if (status != SAI_STATUS_SUCCESS)
    {
        throw std::runtime_error("Can't remove a tunnel map object");
    }
}

static sai_object_id_t create_tunnel(struct tunnel_sai_ids_t* ids, sai_ip_address_t *src_ip)
{
    sai_attribute_t attr;
    std::vector<sai_attribute_t> tunnel_attrs;

    attr.id = SAI_TUNNEL_ATTR_TYPE;
    //CHANGE type
    attr.value.s32 = SAI_TUNNEL_TYPE_VXLAN;
    tunnel_attrs.push_back(attr);

    sai_object_id_t decap_map_list[MAP_SIZE+1];
    uint8_t num_decap_map=0;

    for (uint32_t i = 0; i < nvgreDecapTunnelMap.size(); i++)
    {
        if (ids->tunnel_decap_id[i] != SAI_NULL_OBJECT_ID)
        {
            decap_map_list[num_decap_map] = ids->tunnel_decap_id[i];
            SWSS_LOG_INFO("create_tunnel:maplist[%d]=0x%" PRIx64 "", num_decap_map, decap_map_list[num_decap_map]);
            num_decap_map++;
        }
    }
      
    attr.id = SAI_TUNNEL_ATTR_DECAP_MAPPERS;
    attr.value.objlist.count = num_decap_map;
    attr.value.objlist.list = decap_map_list;
    tunnel_attrs.push_back(attr);

    sai_object_id_t encap_map_list[MAP_SIZE+1];
    uint8_t num_encap_map=0;

    for (uint32_t i = 0; i < nvgreEncapTunnelMap.size(); i++)
    {
        if (ids->tunnel_encap_id[i] != SAI_NULL_OBJECT_ID)
        {
            encap_map_list[num_encap_map] = ids->tunnel_encap_id[i];
            SWSS_LOG_NOTICE("create_tunnel:encapmaplist[%d]=0x%" PRIx64 "", num_encap_map, encap_map_list[num_encap_map]);
            num_encap_map++;
        }
    }

    attr.id = SAI_TUNNEL_ATTR_ENCAP_MAPPERS;
    attr.value.objlist.count = num_encap_map;
    attr.value.objlist.list = encap_map_list;
    tunnel_attrs.push_back(attr);

    // source ip check if it is not NULL
    if (src_ip != nullptr)
    {
        attr.id = SAI_TUNNEL_ATTR_ENCAP_SRC_IP;
        attr.value.ipaddr = *src_ip;
        tunnel_attrs.push_back(attr);
    }

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

static void remove_tunnel(sai_object_id_t tunnel_id)
{
    if (tunnel_id != SAI_NULL_OBJECT_ID)
    {
        sai_status_t status = sai_tunnel_api->remove_tunnel(tunnel_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            throw std::runtime_error("Can't remove a tunnel object");
        }
    }
    else
    {
        SWSS_LOG_DEBUG("Tunnel id is NULL.");
    }
}

void NvgreTunnel::createTunnelMapCapabilities()
{
    for (uint32_t i = 0; i < nvgreEncapTunnelMap.size(); ++i)
        tunnel_ids_.tunnel_encap_id[i] = create_tunnel_map(nvgreEncapTunnelMap[i]);

    for (uint32_t i = 0; i < nvgreDecapTunnelMap.size(); ++i)
        tunnel_ids_.tunnel_decap_id[i] = create_tunnel_map(nvgreDecapTunnelMap[i]);
}

void NvgreTunnel::removeTunnelMapCapabilities()
{
    for (uint32_t i = 0; i < nvgreEncapTunnelMap.size(); ++i)
        remove_tunnel_map(tunnel_ids_.tunnel_encap_id[i]);

    for (uint32_t i = 0; i < nvgreDecapTunnelMap.size(); ++i)
        remove_tunnel_map(tunnel_ids_.tunnel_decap_id[i]);
}

void NvgreTunnel::createTunnel()
{
    try {
        sai_ip_address_t ip_addr;
        swss::copy(ip_addr, src_ip_);

        tunnel_ids_.tunnel_id = create_tunnel(&tunnel_ids_, &ip_addr);
    }
    catch (const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error while creating tunnel %s: %s", tunnel_name_.c_str(), error.what());
    }

    SWSS_LOG_NOTICE("Nvgre tunnel '%s' was created", tunnel_name_.c_str());
}

void NvgreTunnel::removeTunnel()
{
    try
    {
        remove_tunnel(tunnel_ids_.tunnel_id);
    }
    catch(const std::runtime_error& error)
    {
        SWSS_LOG_ERROR("Error while removing tunnel entry. Tunnel: %s. Error: %s", tunnel_name_.c_str(), error.what());
    }

    SWSS_LOG_NOTICE("Nvgre tunnel '%s' was removed", tunnel_name_.c_str());
}

NvgreTunnel::NvgreTunnel(std::string tunnelName, IpAddress srcIp) :
                         tunnel_name_(tunnelName),
                         src_ip_(srcIp)
{
    createTunnelMapCapabilities();
    createTunnel();
}

NvgreTunnel::~NvgreTunnel()
{
    removeTunnelMapCapabilities();
    removeTunnel();
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

    SWSS_LOG_NOTICE("Nvgre tunnel '%s' was added", tunnel_name.c_str());

    return true;
}

bool NvgreTunnelOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();

    const auto& tunnel_name = request.getKeyString(0);

    if (!isTunnelExists(tunnel_name))
    {
        SWSS_LOG_ERROR("Nvgre tunnel '%s' doesn't exist", tunnel_name.c_str());
        return true;
    }

    nvgre_tunnel_table_.erase(tunnel_name);

    SWSS_LOG_NOTICE("Nvgre tunnel '%s' was removed", tunnel_name.c_str());

    return true;
}

bool NvgreTunnelMapOrch::addOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_WARN("ADD operation is not implemented");

    // createNvgreTunnelMap
    // NvgreTunnelOrch::getTunnelObj()
    // set mappers to TunnelObj // update cache

    return true;
}

bool NvgreTunnelMapOrch::delOperation(const Request& request)
{
    SWSS_LOG_ENTER();
    SWSS_LOG_WARN("DEL operation is not implemented");

    return true;
}