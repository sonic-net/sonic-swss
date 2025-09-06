// Copyright (c) 2025 Arista Networks, Inc.  All rights reserved.
// Arista Networks, Inc. Confidential and Proprietary.

#ifndef ORCHAGENT_MACSECPOST_H
#define ORCHAGENT_MACSECPOST_H

using namespace std;

namespace swss {

void setMacsecPostState(DBConnector *stateDb, string postState);
string getMacsecPostState(DBConnector *stateDb);

}

#endif // ORCHAGENT_MACSECPOST_H
