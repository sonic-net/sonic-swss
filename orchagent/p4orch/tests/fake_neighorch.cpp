#include "neighorch.h"

/* vxlanorch.cpp asks NeighOrch to resolve remote VTEPs; no p4orch test reaches it. */
void NeighOrch::resolveNeighbor(const NeighborEntry &, bool)
{
}

void NeighOrch::cancelResolveNeighbor(const NeighborEntry &)
{
}

bool NeighOrch::hasNextHop(const NextHopKey &)
{
    return false;
}
