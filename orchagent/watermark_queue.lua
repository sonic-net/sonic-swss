-- KEYS - queue IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval
-- return nothing for now 

local counters_db = ARGV[1]
local counters_table_name = "COUNTERS" 

local periodic_table_name = "PERIODIC_WATERMARKS"
local persistent_table_name = "PERSISTENT_WATERMARKS"
local user_table_name = "USER_WATERMARKS"

local rets = {}

redis.call('SELECT', counters_db)

-- Iterate through each queue
local n = table.getn(KEYS)
for i = n, 1, -1 do
    -- Gen new WM value
    local queue_shared_wm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES')

    -- Get the last values in the other tables (assume 0 if does not exist)
    local periodic_shared_wm = redis.call('HGET', periodic_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES') 
    local persistent_shared_wm = redis.call('HGET', persistent_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES')
    local user_shared_wm = redis.call('HGET', user_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES') 

    -- Set the values into the other tables. Make comparioson, if th evalue was absent in COUNTERS, set to N/A
    if tonumber(queue_shared_wm) then
        redis.call('HSET', periodic_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES', periodic_shared_wm and math.max(queue_shared_wm, periodic_shared_wm) or queue_shared_wm)
        redis.call('HSET', persistent_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES', persistent_shared_wm and math.max(queue_shared_wm, persistent_shared_wm) or queue_shared_wm)
        redis.call('HSET', user_table_name .. ':' .. KEYS[i], 'SAI_QUEUE_STAT_SHARED_WATERMARK_BYTES', user_shared_wm and math.max(queue_shared_wm, user_shared_wm) or queue_shared_wm)
    end
end

return rets
