#pragma once

#include <net/if.h>
#include <string>

namespace swss
{

// This validates interface-name syntax only. Category semantics remain in the
// YANG models and component-specific parsers. IFNAMSIZ includes the
// terminating NUL.
inline bool isValidIfname(const std::string &name)
{
    if (
        name.empty() ||
        name == "." ||
        name == ".." ||
        name.size() >= IFNAMSIZ ||
        name.front() == '-'
    )
    {
        return false;
    }

    for (unsigned char character : name)
    {
        if (!(
            (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '.' ||
            character == '_' ||
            character == '-'
        ))
        {
            return false;
        }
    }

    return true;
}

}
