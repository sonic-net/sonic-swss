-- KEYS - port name
-- ARGV[1] - profile name
-- ARGV[2] - new size
-- ARGV[3] - pg to add

local port = KEYS[1]
local input_profile_name = ARGV[1]
local input_profile_size = ARGV[2]
local new_pg = ARGV[3]
local accumulative_size = 0

local appl_db = "0"
local config_db = "4"
local state_db = "6"

local ret_true = {}
local ret_false = {}
local ret = {}
local default_ret = {}

table.insert(ret_true, "result:true")
table.insert(ret_false, "result:false")

-- Fetch the cable length from CONFIG_DB
redis.call('SELECT', config_db)
local cable_length_keys = redis.call('KEYS', 'CABLE_LENGTH*')
if #cable_length_keys == 0 then
    return ret_true
end

-- Check whether cable length exceeds 300m (maximum value in the non-dynamic-buffer solution)
local cable_length_str = redis.call('HGET', cable_length_keys[1], port)
if cable_length_str == nil then
    return ret_true
end
local cable_length = tonumber(string.sub(cable_length_str, 1, -2))
if cable_length > 300 then
    default_ret = ret_false
else
    default_ret = ret_true
end
table.insert(default_ret, 'debug:no max_headroom_size configured, check cable length instead')

local speed = redis.call('HGET', 'PORT|' .. port, 'speed')

-- Fetch the threshold from STATE_DB
redis.call('SELECT', state_db)

local max_headroom_size = tonumber(redis.call('HGET', 'BUFFER_MAX_PARAM_TABLE|' .. port, 'max_headroom_size'))
if max_headroom_size == nil then
    return default_ret
end

local asic_keys = redis.call('KEYS', 'ASIC_TABLE*')
local pipeline_delay = tonumber(redis.call('HGET', asic_keys[1], 'pipeline_latency'))
if speed == 400000 then
    pipeline_delay = pipeline_delay * 2 - 1
end
accumulative_size = accumulative_size + 2 * pipeline_delay * 1024

-- Fetch all keys in BUFFER_PG according to the port
redis.call('SELECT', appl_db)

local debuginfo = {}

local function get_number_of_pgs(keyname)
    local range = string.match(keyname, "Ethernet%d+:([^%s]+)$")
    local size
    if range == nil then
        table.insert(debuginfo, "debug:invalid pg:" .. keyname)
        return 0
    end
    if string.len(range) == 1 then
        size = 1
    else
        size = 1 + tonumber(string.sub(range, -1)) - tonumber(string.sub(range, 1, 1))
    end
    return size
end

local no_input_pg = true
if new_pg ~= nil then
    if get_number_of_pgs(new_pg) ~= 0 then
        no_input_pg = false
        new_pg = 'BUFFER_PG_TABLE:' .. new_pg
    end
end

-- Fetch all the PGs, accumulate the sizes
-- Assume there is only one lossless profile configured among all PGs on each port
table.insert(debuginfo, 'debug:other overhead:' .. accumulative_size)
local pg_keys = redis.call('KEYS', 'BUFFER_PG_TABLE:' .. port .. ':*')
for i = 1, #pg_keys do
    local profile = string.sub(redis.call('HGET', pg_keys[i], 'profile'), 2, -2)
    local current_profile_size
    if profile ~= 'BUFFER_PROFILE_TABLE:ingress_lossy_profile' and (no_input_pg or new_pg ~= pg_keys[i]) then
        if profile ~= input_profile_name and not no_input_pg then
            local referenced_profile = redis.call('HGETALL', profile)
            for j = 1, #referenced_profile, 2 do
                if referenced_profile[j] == 'size' then
                    current_profile_size = tonumber(referenced_profile[j+1])
                end
            end
        else
            current_profile_size = input_profile_size
            profile = input_profile_name
        end
        accumulative_size = accumulative_size + current_profile_size * get_number_of_pgs(pg_keys[i])
        table.insert(debuginfo, 'debug:' .. pg_keys[i] .. ':' .. profile .. ':' .. current_profile_size .. ':' .. get_number_of_pgs(pg_keys[i]) .. ':accu:' .. accumulative_size)
    end
end

if not no_input_pg then
    accumulative_size = accumulative_size + input_profile_size * get_number_of_pgs(new_pg)
    table.insert(debuginfo, 'debug:' .. new_pg .. '*:' .. input_profile_name .. ':' .. input_profile_size .. ':' .. get_number_of_pgs(new_pg) .. ':accu:' .. accumulative_size)
end

if max_headroom_size > accumulative_size then
    table.insert(ret, "result:true")
else
    table.insert(ret, "result:false")
end

table.insert(ret, "debug:max headroom:" .. max_headroom_size)
table.insert(ret, "debug:accumulative headroom:" .. accumulative_size)

for i = 1, #debuginfo do
    table.insert(ret, debuginfo[i])
end

return ret
