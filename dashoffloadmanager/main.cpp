#include <sstream>
#include <memory>
#include <boost/program_options.hpp>

#include "dbconnector.h"
#include "zmqserver.h"
#include "select.h"

#include "dashoffloadmanager.h"
#include "dpuinfoprovider.h"

#define DEFAULT_ZMQ_PORT 8100

using namespace swss;
using namespace std;
namespace po = boost::program_options;

struct Args
{
    string zmq_server_base_addr;
    uint16_t zmq_port;
};

bool parse_args(int argc, char **argv, Args &args)
{
    po::options_description desc("Usage");
    desc.add_options()
        ("help,h", "Show help")
        ("zmq_server_base_addr", po::value<std::string>(&args.zmq_server_base_addr)->required(), "ZMQ Proxy server base address")
        ("zmq_port", po::value<uint16_t>(&args.zmq_port)->default_value(DEFAULT_ZMQ_PORT), "ZMQ port")
    ;

    try
    {
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help"))
        {
            cout << desc << "\n";
            return 0;
        }

        po::notify(vm);
    }
    catch(po::error& e)
    {
        cerr << "Error: " << e.what() << "\n\n";
        cerr << desc << "\n";
        return false;
    }

    return true;
}

string getIpByindex(const string& base, uint32_t index)
{
    struct in_addr addr;
    inet_pton(AF_INET, base.c_str(), &addr);

    uint32_t new_ip = ntohl(addr.s_addr) + index;
    addr.s_addr = htonl(new_ip);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);

    return ip_str;
}

int main(int argc, char **argv)
{
    SonicDBConfig::initializeGlobalConfig(SonicDBConfig::DEFAULT_SONIC_DB_GLOBAL_CONFIG_FILE);
    Logger::linkToDbNative("dashoffloadmanager");

    Args args;
    if (!parse_args(argc, argv, args))
    {
        return EXIT_FAILURE;
    }

    vector<DpuInfo> dpus;
    auto ok = getDpuInfo(dpus);
    if (!ok)
    {
        SWSS_LOG_ERROR("Failed to read dpu info");
        return EXIT_FAILURE;
    }

    std::vector<unique_ptr<DashOffloadManager>> dpu_offload_managers;

    for (const auto& dpu : dpus)
    {
        string zmqServerAddr = "tcp://" + getIpByindex(args.zmq_server_base_addr, dpu.dpuId) + ":" + to_string(args.zmq_port);
        string zmqDpuAddr = "tcp://" + dpu.mgmtAddr + ":" + to_string(args.zmq_port);

        auto om = make_unique<DashOffloadManager>(dpu, zmqServerAddr, zmqDpuAddr);
        om->start();

        dpu_offload_managers.push_back(move(om));
    }

    for (auto& manager: dpu_offload_managers)
    {
        manager->join();
    }

    return 0;
}
