-- KEYS - None
-- ARGV - None

local result = {}
local config_db = "4"
local state_db = "6"

redis.call("SELECT", state_db)
local asic_keys = redis.call("KEYS", "ASIC_TABLE*")
local cell_size = tonumber(redis.call("HGET", asic_keys[1], "cell_size"))

-- Based on cell_size, calculate singular headroom
local ppg_headroom = 400 * cell_size

redis.call("SELECT", config_db)
local ports = redis.call("KEYS", "PORT|*")
local ports_num = #ports

-- 2 PPGs per port, 70% of possible maximum value.
local shp_size = math.ceil(ports_num * 2 * ppg_headroom * 0.7)

local ingress_lossless_pool_size_fixed = tonumber(redis.call('HGET', 'BUFFER_POOL|ingress_lossless_pool', 'size'))
local ingress_lossy_pool_size_fixed = tonumber(redis.call('HGET', 'BUFFER_POOL|ingress_lossy_pool', 'size'))
local egress_lossy_pool_size_fixed = tonumber(redis.call('HGET', 'BUFFER_POOL|egress_lossy_pool', 'size'))

table.insert(result, "ingress_lossless_pool" .. ":" .. ingress_lossless_pool_size_fixed .. ":" .. shp_size)
table.insert(result, "ingress_lossy_pool" .. ":" .. ingress_lossy_pool_size_fixed)
table.insert(result, "egress_lossy_pool" .. ":" .. egress_lossy_pool_size_fixed)

return result
