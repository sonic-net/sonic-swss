-- KEYS - MACsec SA IDs
-- ARGV[1] - gearbox counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval
-- return OK on success

local counters_db = '2'
local gb_counters_db = ARGV[1]
local table_name = ARGV[2]

local ret = {}
local reply = {}

local n = table.getn(KEYS)
redis.call('SELECT', gb_counters_db)
for i = 1, n do
    reply[i] = redis.call('DUMP', table_name .. ':' .. KEYS[i])
end

redis.call('SELECT', counters_db)
for i = 1, n do
    ret = redis.call('RESTORE', table_name .. ':' .. KEYS[i], 0, reply[i], 'REPLACE')
end

return ret

