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

-- Get configuration
redis.call('SELECT', counters_db)

-- For each port ID in KEYS
for _, port in ipairs(KEYS) do
    -- Get current values from COUNTERS DB
    local trim_packets = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_TRIM_PACKETS')
    local trim_sent_packets = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_TX_TRIM_PACKETS')

    if trim_packets and trim_sent_packets then
        -- Calculate dropped packets
        local dropped_packets = tonumber(trim_packets) - tonumber(trim_sent_packets)
        -- Write result back to COUNTERS DB
        redis.call('HSET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_DROPPED_TRIM_PACKETS', dropped_packets)
        logit("Port " .. port .. " DROPPED_TRIM_PACKETS: " .. dropped_packets)
    else
        logit("Port " .. port .. " missing required counters")
    end
end

return logtable
