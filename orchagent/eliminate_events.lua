-- KEYS - None
-- ARGV - None

local state_db = "6"
local config_db = "4"

local result = {}

redis.call('SELECT', config_db)
local severity_keys = redis.call('KEYS', 'SUPPRESS_ASIC_SDK_HEALTH_EVENT*')
if #severity_keys == 0 then
    return result
end

local max_events = {}
for i = 1, #severity_keys, 1 do
    local max_event = redis.call('HGET', severity_keys[i], 'max_events')
    if max_event then
        max_events[string.sub(severity_keys[i], 32, -1)] = tonumber(max_event)
    end
end

if not next (max_events) then
    return result
end

redis.call('SELECT', state_db)
local events = {}

local event_keys = redis.call('KEYS', 'ASIC_SDK_HEALTH_EVENT_TABLE*')

if #event_keys == 0 then
    return result
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
    local number_received_events = 0
    if events[severity] ~= nil then
        number_received_events = #events[severity]
    end
    if number_received_events > max_events[severity] then
        table.sort(events[severity])
        local number_to_eliminate = number_received_events - max_events[severity]
        for i = 1, number_to_eliminate, 1 do
            redis.call('DEL', events[severity][i])
        end
        table.insert(result, severity .. " events: maximum " .. max_events[severity] .. ", received " .. number_received_events .. ", eliminated " .. number_to_eliminate)
    else
        table.insert(result, severity .. " events: maximum " .. max_events[severity] .. ", received " .. number_received_events .. ", not exceeding the maximum")
    end
end

return result
