#include <iostream>
#include <inttypes.h>
#include "logger.h"
#include "select.h"
#include "selectabletimer.h"
#include "netdispatcher.h"
#include "warmRestartHelper.h"
#include "bfdsyncd/bfdlink.h"
#include "subscriberstatetable.h"


using namespace std;
using namespace swss;

int main(int argc, char **argv)
{
    int dflag = 0;
    char *port_str = NULL;
    int index;
    int c;
    unsigned short port = 0;

    opterr = 0;

    while ((c = getopt (argc, argv, "hdp:")) != -1)
        switch (c)
            {
            case 'h':
                cout << "Usage: bfdsyncd -d -p <tcp port number>" << endl;
                break;
            case 'd':
                dflag = 1;
                break;
            case 'p':
                port_str = optarg;
                sscanf(port_str, "%hd", &port);
                break;
            case '?':
                if (optopt == 'p')
                    fprintf (stderr, "Option -%c requires a TCP port number.\n", optopt);
                else if (isprint (optopt))
                {
                    fprintf (stderr, "Unknown option `-%c'.\n", optopt);
                    SWSS_LOG_ERROR("Unknown option `-%c'.\n", optopt);
                }
                else
                {
                    fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
                    SWSS_LOG_ERROR( "Unknown option character `\\x%x'.\n", optopt);
                }
                return 1;
            default:
                break;
            }

    if (port == 0)
    {
        port = BFD_DATA_PLANE_DEFAULT_PORT;
    }
    cout << "debug flag " << dflag << ", port = " << port << endl;

    for (index = optind; index < argc; index++)
        cout << "Non-option argument " << argv[index] << endl;

    swss::Logger::linkToDbNative("bfdsyncd");
    DBConnector db("APPL_DB", 0);
    RedisPipeline pipeline(&db);

    DBConnector stateDb("STATE_DB", 0);

    SubscriberStateTable bfdstateTableSubscriber(&stateDb, STATE_BFD_SESSION_TABLE_NAME);

    while (true)
    {
        try
        {
            BfdLink bfd(&db, &stateDb, port, dflag);
            Select s;
           
            /*
             * Pipeline should be flushed right away to deal with state pending
             * from previous try/catch iterations.
             */
            pipeline.flush();

            SWSS_LOG_INFO("Waiting for bfd-client connection... ");
            cout << "Waiting for bfd-client connection... " << endl;
            bfd.accept();
            SWSS_LOG_INFO("bfd-client connected!");
            cout << "bfd-client connected" << endl;

            s.addSelectable(&bfd);
            s.addSelectable(&bfdstateTableSubscriber);

            while (true)
            {
                Selectable *temps;

                s.select(&temps);

                if (temps == &bfdstateTableSubscriber)
                {
                    std::deque<KeyOpFieldsValuesTuple> keyOpFvsQueue;
                    bfdstateTableSubscriber.pops(keyOpFvsQueue);

                    for (const auto& keyOpFvs: keyOpFvsQueue)
                    {
                        const auto& key = kfvKey(keyOpFvs);
                        const auto& op = kfvOp(keyOpFvs);
                        const auto& fvs = kfvFieldsValues(keyOpFvs);

                        SWSS_LOG_DEBUG("Received bfd state update for key %s, op %s", key.c_str(), op.c_str());

                        //Does not support DEL_COMMAND for state update 
                        if (op != SET_COMMAND) 
                        {
                            SWSS_LOG_INFO("bfdsyncd support SET_OP only, get key %s, op %s for state_db, ignored", key.c_str(), op.c_str());
                            continue;
                        }

                        bfd.handleBfdStateUpdate(key, fvs);
                    }
                }
                else if (temps == &bfd) {
                    SWSS_LOG_DEBUG("Received bfd message (select)");
                }
                else
                {
                    pipeline.flush();
                    SWSS_LOG_DEBUG("Pipeline flushed");
                }
            }
        }
        catch (BfdLink::BfdConnectionClosedException &e)
        {
            cout << "Connection lost \"" << e.what() << "\" reconnecting..." << endl;
            SWSS_LOG_ERROR("Bfdd connection closed exception had been thrown in daemon.");
        }
        catch (const exception& e)
        {
            cout << "Exception \"" << e.what() << "\" had been thrown in daemon" << endl;
            SWSS_LOG_ERROR("Exception had been thrown in daemon.");
            return 1;
        }
    }

    return 1;
}
