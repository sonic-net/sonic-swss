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
    local counter_num = 0
    local old_counter_num = 0
    local is_deadlock = false
    local pfc_wd_status = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_STATUS')
    local pfc_wd_action = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_ACTION')
    if pfc_wd_status == 'operational' or pfc_wd_action == 'alert' then
        local detection_time = tonumber(redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_DETECTION_TIME'))
        local time_left = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_DETECTION_TIME_LEFT')
        if not time_left  then
            time_left = detection_time
        else
            time_left = tonumber(time_left)
        end

        local queue_index = redis.call('HGET', 'COUNTERS_QUEUE_INDEX_MAP', KEYS[i])
        local port_id = redis.call('HGET', 'COUNTERS_QUEUE_PORT_MAP', KEYS[i])
        local pfc_rx_pkt_key = 'SAI_PORT_STAT_PFC_' .. queue_index .. '_RX_PKTS'
        local pfc_on2off_key = 'SAI_PORT_STAT_PFC_' .. queue_index .. '_ON2OFF_RX_PKTS'

        -- Get all counters
        local occupancy_bytes = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES')
        local packets = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS')
        local pfc_rx_packets = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key)
        local pfc_on2off = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_on2off_key)
        local queue_pause_status = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_ATTR_PAUSE_STATUS')

        if occupancy_bytes and packets and pfc_rx_packets and pfc_on2off and queue_pause_status then
            occupancy_bytes = tonumber(occupancy_bytes)
            packets = tonumber(packets)
            pfc_rx_packets = tonumber(pfc_rx_packets)
            pfc_on2off = tonumber(pfc_on2off)

            local packets_last = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS_last')
            local pfc_rx_packets_last = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key .. '_last')
            local pfc_on2off_last = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_on2off_key .. '_last')
            local queue_pause_status_last = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_ATTR_PAUSE_STATUS_last')

            -- DEBUG CODE START. Uncomment to enable
            local debug_storm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'DEBUG_STORM')
            -- DEBUG CODE END.

            -- If this is not a first run, then we have last values available
            if packets_last and pfc_rx_packets_last and pfc_on2off_last and queue_pause_status_last then
                packets_last = tonumber(packets_last)
                pfc_rx_packets_last = tonumber(pfc_rx_packets_last)
                pfc_on2off_last = tonumber(pfc_on2off_last)

                -- Check actual condition of queue being in PFC storm
                if (occupancy_bytes > 0 and packets - packets_last == 0 and pfc_rx_packets - pfc_rx_packets_last > 0) or
                    -- DEBUG CODE START. Uncomment to enable
                    (debug_storm == "enabled") or
                    -- DEBUG CODE END.
                    (occupancy_bytes == 0 and pfc_rx_packets - pfc_rx_packets_last > 0 and pfc_on2off - pfc_on2off_last == 0 and queue_pause_status_last == 'true' and queue_pause_status == 'true') then
                    if time_left <= poll_time then
                        redis.call('PUBLISH', 'PFC_WD', '["' .. KEYS[i] .. '","storm"]')
                        is_deadlock = true
                        time_left = detection_time
                    else
                        time_left = time_left - poll_time
                    end
                else
                    if pfc_wd_action == 'alert' and pfc_wd_status ~= 'operational' then
                        redis.call('PUBLISH', 'PFC_WD', '["' .. KEYS[i] .. '","restore"]')
                    end
                    time_left = detection_time
                end
            end

        -- Save values for next run
            redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_ATTR_PAUSE_STATUS_last', queue_pause_status)
            redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS_last', packets)
            redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_DETECTION_TIME_LEFT', time_left)
            redis.call('HSET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key .. '_last', pfc_rx_packets)
            redis.call('HSET', counters_table_name .. ':' .. port_id, pfc_on2off_key .. '_last', pfc_on2off)
        end
    end
end

return rets
