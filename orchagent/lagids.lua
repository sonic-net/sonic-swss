-- KEYS - None
-- ARGV[1] - operation (add/del/get)
-- ARGV[2] - lag name
-- ARGV[3] - current lag id (for "add" operation only)

-- return lagid if success for "add"/"del"
-- return 0 if lag does not exist for "del"
-- return -1 if lag table full for "add"
-- return -2 if lag does not exist for "get"
-- return -3 if invalid operation

local op = ARGV[1]
local pcname = ARGV[2]

local lagid_start = tonumber(redis.call("get", "SYSTEM_LAG_ID_START"))
local lagid_end = tonumber(redis.call("get", "SYSTEM_LAG_ID_END"))

if op == "add" then

    local plagid = tonumber(ARGV[3])

    local dblagid = redis.call("hget", "SYSTEM_LAG_ID_TABLE", pcname)

    if dblagid then
        dblagid = tonumber(dblagid)
        if plagid == 0 then
            -- no lagid proposed. Return the existing lagid
            return dblagid
        end
    end

    -- lagid allocation request with a lagid proposal
    if plagid >= lagid_start and plagid <= lagid_end then
        if plagid == dblagid then
            -- proposed lagid is same as the lagid in database
            return plagid
        end
        -- proposed lag id is different than that in database OR
        -- the portchannel does not exist in the database
        -- If proposed lagid is not available, lpop the first availabe ID
        local index = redis.call("lpos", "SYSTEM_LAG_IDS_FREE_LIST", tostring(plagid))
        if index ~= false then
            if redis.call("sismember", "SYSTEM_LAG_ID_SET", tostring(plagid)) == 0 then
                redis.call("lrem", "SYSTEM_LAG_IDS_FREE_LIST", 0, tostring(plagid))
                redis.call("hset", "SYSTEM_LAG_ID_TABLE", pcname, tostring(plagid))
                redis.call("sadd", "SYSTEM_LAG_ID_SET", tostring(plagid))
                if dblagid then
                    redis.call("srem", "SYSTEM_LAG_ID_SET", tostring(dblagid))
                    if redis.call("lpos", "SYSTEM_LAG_IDS_FREE_LIST", tostring(dblagid)) == false then
                        redis.call("rpush", "SYSTEM_LAG_IDS_FREE_LIST", tostring(dblagid))
                    end
                end
                return plagid
            else
                redis.call("lrem", "SYSTEM_LAG_IDS_FREE_LIST", 0, tostring(plagid))
            end
        end
    end

    if redis.call("llen", "SYSTEM_LAG_IDS_FREE_LIST") <= 0 then
        return -1
    end
    local lagid = redis.call("lpop", "SYSTEM_LAG_IDS_FREE_LIST")

    -- check if the first one is in the SYSTEM_LAG_ID_SET (which could be set by LC which
    -- is running previous image with the old allocation method
    -- remove from free_list and check/get the next one from the free_list
    while redis.call("sismember", "SYSTEM_LAG_ID_SET", tostring(lagid)) == 1 do
        if redis.call("llen", "SYSTEM_LAG_IDS_FREE_LIST") <= 0 then
            return -1
        end
        lagid = redis.call("lpop", "SYSTEM_LAG_IDS_FREE_LIST")
    end

    -- Remove it from free list in case there is duplicated one 
    redis.call("lrem", "SYSTEM_LAG_IDS_FREE_LIST", 0, lagid)

    redis.call("hset", "SYSTEM_LAG_ID_TABLE", pcname, lagid)
    redis.call("sadd", "SYSTEM_LAG_ID_SET", lagid)
    if dblagid then
        if redis.call("lpos", "SYSTEM_LAG_IDS_FREE_LIST", tostring(dblagid)) == false then
            redis.call("rpush", "SYSTEM_LAG_IDS_FREE_LIST", tostring(dblagid))
        end
    end
    return tonumber(lagid)
end

if op == "del" then

    if redis.call("hexists", "SYSTEM_LAG_ID_TABLE", pcname) == 1 then
        local lagid = redis.call("hget", "SYSTEM_LAG_ID_TABLE", pcname)
        redis.call("srem", "SYSTEM_LAG_ID_SET", lagid)
        redis.call("hdel", "SYSTEM_LAG_ID_TABLE", pcname)
        if redis.call("lpos", "SYSTEM_LAG_IDS_FREE_LIST", lagid) == false then
            redis.call("rpush", "SYSTEM_LAG_IDS_FREE_LIST", lagid)
        end
        return tonumber(lagid)
    end

    return 0

end

if op == "get" then

    if redis.call("hexists", "SYSTEM_LAG_ID_TABLE", pcname) == 1 then
        local lagid = redis.call("hget", "SYSTEM_LAG_ID_TABLE", pcname)
        return tonumber(lagid)
    end

    return -2

end

return -3
