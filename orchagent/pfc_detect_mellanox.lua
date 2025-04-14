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

-- Record the polling time
local timestamp_last = redis.call('HGET', 'TIMESTAMP', 'pfcwd_poll_timestamp_last')
local timestamp_struct = redis.call('TIME')
local timestamp_current = timestamp_struct[1] + timestamp_struct[2] / 1000000
local timestamp_string = tostring(timestamp_current)
redis.call('HSET', 'TIMESTAMP', 'pfcwd_poll_timestamp_last', timestamp_string)
local global_effective_poll_time = poll_time
local global_effective_poll_time_lasttime = redis.call('HGET', 'TIMESTAMP', 'effective_pfcwd_poll_time_last')
if timestamp_last ~= false then
    global_effective_poll_time = (timestamp_current - tonumber(timestamp_last)) * 1000000
    redis.call('HSET', 'TIMESTAMP', 'effective_pfcwd_poll_time_last', global_effective_poll_time)
end

local effective_poll_time
local effective_poll_time_lasttime
local port_timestamp_last_cache = {}

local debug_storm_global = redis.call('HGET', 'DEBUG_STORM', 'enabled') == 'true'
local debug_storm_threshold = tonumber(redis.call('HGET', 'DEBUG_STORM', 'threshold'))

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
                local pfc_duration_key = 'SAI_PORT_STAT_PFC_' .. queue_index .. '_RX_PAUSE_DURATION_US'

                -- Get port specific timestamp
                local port_timestamp_current = tonumber(redis.call('HGET', counters_table_name .. ':' .. port_id, 'PFC_WD_time_stamp'))
                if port_timestamp_current ~= nil then
                    local port_timestamp_lasttime = port_timestamp_last_cache[port_id]
                    if port_timestamp_lasttime == nil then
                        port_timestamp_lasttime = tonumber(redis.call('HGET', counters_table_name .. ':' .. port_id, 'PFC_WD_time_stamp_last'))
                        port_timestamp_last_cache[port_id] = port_timestamp_lasttime 
                        redis.call('HSET', counters_table_name .. ':' .. port_id, 'PFC_WD_time_stamp_last', port_timestamp_current)
                    end

                    if port_timestamp_lasttime ~= nil then
                        effective_poll_time = (port_timestamp_current - port_timestamp_lasttime) / 1000
                    else
                        effective_poll_time = global_effective_poll_time
                    end
                    effective_poll_time_lasttime = false
                else
                    effective_poll_time = global_effective_poll_time
                    effective_poll_time_lasttime = global_effective_poll_time_lasttime
                end

                -- Get all counters
                local occupancy_bytes = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES')
                local packets = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS')
                local pfc_rx_packets = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key)
                local pfc_duration = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_duration_key)

                if debug_storm_global then
                    redis.call('PUBLISH', 'PFC_WD_DEBUG', 'Port ID ' .. port_id .. ' Queue index ' .. queue_index .. ' occupancy ' .. occupancy_bytes .. ' packets ' .. packets .. ' pfc rx ' .. pfc_rx_packets .. ' pfc duration ' .. pfc_duration .. ' effective poll time ' .. tostring(effective_poll_time) .. '(global ' .. tostring(global_effective_poll_time) .. ')')
                end

                if occupancy_bytes and packets and pfc_rx_packets and pfc_duration then
                    occupancy_bytes = tonumber(occupancy_bytes)
                    packets = tonumber(packets)
                    pfc_rx_packets = tonumber(pfc_rx_packets)
                    pfc_duration =  tonumber(pfc_duration)

                    local packets_last = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS_last')
                    local pfc_rx_packets_last = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key .. '_last')
                    local pfc_duration_last = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_duration_key .. '_last')
                    -- DEBUG CODE START. Uncomment to enable
                    local debug_storm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'DEBUG_STORM')
                    -- DEBUG CODE END.

                    -- If this is not a first run, then we have last values available
                    if packets_last and pfc_rx_packets_last and pfc_duration_last then
                        packets_last = tonumber(packets_last)
                        pfc_rx_packets_last = tonumber(pfc_rx_packets_last)
                        pfc_duration_last = tonumber(pfc_duration_last)
                        local storm_condition = (pfc_duration - pfc_duration_last) > (effective_poll_time * 0.99)

                        if debug_storm_threshold ~= nil and (pfc_duration - pfc_duration_last) > (effective_poll_time * debug_storm_threshold / 100) then
                            redis.call('PUBLISH', 'PFC_WD_DEBUG', 'Port ID ' .. port_id .. ' Queue index ' .. queue_index .. ' occupancy ' .. occupancy_bytes .. ' packets ' .. packets .. ' pfc rx ' .. pfc_rx_packets .. ' pfc duration ' .. pfc_duration .. ' effective poll time ' .. tostring(effective_poll_time) .. ', triggered by threshold ' .. debug_storm_threshold .. '%')
                        end

                        -- Check actual condition of queue being in PFC storm
                        if (occupancy_bytes > 0 and packets - packets_last == 0 and storm_condition) or
                            -- DEBUG CODE START. Uncomment to enable
                            (debug_storm == "enabled")
                            -- DEBUG CODE END.
                            then
                            if time_left <= effective_poll_time then
                                redis.call('HDEL', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key .. '_last')
                                redis.call('HDEL', counters_table_name .. ':' .. port_id, pfc_duration_key .. '_last')
                                local occupancy_string = '"occupancy","' .. tostring(occupancy_bytes) .. '",'
                                local packets_string = '"packets","' .. tostring(packets) .. '","packets_last","' .. tostring(packets_last) .. '",'
                                local pfc_rx_packets_string = '"pfc_rx_packets","' .. tostring(pfc_rx_packets) .. '","pfc_rx_packets_last","' .. tostring(pfc_rx_packets_last) .. '",'
                                local storm_condition_string = '"pfc_duration","' .. tostring(pfc_duration) .. '","pfc_duration_last","' .. tostring(pfc_duration_last) .. '",'
                                local timestamps = '"timestamp","' .. timestamp_string .. '","timestamp_last","' .. timestamp_last .. '","effective_poll_time","' .. effective_poll_time .. '"'
                                if effective_poll_time_lasttime ~= false then
                                    timestamps = timestamps .. ',"effective_pfcwd_poll_time_last","' .. effective_poll_time_lasttime .. '"'
                                end
                                redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","storm",' .. occupancy_string .. packets_string .. pfc_rx_packets_string .. storm_condition_string .. timestamps .. ']')
                                is_deadlock = true
                                time_left = detection_time
                            else
                                time_left = time_left - effective_poll_time
                            end
                        else
                            if pfc_wd_action == 'alert' and pfc_wd_status ~= 'operational' then
                                redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","restore"]')
                            end
                            time_left = detection_time
                        end
                    end

                    -- Save values for next run
                    redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_PACKETS_last', packets)
                    redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_DETECTION_TIME_LEFT', time_left)
                    if is_deadlock == false then
                        redis.call('HSET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key .. '_last', pfc_rx_packets)
                        redis.call('HSET', counters_table_name .. ':' .. port_id, pfc_duration_key .. '_last', pfc_duration)
                    end
                end
            end
        end
    end
end

return rets
