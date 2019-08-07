-- KEYS - buffer IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval
-- return nothing for now

local counters_db = ARGV[1]
local counters_table_name = 'COUNTERS'

local user_table_name = 'USER_WATERMARKS'
local persistent_table_name = 'PERSISTENT_WATERMARKS'
local periodic_table_name = 'PERIODIC_WATERMARKS'

local sai_buffer_pool_watermark_stat_name = 'SAI_BUFFER_POOL_STAT_WATERMARK_BYTES'

local rets = {}

redis.call('SELECT', counters_db)

-- Iterate through each buffer pool oid
local n = table.getn(KEYS)
for i = n, 1, -1 do
    -- Get new watermark value from COUNTERS
    local wm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)
    if wm then
        wm = tonumber(wm)

        -- Get last value from *_WATERMARKS
        local user_wm_last = redis.call('HGET', user_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)

        -- Set higher value to *_WATERMARKS
        if user_wm_last then
            user_wm_last = tonumber(user_wm_last)
            if wm > user_wm_last then
                redis.call('HSET', user_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name, wm)
            end
        else
            redis.call('HSET', user_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name, wm)
        end

        local persistent_wm_last = redis.call('HGET', persistent_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)
        if persistent_wm_last then
            persistent_wm_last = tonumber(persistent_wm_last)
            if wm > persistent_wm_last then
                redis.call('HSET', persistent_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name, wm)
            end
        else
            redis.call('HSET', persistent_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name, wm)
        end

        local periodic_wm_last = redis.call('HGET', periodic_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)
        if periodic_wm_last then
            periodic_wm_last = tonumber(periodic_wm_last)
            if wm > periodic_wm_last then
                redis.call('HSET', periodic_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name, wm)
            end
        else
            redis.call('HSET', periodic_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name, wm)
        end
    end
end

return rets
