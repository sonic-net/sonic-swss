#ifndef __LLRORCH_H__
#define __LLRORCH_H__

#include <string>
#include <set>
#include "orch.h"
#include "dbconnector.h"

using namespace std;
const string llr_max_outstanding_frames_field_name  = "max_outstanding_frames";
const string llr_max_outstanding_bytes_field_name   = "max_outstanding_bytes";
const string llr_max_replay_count_field_name        = "max_replay_count";
const string llr_max_replay_timer_field_name        = "max_replay_timer";
const string llr_pcs_lost_timeout_field_name        = "pcs_lost_timeout";
const string llr_data_age_timeout_field_name        = "data_age_timeout";
const string llr_ctlos_spacing_bytes_field_name     = "ctlos_spacing_bytes";
const string llr_init_action_field_name             = "init_action";
const string llr_flush_action_field_name            = "flush_action";

// LLR frame action values
const string llr_frame_action_field_value_discard     = "discard";
const string llr_frame_action_field_value_block       = "block";
const string llr_frame_action_field_value_best_effort = "best_effort";


const string llr_mode_field_name     = "llr_mode";    
const string llr_local_field_name    = "llr_local";
const string llr_remote_field_name   = "llr_remote";
const string llr_profile_field_name  = "llr_profile";

class LlrOrch : public Orch {
public:
    LlrOrch(DBConnector* applDb, DBConnector *stateDb, const std::vector<std::string> &tables);
    void doTask(Consumer &consumer);

    static type_map m_llr_type_maps; // Added static member for tracking LLR profile references
    static bool getLlrFrameActionFromString(const string &action, sai_llr_frame_action_t &out);
    void updateLlrCapabilityStateDB(DBConnector *stateDb);
    void handleLlrProfileTableEvent(Consumer &consumer);
    void handleLlrPortTableEvent(Consumer &consumer);

private:
    struct {
        bool llr_supported = false;
        std::set<sai_port_llr_profile_attr_t> supported_profile_attrs;  // create or set
        std::set<sai_port_llr_profile_attr_t> settable_profile_attrs;   // set only
    } capability;

    // Per-port LLR enablement state — used to detect profile switches while active.
    struct PortLlrState {
        bool   local_enabled  = false;
        bool   remote_enabled = false;
        string bound_profile;           // profile name currently bound
    };
    std::map<std::string, PortLlrState> m_llrPortState;

    // Set SAI_PORT_ATTR_LLR_MODE_LOCAL or _REMOTE; returns true on success.
    bool setLlrPortMode(sai_object_id_t port_id, const string &port_name,
                        bool is_local, bool enable);
};

#endif // __LLRORCH_H__
