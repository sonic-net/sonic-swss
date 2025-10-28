-- KEYS - port IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval (milliseconds)

local counters_db = ARGV[1]
local config_db = 4
local debug_drop_monitor_stat_table = 'DEBUG_DROP_MONITOR_STATS'
local persistent_drop_alert_table = 'PERSISTENT_DROP_ALERTS'

redis.call('SELECT', counters_db)

-- Helper functions
local function parse_boolean(str) return str == "true" end
local function parse_number(str) return tonumber(str) or 0 end

-- Get the debug counters and port name map
local debug_counter_to_port_stat_map = redis.call('HGETALL', "COUNTERS_DEBUG_NAME_PORT_STAT_MAP")
local debug_counter_to_port_stat_map_len = redis.call('HLEN', "COUNTERS_DEBUG_NAME_PORT_STAT_MAP")
local port_name_map = redis.call('HGETALL', "COUNTERS_PORT_NAME_MAP")
local port_name_map_len = redis.call('HLEN', "COUNTERS_PORT_NAME_MAP")

-- Iterate over the debug counter and get their specific configuration
for debug_counter_index = 1, debug_counter_to_port_stat_map_len, 2 do
    local debug_counter = debug_counter_to_port_stat_map[debug_counter_index]
    local debug_counter_stat = debug_counter_to_port_stat_map[debug_counter_index + 1]

    -- Get the configuration of debug counter
    redis.call('SELECT', config_db)
    local debug_counter_table = "DEBUG_COUNTER|" .. debug_counter
    local status = redis.call('HGET', debug_counter_table, 'drop_monitor_status')
    local drop_count_threshold = parse_number(redis.call('HGET', debug_counter_table, 'drop_count_threshold'))
    local incident_count_threshold = parse_number(redis.call('HGET', debug_counter_table, 'incident_count_threshold'))
    local window = parse_number(redis.call('HGET', debug_counter_table, 'window'))
    redis.call('SELECT', counters_db)

    -- Detect persistent drops if status is enabled
    if status == 'enabled' then
        -- Iterate over all ports
        for port_index = 1, port_name_map_len, 2 do
            -- Get counter stats
            local port = port_name_map[port_index]
            local port_oid = port_name_map[port_index + 1]
            local counter_stat_map = "COUNTERS:" .. port_oid
            local current_drop_count = parse_number(redis.call('HGET', counter_stat_map, debug_counter_stat))

            -- Calculate the delta since previous poll
            local prev_drop_count = parse_number(redis.call('HGET', debug_drop_monitor_stat_table .. '|' .. debug_counter .. '|' .. port, 'prev_drop_count'))
            local delta_drop_count = current_drop_count - prev_drop_count

            -- Update the previous drop count
            redis.call('HSET', debug_drop_monitor_stat_table .. '|' .. debug_counter .. '|' .. port, 'prev_drop_count', current_drop_count)

            -- Fetch the current timestamp
            local time = redis.call('TIME')
            local curr_unix_timestamp = tonumber(time[1])

            -- Check if drop count is greater than drop count threshold
            if delta_drop_count > drop_count_threshold then
                redis.call('RPUSH', debug_drop_monitor_stat_table .. '|' .. debug_counter .. '|' .. port .. '|incidents', curr_unix_timestamp)
            end

            -- Remove outdated incidents
            local incident_count = 0
            local number_of_outdated_incidents = 0
            local number_of_incidents = redis.call('LLEN', debug_drop_monitor_stat_table .. '|' .. debug_counter .. '|' .. port .. '|incidents')
            local incident_timestamps = redis.call('LRANGE', debug_drop_monitor_stat_table .. '|' .. debug_counter .. '|' .. port .. '|incidents', 0, number_of_incidents)
            for incident_index = 1, number_of_incidents do
                local time_delta = curr_unix_timestamp - incident_timestamps[incident_index]
                if (time_delta > window) then
                    number_of_outdated_incidents = number_of_outdated_incidents + 1
                else
                    incident_count = incident_count + 1
                end
            end

            -- Delete incidents that are outside the window
            redis.call('LPOP', debug_drop_monitor_stat_table .. '|' .. debug_counter .. '|' .. port .. '|incidents', number_of_outdated_incidents)

            if incident_count > incident_count_threshold then
                -- Generate alert for persistent drops
                redis.call('HSET', persistent_drop_alert_table, debug_counter .. '|' .. curr_unix_timestamp, 'Persistent packet drops detected on ' .. port)
                -- Delete all incidents since a persistent drop alert was issued
                redis.call('DEL', debug_drop_monitor_stat_table .. '|' .. debug_counter .. '|' .. port .. '|incidents')
            end
        end
    end
end
