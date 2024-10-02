#include <string>
#include <vector>

int mockCmdReturn = 0;
std::string mockCmdStdcout = "";
std::vector<std::string> mockCallArgs;

namespace swss {
    int exec(const std::string &cmd, std::string &stdout)
    {
        mockCallArgs.push_back(cmd);
        stdout = mockCmdStdcout;
        return mockCmdReturn;
    }
}
