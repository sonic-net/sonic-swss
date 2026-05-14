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

local function parse_boolean(str) return str == 'true' end
local function parse_number(str) return tonumber(str) or 0 end

local function updateTimePaused(port_key, prio, time_since_last_poll)
    -- Estimate that queue paused for entire poll duration
    local total_pause_time_field        = 'SAI_PORT_STAT_PFC_' .. prio .. '_RX_PAUSE_DURATION_US'
    local recent_pause_time_field       = 'EST_PORT_STAT_PFC_' .. prio .. '_RECENT_PAUSE_TIME_US'

    local recent_pause_time_us = parse_number(
        redis.call('HGET', port_key, recent_pause_time_field)
    )
    local total_pause_time_us = redis.call('HGET', port_key, total_pause_time_field)

    -- Only estimate total time when no SAI support
    if not total_pause_time_us then
        total_pause_time_field = 'EST_PORT_STAT_PFC_' .. prio .. '_RX_PAUSE_DURATION_US'
        total_pause_time_us = parse_number(
            redis.call('HGET', port_key, total_pause_time_field)
        )

        local total_pause_time_us_new = total_pause_time_us + time_since_last_poll
        redis.call('HSET', port_key, total_pause_time_field, total_pause_time_us_new)
    end

    local recent_pause_time_us_new = recent_pause_time_us + time_since_last_poll
    redis.call('HSET', port_key, recent_pause_time_field, recent_pause_time_us_new)
end

local function restartRecentTime(port_key, prio, timestamp_last)
    local recent_pause_time_field      = 'EST_PORT_STAT_PFC_' .. prio .. '_RECENT_PAUSE_TIME_US'
    local recent_pause_timestamp_field = 'EST_PORT_STAT_PFC_' .. prio .. '_RECENT_PAUSE_TIMESTAMP'

    redis.call('HSET', port_key, recent_pause_timestamp_field, timestamp_last)
    redis.call('HSET', port_key, recent_pause_time_field, 0)
end

-- Get the time since the last poll, used to compute total and recent times
local timestamp_field_last = 'PFCWD_POLL_TIMESTAMP_last'
local timestamp_last = redis.call('HGET', 'TIMESTAMP', timestamp_field_last)
local time = redis.call('TIME')
-- convert to microseconds
local timestamp_current = tonumber(time[1]) * 1000000 + tonumber(time[2])

-- save current poll as last poll
local timestamp_string = tostring(timestamp_current)
redis.call('HSET', 'TIMESTAMP', timestamp_field_last, timestamp_string)

local time_since_last_poll = poll_time
-- not first poll
if timestamp_last ~= false then
    time_since_last_poll = (timestamp_current - tonumber(timestamp_last))
end

-- Iterate through each queue
local n = table.getn(KEYS)
for i = n, 1, -1 do
    local counter_keys = redis.call('HKEYS', counters_table_name .. ':' .. KEYS[i])
    local counter_num = 0
    local old_counter_num = 0
    local is_deadlock = false
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
            -- If there is no entry in COUNTERS_QUEUE_INDEX_MAP or COUNTERS_QUEUE_PORT_MAP then
            -- it means KEYS[i] queue is inserted into FLEX COUNTER DB but the corresponding
            -- maps haven't been updated yet.
            if queue_index and port_id then
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
                        if (pfc_rx_packets - pfc_rx_packets_last > 0 and pfc_on2off - pfc_on2off_last == 0 and queue_pause_status_last == 'true' and queue_pause_status == 'true') or
                            (debug_storm == "enabled") then
                            if time_left <= poll_time then
                                redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","storm"]')
                                is_deadlock = true
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

                        -- estimate history
                        local pfc_stat_history = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_STAT_HISTORY')
                        if pfc_stat_history and pfc_stat_history == "enable" then
                            local port_key      = counters_table_name .. ':' .. port_id
                            local was_paused    = parse_boolean(queue_pause_status_last)
                            local now_paused    = parse_boolean(queue_pause_status)

                            -- Activity has occured
                            if pfc_rx_packets > pfc_rx_packets_last then
                                -- fresh recent pause period
                                if not was_paused then
                                    restartRecentTime(port_key, queue_index, timestamp_last)
                                end
                                -- Estimate entire interval paused if there was pfc activity
                                updateTimePaused(port_key, queue_index, time_since_last_poll)
                            else
                                -- queue paused entire interval without activity
                                if now_paused and was_paused then
                                    updateTimePaused(port_key, queue_index, time_since_last_poll)
                                end
                            end
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
    end
end

return rets
