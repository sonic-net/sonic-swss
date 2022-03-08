#include <string>

int mockCmdReturn = 0;
std::string mockCmdStdcout = "";

namespace swss {
    int exec(const std::string &cmd, std::string &stdout)
    {
        stdout = mockCmdStdcout;
        return mockCmdReturn;
    }
}
