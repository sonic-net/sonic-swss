#include "portsorch.h"
#include "llrorch.h"

extern sai_port_api_t *sai_port_api;

extern sai_object_id_t      gSwitchId;

extern PortsOrch *gPortsOrch;

type_map LlrOrch::m_llr_type_maps = {
    {APP_LLR_PROFILE_TABLE_NAME, std::make_shared<object_reference_map>()},
    {APP_LLR_PORT_TABLE_NAME, std::make_shared<object_reference_map>()}
};

bool LlrOrch::getLlrFrameActionFromString(const string &action, sai_llr_frame_action_t &out)
{
    SWSS_LOG_ENTER();

    if (action == llr_frame_action_field_value_discard)
    {
        out = SAI_LLR_FRAME_ACTION_DISCARD;
        return true;
    }
    if (action == llr_frame_action_field_value_block)
    {
        out = SAI_LLR_FRAME_ACTION_BLOCK;
        return true;
    }
    if (action == llr_frame_action_field_value_best_effort)
    {
        out = SAI_LLR_FRAME_ACTION_BEST_EFFORT;
        return true;
    }

    SWSS_LOG_WARN("Unknown LLR frame action value '%s'; skipping attribute", action.c_str());
    return false;
}

void LlrOrch::handleLlrProfileTableEvent(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        sai_object_id_t llr_profile_id;

        KeyOpFieldsValuesTuple tuple = it->second;
        string object_name = kfvKey(tuple);
        string op = kfvOp(tuple);

        auto profile_it = m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->find(object_name);
        auto profile_present = (profile_it != m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->end());

        if (op == SET_COMMAND)
        {
            if (profile_present && profile_it->second.m_pendingRemove)
            {
                SWSS_LOG_NOTICE("Entry %s %s is pending remove, need retry", APP_LLR_PROFILE_TABLE_NAME, object_name.c_str());
                it++;
                continue;
            }

            SWSS_LOG_NOTICE("Updated LLR_PROFILE: %s", object_name.c_str());

            vector<sai_attribute_t> attribs;
            for (const auto &fv : kfvFieldsValues(tuple))
            {
                const string &field = fv.first;
                const string &value = fv.second;

                sai_attribute_t attr;
                if (field == llr_max_outstanding_frames_field_name)
                {
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_FRAMES_MAX;
                    attr.value.u32 = static_cast<uint32_t>(stoul(value));
                }
                else if (field == llr_max_outstanding_bytes_field_name)
                {
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_BYTES_MAX;
                    attr.value.u32 = static_cast<uint32_t>(stoul(value));
                }
                else if (field == llr_max_replay_timer_field_name)
                {
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_REPLAY_TIMER_MAX;
                    attr.value.u32 = static_cast<uint32_t>(stoul(value));
                }
                else if (field == llr_max_replay_count_field_name)
                {
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_REPLAY_COUNT_MAX;
                    attr.value.u32 = static_cast<uint32_t>(stoul(value));
                }
                else if (field == llr_pcs_lost_timeout_field_name)
                {
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_PCS_LOST_TIMEOUT;
                    attr.value.u32 = static_cast<uint32_t>(stoul(value));
                }
                else if (field == llr_data_age_timeout_field_name)
                {
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_DATA_AGE_TIMEOUT;
                    attr.value.u32 = static_cast<uint32_t>(stoul(value));
                }
                else if (field == llr_ctlos_spacing_bytes_field_name)
                {
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_CTLOS_TARGET_SPACING;
                    attr.value.u32 = static_cast<uint32_t>(stoul(value));
                }
                else if (field == llr_init_action_field_name)
                {
                    sai_llr_frame_action_t action;
                    if (!getLlrFrameActionFromString(value, action))
                    {
                        continue;
                    }
                    attr.id = SAI_PORT_LLR_PROFILE_ATTR_INIT_LLR_FRAME_ACTION;
                    attr.value.s32 = action;
                }
                else if (field == llr_flush_action_field_name)
                {
                    sai_llr_frame_action_t action;
                    if (!getLlrFrameActionFromString(value, action))
                    {
                        continue;
                    }
                    attr.id    = SAI_PORT_LLR_PROFILE_ATTR_FLUSH_LLR_FRAME_ACTION;
                    attr.value.s32 = action;
                }
                else
                {
                    SWSS_LOG_WARN("Unknown field %s in LLR_PROFILE_TABLE", field.c_str());
                    continue;
                }

                attribs.push_back(attr);
            }

            // Filter out unsupported attrs once — both update and create use the result.
            vector<sai_attribute_t> supported_attribs;
            for (const auto &attrib : attribs)
            {
                if (capability.supported_profile_attrs.count(static_cast<sai_port_llr_profile_attr_t>(attrib.id)))
                {
                    supported_attribs.push_back(attrib);
                }
                else
                {
                    SWSS_LOG_WARN("LLR profile attr %d not supported, skipping", attrib.id);
                }
            }

            // OUTSTANDING_FRAMES_MAX and OUTSTANDING_BYTES_MAX are MANDATORY_ON_CREATE.
            // Validate presence before either create or update path.
            bool has_frames = false, has_bytes = false;
            for (const auto &a : supported_attribs)
            {
                if (a.id == SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_FRAMES_MAX) has_frames = true;
                if (a.id == SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_BYTES_MAX)  has_bytes  = true;
            }

            if (!has_frames || !has_bytes)
            {
                SWSS_LOG_ERROR("LLR_PROFILE %s missing mandatory attr%s%s; erasing event",
                               object_name.c_str(),
                               !has_frames ? " OUTSTANDING_FRAMES_MAX" : "",
                               !has_bytes  ? " OUTSTANDING_BYTES_MAX"  : "");
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (profile_present)
            {
                // Update existing profile — only send attrs the capability query reports as settable.
                llr_profile_id = profile_it->second.m_saiObjectId;
                for (const auto &attrib : supported_attribs)
                {
                    if (!capability.settable_profile_attrs.count(
                            static_cast<sai_port_llr_profile_attr_t>(attrib.id)))
                    {
                        SWSS_LOG_INFO("Skip non-settable LLR profile attr %d for existing profile %s",
                                      attrib.id, object_name.c_str());
                        continue;
                    }
                    sai_status_t status = sai_port_api->set_port_llr_profile_attribute(llr_profile_id, &attrib);
                    if (status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to set LLR_PROFILE %s attr %d, status: %d",
                                       object_name.c_str(), attrib.id, status);
                    }
                }
                SWSS_LOG_NOTICE("Updated LLR_PROFILE %s attributes", object_name.c_str());
            }
            else
            {

                // Always inject RE_INIT_ON_FLUSH=true on create if supported.
                if (capability.supported_profile_attrs.count(SAI_PORT_LLR_PROFILE_ATTR_RE_INIT_ON_FLUSH))
                {
                    sai_attribute_t reinit_attr;
                    reinit_attr.id             = SAI_PORT_LLR_PROFILE_ATTR_RE_INIT_ON_FLUSH;
                    reinit_attr.value.booldata = true;
                    supported_attribs.push_back(reinit_attr);
                }

                sai_status_t status = sai_port_api->create_port_llr_profile(
                    &llr_profile_id,
                    gSwitchId,
                    static_cast<uint32_t>(supported_attribs.size()),
                    supported_attribs.data());

                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to create LLR_PROFILE %s, status: %d", object_name.c_str(), status);
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                (*m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME])[object_name].m_saiObjectId = llr_profile_id;
                (*m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME])[object_name].m_pendingRemove = false;
                SWSS_LOG_NOTICE("Created LLR_PROFILE %s", object_name.c_str());
            }
        }
        else if (op == DEL_COMMAND)
        {
            if (!profile_present)
            {
                SWSS_LOG_WARN("DEL for unknown LLR_PROFILE %s, ignoring", object_name.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            if (isObjectBeingReferenced(m_llr_type_maps, APP_LLR_PROFILE_TABLE_NAME, object_name))
            {
                SWSS_LOG_WARN("Cannot delete %s:%s as it is being referenced", APP_LLR_PROFILE_TABLE_NAME, object_name.c_str());
                auto &profile = (*m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME])[object_name];
                profile.m_pendingRemove = true;
                it++;
                continue;
            }
            
            llr_profile_id = profile_it->second.m_saiObjectId;
            sai_status_t remove_status = sai_port_api->remove_port_llr_profile(llr_profile_id);
            if (remove_status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove LLR_PROFILE %s (OID 0x%" PRIx64 "), status: %d",
                               object_name.c_str(), llr_profile_id, remove_status);
            }
            else
            {
                removeObject(m_llr_type_maps, APP_LLR_PROFILE_TABLE_NAME, object_name);
                SWSS_LOG_NOTICE("Deleted LLR_PROFILE %s", object_name.c_str());
            }
        }

        it = consumer.m_toSync.erase(it);
    }
}

void LlrOrch::handleLlrPortTableEvent(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple tuple = it->second;
        string port_name = kfvKey(tuple);
        string op = kfvOp(tuple);

        Port port;
        if (!gPortsOrch->getPort(port_name, port))
        {
            SWSS_LOG_ERROR("Port %s does not exist, cannot set LLR_PORT", port_name.c_str());
            it++;
            continue;
        }

        if (port.m_type != Port::PHY)
        {
            SWSS_LOG_ERROR("Port %s is not a PHY port, cannot set LLR_PORT", port_name.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        sai_attribute_t attr;

        if (op == SET_COMMAND)
        {            
            // Flow: disable → bind profile → enable.
            string llr_local_val, llr_remote_val, llr_profile_val;
            bool has_local = false, has_remote = false, has_profile = false;

            for (const auto &fv : kfvFieldsValues(tuple))
            {
                if (fv.first == llr_local_field_name)
                {
                    llr_local_val = fv.second;
                    has_local     = true;
                }
                else if (fv.first == llr_remote_field_name)
                {
                    llr_remote_val = fv.second;
                    has_remote     = true;
                }
                else if (fv.first == llr_profile_field_name)
                {
                    llr_profile_val = fv.second;
                    has_profile     = true;
                }
                else if (fv.first == llr_mode_field_name)
                {
                    // There is no action for llr_mode in orchagent.
                }
                else
                {
                    SWSS_LOG_WARN("Unknown field %s in LLR_PORT_TABLE", fv.first.c_str());
                }
            }

            // Detect a profile switch while LLR is already active on this port.
            // A profile switch requires disable → rebind → re-enable.
            const auto &ps       = m_llrPortState[port_name];
            bool is_profile_change = has_profile
                                   && !ps.bound_profile.empty()
                                   && ps.bound_profile != llr_profile_val;

            // Merge disable conditions: explicit "disabled" value OR profile is changing
            // while the mode is enabled.
            bool should_disable_local  = (has_local && llr_local_val  == "disabled")
                                       || (is_profile_change && ps.local_enabled);
            bool should_disable_remote = (has_remote && llr_remote_val == "disabled")
                                       || (is_profile_change && ps.remote_enabled);

            // Step 1: Disable local before any profile change.
            if (should_disable_local)
            {
                setLlrPortMode(port.m_port_id, port_name, true, false);
            }

            // Step 2: Disable remote before any profile change.
            if (should_disable_remote)
            {
                setLlrPortMode(port.m_port_id, port_name, false, false);
            }

            // Step 3: Bind the LLR profile (after both directions are disabled).
            if (has_profile)
            {
                sai_object_id_t   profile_id;
                string            resolved_profile;
                ref_resolve_status resolve_status = resolveFieldRefValue(
                    m_llr_type_maps,
                    llr_profile_field_name, APP_LLR_PROFILE_TABLE_NAME,
                    tuple, profile_id, resolved_profile);

                if (resolve_status == ref_resolve_status::not_resolved)
                {
                    // Profile SAI object not yet created — retry later.
                    SWSS_LOG_INFO("LLR profile '%s' not created yet %s; retrying",
                                  llr_profile_val.c_str(), port_name.c_str());
                    it++;
                    continue;
                }
                else if (resolve_status != ref_resolve_status::success)
                {
                    SWSS_LOG_ERROR("Failed to resolve LLR profile reference '%s' for port %s",
                                   llr_profile_val.c_str(), port_name.c_str());
                }
                else
                {
                    // Profile is resolved but may be scheduled for removal —
                    // defer the bind until the pending DEL completes.
                    auto profIt = m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->find(resolved_profile);
                    if (profIt != m_llr_type_maps[APP_LLR_PROFILE_TABLE_NAME]->end() &&
                        profIt->second.m_pendingRemove)
                    {
                        SWSS_LOG_INFO("LLR profile '%s' is pending removal; deferring port %s bind",
                                      resolved_profile.c_str(), port_name.c_str());
                        it++;
                        continue;
                    }

                    attr.id        = SAI_PORT_ATTR_LLR_PROFILE;
                    attr.value.oid = profile_id;
                    sai_status_t s = sai_port_api->set_port_attribute(port.m_port_id, &attr);
                    if (s != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to bind LLR profile '%s' to port %s, status: %d",
                                       resolved_profile.c_str(), port_name.c_str(), s);
                    }
                    else
                    {
                        setObjectReference(m_llr_type_maps, APP_LLR_PORT_TABLE_NAME,
                                           port_name, llr_profile_field_name, resolved_profile);
                        m_llrPortState[port_name].bound_profile = llr_profile_val;
                        SWSS_LOG_INFO("Bound LLR profile '%s' to port %s",
                                      resolved_profile.c_str(), port_name.c_str());
                    }
                }
            }

            // Step 4: enable only if a profile is currently bound.
            if (m_llrPortState[port_name].bound_profile.empty())
            {
                if ((has_remote && llr_remote_val == "enabled") ||
                    (has_local  && llr_local_val  == "enabled"))
                {
                    SWSS_LOG_ERROR("Port %s: cannot enable LLR — no profile is bound; erasing event",
                                  port_name.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
            }
            else
            {
                // Step 4: Enable remote and local after profile is bound.
                if (has_remote && llr_remote_val == "enabled")
                {
                    setLlrPortMode(port.m_port_id, port_name, false, true);
                }

                if (has_local && llr_local_val == "enabled")
                {
                    setLlrPortMode(port.m_port_id, port_name, true, true);
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            /* Disable sequence: remote disable -> local off -> unbind profile.
             * Only attempt unbind after both modes are successfully disabled.
             */
            bool remote_off = setLlrPortMode(port.m_port_id, port_name, false, false);
            bool local_off  = setLlrPortMode(port.m_port_id, port_name, true,  false);

            if (!remote_off || !local_off)
            {
                SWSS_LOG_ERROR("Failed to disable LLR modes for port %s, will retry",
                               port_name.c_str());
                it++;
                continue;
            }

            attr.id        = SAI_PORT_ATTR_LLR_PROFILE;
            attr.value.oid = SAI_NULL_OBJECT_ID;
            sai_status_t status = sai_port_api->set_port_attribute(port.m_port_id, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to unbind LLR profile for port %s, status: %d",
                               port_name.c_str(), status);
                it++;
                continue;
            }

            removeObject(m_llr_type_maps, APP_LLR_PORT_TABLE_NAME, port_name);
            m_llrPortState.erase(port_name);
            SWSS_LOG_NOTICE("LLR deleted for port %s", port_name.c_str());
        }

        it = consumer.m_toSync.erase(it);
    }
}

bool LlrOrch::setLlrPortMode(sai_object_id_t port_id, const string &port_name,
                              bool is_local, bool enable)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id             = is_local ? SAI_PORT_ATTR_LLR_MODE_LOCAL : SAI_PORT_ATTR_LLR_MODE_REMOTE;
    attr.value.booldata = enable;
    sai_status_t status = sai_port_api->set_port_attribute(port_id, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to %s LLR_MODE_%s for port %s, status: %d",
                       enable ? "enable" : "disable",
                       is_local ? "LOCAL" : "REMOTE",
                       port_name.c_str(), status);
        return false;
    }

    if (is_local)
        m_llrPortState[port_name].local_enabled  = enable;
    else
        m_llrPortState[port_name].remote_enabled = enable;
    
    return true;
}

void LlrOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (!capability.llr_supported)
    {
        SWSS_LOG_NOTICE("LLR not supported on this platform; ignoring %s events",
                        consumer.getTableName().c_str());
        auto it = consumer.m_toSync.begin();
        while (it != consumer.m_toSync.end()) it = consumer.m_toSync.erase(it);
        return;
    }

    auto table = consumer.getTableName();

    if (table == APP_LLR_PROFILE_TABLE_NAME)
    {
        handleLlrProfileTableEvent(consumer);
    }
    else if (table == APP_LLR_PORT_TABLE_NAME)
    {
        handleLlrPortTableEvent(consumer);
    }
}

void LlrOrch::updateLlrCapabilityStateDB(DBConnector *stateDb)
{
    SWSS_LOG_ENTER();

    // Query all three LLR port attributes to determine LLR support.
    // All three must be set_implemented for LLR to be considered supported.
    static const vector<pair<sai_port_attr_t, string>> llrPortAttrList = {
        {SAI_PORT_ATTR_LLR_PROFILE,      "LLR_PROFILE"},
        {SAI_PORT_ATTR_LLR_MODE_LOCAL,   "LLR_MODE_LOCAL"},
        {SAI_PORT_ATTR_LLR_MODE_REMOTE,  "LLR_MODE_REMOTE"},
    };

    this->capability.llr_supported = true;
    for (const auto &entry : llrPortAttrList)
    {
        sai_attr_capability_t portAttrCap = {};
        sai_status_t status = sai_query_attribute_capability(
            gSwitchId, SAI_OBJECT_TYPE_PORT, entry.first, &portAttrCap);
        bool supported = (status == SAI_STATUS_SUCCESS && portAttrCap.set_implemented);
        SWSS_LOG_NOTICE("LLR port attr capability: %s=%s",
                        entry.second.c_str(), supported ? "true" : "false");
        if (!supported)
        {
            this->capability.llr_supported = false;
        }
    }

    SWSS_LOG_NOTICE("LLR capability: llr_supported=%s",
                    this->capability.llr_supported ? "true" : "false");

    static const vector<pair<sai_port_llr_profile_attr_t, string>> llrProfileAttrList = {
        {SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_FRAMES_MAX,  "OUTSTANDING_FRAMES_MAX"},
        {SAI_PORT_LLR_PROFILE_ATTR_OUTSTANDING_BYTES_MAX,   "OUTSTANDING_BYTES_MAX"},
        {SAI_PORT_LLR_PROFILE_ATTR_REPLAY_TIMER_MAX,        "REPLAY_TIMER_MAX"},
        {SAI_PORT_LLR_PROFILE_ATTR_REPLAY_COUNT_MAX,        "REPLAY_COUNT_MAX"},
        {SAI_PORT_LLR_PROFILE_ATTR_PCS_LOST_TIMEOUT,        "PCS_LOST_TIMEOUT"},
        {SAI_PORT_LLR_PROFILE_ATTR_DATA_AGE_TIMEOUT,        "DATA_AGE_TIMEOUT"},
        {SAI_PORT_LLR_PROFILE_ATTR_INIT_LLR_FRAME_ACTION,   "INIT_LLR_FRAME_ACTION"},
        {SAI_PORT_LLR_PROFILE_ATTR_FLUSH_LLR_FRAME_ACTION,  "FLUSH_LLR_FRAME_ACTION"},
        {SAI_PORT_LLR_PROFILE_ATTR_RE_INIT_ON_FLUSH,        "RE_INIT_ON_FLUSH"},
        {SAI_PORT_LLR_PROFILE_ATTR_CTLOS_TARGET_SPACING,    "CTLOS_TARGET_SPACING"},
    };

    string supportedAttrs;
    this->capability.supported_profile_attrs.clear();
    this->capability.settable_profile_attrs.clear();

    for (const auto &entry : llrProfileAttrList)
    {
        sai_attr_capability_t cap;
        sai_status_t rc = sai_query_attribute_capability(
            gSwitchId, SAI_OBJECT_TYPE_PORT_LLR_PROFILE, entry.first, &cap);
        if (rc != SAI_STATUS_SUCCESS)
        {
            continue;
        }
        if (cap.create_implemented || cap.set_implemented)
        {
            if (!supportedAttrs.empty())
            {
                supportedAttrs += ", ";
            }
            supportedAttrs += entry.second;
            this->capability.supported_profile_attrs.insert(entry.first);
        }
        if (cap.set_implemented)
        {
            this->capability.settable_profile_attrs.insert(entry.first);
        }
    }

    SWSS_LOG_NOTICE("LLR profile-object capability: supported_attrs=[%s]",
                    supportedAttrs.c_str());

    Table switchCapabilityTable(stateDb, STATE_SWITCH_CAPABILITY_TABLE_NAME);
    vector<FieldValueTuple> capFvs = {
        {"LLR_CAPABLE",                      this->capability.llr_supported ? "true" : "false"},
        {"LLR_SUPPORTED_PROFILE_ATTRIBUTES", supportedAttrs},
    };
    switchCapabilityTable.set("switch", capFvs);

    SWSS_LOG_NOTICE("LLR capability published: LLR_CAPABLE=%s LLR_SUPPORTED_PROFILE_ATTRIBUTES=%s",
                    this->capability.llr_supported ? "true" : "false",
                    supportedAttrs.c_str());
}

LlrOrch::LlrOrch(DBConnector* applDb, DBConnector *stateDb, const std::vector<std::string> &tables)
    : Orch(applDb, tables)
{
    SWSS_LOG_ENTER();

    updateLlrCapabilityStateDB(stateDb);
}
