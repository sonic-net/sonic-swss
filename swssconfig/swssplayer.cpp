#include <fstream>
#include <iostream>

#include <dbconnector.h>
#include <producerstatetable.h>
#include <schema.h>
#include <tokenize.h>

using namespace std;
using namespace swss;

constexpr int DB_PORT = 6379;
constexpr char* DB_HOSTNAME = "localhost";

static int line_index = 0;
static DBConnector db(APPL_DB, DB_HOSTNAME, DB_PORT, 0);

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

void processTokens(vector<string> tokens)
{
	auto key = tokens[1];

	/* Process the key */
	auto v_key = tokenize(key, ':', 1);
	auto table_name = v_key[0];
	auto key_name = v_key[1];

	ProducerStateTable producer(&db, table_name);

	/* Process the operation */
	auto op = tokens[2];
	if (op == SET_COMMAND)
	{
		auto tuples = processFieldsValuesTuple(tokens[3]);
		producer.set(key_name, tuples, SET_COMMAND);
	}
	else if (op == DEL_COMMAND)
	{
		producer.del(key_name, DEL_COMMAND);
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

	while (getline(file, line))
	{
		auto tokens = tokenize(line, '|', 3);
		processTokens(tokens);

		line_index++;
	}
}
