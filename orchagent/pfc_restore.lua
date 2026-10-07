-- KEYS - queue IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval (milliseconds)
-- ARGV[4] - secondary poll factor, 0 when unset
-- ARGV[5] - monotonic cycle start, microseconds
-- return queue Ids that satisfy criteria

local counters_db = ARGV[1]
local counters_table_name = ARGV[2]
local poll_time = tonumber(ARGV[3]) * 1000
-- Previous poll's cycle start.  This is plugin state, not telemetry, so it
-- lives in the TIMESTAMP hash with the poll stamps the detect plugins keep
-- there, not in PFCWD_POLL_STATS: that hash is written only by
-- pfc_detect_broadcom.lua, while this script runs on every platform but
-- cisco-8000.
local timestamp_key = 'TIMESTAMP'
local cycle_start_field = 'pfcwd_restore_cycle_start_last'

local rets = {}

redis.call('SELECT', counters_db)

-- Charge the restoration timer the time that actually elapsed, not the interval
-- the poll was configured with.  Charging the configured value makes
-- restoration a poll COUNT rather than a time: it needs
-- ceil(restoration_time / poll_interval) quiet polls however long those polls
-- really took, so whenever the poll loop overruns the queue stays mitigated for
-- longer than was asked for, and nothing reports it.  This is the same
-- correction already made on the detection side.
--
-- ARGV[5] is a steady_clock stamp taken before collection, so successive values
-- differ by the true cycle period.  It arrives only from a syncd carrying
-- sonic-sairedis#2071; with an older syncd it is nil and the configured interval
-- is charged exactly as before.
--
-- The detection plugin runs in the same cycle off the same ARGV and keeps its
-- own 'cycle_start_last' (in PFCWD_POLL_STATS).  This one must not share it:
-- whichever plugin ran second would read the stamp the first had just
-- written, see a zero gap and charge nothing at all.
local restore_charge = poll_time
local cycle_start_us = tonumber(ARGV[5])

if cycle_start_us ~= nil then
    local cycle_last = redis.call('HGET', timestamp_key, cycle_start_field)
    -- %.0f, not tostring(): lua 5.1 renders numbers with %.14g and would drop
    -- the low digits of a large microsecond stamp.
    redis.call('HSET', timestamp_key, cycle_start_field,
               string.format('%.0f', cycle_start_us))
    if cycle_last ~= false then
        local monotonic_gap = cycle_start_us - tonumber(cycle_last)
        if monotonic_gap > 0 then
            restore_charge = monotonic_gap
        end
    end
end

-- The whole gap is charged, however long.  A poll counts as quiet only when
-- the port's PFC RX counter has not moved since the previous poll, and that
-- counter accumulates, so a quiet poll after a long gap means no pause frame
-- arrived at any point in that gap: the queue really was quiet for all of it.
-- Charging less would stretch restoration by the overrun, which is what this
-- change removes.  A gap that spans a stop/start or an orchagent restart is
-- not charged at all, because orchagent deletes the baseline whenever a queue
-- is registered or unregistered; the first poll then only records one.

-- Iterate through each queue
local n = table.getn(KEYS)
for i = n, 1, -1 do
    local pfc_rx_pkt_key = ''
    local pfc_wd_status = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_STATUS')
    local restoration_time = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_RESTORATION_TIME')
    local pfc_wd_action = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_ACTION')
    local big_red_switch_mode = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'BIG_RED_SWITCH_MODE')
    if not big_red_switch_mode and pfc_wd_status ~= 'operational'  and pfc_wd_action ~= 'alert' and restoration_time and restoration_time ~= '' then
        restoration_time = tonumber(restoration_time)
        local time_left = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'PFC_WD_RESTORATION_TIME_LEFT')
        if not time_left then
            time_left = restoration_time
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
                    if time_left <= restore_charge then
                        redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","restore"]')
                        time_left = restoration_time
                    else
                        time_left = time_left - restore_charge
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
end

return rets
