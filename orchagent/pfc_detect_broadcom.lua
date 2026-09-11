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

--------------------------------------------------------------------------------
-- Poll cadence accounting, in the PFCWD_POLL_STATS hash.
--
--   reset:   redis-cli -n <counters_db> DEL PFCWD_POLL_STATS
--   collect: redis-cli -n <counters_db> HGETALL PFCWD_POLL_STATS
--
-- COUNTERS_DB is not persisted, so these are per-boot totals; they also reset
-- whenever the database container restarts, not only on an explicit DEL.
--
-- Cost is ~16 redis calls per poll, measured at -84us +/- 40 SEM on a 21ms
-- script, i.e. below the noise floor.  Left unconditional deliberately: a debug
-- flag costs a config read per poll and carries the failure mode where the data
-- is absent the one time it is needed.
--------------------------------------------------------------------------------
local stats_key = 'PFCWD_POLL_STATS'

local function stats_incr(field, amount)
    redis.call('HINCRBY', stats_key, field, string.format('%d', amount))
end

local function stats_extreme(field, value, keep_greater)
    local cur = tonumber(redis.call('HGET', stats_key, field))
    if cur == nil or (keep_greater and value > cur) or (not keep_greater and value < cur) then
        redis.call('HSET', stats_key, field, string.format('%d', value))
    end
end

-- HMGET is capped so a very large queue set cannot overflow the Lua C stack
-- that unpack() expands into.
local function hmget_chunked(hash, keys, count)
    local out = {}
    local first = 1
    while first <= count do
        local last = math.min(first + 255, count)
        local slice = {}
        for j = first, last do
            slice[#slice + 1] = keys[j]
        end
        local res = redis.call('HMGET', hash, unpack(slice))
        for j = 1, #res do
            out[first + j - 1] = res[j]
        end
        first = last + 1
    end
    return out
end

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
--
-- string.format('%.0f') rather than tostring(): redis lua is 5.1, whose number
-- to string conversion is %.14g.  A 16 digit microsecond epoch comes back out
-- as '1.7872668687188e+15' -- the low digits are lost, and an int() on the read
-- side raises instead of parsing.
local timestamp_string = string.format('%.0f', timestamp_current)
redis.call('HSET', 'TIMESTAMP', timestamp_field_last, timestamp_string)

local time_since_last_poll = poll_time
-- not first poll
if timestamp_last ~= false then
    time_since_last_poll = (timestamp_current - tonumber(timestamp_last))
end

-- How much elapsed time a single poll may charge to the detection timer,
-- as a multiple of the configured interval.
local MAX_DETECT_CHARGE_POLLS = 2

stats_incr('poll_count', 1)
redis.call('HSET', stats_key, 'configured_us', string.format('%d', poll_time))

if timestamp_last ~= false then
    if time_since_last_poll <= 0 then
        -- redis TIME is CLOCK_REALTIME, so an NTP step lands here.  The flex
        -- counter loop sleeps on steady_clock, so this is never a real stall.
        -- The correction is unconditional: the detection decrement below and
        -- the pause-duration estimate both consume time_since_last_poll.
        stats_incr('clock_anomaly', 1)
        time_since_last_poll = poll_time
    else
        redis.call('HSET', stats_key, 'effective_us_last',
                   string.format('%d', time_since_last_poll))
        stats_incr('effective_us_sum', time_since_last_poll)
        stats_extreme('effective_us_max', time_since_last_poll, true)
        stats_extreme('effective_us_min', time_since_last_poll, false)

        -- Histogram of actual/configured in tenths: ratio_010 is 1.0x,
        -- ratio_020 is 2.0x.  The flex counter loop sleeps
        -- pollInterval - (delay % pollInterval), so a cycle that genuinely
        -- overran lands on an integer multiple; a smeared distribution means
        -- the stall is somewhere other than the poll loop.  The top bucket
        -- saturates, so the histogram alone does not describe the tail --
        -- effective_us_max does.
        if poll_time > 0 then
            local ratio_tenths = math.floor((time_since_last_poll * 10) / poll_time)
            if ratio_tenths > 200 then
                ratio_tenths = 200
            end
            stats_incr(string.format('ratio_%03d', ratio_tenths), 1)
        end
    end
end

-- time_since_last_poll is the gap between samples, not how long the queue was
-- actually paused: SAI_QUEUE_ATTR_PAUSE_STATUS is a point-in-time attribute, so
-- "paused for the whole interval" is an inference from two samples that gets
-- weaker as the gap grows.  Charging an unbounded gap to the detection timer
-- lets a single sample satisfy the whole detection time.  That is reachable in
-- practice: PFCWD_POLL_TIMESTAMP_last and the *_last counters all live in
-- COUNTERS_DB and survive pfcwd being disabled and re-enabled, so the first
-- poll after such a gap sees minutes; a long orchagent stall does the same.
-- Bounding it keeps detection at no fewer than
-- ceil(detection_time / (MAX_DETECT_CHARGE_POLLS * poll_time)) samples, and
-- the per-queue cap below raises that to at least two whenever the detection
-- time exceeds one poll interval.
-- The pause-duration estimate below deliberately keeps the true delta.
local detect_charge = time_since_last_poll
if poll_time > 0 and detect_charge > MAX_DETECT_CHARGE_POLLS * poll_time then
    detect_charge = MAX_DETECT_CHARGE_POLLS * poll_time
    stats_incr('poll_overrun_clamped', 1)
end

-- Queue and port hash reads are batched: one HMGET each rather than one HGET
-- per field.  At 496 queues the per-call overhead dominates this script.
local Q_FIELDS = {
    'PFC_WD_STATUS',
    'PFC_WD_ACTION',
    'BIG_RED_SWITCH_MODE',
    'PFC_WD_DETECTION_TIME',
    'PFC_WD_DETECTION_TIME_LEFT',
    'SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES',
    'SAI_QUEUE_STAT_PACKETS',
    'SAI_QUEUE_ATTR_PAUSE_STATUS',
    'SAI_QUEUE_STAT_PACKETS_last',
    'SAI_QUEUE_ATTR_PAUSE_STATUS_last',
    'DEBUG_STORM',
    'PFC_STAT_HISTORY',
}

-- Indices are derived, never written down twice: inserting or reordering a
-- field in Q_FIELDS cannot silently shift the reads below.
local QF = {}
for idx, name in ipairs(Q_FIELDS) do
    QF[name] = idx
end

-- Iterate through each queue
local n = table.getn(KEYS)

-- The two queue maps are looked up once for the whole key set instead of twice
-- per queue.
local queue_index_all = hmget_chunked('COUNTERS_QUEUE_INDEX_MAP', KEYS, n)
local port_id_all = hmget_chunked('COUNTERS_QUEUE_PORT_MAP', KEYS, n)

for i = n, 1, -1 do
    local queue_key = counters_table_name .. ':' .. KEYS[i]
    local qv = redis.call('HMGET', queue_key, unpack(Q_FIELDS))

    local pfc_wd_status = qv[QF.PFC_WD_STATUS]
    local pfc_wd_action = qv[QF.PFC_WD_ACTION]
    local big_red_switch_mode = qv[QF.BIG_RED_SWITCH_MODE]

    if not big_red_switch_mode and (pfc_wd_status == 'operational' or pfc_wd_action == 'alert') then
        local detection_time = qv[QF.PFC_WD_DETECTION_TIME]
        if detection_time then
            detection_time = tonumber(detection_time)
            -- A storm has to be visible in at least two samples, so one poll
            -- must never charge the whole detection time.
            local charge_cap = detection_time - poll_time
            if charge_cap < poll_time then
                charge_cap = poll_time
            end
            local queue_charge = detect_charge
            if queue_charge > charge_cap then
                queue_charge = charge_cap
            end

            local time_left = qv[QF.PFC_WD_DETECTION_TIME_LEFT]
            if not time_left  then
                time_left = detection_time
            else
                time_left = tonumber(time_left)
            end

            local queue_index = queue_index_all[i]
            local port_id = port_id_all[i]
            -- If there is no entry in COUNTERS_QUEUE_INDEX_MAP or COUNTERS_QUEUE_PORT_MAP then
            -- it means KEYS[i] queue is inserted into FLEX COUNTER DB but the corresponding
            -- maps haven't been updated yet.
            if queue_index and port_id then
                local port_key = counters_table_name .. ':' .. port_id
                local pfc_rx_pkt_key = 'SAI_PORT_STAT_PFC_' .. queue_index .. '_RX_PKTS'
                local pfc_on2off_key = 'SAI_PORT_STAT_PFC_' .. queue_index .. '_ON2OFF_RX_PKTS'

                -- Get all counters
                local occupancy_bytes = qv[QF.SAI_QUEUE_STAT_CURR_OCCUPANCY_BYTES]
                local packets = qv[QF.SAI_QUEUE_STAT_PACKETS]
                local queue_pause_status = qv[QF.SAI_QUEUE_ATTR_PAUSE_STATUS]
                local pv = redis.call('HMGET', port_key,
                                      pfc_rx_pkt_key, pfc_on2off_key,
                                      pfc_rx_pkt_key .. '_last', pfc_on2off_key .. '_last')
                -- unpacked in the same order as the HMGET directly above
                local pfc_rx_packets      = pv[1]
                local pfc_on2off          = pv[2]
                local pfc_rx_packets_last = pv[3]
                local pfc_on2off_last     = pv[4]

                if occupancy_bytes and packets and pfc_rx_packets and pfc_on2off and queue_pause_status then
                    occupancy_bytes = tonumber(occupancy_bytes)
                    packets = tonumber(packets)
                    pfc_rx_packets = tonumber(pfc_rx_packets)
                    pfc_on2off = tonumber(pfc_on2off)

                    local packets_last = qv[QF.SAI_QUEUE_STAT_PACKETS_last]
                    local queue_pause_status_last = qv[QF.SAI_QUEUE_ATTR_PAUSE_STATUS_last]

                    -- DEBUG CODE START. Uncomment to enable
                    local debug_storm = qv[QF.DEBUG_STORM]
                    -- DEBUG CODE END.

                    -- If this is not a first run, then we have last values available
                    if packets_last and pfc_rx_packets_last and pfc_on2off_last and queue_pause_status_last then
                        packets_last = tonumber(packets_last)
                        pfc_rx_packets_last = tonumber(pfc_rx_packets_last)
                        pfc_on2off_last = tonumber(pfc_on2off_last)

                        -- Check actual condition of queue being in PFC storm
                        if (pfc_rx_packets - pfc_rx_packets_last > 0 and pfc_on2off - pfc_on2off_last == 0 and queue_pause_status_last == 'true' and queue_pause_status == 'true') or
                            (debug_storm == "enabled") then
                            -- Charge the detection timer the time that actually
                            -- elapsed.  Spending the configured interval instead
                            -- makes detection a poll *count* rather than a time:
                            -- it always takes ceil(detection_time / poll_interval)
                            -- samples no matter how long those samples took, so
                            -- whenever the poll loop overruns the effective
                            -- detection time stretches by the overrun ratio,
                            -- silently.
                            if time_left <= queue_charge then
                                redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","storm"]')
                                time_left = detection_time
                            else
                                time_left = time_left - queue_charge
                            end
                        else
                            if pfc_wd_action == 'alert' and pfc_wd_status ~= 'operational' then
                                redis.call('PUBLISH', 'PFC_WD_ACTION', '["' .. KEYS[i] .. '","restore"]')
                            end
                            time_left = detection_time
                        end

                        -- estimate history
                        local pfc_stat_history = qv[QF.PFC_STAT_HISTORY]
                        if pfc_stat_history and pfc_stat_history == "enable" then
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
                    redis.call('HSET', queue_key,
                               'SAI_QUEUE_ATTR_PAUSE_STATUS_last', queue_pause_status,
                               'SAI_QUEUE_STAT_PACKETS_last', packets,
                               'PFC_WD_DETECTION_TIME_LEFT', time_left)
                    redis.call('HSET', port_key,
                               pfc_rx_pkt_key .. '_last', pfc_rx_packets,
                               pfc_on2off_key .. '_last', pfc_on2off)
                end
            end
        end
    end
end

return rets
