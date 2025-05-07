#include <fstream>
#include <iostream>

#include <dbconnector.h>
#include <producerstatetable.h>
#include "zmqclient.h"
#include "zmqproducerstatetable.h"
#include "orch_zmq_config.h"
#include <schema.h>
#include <tokenize.h>

using namespace std;
using namespace swss;

static int line_index = 0;
static DBConnector db("APPL_DB", 0, true);

void usage()
{
	cout << "Usage: swssplayer <file>" << endl;
	/* TODO: Add sample input file */
}

vector<FieldValueTuple> processFieldsValuesTuple(string s)
{
	vector<FieldValueTuple> result;

	auto tuples = tokenize(s, '|');
	for (auto tuple : tuples)
	{
		auto v_tuple = tokenize(tuple, ':', 1);
		auto field = v_tuple[0];
		auto value = v_tuple.size() == 1 ? "" : v_tuple[1];
		result.push_back(FieldValueTuple(field, value));
	}

	return result;
}

shared_ptr<ProducerStateTable> get_table(unordered_map<string, shared_ptr<ProducerStateTable>>& table_map, string table_name, set<string>  zmq_tables, std::shared_ptr<ZmqClient> zmq_client)
{
    shared_ptr<ProducerStateTable> p_table= nullptr;
    auto findResult = table_map.find(table_name);
    if (findResult == table_map.end())
    {
        if ((zmq_tables.find(table_name) != zmq_tables.end()) && (zmq_client != nullptr)) {
            p_table = make_shared<ZmqProducerStateTable>(&db, table_name, *zmq_client, true);
        }
        else {
            p_table = make_shared<ProducerStateTable>(&db, table_name);
        }

        table_map.emplace(table_name, p_table);
    }
    else
    {
        p_table = findResult->second;
    }

    return p_table;
}

void processTokens(vector<string> tokens, unordered_map<string, shared_ptr<ProducerStateTable>>& table_map, set<string>  zmq_tables, std::shared_ptr<ZmqClient> zmq_client)
{
	auto key = tokens[1];

	/* Process the key */
	auto v_key = tokenize(key, ':', 1);
	auto table_name = v_key[0];
	auto key_name = v_key[1];

	auto p_producer= get_table(table_map, table_name, zmq_tables, zmq_client);

	/* Process the operation */
	auto op = tokens[2];
	if (op == SET_COMMAND)
	{
		auto tuples = processFieldsValuesTuple(tokens[3]);
		p_producer->set(key_name, tuples, SET_COMMAND);
	}
	else if (op == DEL_COMMAND)
	{
		p_producer->del(key_name, DEL_COMMAND);
	}
}

int main(int argc, char **argv)
{
	if (argc != 2)
	{
		usage();
		exit(EXIT_FAILURE);
	}

	ifstream file(argv[1]);
	string line;

    auto zmq_tables = load_zmq_tables();
    std::shared_ptr<ZmqClient> zmq_client = nullptr;
    if (zmq_tables.size() > 0)
    {
        zmq_client = create_zmq_client(ZMQ_LOCAL_ADDRESS);
    }

    unordered_map<string, shared_ptr<ProducerStateTable>> table_map;
	while (getline(file, line))
	{
		auto tokens = tokenize(line, '|', 3);
		processTokens(tokens, table_map, zmq_tables, zmq_client);

		line_index++;
	}
}
