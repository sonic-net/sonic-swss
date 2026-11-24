-- KEYS - queue IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval (milliseconds)
-- ARGV[4] - secondary poll factor, 0 when unset
-- ARGV[5] - steady clock cycle start (microseconds), only from a syncd with sonic-sairedis#2071
--
-- PFC stat history for the hardware PFC watchdog, which runs no detect plugin.
-- Writes the same EST_PORT_STAT_PFC_* fields as the history in the software
-- watchdog's pfc_detect_broadcom.lua, for queues with PFC_STAT_HISTORY=enable
-- on the ports listed in PFC_WD_STAT_HISTORY:PORTS.

local counters_db = ARGV[1]
local counters_table_name = ARGV[2]
local poll_time = tonumber(ARGV[3]) * 1000

local rets = {}

redis.call('SELECT', counters_db)

-- The orch lists the ports with history enabled; only their queues are read.
local listed_ports = redis.call('HKEYS', 'PFC_WD_STAT_HISTORY:PORTS')
if #listed_ports == 0 then
    return rets
end
local history_ports = {}
for _, port in ipairs(listed_ports) do
    history_ports[port] = true
end

local function parse_boolean(str) return str == 'true' end
local function parse_number(str) return tonumber(str) or 0 end

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
    local total_pause_time_field  = 'SAI_PORT_STAT_PFC_' .. prio .. '_RX_PAUSE_DURATION_US'
    local recent_pause_time_field = 'EST_PORT_STAT_PFC_' .. prio .. '_RECENT_PAUSE_TIME_US'

    -- Only estimate total time when the SAI does not provide it
    if not redis.call('HGET', port_key, total_pause_time_field) then
        total_pause_time_field = 'EST_PORT_STAT_PFC_' .. prio .. '_RX_PAUSE_DURATION_US'
        redis.call('HINCRBY', port_key, total_pause_time_field,
                   string.format('%d', time_since_last_poll))
    end

    redis.call('HINCRBY', port_key, recent_pause_time_field,
               string.format('%d', time_since_last_poll))
end

local function restartRecentTime(port_key, prio, timestamp)
    redis.call('HSET', port_key,
               'EST_PORT_STAT_PFC_' .. prio .. '_RECENT_PAUSE_TIMESTAMP', timestamp,
               'EST_PORT_STAT_PFC_' .. prio .. '_RECENT_PAUSE_TIME_US', 0)
end

-- The steady clock cycle stamp, when passed, is preferred over redis TIME,
-- which is CLOCK_REALTIME and read only after counter collection.
local time = redis.call('TIME')
local timestamp_current = tonumber(time[1]) * 1000000 + tonumber(time[2])
-- %.0f, not tostring(): lua 5.1 renders numbers with %.14g and would drop the
-- low digits of a microsecond epoch.
local timestamp_string = string.format('%.0f', timestamp_current)
local timestamp_last = redis.call('HGET', 'TIMESTAMP', 'PFCWD_POLL_TIMESTAMP_last')
redis.call('HSET', 'TIMESTAMP', 'PFCWD_POLL_TIMESTAMP_last', timestamp_string)

local time_since_last_poll = poll_time
if timestamp_last ~= false then
    time_since_last_poll = timestamp_current - tonumber(timestamp_last)
end

local cycle_start_us = tonumber(ARGV[5])
if cycle_start_us ~= nil then
    -- same field as pfc_detect_broadcom.lua
    local cycle_last = redis.call('HGET', 'PFCWD_POLL_STATS', 'cycle_start_last')
    redis.call('HSET', 'PFCWD_POLL_STATS', 'cycle_start_last',
               string.format('%.0f', cycle_start_us))
    if cycle_last ~= false and cycle_start_us > tonumber(cycle_last) then
        time_since_last_poll = cycle_start_us - tonumber(cycle_last)
    end
end

-- An NTP step can make the realtime gap zero or negative
if time_since_last_poll <= 0 then
    time_since_last_poll = poll_time
end

-- A new pause period started at the previous poll
local recent_pause_timestamp = timestamp_last
if recent_pause_timestamp == false then
    recent_pause_timestamp = string.format('%.0f', timestamp_current - time_since_last_poll)
end

local n = table.getn(KEYS)
local queue_index_all = hmget_chunked('COUNTERS_QUEUE_INDEX_MAP', KEYS, n)
local port_id_all = hmget_chunked('COUNTERS_QUEUE_PORT_MAP', KEYS, n)

for i = 1, n do
    local queue_index = queue_index_all[i]
    local port_id = port_id_all[i]

    local queue_key = counters_table_name .. ':' .. KEYS[i]

    -- A queue can be in the FLEX COUNTER DB before the queue maps carry it
    local qv = {}
    if queue_index and port_id and history_ports[port_id] then
        qv = redis.call('HMGET', queue_key,
                        'PFC_STAT_HISTORY',
                        'SAI_QUEUE_ATTR_PAUSE_STATUS',
                        'SAI_QUEUE_ATTR_PAUSE_STATUS_last')
    end
    local pfc_stat_history = qv[1]
    local queue_pause_status = qv[2]
    local queue_pause_status_last = qv[3]

    if pfc_stat_history == 'enable' and queue_pause_status then
        local port_key = counters_table_name .. ':' .. port_id
        local pfc_rx_pkt_key = 'SAI_PORT_STAT_PFC_' .. queue_index .. '_RX_PKTS'
        local pv = redis.call('HMGET', port_key, pfc_rx_pkt_key, pfc_rx_pkt_key .. '_last')
        local pfc_rx_packets = pv[1]
        local pfc_rx_packets_last = pv[2]

        if pfc_rx_packets then
            -- The first poll after registration only records the baseline. It
            -- stores "not paused", so a pause it sees opens a new period at the
            -- next poll instead of continuing one from before the registration.
            local paused = false

            if pfc_rx_packets_last and queue_pause_status_last then
                local was_paused = parse_boolean(queue_pause_status_last)
                local rx = parse_number(pfc_rx_packets)
                local rx_last = parse_number(pfc_rx_packets_last)

                -- PFC activity, or a paused queue, means paused for the whole
                -- interval.  Pause status can read paused a poll before the RX
                -- counter moves, so a pause seen first opens the period too.
                -- A queue the watchdog has acted on reads unpaused while PFC
                -- keeps arriving, so this verdict, not the status, is the baseline.
                paused = parse_boolean(queue_pause_status)
                if rx < rx_last then
                    -- Another flex counter group can write the same RX counter
                    -- from an older read, and a counter can reset: the poll
                    -- keeps the previous verdict rather than reading as quiet
                    paused = was_paused or paused
                else
                    paused = rx > rx_last or paused
                end
                if paused then
                    if not was_paused then
                        restartRecentTime(port_key, queue_index, recent_pause_timestamp)
                    end
                    updateTimePaused(port_key, queue_index, time_since_last_poll)
                end
            end

            redis.call('HSET', queue_key, 'SAI_QUEUE_ATTR_PAUSE_STATUS_last', paused and 'true' or 'false')
            redis.call('HSET', port_key, pfc_rx_pkt_key .. '_last', pfc_rx_packets)
        end
    end
end

return rets
