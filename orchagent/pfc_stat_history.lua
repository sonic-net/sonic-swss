-- KEYS - port IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval (milliseconds)
local counters_db = ARGV[1]
local counters_table_name = ARGV[2]
local poll_time = tonumber(ARGV[3]) * 1000


local pfc_hist_table_name = "PFC_STAT_HISTORY"
local rets = {}

local function parse_boolean(str) return str == "true" end
local function parse_number(str) return tonumber(str) or 0 end

redis.call('SELECT', counters_db)


local n = table.getn(KEYS)

for i = n, 1, -1 do
    -- port oid
    local port = KEYS[i]
    local port_key = pfc_hist_table_name .. ':' .. port

    for prio = 0, 7 do
        local total_pause_time_field        = 'TOTAL_PAUSE_TIME_MS_' .. prio
        local total_pause_transitions_field = 'TOTAL_PAUSE_TRANSITIONS_' .. prio
        local recent_pause_timestamp_field  = 'RECENT_PAUSE_TIMESTAMP_' .. prio
        local recent_pause_time_field       = 'RECENT_PAUSE_TIME_MS_' .. prio
        local prev_num_frames_field         = 'PREV_RX_PKTS_' .. prio
        local pause_field                   = 'PAUSE_' .. prio

        -- 1) get the updated pfc frame count
        local num_frames_new = parse_number(
            redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_PFC_' .. prio .. '_RX_PKTS')
        )
        -- skip this port if no data
        if num_frames_new == false then break end

        -- 2) get the info stored at last poll
        local total_pause_time_ms = parse_number(
            redis.call('HGET', port_key, total_pause_time_field)
        )
        local total_pause_transitions = parse_number(
            redis.call('HGET', port_key, total_pause_transitions_field)
        )
        local recent_pause_timestamp = parse_number(
            redis.call('HGET', port_key, recent_pause_timestamp_field)
        )
        local recent_pause_time_ms = parse_number(
            redis.call('HGET', port_key, recent_pause_time_field)
        )
        local num_frames_old = parse_number(
            redis.call('HGET', port_key, prev_num_frames_field)
        )
        local was_paused = parse_boolean(
            redis.call('HGET', port_key, pause_field)
        )

        -- if the framecount increased then pause activity occured
        local now_paused = num_frames_new > num_frames_old
        -- Fetch current time from Redis, microsecond precision
        local time = redis.call('TIME')
        local seconds = tonumber(time[1])
        local microseconds = tonumber(time[2])
        local now_ms = (seconds * 1000) + math.floor(microseconds / 1000)

        -- transitioned unpaused to paused, assume pause starts this moment, update db to reflect that
        if not was_paused and now_paused then
            local total_pause_transitions_new = total_pause_transitions + 1;
            redis.call('HSET', port_key, total_pause_transitions_field, total_pause_transitions_new)
            redis.call('HSET', port_key, recent_pause_timestamp_field, now_ms)
            redis.call('HSET', port_key, recent_pause_time_field, 0)

        --  paused to paused OR paused to unpaused, assume paused since last check
        elseif was_paused then
            -- recent time is diff between now and when pause started
            local recent_pause_time_ms_new = now_ms - recent_pause_timestamp
            -- total time is current total time adds new recentpause
            local total_pause_time_ms_new = total_pause_time_ms
                - recent_pause_time_ms
                + recent_pause_time_ms_new;

            redis.call('HSET', port_key, total_pause_time_field, total_pause_time_ms_new)
            redis.call('HSET', port_key, recent_pause_time_field, recent_pause_time_ms_new)
        end

        redis.call('HSET', port_key, prev_num_frames_field, num_frames_new)
        redis.call('HSET', port_key, pause_field, tostring(now_paused))
    end
end

return rets
