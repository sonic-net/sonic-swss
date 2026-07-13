extern "C"
{

#include <cstddef>
#include <team.h>

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
