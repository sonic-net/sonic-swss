-- KEYS - None
-- ARGV - None

local state_db = "6"
local config_db = "4"

redis.call('SELECT', config_db)
local severity_keys = redis.call('KEYS', 'SUPPRESS_ASIC_SDK_HEALTH_EVENT*')
if #severity_keys == 0 then
    return
end

local max_events = {}
for i = 1, #severity_keys, 1 do
    local max_event = redis.call('HGET', severity_keys[i], 'max_events')
    if max_event then
        max_events[string.sub(severity_keys[i], 32, -1)] = tonumber(max_event)
    end
end

if not next (max_events) then
    return
end

redis.call('SELECT', state_db)
local events = {}

local event_keys = redis.call('KEYS', 'ASIC_SDK_HEALTH_EVENT_TABLE*')

if #event_keys == 0 then
    return
end

for i = 1, #event_keys, 1 do
    local severity = redis.call('HGET', event_keys[i], 'severity')
    if max_events[severity] ~= nil then
        if events[severity] == nil then
            events[severity] = {}
        end
        table.insert(events[severity], event_keys[i])
    end
end

for severity in pairs(max_events) do
    local number_event = #events[severity]
    if number_event > max_events[severity] then
        table.sort(events[severity])
        number_event = number_event - max_events[severity]
        for i = 1, number_event, 1 do
            redis.call('DEL', events[severity][i])
        end
    end
end

return events
