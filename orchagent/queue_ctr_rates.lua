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
local counters_table_name = "COUNTERS"
local rates_table_name = "RATES"

-- Get configuration
redis.call('SELECT', counters_db)
local alpha = redis.call('HGET', rates_table_name .. ':' .. 'QUEUE', 'QUEUE_ALPHA')
if not alpha then
  logit("Alpha is not defined")
  return logtable
end
local one_minus_alpha = 1.0 - alpha
local delta = tonumber(ARGV[3])

logit(alpha)
logit(one_minus_alpha)
logit(delta)

local function compute_qrate(key)
    local state_table = rates_table_name .. ':' .. key .. ':' .. 'QUEUE'
    local initialized = redis.call('HGET', state_table, 'INIT_DONE')
    logit(initialized)

    -- Get new COUNTERS values
    local total_packets = redis.call('HGET', counters_table_name .. ':' .. key, 'SAI_QUEUE_STAT_PACKETS')
    local total_bytes = redis.call('HGET', counters_table_name .. ':' .. key, 'SAI_QUEUE_STAT_BYTES')

    if not total_packets or not total_bytes then
        logit("Not found queue counters stats on " .. key)
        return
    end

    if initialized == 'DONE' or initialized == 'COUNTERS_LAST' then
        -- Get old COUNTERS values
        local total_packets_last = redis.call('HGET', rates_table_name .. ':' .. key, 'SAI_QUEUE_STAT_PACKETS_last')
        local total_bytes_last = redis.call('HGET', rates_table_name .. ':' .. key, 'SAI_QUEUE_STAT_BYTES_last')

        -- Calculate new rates values
        local pkts_ps_new = (total_packets - total_packets_last) / delta * 1000 * 1000
        local bytes_ps_new = (total_bytes - total_bytes_last) / delta * 1000 * 1000
       local bits_ps_new = bytes_ps_new * 8

        if initialized == "DONE" then
            -- Get old rates values
            local pkts_ps_old = redis.call('HGET', rates_table_name .. ':' .. key, 'Q_PPS')
            local bytes_ps_old = redis.call('HGET', rates_table_name .. ':' .. key, 'Q_BPS')
           local bits_ps_old = redis.call('HGET', rates_table_name .. ':' .. key, 'Q_bPS')

            -- Smooth the rates values and store them in DB
            redis.call('HSET', rates_table_name .. ':' .. key, 'Q_PPS', alpha*pkts_ps_new + one_minus_alpha*pkts_ps_old)
            redis.call('HSET', rates_table_name .. ':' .. key, 'Q_BPS', alpha*bytes_ps_new + one_minus_alpha*bytes_ps_old)
           redis.call('HSET', rates_table_name .. ':' .. key, 'Q_bPS', alpha*bits_ps_new + one_minus_alpha*bits_ps_old)
        else
            -- Store unsmoothed initial rates values in DB
            redis.call('HSET', rates_table_name .. ':' .. key, 'Q_PPS', pkts_ps_new)
            redis.call('HSET', rates_table_name .. ':' .. key, 'Q_BPS', bytes_ps_new)
           redis.call('HSET', rates_table_name .. ':' .. key, 'Q_bPS', bits_ps_new)
            redis.call('HSET', state_table, 'INIT_DONE', 'DONE')
        end
    else
        redis.call('HSET', state_table, 'INIT_DONE', 'COUNTERS_LAST')
    end

    -- Set old COUNTERS values
    redis.call('HSET', rates_table_name .. ':' .. key, 'SAI_QUEUE_STAT_PACKETS_last', total_packets)
    redis.call('HSET', rates_table_name .. ':' .. key, 'SAI_QUEUE_STAT_BYTES_last', total_bytes)
end

local n = table.getn(KEYS)
for i = 1, n do
    compute_qrate(KEYS[i])
end
