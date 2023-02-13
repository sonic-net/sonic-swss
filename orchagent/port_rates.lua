-- KEYS - port IDs
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
local smooth_interval = redis.call('HGET', rates_table_name .. ':' .. 'PORT', 'PORT_SMOOTH_INTERVAL')
local alpha = redis.call('HGET', rates_table_name .. ':' .. 'PORT', 'PORT_ALPHA')
if not alpha then
  logit("Alpha is not defined")
  return logtable
end
local one_minus_alpha = 1.0 - alpha
local delta = tonumber(ARGV[3])

logit(alpha)
logit(one_minus_alpha)
logit(delta)

local function compute_rate(port)
    local state_table = rates_table_name .. ':' .. port .. ':' .. 'PORT'
    local initialized = redis.call('HGET', state_table, 'INIT_DONE')
    logit(initialized)

    -- Get new COUNTERS values
    local in_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_UCAST_PKTS')
    local in_non_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS')
    local out_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS')
    local out_non_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS')
    local in_octets = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_OCTETS')
    local out_octets = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_OCTETS')

    if not in_ucast_pkts or not in_non_ucast_pkts or not out_ucast_pkts or
       not out_non_ucast_pkts or not in_octets or not out_octets then
        logit("Not found some counters on " .. port)
        return
    end

    if initialized == 'DONE' or initialized == 'COUNTERS_LAST' then
        -- Get old COUNTERS values
        local in_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_UCAST_PKTS_last')
        local in_non_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS_last')
        local out_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS_last')
        local out_non_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS_last')
        local in_octets_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_OCTETS_last')
        local out_octets_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_OCTETS_last')

        -- Calculate new rates values
        local rx_bps_new = (in_octets - in_octets_last) / delta * 1000
        local tx_bps_new = (out_octets - out_octets_last) / delta * 1000
        local rx_pps_new = ((in_ucast_pkts + in_non_ucast_pkts) - (in_ucast_pkts_last + in_non_ucast_pkts_last)) / delta * 1000
        local tx_pps_new = ((out_ucast_pkts + out_non_ucast_pkts) - (out_ucast_pkts_last + out_non_ucast_pkts_last)) / delta * 1000

        if initialized == "DONE" then
            -- Get old rates values
            local rx_bps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'RX_BPS')
            local rx_pps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'RX_PPS')
            local tx_bps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'TX_BPS')
            local tx_pps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'TX_PPS')

            -- Smooth the rates values and store them in DB
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_BPS', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_PPS', alpha*rx_pps_new + one_minus_alpha*rx_pps_old)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_BPS', alpha*tx_bps_new + one_minus_alpha*tx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_PPS', alpha*tx_pps_new + one_minus_alpha*tx_pps_old)
        else
            -- Store unsmoothed initial rates values in DB
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_BPS', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_PPS', rx_pps_new)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_BPS', tx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_PPS', tx_pps_new)
            redis.call('HSET', state_table, 'INIT_DONE', 'DONE')
        end
    else
        redis.call('HSET', state_table, 'INIT_DONE', 'COUNTERS_LAST')
    end

    -- Set old COUNTERS values
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_UCAST_PKTS_last', in_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS_last', in_non_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS_last', out_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS_last', out_non_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_OCTETS_last', in_octets)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_OCTETS_last', out_octets)
end

local n = table.getn(KEYS)
for i = 1, n do
    compute_rate(KEYS[i])
end

return logtable
