    #ifndef __DBCONNECTOR__
#define __DBCONNECTOR__

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <map>
#include <memory>
#include <mutex>
#include <boost/functional/hash.hpp>
#include <boost/algorithm/string.hpp>

#include <hiredis/hiredis.h>
#include "rediscommand.h"
#include "redisreply.h"
#define EMPTY_NAMESPACE std::string()
#define EMPTY_CONTAINERNAME std::string()

namespace swss {

class DBConnector;
class PubSub;

class RedisInstInfo
{
public:
    std::string unixSocketPath;
    std::string hostname;
    int port;
};

class SonicDBInfo
{
public:
    std::string instName;
    int dbId;
    std::string separator;
};

struct SonicDBKey
{
    /* Container name is used to identify the container that the DB instance is running in.
     * Namespace is used to identify the network namespace that the DB instance is running in.
     * In our design, we allow multiple containers to share a same namespace.
     * So, this combination of container name and namespace is used to uniquely identify a DB instance.
     * If the namespace is empty, it means the DB instance is running in the default(host) namespace.
     * If the container name is empty, it for adapting the old design that only one DB instance is 
     * running in a namespace.
     */
    std::string containerName;
    std::string netns;

    SonicDBKey() = default;
    SonicDBKey(const std::string &ns) : netns(ns) {}

    bool operator==(const SonicDBKey& other) const
    {
        return containerName == other.containerName && netns == other.netns;
    }

    bool isEmpty() const
    {
        return containerName.empty() && netns.empty();
    }

    std::string toString() const
    {
        std::vector<std::string> buffer;
        buffer.push_back(containerName);
        buffer.push_back(netns);
        return boost::algorithm::join(buffer, ":");
    }
};

struct SonicDBKeyHash
{
    std::size_t operator()(const SonicDBKey& k) const
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, k.containerName);
        boost::hash_combine(seed, k.netns);
        return seed;
    }
};

class SonicDBConfig
{
public:
    static constexpr const char *DEFAULT_SONIC_DB_CONFIG_FILE = "/var/run/redis/sonic-db/database_config.json";
    static constexpr const char *DEFAULT_SONIC_DB_GLOBAL_CONFIG_FILE = "/var/run/redis/sonic-db/database_global.json";
    static void initialize(const std::string &file = DEFAULT_SONIC_DB_CONFIG_FILE);
#if defined(SWIG) && defined(SWIGPYTHON)
    %pythoncode %{
        ## TODO: the python function and C++ one is not on-par
        @staticmethod
        def load_sonic_db_config(sonic_db_file_path=DEFAULT_SONIC_DB_CONFIG_FILE):
            SonicDBConfig.initialize(sonic_db_file_path)
    %}
#endif

    static void initializeGlobalConfig(const std::string &file = DEFAULT_SONIC_DB_GLOBAL_CONFIG_FILE);
#if defined(SWIG) && defined(SWIGPYTHON)
    %pythoncode %{
        ## TODO: the python function and C++ one is not on-par
        @staticmethod
        def load_sonic_global_db_config(global_db_file_path=DEFAULT_SONIC_DB_GLOBAL_CONFIG_FILE, namespace=None):
            SonicDBConfig.initializeGlobalConfig(global_db_file_path)
    %}
#endif
    static void reset();

    static void validateNamespace(const std::string &netns);
    static std::string getDbInst(const std::string &dbName, const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static std::string getDbInst(const std::string &dbName, const SonicDBKey &key);
    static int getDbId(const std::string &dbName, const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static int getDbId(const std::string &dbName, const SonicDBKey &key);
    static std::string getSeparator(const std::string &dbName, const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static std::string getSeparator(const std::string &dbName, const SonicDBKey &key);
    static std::string getSeparator(int dbId, const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static std::string getSeparator(int dbId, const SonicDBKey &key);
    static std::string getSeparator(const DBConnector* db);
    static std::string getDbSock(const std::string &dbName, const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static std::string getDbSock(const std::string &dbName, const SonicDBKey &key);
    static std::string getDbHostname(const std::string &dbName, const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static std::string getDbHostname(const std::string &dbName, const SonicDBKey &key);
    static int getDbPort(const std::string &dbName, const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static int getDbPort(const std::string &dbName, const SonicDBKey &key);
    static std::vector<std::string> getNamespaces();
    static std::vector<SonicDBKey> getDbKeys();
#if defined(SWIG) && defined(SWIGPYTHON)
    %pythoncode %{
        ## TODO: the python function and C++ one is not on-par
        @staticmethod
        def get_ns_list():
            return SonicDBConfig.getNamespaces()
    %}
#endif

    static std::vector<std::string> getDbList(const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static std::vector<std::string> getDbList(const SonicDBKey &key);
    static bool isInit() { return m_init; };
    static bool isGlobalInit() { return m_global_init; };
    static std::map<std::string, RedisInstInfo> getInstanceList(const std::string &netns = EMPTY_NAMESPACE, const std::string &containerName=EMPTY_CONTAINERNAME);
    static std::map<std::string, RedisInstInfo> getInstanceList(const SonicDBKey &key);

private:
    static std::recursive_mutex m_db_info_mutex;
    // { {containerName, namespace}, { instName, { unix_socket_path, hostname, port } } }
    static std::unordered_map<SonicDBKey, std::map<std::string, RedisInstInfo>, SonicDBKeyHash> m_inst_info;
    // { {containerName, namespace}, { dbName, {instName, dbId, separator} } }
    static std::unordered_map<SonicDBKey, std::unordered_map<std::string, SonicDBInfo>, SonicDBKeyHash> m_db_info;
    // { {containerName, namespace}, { dbId, separator } }
    static std::unordered_map<SonicDBKey, std::unordered_map<int, std::string>, SonicDBKeyHash> m_db_separator;
    static bool m_init;
    static bool m_global_init;
    static void parseDatabaseConfig(const std::string &file,
                                    std::map<std::string, RedisInstInfo> &inst_entry,
                                    std::unordered_map<std::string, SonicDBInfo> &db_entry,
                                    std::unordered_map<int, std::string> &separator_entry);
    static RedisInstInfo& getRedisInfo(const std::string &dbName, const SonicDBKey &key);
    static SonicDBInfo& getDbInfo(const std::string &dbName, const SonicDBKey &key);
};

class RedisContext
{
public:
    static constexpr const char *DEFAULT_UNIXSOCKET = "/var/run/redis/redis.sock";

    /*
     * Connect to Redis DB wither with a hostname:port or unix socket
     * Select the database index provided by "db"
     *
     * Timeout - The time in milisecond until exception is been thrown. For
     *           infinite wait, set this value to 0
     */
    RedisContext(const RedisContext &other);
    RedisContext& operator=(const RedisContext&) = delete;

    ~RedisContext();

    redisContext *getContext() const;

    /*
     * Assign a name to the Redis client used for this connection
     * This is helpful when debugging Redis clients using `redis-cli client list`
     */
    void setClientName(const std::string& clientName);

    std::string getClientName();

protected:
    RedisContext();
    void initContext(const char *host, int port, const timeval *tv);
    void initContext(const char *path, const timeval *tv);
    void setContext(redisContext *ctx);

private:
    redisContext *m_conn;
};

class DBConnector : public RedisContext
{
public:
    static constexpr const char *DEFAULT_UNIXSOCKET = "/var/run/redis/redis.sock";

    /*
     * Connect to Redis DB wither with a hostname:port or unix socket
     * Select the database index provided by "db"
     *
     * Timeout - The time in milisecond until exception is been thrown. For
     *           infinite wait, set this value to 0
     */
    explicit DBConnector(const DBConnector &other);
    DBConnector(int dbId, const RedisContext &ctx);
    DBConnector(int dbId, const std::string &hostname, int port, unsigned int timeout_ms);
    DBConnector(int dbId, const std::string &unixPath, unsigned int timeout_ms);
    DBConnector(const std::string &dbName, unsigned int timeout_ms, bool isTcpConn = false);
    DBConnector(const std::string &dbName, unsigned int timeout_ms, bool isTcpConn, const std::string &netns);
    DBConnector(const std::string &dbName, unsigned int timeout_ms, bool isTcpConn, const SonicDBKey &key);
    DBConnector& operator=(const DBConnector&) = delete;

    int getDbId() const;
    std::string getDbName() const;
    std::string getNamespace() const;

#if defined(SWIG) && defined(SWIGPYTHON)
    %pythoncode %{
        namespace = property(getNamespace)
    %}
#endif
    SonicDBKey getDBKey() const;

    static void select(DBConnector *db);

    /* Create new context to DB */
    DBConnector *newConnector(unsigned int timeout) const;

#ifndef SWIG
    __attribute__((deprecated))
#endif
    PubSub *pubsub();

    int64_t del(const std::string &key);

#if defined(SWIG) && defined(SWIGPYTHON)
    // SWIG interface file (.i) globally rename map C++ `del` to python `delete`,
    // but applications already followed the old behavior of auto renamed `_del`.
    // So we implemented old behavior for backward compatibility
    // TODO: remove this function after applications use the function name `delete`
    %pythoncode %{
        def _del(self, *args, **kwargs):
            return self.delete(*args, **kwargs)
    %}
#endif

    bool exists(const std::string &key);

    int64_t hdel(const std::string &key, const std::string &field);

    int64_t hdel(const std::string &key, const std::vector<std::string> &fields);

    void del(const std::vector<std::string>& keys);

    template <typename ReturnType=std::unordered_map<std::string, std::string>>
    ReturnType hgetall(const std::string &key);

#ifndef SWIG
    template <typename OutputIterator>
    void hgetall(const std::string &key, OutputIterator result);
#endif

    std::vector<std::string> keys(const std::string &key);

    std::pair<int, std::vector<std::string>> scan(int cursor = 0, const char *match = "", uint32_t count = 10);

    bool set(const std::string &key, const std::string &value);
    bool set(const std::string &key, int value);

    void hset(const std::string &key, const std::string &field, const std::string &value);

    template<typename InputIterator>
    void hmset(const std::string &key, InputIterator start, InputIterator stop);

    void hmset(const std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>& multiHash);

    std::shared_ptr<std::string> get(const std::string &key);

    std::shared_ptr<std::string> hget(const std::string &key, const std::string &field);

    bool hexists(const std::string &key, const std::string &field);

    int64_t incr(const std::string &key);

    int64_t decr(const std::string &key);

    int64_t rpush(const std::string &list, const std::string &item);

    std::shared_ptr<std::string> blpop(const std::string &list, int timeout);

    void subscribe(const std::string &pattern);

    void psubscribe(const std::string &pattern);

    void punsubscribe(const std::string &pattern);

    int64_t publish(const std::string &channel, const std::string &message);

    void config_set(const std::string &key, const std::string &value);

    bool flushdb();

    std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> getall();
private:
    void setNamespace(const std::string &netns);
    void setDBKey(const SonicDBKey &key);

    int m_dbId;
    std::string m_dbName;
    SonicDBKey m_key;
};

template <typename ReturnType>
ReturnType DBConnector::hgetall(const std::string &key)
{
    ReturnType map;
    hgetall(key, std::inserter(map, map.end()));
    return map;
}

#ifndef SWIG
template<typename OutputIterator>
void DBConnector::hgetall(const std::string &key, OutputIterator result)
{
    RedisCommand shgetall;
    shgetall.format("HGETALL %s", key.c_str());
    RedisReply r(this, shgetall, REDIS_REPLY_ARRAY);

    auto ctx = r.getContext();

    for (unsigned int i = 0; i < ctx->elements; i += 2)
    {
        *result = std::make_pair(ctx->element[i]->str, ctx->element[i+1]->str);
        ++result;
    }
}
#endif

template<typename InputIterator>
void DBConnector::hmset(const std::string &key, InputIterator start, InputIterator stop)
{
    RedisCommand shset;
    shset.formatHSET(key, start, stop);
    RedisReply r(this, shset, REDIS_REPLY_INTEGER);
}

}
#endif
