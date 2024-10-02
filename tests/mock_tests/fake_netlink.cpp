#include <swss/linkcache.h>
#include <swss/logger.h>

static rtnl_link* g_fakeLink = [](){
    auto fakeLink = rtnl_link_alloc();
    rtnl_link_set_ifindex(fakeLink, 42);
    return fakeLink;
}();

extern "C"
{

struct rtnl_link* rtnl_link_get_by_name(struct nl_cache *cache, const char *name)
{
    return g_fakeLink;
}

}
