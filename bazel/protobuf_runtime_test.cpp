// Verify DASH messages and SWSS's dependency closure use one shared runtime.
#include <dlfcn.h>
#include <link.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <google/protobuf/stubs/common.h>
#include "dash_api/vnet.pb.h"

static_assert(GOOGLE_PROTOBUF_VERSION == 3021012,
              "SWSS and DASH require Protobuf 3.21.12 headers");

namespace {
int find_protobuf(struct dl_phdr_info *info, size_t, void *data)
{
    const std::string path(info->dlpi_name);
    if (path.find("libprotobuf") != std::string::npos)
        static_cast<std::vector<std::string> *>(data)->push_back(path);
    return 0;
}
}

int main()
{
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    dash::vnet::Vnet original;
    original.set_vni(4242);
    std::string bytes;
    dash::vnet::Vnet parsed;
    if (!original.SerializeToString(&bytes) || !parsed.ParseFromString(bytes) ||
        parsed.vni() != original.vni() || parsed.ShortDebugString().empty())
        return EXIT_FAILURE;

    Dl_info location{};
    if (!dladdr(reinterpret_cast<void *>(&google::protobuf::internal::VerifyVersion), &location))
        return EXIT_FAILURE;
    std::vector<std::string> runtimes;
    dl_iterate_phdr(find_protobuf, &runtimes);
    if (runtimes.size() != 1 || runtimes.front().find("libprotobuf.so.32") == std::string::npos ||
        std::string(location.dli_fname).find("libprotobuf.so.32") == std::string::npos)
    {
        std::cerr << "Expected one shared libprotobuf.so.32; found " << runtimes.size() << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "protobuf_version=" << GOOGLE_PROTOBUF_VERSION << "\n"
              << "protobuf_library=" << location.dli_fname << "\n"
              << "protobuf_loaded=" << runtimes.front() << "\n"
              << "dash_roundtrip=passed\n";
    return EXIT_SUCCESS;
}
