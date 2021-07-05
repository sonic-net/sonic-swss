#ifndef __LABEL__
#define __LABEL__

#include <stdint.h>
#include <vector>
#include <string>
#include "converter.h"
#include "tokenize.h"

namespace swss {

typedef uint32_t Label;

#define LABEL_DELIMITER '/'
#define LABEL_VALUE_MIN 0
#define LABEL_VALUE_MAX 0xFFFFF

struct LabelStack
{
    std::vector<Label> m_labelstack;
    sai_outseg_type_t  m_outseg_type;

    LabelStack() :
        m_outseg_type(SAI_OUTSEG_TYPE_SWAP) {}
    // A list of Labels separated by '/'
    LabelStack(const std::string &str)
    {
        // Expected MPLS format = "<outsegtype><labelstack>+<non-mpls-str>"
        // <outsegtype> = "swap" | "push"
        // <labelstack> = "<label0>/<label1>/../<labelN>"
        // <non-mpls-str> = returned to caller and not parsed here
        // Example = "push10100/10101+10.0.0.3@Ethernet4"
        if (str.find("swap") == 0)
        {
            m_outseg_type = SAI_OUTSEG_TYPE_SWAP;
        }
        else if (str.find("push") == 0)
        {
            m_outseg_type = SAI_OUTSEG_TYPE_PUSH;
        }
        else
        {
            // Malformed string
            std::string err = "Error converting " + str + " to MPLS NextHop";
            throw std::invalid_argument(err);
        }
        auto labels = swss::tokenize(str.substr(4), LABEL_DELIMITER);
        for (const auto &i : labels)
            m_labelstack.emplace_back(to_uint<uint32_t>(i, LABEL_VALUE_MIN, LABEL_VALUE_MAX));
    }

    inline const std::vector<Label> &getLabelStack() const
    {
        return m_labelstack;
    }

    inline size_t getSize() const
    {
        return m_labelstack.size();
    }

    inline bool empty() const
    {
        return m_labelstack.empty();
    }

    inline bool operator<(const LabelStack &o) const
    {
        return tie(m_labelstack, m_outseg_type) <
            tie(o.m_labelstack, o.m_outseg_type);
    }

    inline bool operator==(const LabelStack &o) const
    {
        return (m_labelstack == o.m_labelstack) &&
            (m_outseg_type == o.m_outseg_type);
    }

    inline bool operator!=(const LabelStack &o) const
    {
        return !(*this == o);
    }

    const std::string to_string() const
    {
        std::string str;
        if (m_labelstack.empty())
        {
            return str;
        }
        if (m_outseg_type == SAI_OUTSEG_TYPE_SWAP)
        {
            str += "swap";
        }
        else if (m_outseg_type == SAI_OUTSEG_TYPE_PUSH)
        {
            str += "push";
        }
        for (auto it = m_labelstack.begin(); it != m_labelstack.end(); ++it)
        {
            if (it != m_labelstack.begin())
            {
                str += LABEL_DELIMITER;
            }
            str += std::to_string(*it);
        }
        return str;
    }
};

}

#endif /* __LABEL__ */
