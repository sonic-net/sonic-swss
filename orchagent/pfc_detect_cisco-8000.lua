-- KEYS - queue IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval (milliseconds)
-- return queue Ids that satisfy criteria

local counters_db = ARGV[1]
local counters_table_name = ARGV[2]
local poll_time = tonumber(ARGV[3]) * 1000

local rets = {}

redis.call('SELECT', counters_db)

-- Iterate through each queue
local n = table.getn(KEYS)
for i = n, 1, -1 do
    local counter_keys = redis.call('HKEYS', counters_table_name .. ':' .. KEYS[i])
    local counter_num = 0
    local old_counter_num = 0
    local pfc_wd_status = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_STATUS')
    local pfc_wd_action = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_ACTION')
    local big_red_switch_mode = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'BIG_RED_SWITCH_MODE')
    if not big_red_switch_mode and (pfc_wd_status == 'operational' or pfc_wd_action == 'alert') then
        local detection_time = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_DETECTION_TIME')
        if detection_time then
            detection_time = tonumber(detection_time)
            local time_left = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_DETECTION_TIME_LEFT')
            if not time_left  then
                time_left = detection_time
            else
                time_left = tonumber(time_left)
            end

            local queue_index = redis.call('HGET', 'COUNTERS_QUEUE_INDEX_MAP', KEYS[i])
            local port_id = redis.call('HGET', 'COUNTERS_QUEUE_PORT_MAP', KEYS[i])

            -- Get PFC status
            local packets = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS')
            local queue_pause_status = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_ATTR_PAUSE_STATUS')

            if packets and queue_pause_status then

                -- DEBUG CODE START. Uncomment to enable
                local debug_storm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'DEBUG_STORM')
                -- DEBUG CODE END.

                -- Check actual condition of queue being in PFC storm
                if (queue_pause_status == 'true')
                    -- DEBUG CODE START. Uncomment to enable
                    or (debug_storm == "enabled")
                    -- DEBUG CODE END.
                    then
                    if time_left <= poll_time then
                        redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","storm"]')
                        time_left = detection_time
                    else
                        time_left = time_left - poll_time
                    end
                else
                    if pfc_wd_action == 'alert' and pfc_wd_status ~= 'operational' then
                        redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","restore"]')
                    end
                    time_left = detection_time
                end

                -- Save values for next run
                redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_DETECTION_TIME_LEFT', time_left)
                redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_ATTR_PAUSE_STATUS_last', queue_pause_status)
                redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS_last', packets)
            end
        end
    end
end

return rets
