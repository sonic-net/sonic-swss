#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <mutex>
#include <algorithm>

#include <logger.h>
#include <producerstatetable.h>
#include <macaddress.h>
#include <exec.h>
#include <tokenize.h>
#include <shellcmd.h>
#include <warm_restart.h>
#include <select.h>

#include "macsecmgr.h"
#include "schema.h"
#include "macsecpost.h"

using namespace std;
using namespace swss;

/* select() function timeout retry time, in millisecond */
#define SELECT_TIMEOUT 1000

MacAddress gMacAddress;

static bool received_sigterm = false;
static struct sigaction old_sigaction;

static void sig_handler(int signo)
{
    SWSS_LOG_ENTER();

    if (old_sigaction.sa_handler != SIG_IGN && old_sigaction.sa_handler != SIG_DFL) {
        old_sigaction.sa_handler(signo);
    }

    received_sigterm = true;
    return;
}

/* Check if FIPS mode is enabled */
bool fipsEnabled()
{
    try
    {
        /* Check if FIPS was enabled via sonic-installer */
        std::ifstream proc_cmdline_file("/proc/cmdline");
        if (proc_cmdline_file.is_open())
        {
            std::string cmdline;
            std::getline(proc_cmdline_file, cmdline);
            if (cmdline.find("sonic_fips=1") != std::string::npos)
            {
                SWSS_LOG_NOTICE("FIPS mode detected: enabled via parameter 'sonic_fips=1'");
                return true;
            }
        }
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_ERROR("Exception while checking /proc/cmdline for FIPS mode: %s",
			e.what());
    }

    try
    {
        /* Check if FIPS was enabled via config */
        std::ifstream fips_enable_file("/etc/fips/fips_enable");
        if (fips_enable_file.is_open())
        {
            bool fips_enabled;
            fips_enable_file >> fips_enabled;
            if (fips_enabled)
            {
                SWSS_LOG_NOTICE("FIPS mode detected: enabled via config 'fips_enable=1'");
                return true;
            }
        }
    }
    catch (const std::exception& e)
    {
        SWSS_LOG_ERROR("Exception while checking /etc/fips/fips_enable for FIPS mode: %s",
			e.what());
    }
    return false;
}

/**
 * Get control plane crypto FIPS POST.
 */
bool getCpCryptoFipsPostStatus(DBConnector *stateDb)
{
    std::string res;
    std::string status("unavailable");
    bool ready = false;

    /**
     * FIPS POST status can be queried from wpa_supplicant unsing '-F' argument.
     * Execute the command with timeout to prevent indefinite blocking. Symcrypt provider,
     * on detecting a POST failure, enters an infinite loop.
     */
    int ret = swss::exec("timeout 5 /sbin/wpa_supplicant -F", res);
    if (ret == 0)
    {
        SWSS_LOG_INFO("wpa_supplicant POST query completed successfully: %s", res.c_str());
        if (res.find("FIPS POST status: pass") != std::string::npos)
        {
            ready = true;
            status = "pass";
        }
        else
        {
            status = "fail";
        }
    }
    else
    {
        SWSS_LOG_ERROR("wpa_supplicant timed out or failed (exit code: %d)", ret);
    }

    /* Publish POST status to STATE_DB for monitoring and audit trail */
    Table postTable = Table(stateDb, STATE_FIPS_MACSEC_POST_TABLE_NAME);
    std::ostringstream ostream;
    vector<FieldValueTuple> fvts;

    /* Record the POST status */
    FieldValueTuple fvt("status", status);
    fvts.push_back(fvt);

    /* Add timestamp (UTC date in ISO 8601 format) for audit trail */
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    ostream << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    ostream << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    FieldValueTuple timestamp_fvt("timestamp", ostream.str());
    fvts.push_back(timestamp_fvt);

    /* Publish to STATE_DB */
    postTable.set("crypto", fvts);
    SWSS_LOG_DEBUG("control plane crypto POST status recorded");

    return ready;
}

int main(int argc, char **argv)
{

    try
    {
        Logger::linkToDbNative("macsecmgrd");
        SWSS_LOG_NOTICE("--- Starting macsecmgrd ---");

        /* Register the signal handler for SIGTERM */
        struct sigaction sigact = {};
        sigact.sa_handler = sig_handler;
        if (sigaction(SIGTERM, &sigact, &old_sigaction))
        {
            SWSS_LOG_ERROR("failed to setup SIGTERM action handler");
            exit(EXIT_FAILURE);
        }

        swss::DBConnector cfgDb("CONFIG_DB", 0);
        swss::DBConnector stateDb("STATE_DB", 0);

        std::vector<std::string> cfg_macsec_tables = {
            CFG_MACSEC_PROFILE_TABLE_NAME,
            CFG_PORT_TABLE_NAME,
        };

        /* Perform FIPS compliance check if FIPS mode is enabled */
        bool isCpPostStateReady = false;
        if (fipsEnabled())
        {
            SWSS_LOG_NOTICE("System operating in FIPS compliant mode");
            /* Validate that the control plane cryptographic module passed POST */
            if (getCpCryptoFipsPostStatus(&stateDb))
            {
                SWSS_LOG_NOTICE("control plane crypto is FIPS ready");
                isCpPostStateReady = true;
            }
            else
            {
                SWSS_LOG_ERROR("control plane crypto is not FIPS ready");
            }
        }
        else
        {
            /* Non-FIPS mode: skip POST validation and proceed normally */
            isCpPostStateReady = true;
        }

        MACsecMgr macsecmgr(&cfgDb, &stateDb, cfg_macsec_tables);

        std::vector<Orch *> cfgOrchList = {&macsecmgr};

        swss::Select s;
        for (Orch *o : cfgOrchList)
        {
            s.addSelectables(o->getSelectables());
        }

        bool isPostStateReady = false;

        SWSS_LOG_NOTICE("starting main loop");
        while (!received_sigterm)
        {
            if (!isCpPostStateReady)
            {
                /**
                 * FIPS compliance enforcement: Continue in the infinite loop, making
                 * the service un-available
                 */
                sleep(1);
                continue;
            }

            /* Don't process any config until POST state is ready */
            if (!isPostStateReady)
            {
                std::string state = getMacsecPostState(&stateDb);
                if (state == "pass" || state == "disabled")
                {
                    SWSS_LOG_NOTICE("FIPS MACSec POST ready: state %s", state.c_str());
                    isPostStateReady = true;
                }
                else
                {
                    /* Yield before retry */
                    sleep(1);
                    continue;
                }
            }

            Selectable *sel;
            int ret;

            ret = s.select(&sel, SELECT_TIMEOUT);
            if (ret == Select::ERROR)
            {
                SWSS_LOG_NOTICE("Error: %s!", strerror(errno));
                continue;
            }
            if (ret == Select::TIMEOUT)
            {
                macsecmgr.doTask();
                continue;
            }

            auto *c = (Executor *)sel;
            c->execute();
        }
    }
    catch(const std::exception &e)
    {
        SWSS_LOG_ERROR("Runtime error: %s", e.what());
    }
    return -1;
}
