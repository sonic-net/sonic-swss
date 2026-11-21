#include <dlfcn.h>
#include <map>
#include <utility>

extern "C"
{

#include <cstddef>
#include <team.h>

}

namespace
{

using HandlerRegistration = std::pair<const team_change_handler *, void *>;
std::map<team_handle *, HandlerRegistration> registrations;

}

extern "C"
{

int team_change_handler_register(team_handle *team, const team_change_handler *handler, void *priv)
{
    registrations[team] = {handler, priv};
    return 0;
}

void team_change_handler_unregister(team_handle *team, const team_change_handler *handler, void *priv)
{
    auto registration = registrations.find(team);
    if (registration != registrations.end() &&
        registration->second == HandlerRegistration{handler, priv})
    {
        registrations.erase(registration);
    }
}

bool team_change_handler_registered(team_handle *team)
{
    return registrations.count(team) != 0;
}

int team_change_handler_dispatch(team_handle *team)
{
    auto registration = registrations.find(team);
    if (registration == registrations.end())
    {
        return 0;
    }
    return registration->second.first->func(
            team, registration->second.second, TEAM_PORT_CHANGE);
}

void team_free(team_handle *team)
{
    registrations.erase(team);
    auto realfunc = reinterpret_cast<void (*)(team_handle *)>(dlsym(RTLD_NEXT, "team_free"));
    realfunc(team);
}

team_port* team_get_next_port(team_handle*, team_port*)
{
    return nullptr;
}

}
