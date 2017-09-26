-- KEYS - queue IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval
-- return queue Ids that satisfy criteria

local counters_db = ARGV[1]
local counters_table_name = ARGV[2]
local poll_time = tonumber(ARGV[3])

local rets = {}

redis.call('SELECT', counters_db)

-- Iterate through each queue
local n = table.getn(KEYS)
for i = n, 1, -1 do
    local counter_keys = redis.call('HKEYS', counters_table_name .. ':' .. KEYS[i])
    local pfc_duration_key = ''
    local pfc_rx_pkt_key = ''
    local counter_num = 0
    local old_counter_num = 0
    local is_deadlock = false

    local m = table.getn(counter_keys)
    for j = m, 1, -1 do
        if string.sub(counter_keys[j],-string.len('RX_PKTS'))=='RX_PKTS' then
            pfc_rx_pkt_key = counter_keys[j]
            counter_num = counter_num + 1
        elseif string.sub(counter_keys[j],-string.len('RX_PAUSE_DURATION'))=='RX_PAUSE_DURATION' then
            pfc_duration_key = counter_keys[j]
            counter_num = counter_num + 1
        elseif counter_keys[j] == 'SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES' or counter_keys[j] == 'SAI_QUEUE_STAT_PACKETS' then
            counter_num = counter_num + 1
        elseif string.sub(counter_keys[j],-string.len('_last'))=='_last' then
            old_counter_num = old_counter_num + 1
        end
    end

    -- Check if all counters are present
    if counter_num == 4 then
        local occupancy_bytes = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES'))
        local packets = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS'))
        local pfc_rx_packets = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], pfc_rx_pkt_key))
        local pfc_duration = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], pfc_duration_key))

        local packets_last = 0
        local pfc_rx_packets_last = 0
        local pfc_duration_last = 0
        -- DEBUG CODE START. Uncomment to enable
        local debug_storm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'DEBUG_STORM')
        -- DEBUG CODE END.

        -- If this is a first run for a queue, just save values
        if old_counter_num ~= 3 then
            packets_last = packets
            pfc_rx_packets_last = pfc_rx_packets
            pfc_duration_last = pfc_duration
        else
            packets_last = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS_last'))
            pfc_rx_packets_last = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], pfc_rx_pkt_key .. '_last'))
            pfc_duration_last = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], pfc_duration_key .. '_last'))

            -- Check actual condition of queue being in PFC storm
            if (occupancy_bytes > 0 and packets - packets_last == 0 and pfc_rx_packets - pfc_rx_packets_last > 0) or
                -- DEBUG CODE START. Uncomment to enable
                (debug_storm == "enabled") or
                -- DEBUG CODE END.
                (occupancy_bytes == 0 and packets - packets_last == 0 and (pfc_duration - pfc_duration_last) > poll_time * 0.8) then
                table.insert(rets, KEYS[i])
                is_deadlock = true
            end
        end

        -- Save values for next run
        redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS_last', packets)
        redis.call('HSET', counters_table_name .. ':' .. KEYS[i], pfc_rx_pkt_key .. '_last', pfc_rx_packets)
        if is_deadlock then
            redis.call('HDEL', counters_table_name .. ':' .. KEYS[i], pfc_duration_key .. '_last')
        else
            redis.call('HSET', counters_table_name .. ':' .. KEYS[i], pfc_duration_key .. '_last', pfc_duration)
        end
    end
end

return rets
