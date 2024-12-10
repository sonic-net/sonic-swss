#include <swss/linkcache.h>
#include <swss/logger.h>
#include <netlink/route/route.h>

static rtnl_link* g_fakeLink = [](){
    auto fakeLink = rtnl_link_alloc();
    rtnl_link_set_ifindex(fakeLink, 42);
    return fakeLink;
}();

extern int rt_build_ret;
extern bool nlmsg_alloc_ret;
extern "C"
{

struct rtnl_link* rtnl_link_get_by_name(struct nl_cache *cache, const char *name)
{
    return g_fakeLink;
}

static int build_route_msg(struct rtnl_route *tmpl, int cmd, int flags,
			   struct nl_msg **result)
{
	struct nl_msg *msg;
	int err;
	if (!(msg = nlmsg_alloc_simple(cmd, flags)))
		return -NLE_NOMEM;
	if ((err = rtnl_route_build_msg(msg, tmpl)) < 0) {
		nlmsg_free(msg);
		return err;
	}
	*result = msg;
	return 0;
}

int rtnl_route_build_add_request(struct rtnl_route *tmpl, int flags,
				 struct nl_msg **result)
{
    if (rt_build_ret != 0)
    {
        return rt_build_ret;
    }
    else if (!nlmsg_alloc_ret)
    {
	*result = NULL;
        return 0;
    }
    return build_route_msg(tmpl, RTM_NEWROUTE, NLM_F_CREATE | flags,
                           result);
}
}
