-- KEYS - generic counter IDs
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
local smooth_interval = redis.call('HGET', rates_table_name .. ':' .. 'TRAP', 'TRAP_SMOOTH_INTERVAL')
local alpha = redis.call('HGET', rates_table_name .. ':' .. 'TRAP', 'TRAP_ALPHA')
if not alpha then
  logit("Alpha is not defined")
  return logtable
end
local one_minus_alpha = 1.0 - alpha
local delta = tonumber(ARGV[3])

logit(alpha)
logit(one_minus_alpha)
logit(delta)

local n = table.getn(KEYS)
for i = 1, n do
    local state_table = rates_table_name .. ':' .. KEYS[i] .. ':' .. 'TRAP'
    local initialized = redis.call('HGET', state_table, 'INIT_DONE')
    logit(initialized)

    -- Get new COUNTERS values
    local in_pkts = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_COUNTER_STAT_PACKETS')

    if initialized == 'DONE' or initialized == 'COUNTERS_LAST' then
        -- Get old COUNTERS values
        local in_pkts_last = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'SAI_COUNTER_STAT_PACKETS_last')
        
        -- Calculate new rates values
        local rx_pps_new = (in_pkts - in_pkts_last) / delta * 1000

        if initialized == "DONE" then
            -- Get old rates values
            local rx_pps_old = redis.call('HGET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS')

            -- Smooth the rates values and store them in DB
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS', alpha*rx_pps_new + one_minus_alpha*rx_pps_old)
        else
            -- Store unsmoothed initial rates values in DB
            redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'RX_PPS', rx_pps_new)
            redis.call('HSET', state_table, 'INIT_DONE', 'DONE')
        end        
    else  
        redis.call('HSET', state_table, 'INIT_DONE', 'COUNTERS_LAST')
    end

    -- Set old COUNTERS values
    redis.call('HSET', rates_table_name .. ':' .. KEYS[i], 'SAI_COUNTER_STAT_PACKETS_last', in_pkts)    
end

return logtable
