-- KEYS - port name

local port = KEYS[1]
local state_db = "6"
local ret = {}

-- Connect to STATE_DB
redis.call('SELECT', state_db)
local max_headroom_size = tonumber(redis.call('HGET', 'BUFFER_MAX_PARAM_TABLE|' .. port, 'max_headroom_size'))
if max_headroom_size == 0 or max_headroom_size == nil then
    table.insert(ret, "result:true")
    table.insert(ret, "debug:No need to check port headroom limit as shared headroom pool model is supported.")
else
    table.insert(ret, "result:false")
    table.insert(ret, "debug:Independent headroom model is not supported on Barefoot's platforms.")
end
return ret
