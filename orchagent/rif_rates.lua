-- KEYS - rif IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval
-- return log

local logtable = {}

local function logit(msg)
  logtable[#logtable+1] = tostring(msg)
end

local counters_db = ARGV[1]
local counters_table_name = ARGV[2]
local rates_table_name = "RATES"

-- Get configuration
redis.call('SELECT', counters_db)
local smooth_interval = redis.call('HGET', rates_table_name .. ':' .. 'RIF', 'RIF_SMOOTH_INTERVAL')
local alpha = redis.call('HGET', rates_table_name .. ':' .. 'RIF', 'RIF_ALPHA')
if not alpha then
  logit("Alpha is not defined")
  return logtable
end
local one_minus_alpha = 1.0 - alpha
local delta = tonumber(ARGV[3])

local n = table.getn(KEYS)
for i = 1, n do
    local state_table = rates_table_name .. ':' .. KEYS[i] .. ':' .. 'RIF'
    local initialized = redis.call('HGET', state_table, 'INIT_DONE')
    logit(initialized)

    -- Get new COUNTERS values
    local in_octets = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_IN_OCTETS')
    local in_pkts = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_IN_PACKETS')
    local out_octets = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_OUT_OCTETS')
    local out_pkts = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_OUT_PACKETS')

    if not in_octets or not in_pkts or not out_octets or not out_pkts then
        return logtable
    end

    if initialized == "DONE" or initialized == "COUNTERS_LAST" then
        -- Get old COUNTERS values
        local in_octets_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_IN_OCTETS_last')
        local in_pkts_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_IN_PACKETS_last')
        local out_octets_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_OUT_OCTETS_last')
        local out_pkts_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_OUT_PACKETS_last')

        if not in_octets_last or not in_pkts_last or not out_octets_last or not out_pkts_last then
            return logtable
        end

        -- Calculate new rates values
        local rx_bps_new = (in_octets - in_octets_last) / delta * 1000
        local tx_bps_new = (out_octets - out_octets_last) / delta * 1000
        local rx_pps_new = (in_pkts - in_pkts_last) / delta * 1000
        local tx_pps_new = (out_pkts - out_pkts_last) / delta * 1000

        if initialized == "DONE" then
            -- Get old rates values
            local rx_bps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'RX_BPS')
            local rx_pps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS')
            local tx_bps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'TX_BPS')
            local tx_pps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'TX_PPS')

            -- Smooth the rates values and store them in DB
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_BPS', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS', alpha*rx_pps_new + one_minus_alpha*rx_pps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_BPS', alpha*tx_bps_new + one_minus_alpha*tx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_PPS', alpha*tx_pps_new + one_minus_alpha*tx_pps_old)
        else
            -- Store unsmoothed initial rates values in DB
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_BPS', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS', rx_pps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_BPS', tx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_PPS', tx_pps_new)
            redis.call('HSET', state_table, 'INIT_DONE', 'DONE')
        end
    else
        redis.call('HSET', state_table, 'INIT_DONE', 'COUNTERS_LAST')
    end

    -- Set old COUNTERS values
    redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_IN_OCTETS_last', in_octets)
    redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_IN_PACKETS_last', in_pkts)
    redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_OUT_OCTETS_last', out_octets)
    redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_ROUTER_INTERFACE_STAT_OUT_PACKETS_last', out_pkts)
end

return logtable
