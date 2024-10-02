#include <string>
#include <vector>

/* Override this pointer for custom behavior */
int (*callback)(const std::string &cmd, std::string &stdout) = nullptr;

int mockCmdReturn = 0;
std::string mockCmdStdcout = "";
std::vector<std::string> mockCallArgs;

namespace swss {
    int exec(const std::string &cmd, std::string &stdout)
    {
        if (callback != nullptr)
        {
            return callback(cmd, stdout);
        }
        else
        {
            mockCallArgs.push_back(cmd);
            stdout = mockCmdStdcout;
            return mockCmdReturn;
        }
    }
}
