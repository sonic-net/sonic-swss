extern "C"
{

#include <cstddef>
#include <team.h>

team_handle* team_alloc()
{
    return reinterpret_cast<team_handle*>(1);
}

int team_init(team_handle*, uint32_t)
{
    return 0;
}

void team_free(team_handle*)
{
}

int team_change_handler_register(team_handle*, const team_change_handler*, void*)
{
    return 0;
}

void team_change_handler_unregister(team_handle*, const team_change_handler*, void*)
{
}

team_port* team_get_next_port(team_handle*, team_port*)
{
    return nullptr;
}

}
