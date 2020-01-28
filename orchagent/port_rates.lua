-- KEYS - port IDs
-- ARGV[1] - poll time interval
-- return nothing 

local asic_db = 1 
local counters_db = 2
local config_db = 4 
-- local fc_db = 5
local counters_table_name = "COUNTERS" 
local asic_table_name = "ASIC_STATE:SAI_OBJECT_TYPE_PORT" 
local rates_table_name = "RATES"
-- local fc_group_table_name = "FLEX_COUNTER_GROUP_TABLE:PORT_STAT_COUNTER"

local rets = {}

-- Get configuration
redis.call('SELECT', config_db)
local smooth_interval = redis.call('GET', rates_table_name .. ':' .. 'PORT_SMOOTH_INTERVAL')
local alpha = redis.call('GET', rates_table_name .. ':' .. 'PORT_ALPHA')
local one_minus_alpha = 1 - alpha
local delta = ARGV[1]

-- Get speeds
local speeds = {}
redis.call('SELECT', asic_db)
local n = table.getn(KEYS)
for i = 1, n do
    speeds[i] = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_PORT_ATTR_SPEED')

redis.call('SELECT', counters_db)

local initialized = redis.call('GET', rates_table_name .. ':' .. 'INIT_DONE')

for i = 1, n do

    -- Get new COUNTERS values
    local in_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_UCAST_PKTS')
    local in_non_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS')
    local out_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS')
    local out_non_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS')
    local in_octets = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_OCTETS')
    local out_octets = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_OCTETS')

    if initialized == "DONE" or initialized == "COUNTERS_LAST" then
        -- Get old COUNTERS values
        local in_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_UCAST_PKTS_last')
        local in_non_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS_last')
        local out_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS_last')
        local out_non_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS_last')
        local in_octets_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_OCTETS_last')
        local out_octets_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_OCTETS_last')
        
        -- Calculate new rates values
        local rx_bps_new = (in_octets - in_octets_last)/delta
        local tx_bps_new = (out_octets - out_octets_last)/delta
        local rx_pps_new = ((in_ucast_pkts_last + in_non_ucast_pkts) - (in_ucast_pkts_last_last + in_non_ucast_pkts_last))/delta
        local tx_pps_new = ((out_ucast_pkts_last + out_non_ucast_pkts) - (out_ucast_pkts_last_last + out_non_ucast_pkts_last))/delta
        local rx_util_new = rx_bps_new/speeds[i]
        local tx_util_new = tx_bps_new/speeds[i]


        if initialized == "DONE" then
            -- Get old rates values
            local rx_bps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'RX_BPS')
            local rx_pps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS')
            local tx_bps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'TX_BPS')
            local tx_pps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'TX_PPS')
            local rx_util_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'RX_UTIL')
            local tx_util_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'TX_UTIL')

            -- Smooth the rates values and store them in DB
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_BPS', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_BPS', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_PPS', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_UTIL', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_UTIL', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)    
        else
            -- Store unsmoothed initial rates values in DB
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_BPS', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_BPS', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_PPS', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_UTIL', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'TX_UTIL', rx_bps_new) 
        end        
    
    elseif initialized == "NONE" then

        -- Set old COUNTERS values
        redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_UCAST_PKTS_last', in_ucast_pkts)
        redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS_last', in_non_ucast_pkts)
        redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS_last', out_ucast_pkts)
        redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS_last', out_non_ucast_pkts)
        redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_OCTETS_last', in_octets)
        redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_OCTETS_last', out_octets)        

        redis.call('SET', rates_table_name .. ':' .. 'INIT_DONE', "COUNTERS_LAST")

    end
