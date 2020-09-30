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
local sai_hdrm_pool_watermark_stat_name = 'SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES'

local rets = redis.call('SELECT', counters_db)

-- Iterate through each buffer pool oid
local n = table.getn(KEYS)
for i = n, 1, -1 do
    -- Get new watermark value from COUNTERS
    local buffer_pool_wm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)
    local hdrm_pool_wm = redis.call('HGET', counters_table_name .. ':' .. KEYS[i], sai_hdrm_pool_watermark_stat_name)

    -- Get last value from *_WATERMARKS
    local user_buffer_pool_wm = redis.call('HGET', user_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)
    local persistent_buffer_pool_wm = redis.call('HGET', persistent_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)
    local periodic_buffer_pool_wm = redis.call('HGET', periodic_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name)

    local user_hdrm_pool_wm = redis.call('HGET', user_table_name .. ':' .. KEYS[i], sai_hdrm_pool_watermark_stat_name)
    local persistent_hdrm_pool_wm = redis.call('HGET', persistent_table_name .. ':' .. KEYS[i], sai_hdrm_pool_watermark_stat_name)
    local periodic_hdrm_pool_wm = redis.call('HGET', periodic_table_name .. ':' .. KEYS[i], sai_hdrm_pool_watermark_stat_name)

    if buffer_pool_wm then
        buffer_pool_wm = tonumber(buffer_pool_wm)

        redis.call('HSET', user_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name,
                   user_buffer_pool_wm and math.max(buffer_pool_wm, user_buffer_pool_wm) or buffer_pool_wm)
        redis.call('HSET', persistent_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name,
                   persistent_buffer_pool_wm and math.max(buffer_pool_wm, persistent_buffer_pool_wm) or buffer_pool_wm)
        redis.call('HSET', periodic_table_name .. ':' .. KEYS[i], sai_buffer_pool_watermark_stat_name,
                   periodic_buffer_pool_wm and math.max(buffer_pool_wm, periodic_buffer_pool_wm) or buffer_pool_wm)
    end

    if hdrm_pool_wm then
        hdrm_pool_wm = tonumber(hdrm_pool_wm)

        redis.call('HSET', user_table_name .. ':' .. KEYS[i], sai_hdrm_pool_watermark_stat_name,
                   user_hdrm_pool_wm and math.max(hdrm_pool_wm, user_hdrm_pool_wm) or hdrm_pool_wm)
        redis.call('HSET', persistent_table_name .. ':' .. KEYS[i], sai_hdrm_pool_watermark_stat_name,
                   persistent_hdrm_pool_wm and math.max(hdrm_pool_wm, persistent_hdrm_pool_wm) or hdrm_pool_wm)
        redis.call('HSET', periodic_table_name .. ':' .. KEYS[i], sai_hdrm_pool_watermark_stat_name,
                   periodic_hdrm_pool_wm and math.max(hdrm_pool_wm, periodic_hdrm_pool_wm) or hdrm_pool_wm)
    end
end

return rets
