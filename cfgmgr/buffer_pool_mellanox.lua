-- KEYS - None
-- ARGV - None

local appl_db = "0"
local config_db = "4"
local state_db = "6"

local lossypg_reserved = 19 * 1024
local lossypg_reserved_400g = 37 * 1024
local lossypg_400g = 0

local result = {}
local profiles = {}

local count_up_port = 0

local mgmt_pool_size = 256 * 1024
local egress_mirror_headroom = 10 * 1024

local function find_profile(ref)
    -- Remove the surrounding square bracket and the find in the list
    local name = string.sub(ref, 2, -2)
    for i = 1, #profiles, 1 do
        if profiles[i][1] == name then
            return i
        end
    end
    return 0
end

local function iterate_all_items(all_items)
    table.sort(all_items)
    local prev_port = "None"
    local port
    local is_up
    local fvpairs
    local status
    local admin_down_ports = 0
    for i = 1, #all_items, 1 do
        -- Check whether the port on which pg or tc hosts is admin down
        port = string.match(all_items[i], "Ethernet%d+")
        if port ~= nil then
            if prev_port ~= port then
                status = redis.call('HGET', 'PORT_TABLE:'..port, 'admin_status')
                prev_port = port
                if status == "down" then
                    is_up = false
                else
                    is_up = true
                end
            end
            if is_up == true then
                local range = string.match(all_items[i], "Ethernet%d+:([^%s]+)$")
                local profile = redis.call('HGET', all_items[i], 'profile')
                local index = find_profile(profile)
                local size
                if string.len(range) == 1 then
                    size = 1
                else
                    size = 1 + tonumber(string.sub(range, -1)) - tonumber(string.sub(range, 1, 1))
                end
                profiles[index][2] = profiles[index][2] + size
                local speed = redis.call('HGET', 'PORT_TABLE:'..port, 'speed')
                if speed == '400000' and profile == '[BUFFER_PROFILE_TABLE:ingress_lossy_profile]' then
                    lossypg_400g = lossypg_400g + size
                end
            end
        end
    end
end

-- Connect to CONFIG_DB
redis.call('SELECT', config_db)

local ports_table = redis.call('KEYS', 'PORT|*')

for i = 1, #ports_table do
    local status = redis.call('HGET', ports_table[i], 'admin_status')
    if status == "up" then
        count_up_port = count_up_port + 1
    end
end

local egress_lossless_pool_size = redis.call('HGET', 'BUFFER_POOL|egress_lossless_pool', 'size')

-- Switch to APPL_DB
redis.call('SELECT', appl_db)

-- Fetch names of all profiles and insert them into the look up table
local all_profiles = redis.call('KEYS', 'BUFFER_PROFILE*')
for i = 1, #all_profiles, 1 do
    table.insert(profiles, {all_profiles[i], 0})
end

-- Fetch all the PGs
local all_pgs = redis.call('KEYS', 'BUFFER_PG*')
local all_tcs = redis.call('KEYS', 'BUFFER_QUEUE*')

iterate_all_items(all_pgs)
iterate_all_items(all_tcs)

local statistics = {}

-- Fetch sizes of all of the profiles, accumulate them
local accumulative_occupied_buffer = 0
for i = 1, #profiles, 1 do
    if profiles[i][1] ~= "BUFFER_PROFILE_TABLE_KEY_SET" and profiles[i][1] ~= "BUFFER_PROFILE_TABLE_DEL_SET" then
        local size = tonumber(redis.call('HGET', profiles[i][1], 'size'))
        if size ~= nil then 
            if profiles[i][1] == "BUFFER_PROFILE_TABLE:ingress_lossy_profile" then
                size = size + lossypg_reserved
            end
            if profiles[i][1] == "BUFFER_PROFILE_TABLE:egress_lossy_profile" then
                profiles[i][2] = count_up_port
            end
            if size ~= 0 then
                accumulative_occupied_buffer = accumulative_occupied_buffer + size * profiles[i][2]
            end
            table.insert(statistics, {profiles[i][1], size, profiles[i][2]})
        end
    end
end

-- Extra lossy xon buffer for 400G port
local lossypg_extra_for_400g = (lossypg_reserved_400g - lossypg_reserved) * lossypg_400g
accumulative_occupied_buffer = accumulative_occupied_buffer + lossypg_extra_for_400g

-- Accumulate sizes for egress mirror and management pool
local accumulative_egress_mirror_overhead = count_up_port * egress_mirror_headroom
accumulative_occupied_buffer = accumulative_occupied_buffer + accumulative_egress_mirror_overhead + mgmt_pool_size

-- Fetch mmu_size
redis.call('SELECT', state_db)
local mmu_size = tonumber(redis.call('HGET', 'BUFFER_MAX_PARAM_TABLE|global', 'mmu_size'))
if mmu_size == nil then
    mmu_size = tonumber(egress_lossless_pool_size)
end
local asic_keys = redis.call('KEYS', 'ASIC_TABLE*')
local cell_size = tonumber(redis.call('HGET', asic_keys[1], 'cell_size'))

-- Align mmu_size at cell size boundary, otherwith the sdk will complain and the syncd will faill
local number_of_cells = math.floor(mmu_size / cell_size)
local ceiling_mmu_size = number_of_cells * cell_size

-- Switch to CONFIG_DB
redis.call('SELECT', config_db)

-- Fetch all the pools that need update
local pools_need_update = {}
local ipools = redis.call('KEYS', 'BUFFER_POOL|ingress*')
local ingress_pool_count = 0
for i = 1, #ipools, 1 do
    local size = tonumber(redis.call('HGET', ipools[i], 'size'))
    if not size then
        table.insert(pools_need_update, ipools[i])
        ingress_pool_count = ingress_pool_count + 1
    end
end

local epools = redis.call('KEYS', 'BUFFER_POOL|egress*')
for i = 1, #epools, 1 do
    local size = redis.call('HGET', epools[i], 'size')
    if not size then
        table.insert(pools_need_update, epools[i])
    end
end

local pool_size
if ingress_pool_count == 1 then
    pool_size = mmu_size - accumulative_occupied_buffer
else
    pool_size = (mmu_size - accumulative_occupied_buffer) / 2
end

if pool_size > ceiling_mmu_size then
    pool_size = ceiling_mmu_size
end

for i = 1, #pools_need_update, 1 do
    local pool_name = string.match(pools_need_update[i], "BUFFER_POOL|([^%s]+)$")
    table.insert(result, pool_name .. ":" .. math.ceil(pool_size))
end

table.insert(result, "debug:mmu_size:" .. mmu_size)
table.insert(result, "debug:accumulative:" .. accumulative_occupied_buffer)
for i = 1, #statistics do
    table.insert(result, "debug:" .. statistics[i][1] .. ":" .. statistics[i][2] .. ":" .. statistics[i][3])
end
table.insert(result, "debug:extra_400g:" .. (lossypg_reserved_400g - lossypg_reserved) .. ":" .. lossypg_400g)
table.insert(result, "debug:mgmt_pool:" .. mgmt_pool_size)
table.insert(result, "debug:egress_mirror:" .. accumulative_egress_mirror_overhead)

return result
