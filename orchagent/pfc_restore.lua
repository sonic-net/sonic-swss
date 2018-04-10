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
    local pfc_rx_pkt_key = ''
    local pfc_wd_status = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_STATUS')
    local restoration_time = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_RESTORATION_TIME')
    local pfc_wd_action = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_ACTION')
    local big_red_switch_mode = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'BIG_RED_SWITCH_MODE')
    if not big_red_switch_mode and pfc_wd_status ~= 'operational'  and pfc_wd_action ~= 'alert' and restoration_time and restoration_time ~= '' then
        restoration_time = tonumber(restoration_time)
        local time_left = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_RESTORATION_TIME_LEFT')
        if time_left == nil then
            time_left = restoration_time
        else
            time_left = tonumber(time_left)
        end

        local queue_index = redis.call('HGET', 'COUNTERS_QUEUE_INDEX_MAP', KEYS[i])
        local port_id = redis.call('HGET', 'COUNTERS_QUEUE_PORT_MAP', KEYS[i])
        local pfc_rx_pkt_key = 'SAI_PORT_STAT_PFC_' .. queue_index .. '_RX_PKTS'

        local pfc_rx_packets = tonumber(redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key))
        local pfc_rx_packets_last = redis.call('HGET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key .. '_last')
        -- DEBUG CODE START. Uncomment to enable
        local debug_storm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'DEBUG_STORM')
        -- DEBUG CODE END.
        if pfc_rx_packets_last then
            pfc_rx_packets_last = tonumber(pfc_rx_packets_last)

            -- Check actual condition of queue being restored from PFC storm
            if (pfc_rx_packets - pfc_rx_packets_last == 0)
                -- DEBUG CODE START. Uncomment to enable
                and (debug_storm ~= "enabled")
                -- DEBUG CODE END.
            then
                if time_left <= poll_time then
                    redis.call('PUBLISH', 'PFC_WD', '["' .. KEYS[i] .. '","restore"]')
                    time_left = restoration_time
                else
                    time_left = time_left - poll_time
                end
            else
                time_left = restoration_time
            end
        end

        -- Save values for next run
        redis.call('HSET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_RESTORATION_TIME_LEFT', time_left)
        redis.call('HSET', counters_table_name .. ':' .. port_id, pfc_rx_pkt_key .. '_last', pfc_rx_packets)
    end
end

return rets
