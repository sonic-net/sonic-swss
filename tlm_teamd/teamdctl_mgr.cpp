#include <cstring>
#include <algorithm>

#include <logger.h>

#include "teamdctl_mgr.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>
#include <sstream>

#define MAX_RETRY 3
///
/// Custom function for libteamdctl logger. IT is empty to prevent libteamdctl to spam us with the error messages
/// @param tdc teamdctl descriptor
/// @param priority priority of the message
/// @param file file where error was raised
/// @param line line in the file where error was raised
/// @param fn function where the error was raised
/// @param format format of the error message
/// @param args arguments of the error message
void teamdctl_log_function(struct teamdctl *tdc, int priority,
                           const char *file, int line,
                           const char *fn, const char *format,
                           va_list args)
{

}


///
/// The destructor clean up handlers to teamds
///
TeamdCtlMgr::~TeamdCtlMgr()
{
    if (!m_teamdUnifiedProcMode) {
        for (const auto & p: m_handlers)
        {
            const auto & lag_name = p.first;
            const auto & tdc = m_handlers[lag_name];
            teamdctl_disconnect(tdc);
            teamdctl_free(tdc);
            SWSS_LOG_NOTICE("Exiting. Disconnecting from teamd. LAG '%s'", lag_name.c_str());
	}
    }
}

///
/// Returns true, if we have LAG with name lag_name
/// in the manager.
/// @param lag_name a name for LAG interface
/// @return true if has key, false if doesn't
///
bool TeamdCtlMgr::has_key(const std::string & lag_name) const
{
    return m_handlers.find(lag_name) != m_handlers.end();
}

///
/// Public method to add a LAG interface with lag_name to the manager
/// This method tries to add. If the method can't add the LAG interface,
/// this action will be postponed.
/// @param lag_name a name for LAG interface
/// @return true if the lag was added or postponed successfully, false otherwise
///
bool TeamdCtlMgr::add_lag(const std::string & lag_name)
{
    if (has_key(lag_name))
    {
        SWSS_LOG_DEBUG("The LAG '%s' was already added. Skip adding it.", lag_name.c_str());
        return true;
    }
    return try_add_lag(lag_name);
}

///
/// Try to adds a LAG interface with lag_name to the manager
/// This method allocates structures to connect to teamd
/// if the method can't add, it will retry to add next time
/// @param lag_name a name for LAG interface
/// @return true if the lag was added successfully, false otherwise
///
bool TeamdCtlMgr::try_add_lag(const std::string & lag_name)
{

    if (m_teamdUnifiedProcMode)
    {
        SWSS_LOG_NOTICE("In default. LAG='%s' will be handled via IPC.", lag_name.c_str());
        m_handlers.emplace(lag_name, nullptr);  
        return true;
    }

    else {
	    if (m_lags_to_add.find(lag_name) == m_lags_to_add.end())
	    {
	    	    m_lags_to_add[lag_name] = 0;
	    }
	
	    int attempt = m_lags_to_add[lag_name];
	    auto tdc = teamdctl_alloc();
	    if (!tdc)
	    {
	    	    SWSS_LOG_ERROR("Can't allocate memory for teamdctl handler. LAG='%s'. attempt=%d", lag_name.c_str(), attempt);
	    	    m_lags_to_add[lag_name]++;
	    	    return false;
	    }
	    teamdctl_set_log_fn(tdc, &teamdctl_log_function);
	    int err = teamdctl_connect(tdc, lag_name.c_str(), nullptr, "usock");
	    if (err)
	    {
		    if (attempt != 0)
	    	    {
			    SWSS_LOG_WARN("Can't connect to teamd LAG='%s', error='%s'. attempt=%d", lag_name.c_str(), strerror(-err), attempt);
	    	    }
	    	    teamdctl_free(tdc);
	    	    m_lags_to_add[lag_name]++;
	    	    return false;
	    }
	    m_handlers.emplace(lag_name, tdc);
	    m_lags_to_add.erase(lag_name);
	    SWSS_LOG_NOTICE("The LAG '%s' has been added.", lag_name.c_str());
	    return true;
    }
}

///
/// Removes a LAG interface with lag_name from the manager
/// This method deallocates teamd structures
/// @param lag_name a name for LAG interface
/// @return true if the lag was removed successfully, false otherwise
///
bool TeamdCtlMgr::remove_lag(const std::string & lag_name)
{

    if (has_key(lag_name))
    {
	if (m_teamdUnifiedProcMode) 
        {
		m_handlers.erase(lag_name);
	}
	else {
		auto tdc = m_handlers[lag_name];
	   	teamdctl_disconnect(tdc);
		teamdctl_free(tdc);
		m_handlers.erase(lag_name);
	}
        SWSS_LOG_NOTICE("The LAG '%s' has been removed from db.", lag_name.c_str());
    }
    else if (m_lags_to_add.find(lag_name) != m_lags_to_add.end())
    {
        m_lags_to_add.erase(lag_name);
        SWSS_LOG_DEBUG("The LAG '%s' has been removed from adding queue.", lag_name.c_str());
    }
    else
    {
        SWSS_LOG_WARN("The LAG '%s' hasn't been added. Can't remove it", lag_name.c_str());
    }

    // If this lag interface errored last time, remove the entry.
    // This is needed as here in this remove API, we do m_handlers.erase(lag_name).
    if (m_lags_err_retry.find(lag_name) != m_lags_err_retry.end())
    {
        SWSS_LOG_NOTICE("The LAG '%s' had errored while getting dump, removing it", lag_name.c_str());
        m_lags_err_retry.erase(lag_name);
    }

    return true;
}

///
/// Process the queue with postponed add operations for LAG.
///
void TeamdCtlMgr::process_add_queue()
{
    std::vector<std::string> lag_names_to_add;
    std::transform(m_lags_to_add.begin(), m_lags_to_add.end(), std::back_inserter(lag_names_to_add), [](const auto & pair) { return pair.first; });
    for (const auto & lag_name: lag_names_to_add)
    {
        bool result = try_add_lag(lag_name);
        if (!result)
        {
            if (m_lags_to_add[lag_name] == TeamdCtlMgr::max_attempts_to_add)
            {
                SWSS_LOG_ERROR("Can't connect to teamd after %d attempts. LAG '%s'", TeamdCtlMgr::max_attempts_to_add, lag_name.c_str());
                m_lags_to_add.erase(lag_name);
            }
        }
    }
}

///
/// Get json dump from teamd for LAG interface with name lag_name
/// @param lag_name a name for LAG interface
/// @param to_retry is the flag used to do retry or not.
/// @return a pair. First element of the pair is true, if the method is successful
///         false otherwise. If the first element is true, the second element has a dump
///         otherwise the second element is an empty string
///
TeamdCtlDump TeamdCtlMgr::get_dump(const std::string & lag_name, bool to_retry)
{
    TeamdCtlDump res = { false, "" };
    if (has_key(lag_name))
    {

     if (m_teamdUnifiedProcMode) 
     {
    
        std::string ipc_response;
        int ret = sendIpcToTeamd("StateDump", {lag_name}, ipc_response); // Send the IPC request
        if (ret == 0)

        {
	    auto pos = ipc_response.find('{');
            if (pos != std::string::npos)
            {
                ipc_response.erase(0, pos);
            }
            res = {true, ipc_response}; // Get the response from teamd via IPC
            m_lags_err_retry.erase(lag_name); // Clear retry count on success
        }
        else
        {
            // In case of failure and retry flag is set, check if it fails for MAX_RETRY times.
            if (to_retry)
            {
                if (m_lags_err_retry.find(lag_name) != m_lags_err_retry.end())
                {
                    if (m_lags_err_retry[lag_name] == MAX_RETRY)
                    {
                        SWSS_LOG_ERROR("Can't get dump for LAG '%s'. Skipping", lag_name.c_str());
                        m_lags_err_retry.erase(lag_name);
                    }
                    else
                        m_lags_err_retry[lag_name]++;
                }
                else
                {

                    // This time a different lag interface errored out.
                    m_lags_err_retry[lag_name] = 1;
                }
            }

            else
            {
                // No need to retry if the flag is not set.
                SWSS_LOG_ERROR("Can't get dump for LAG '%s'. Skipping", lag_name.c_str());
            }
        }

     } else {
     	     auto tdc = m_handlers[lag_name];
       	     char * dump;
     	     int r = teamdctl_state_get_raw_direct(tdc, &dump);
     	     if (r == 0)
     	     {
	 	     res = { true, std::string(dump) };
	 	     // If this lag interface errored last time, remove the entry
                     if (m_lags_err_retry.find(lag_name) != m_lags_err_retry.end())
	 	     {
	     		     SWSS_LOG_NOTICE("The LAG '%s' had errored in get_dump earlier, removing it", lag_name.c_str());
		     	     m_lags_err_retry.erase(lag_name);
	 	     }
     	     }
     	     else
     	     {
	 	     // In case of failure and retry flag is set, check if it fails for MAX_RETRY times.
                     if (to_retry)
	 	     {
	     		     if (m_lags_err_retry.find(lag_name) != m_lags_err_retry.end())
	     		     {
		 		     if (m_lags_err_retry[lag_name] == MAX_RETRY)
		 		     {
		     			     SWSS_LOG_ERROR("Can't get dump for LAG '%s'. Skipping", lag_name.c_str());
		     			     m_lags_err_retry.erase(lag_name);
		 		     }
		 		     else
		     			     m_lags_err_retry[lag_name]++;
	     		     }
	     		     else
	     		     {
		 		     // This time a different lag interface errored out.
		                     m_lags_err_retry[lag_name] = 1;
	     		     }
	 	     }
	 	     else
	 	     {
	     		     // No need to retry if the flag is not set.
                             SWSS_LOG_ERROR("Can't get dump for LAG '%s'. Skipping", lag_name.c_str());
	 	     }
     	     }
     }
    }
    else
    {
        SWSS_LOG_ERROR("Can't update state. LAG not found. LAG='%s'", lag_name.c_str());
    }

    return res;
}

///
/// Get dumps for all registered LAG interfaces
/// @return vector of pairs. Each pair first value is a name of LAG, second value is a dump
///
TeamdCtlDumps TeamdCtlMgr::get_dumps(bool to_retry)
{
    TeamdCtlDumps res;

    for (const auto & p: m_handlers)
    {
        const auto & lag_name = p.first;
        const auto & result = get_dump(lag_name, to_retry);
        const auto & status = result.first;
        const auto & dump = result.second;
        if (status)
        {
            res.push_back({ lag_name, dump });
        }
    }

    return res;
}


int TeamdCtlMgr::sendIpcToTeamd(const std::string& command,
                      const std::vector<std::string>& args,
                      std::string& response_out)
{
    int sockfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sockfd < 0)
    {
        SWSS_LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, TEAMD_MULTI_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(sockfd);
        return -1;
    }

    std::ostringstream message;
    message << TEAMD_IPC_REQ << "\n" << command << "\n";

    for (size_t i = 0; i < args.size(); ++i)
    {
        message << args[i] << "\n";
    }

    std::string final_msg = message.str();
    SWSS_LOG_NOTICE("Sending IPC message to teamd:\n%s", final_msg.c_str());

    ssize_t sent = send(sockfd, final_msg.c_str(), final_msg.length(), 0);
    if (sent < 0)
    {
        SWSS_LOG_ERROR("Failed to send message to teamd: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    char buffer[65536];
    ssize_t received = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (received > 0)
    {
        buffer[received] = '\0';
        response_out = std::string(buffer);
	SWSS_LOG_NOTICE("Response from teamd to teammgrd: %s", buffer);
        close(sockfd);
        return 0;
    }
    else
    {
        SWSS_LOG_WARN("No response from teamd or recv failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }
}

