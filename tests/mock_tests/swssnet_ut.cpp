#include "ut_helper.h"
#include "swssnet.h"

namespace swssnet_test
{
    struct SwssNetTest : public ::testing::Test
    {
        SwssNetTest() {}
    };

    TEST_F(SwssNetTest, CovertSAIPrefixToSONiCPrefix)
    {
        IpPrefix ip_prefix("1.2.3.4/24");
        sai_ip_prefix_t sai_prefix;
        swss::copy(sai_prefix, ip_prefix);
        IpPrefix ip_prefix_copied = swss::getIpPrefixFromSaiPrefix(sai_prefix);
        ASSERT_EQ("1.2.3.4/24", ip_prefix_copied.to_string());

        IpPrefix ip_prefix1("1.2.3.4/32");
        swss::copy(sai_prefix, ip_prefix1);
        ip_prefix_copied = swss::getIpPrefixFromSaiPrefix(sai_prefix);
        ASSERT_EQ("1.2.3.4/32", ip_prefix_copied.to_string());

        IpPrefix ip_prefix2("0.0.0.0/0");
        swss::copy(sai_prefix, ip_prefix2);
        ip_prefix_copied = swss::getIpPrefixFromSaiPrefix(sai_prefix);
        ASSERT_EQ("0.0.0.0/0", ip_prefix_copied.to_string());

        IpPrefix ip_prefix3("2000::1/128");
        swss::copy(sai_prefix, ip_prefix3);
        ip_prefix_copied = swss::getIpPrefixFromSaiPrefix(sai_prefix);
        ASSERT_EQ("2000::1/128", ip_prefix_copied.to_string());

        IpPrefix ip_prefix4("::/0");
        swss::copy(sai_prefix, ip_prefix4);
        ip_prefix_copied = swss::getIpPrefixFromSaiPrefix(sai_prefix);
        ASSERT_EQ("::/0", ip_prefix_copied.to_string());
    }
}
