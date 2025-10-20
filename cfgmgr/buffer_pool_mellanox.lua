-- KEYS - None
-- ARGV - None

local appl_db = "0"
local config_db = "4"
local state_db = "6"

-- Number of ports with 8 lanes (whose pipeline latency should be doubled)
local port_count_8lanes = 0
-- Number of lossy PG on ports with 8 lanes
local lossypg_8lanes = 0

local ingress_profile_is_lossless = {}

-- Private headrom
local private_headroom = 10 * 1024

local result = {}
local profiles = {}

local total_port = 0

local mgmt_pool_size = 256 * 1024
local egress_mirror_headroom = 10 * 1024
local modification_descriptors_pool_size = 0

-- The set of ports with 8 lanes
local port_set_8lanes = {}
-- Number of ports with lossless profiles
local lossless_port_count = 0

local function iterate_all_items(all_items, check_lossless)
    -- Iterates all items in all_items, check the buffer profile each item referencing, and update reference count accordingly
    -- Arguments:
    --     all_items is a list, holding all keys in BUFFER_PORT_INGRESS_PROFILE_LIST or BUFFER_PORT_EGRESS_PROFILE_LIST table
    --     format of keys: <port name>|<ID map>, like Ethernet0|3-4
    -- Return:
    --     0 successful
    --     1 failure, typically caused by the items just updated are still pended in orchagent's queue
    table.sort(all_items)
    local lossless_ports = {}
    local port
    local fvpairs
    for i = 1, #all_items, 1 do
        -- XXX_TABLE_KEY_SET or XXX_TABLE_DEL_SET existing means the orchagent hasn't handled all updates
        -- In this case, the pool sizes are not calculated for now and will retry later
        if string.sub(all_items[i], -4, -1) == "_SET" then
            return 1
        end
        -- Count the number of priorities or queues in each BUFFER_PG or BUFFER_QUEUE item
        -- For example, there are:
        --     3 queues in 'BUFFER_QUEUE_TABLE:Ethernet0:0-2'
        --     2 priorities in 'BUFFER_PG_TABLE:Ethernet0:3-4'
        port = string.match(all_items[i], "Ethernet%d+")
        if port ~= nil then
            local range = string.match(all_items[i], "Ethernet%d+:([^%s]+)$")
            local profile_name_without_table = redis.call('HGET', all_items[i], 'profile')
            if not profile_name_without_table then
                return 1
            end
            local profile_name = "BUFFER_PROFILE_TABLE:" .. profile_name_without_table
            local profile_ref_count = profiles[profile_name]
            if profile_ref_count == nil then
                -- Indicate an error in case the referenced profile hasn't been inserted or has been removed
                -- It's possible when the orchagent is busy
                -- The buffermgrd will take care of it and retry later
                return 1
            end
            local size
            if string.len(range) == 1 then
                size = 1
            else
                -- Extract start and end numbers from the range (e.g., "8-15")
                local start_num, end_num = string.match(range, "(%d+)-(%d+)")
                size = tonumber(end_num) - tonumber(start_num) + 1
            end
            profiles[profile_name] = profile_ref_count + size
            if port_set_8lanes[port] and ingress_profile_is_lossless[profile_name] == false then
                -- Handle additional buffer reserved for lossy PG on 8-lane ports
                lossypg_8lanes = lossypg_8lanes + size
            end
            if check_lossless and ingress_profile_is_lossless[profile_name] then
                if lossless_ports[port] == nil then
                    lossless_port_count = lossless_port_count + 1
                    lossless_ports[port] = true
                end
            end
        end
    end
    return 0
end

local function iterate_profile_list(all_items)
    -- Iterates all items in all_items, check the buffer profiles each item referencing, and update reference count accordingly
    -- Arguments:
    --     all_items is a list, holding all keys in BUFFER_PORT_INGRESS_PROFILE_LIST or BUFFER_PORT_EGRESS_PROFILE_LIST table
    --     format of keys: <port name>
    -- Return:
    --     0 successful
    --     1 failure, typically caused by the items just updated are still pended in orchagent's queue
    local port
    for i = 1, #all_items, 1 do
        -- XXX_TABLE_KEY_SET or XXX_TABLE_DEL_SET existing means the orchagent hasn't handled all updates
        -- In this case, the pool sizes are not calculated for now and will retry later
        if string.sub(all_items[i], -4, -1) == "_SET" then
            return 1
        end
        port = string.match(all_items[i], "Ethernet%d+")
        local profile_list = redis.call('HGET', all_items[i], 'profile_list')
        if not profile_list then
            return 0
        end
        for profile_name in string.gmatch(profile_list, "([^,]+)") do
            -- The format of profile_list is profile_name,profile_name
            -- We need to handle each of the profile in the list
            -- The ingress_lossy_profile is shared by both BUFFER_PG|<port>|0 and BUFFER_PORT_INGRESS_PROFILE_LIST
            -- It occupies buffers in BUFFER_PG but not in BUFFER_PORT_INGRESS_PROFILE_LIST
            -- To distinguish both cases, a new name "ingress_lossy_profile_list" is introduced to indicate
            -- the profile is used by the profile list where its size should be zero.
            profile_name = 'BUFFER_PROFILE_TABLE:' .. profile_name
            -- TODO CHECK ALL LOSSY PROFILES
            if ingress_profile_is_lossless[profile_name] == false then
                profile_name = profile_name .. '_list'
                if profiles[profile_name] == nil then
                    profiles[profile_name] = 0
                end
            end
            local profile_ref_count = profiles[profile_name]
            if profile_ref_count == nil then
                return 1
            end
            profiles[profile_name] = profile_ref_count + 1
        end
    end

    return 0
end

local function fetch_buffer_pool_size_from_appldb(shp_enabled)
    local buffer_pools = {}
    redis.call('SELECT', config_db)
    local buffer_pool_keys = redis.call('KEYS', 'BUFFER_POOL|*')
    local pool_name
    for i = 1, #buffer_pool_keys, 1 do
         local size = redis.call('HGET', buffer_pool_keys[i], 'size')
         if not size then
             pool_name = string.match(buffer_pool_keys[i], "BUFFER_POOL|([^%s]+)$")
             table.insert(buffer_pools, pool_name)
         end
    end

    redis.call('SELECT', appl_db)
    buffer_pool_keys = redis.call('KEYS', 'BUFFER_POOL_TABLE:*')
    local size
    local xoff
    local output
    for i = 1, #buffer_pools, 1 do
        size = redis.call('HGET', 'BUFFER_POOL_TABLE:' .. buffer_pools[i], 'size')
        if not size then
            size = "0"
        end
        xoff = redis.call('HGET', 'BUFFER_POOL_TABLE:' .. buffer_pools[i], 'xoff')
        if not xoff then
            if shp_enabled and size == "0" and buffer_pools[i] == "ingress_lossless_pool" then
                -- During initialization, if SHP is enabled
                --   1. the buffer pool sizes, xoff have initialized to 0, which means the shared headroom pool is disabled
                --   2. but the buffer profiles already indicate the shared headroom pool is enabled
                --   3. later on the buffer pool sizes are updated with xoff being non-zero
                -- In case the orchagent starts handling buffer configuration between 2 and 3,
                -- It is inconsistent between buffer pools and profiles, which fails Mellanox SAI sanity check
                -- To avoid it, it indicates the shared headroom pool is enabled by setting a very small buffer pool and shared headroom pool sizes
                if size == "0" then
                    table.insert(result, buffer_pools[i] .. ':2048:1024')
                else
                    table.insert(result, buffer_pools[i] .. ":" .. size .. ':1024')
                end
            else
                table.insert(result, buffer_pools[i] .. ':' .. size)
            end
        else
            table.insert(result, buffer_pools[i] .. ':' .. size .. ':' .. xoff)
        end
    end
end

-- Main --
-- Connect to CONFIG_DB
redis.call('SELECT', config_db)

-- Check if platform is SPC6 or later and set modification descriptors pool size
-- Extract first digit of model number from platform string (e.g., "sn6600" -> 6, "sn6800" -> 6)
local platform = redis.call('HGET', 'DEVICE_METADATA|localhost', 'platform')
if platform then
    local model_str = string.match(platform, "sn(%d)")
    if model_str then
        local model_number = tonumber(model_str)
        -- SPC6 or later models (6xxx, 7xxx, etc.), reserve 32MB for modification descriptors pool
        if model_number and model_number >= 6 then
            modification_descriptors_pool_size = 32 * 1024 * 1024
        end
    end
end

-- Parse all the pools and seperate them according to the direction
local ipools = {}
local epools = {}
local pools = redis.call('KEYS', 'BUFFER_POOL|*')
for i = 1, #pools, 1 do
    local type = redis.call('HGET', pools[i], 'type')
    if type == 'ingress' then
        table.insert(ipools, pools[i])
    else
        if type == 'egress' then
            table.insert(epools, pools[i])
        end
    end
end

local ports_table = redis.call('KEYS', 'PORT|*')

total_port = #ports_table

-- Initialize the port_set_8lanes set
local lanes
local number_of_lanes = 0
local admin_status
local admin_up_port = 0
local admin_up_8lanes_port = 0
local port
for i = 1, total_port, 1 do
    -- Load lanes from PORT table
    lanes = redis.call('HGET', ports_table[i], 'lanes')
    if lanes then
        local _
        _, number_of_lanes = string.gsub(lanes, ",", ",")
        number_of_lanes = number_of_lanes + 1
        port = string.sub(ports_table[i], 6, -1)
        if (number_of_lanes == 8) then
            port_set_8lanes[port] = true
            port_count_8lanes = port_count_8lanes + 1
        else
            port_set_8lanes[port] = false
        end
    end
    admin_status = redis.call('HGET', ports_table[i], 'admin_status')
    if admin_status == 'up' then
        admin_up_port = admin_up_port + 1
        if (number_of_lanes == 8) then
            admin_up_8lanes_port = admin_up_8lanes_port + 1
        end
    end
    number_of_lanes = 0
end

local egress_lossless_pool_size = redis.call('HGET', 'BUFFER_POOL|egress_lossless_pool', 'size')

-- Whether shared headroom pool is enabled?
local default_lossless_param_keys = redis.call('KEYS', 'DEFAULT_LOSSLESS_BUFFER_PARAMETER*')
local over_subscribe_ratio
if #default_lossless_param_keys > 0 then
    over_subscribe_ratio = tonumber(redis.call('HGET', default_lossless_param_keys[1], 'over_subscribe_ratio'))
else
    over_subscribe_ratio = 0
end

-- Fetch the shared headroom pool size
local shp_size = tonumber(redis.call('HGET', 'BUFFER_POOL|ingress_lossless_pool', 'xoff'))

local shp_enabled = false
if over_subscribe_ratio ~= nil and over_subscribe_ratio ~= 0 then
    shp_enabled = true
end

if shp_size ~= nil and shp_size ~= 0 then
    shp_enabled = true
else
    shp_size = 0
end

-- Fetch mmu_size
redis.call('SELECT', state_db)
local mmu_size = tonumber(redis.call('HGET', 'BUFFER_MAX_PARAM_TABLE|global', 'mmu_size'))
if mmu_size == nil then
    mmu_size = tonumber(egress_lossless_pool_size)
end
local asic_keys = redis.call('KEYS', 'ASIC_TABLE*')
local cell_size = tonumber(redis.call('HGET', asic_keys[1], 'cell_size'))
local pipeline_latency = tonumber(redis.call('HGET', asic_keys[1], 'pipeline_latency'))

local lossypg_reserved = pipeline_latency * 1024
local lossypg_reserved_8lanes = (2 * pipeline_latency - 1) * 1024

-- Align mmu_size at cell size boundary, otherwise the sdk will complain and the syncd will fail
local number_of_cells = math.floor(mmu_size / cell_size)
local ceiling_mmu_size = number_of_cells * cell_size

-- Switch to APPL_DB
redis.call('SELECT', appl_db)

-- Fetch names of all profiles and insert them into the look up table
local all_profiles = redis.call('KEYS', 'BUFFER_PROFILE*')
for i = 1, #all_profiles, 1 do
    if all_profiles[i] ~= "BUFFER_PROFILE_TABLE_KEY_SET" and all_profiles[i] ~= "BUFFER_PROFILE_TABLE_DEL_SET" then
        local pool = redis.call('HGET', all_profiles[i], 'pool')
        for j = 1, #ipools, 1 do
            if "BUFFER_POOL|" .. pool == ipools[j] then
                -- For ingress profiles, check whether it is lossless or lossy
                -- For lossy profiles, there is buffer implicitly reserved when they are applied on PGs
                local xoff = redis.call('HGET', all_profiles[i], 'xoff')
                if xoff then
                    ingress_profile_is_lossless[all_profiles[i]] = true
                else
                    ingress_profile_is_lossless[all_profiles[i]] = false
                end
                break
            end
        end
        profiles[all_profiles[i]] = 0
    end
end

-- Fetch all the PGs
local all_pgs = redis.call('KEYS', 'BUFFER_PG*')
local all_tcs = redis.call('KEYS', 'BUFFER_QUEUE*')

local fail_count = 0
fail_count = fail_count + iterate_all_items(all_pgs, true)
fail_count = fail_count + iterate_all_items(all_tcs, false)
if fail_count > 0 then
    fetch_buffer_pool_size_from_appldb(shp_enabled)
    return result
end

local all_ingress_profile_lists = redis.call('KEYS', 'BUFFER_PORT_INGRESS_PROFILE_LIST*')
local all_egress_profile_lists = redis.call('KEYS', 'BUFFER_PORT_EGRESS_PROFILE_LIST*')

fail_count = fail_count + iterate_profile_list(all_ingress_profile_lists)
fail_count = fail_count + iterate_profile_list(all_egress_profile_lists)
if fail_count > 0 then
    fetch_buffer_pool_size_from_appldb(shp_enabled)
    return result
end

local statistics = {}

-- Fetch sizes of all of the profiles, accumulate them
local accumulative_occupied_buffer = 0
local accumulative_xoff = 0

for name in pairs(profiles) do
    if name ~= "BUFFER_PROFILE_TABLE_KEY_SET" and name ~= "BUFFER_PROFILE_TABLE_DEL_SET" then
        local size = tonumber(redis.call('HGET', name, 'size'))
        if size ~= nil then
            -- Handle the implicitly reserved buffer for lossy profile applied on PG
            if ingress_profile_is_lossless[name] == false then
               size = size + lossypg_reserved
            end
            if size ~= 0 then
                if shp_size == 0 then
                    local xon = tonumber(redis.call('HGET', name, 'xon'))
                    local xoff = tonumber(redis.call('HGET', name, 'xoff'))
                    if xon ~= nil and xoff ~= nil and xon + xoff > size then
                        accumulative_xoff = accumulative_xoff + (xon + xoff - size) * profiles[name]
                    end
                end
                accumulative_occupied_buffer = accumulative_occupied_buffer + size * profiles[name]
            end
            table.insert(statistics, {name, size, profiles[name]})
        else
            table.insert(statistics, {name, "-", profiles[name]})
        end
    end
end

-- Extra lossy xon buffer for ports with 8 lanes
local lossypg_extra_for_8lanes = (lossypg_reserved_8lanes - lossypg_reserved) * lossypg_8lanes
accumulative_occupied_buffer = accumulative_occupied_buffer + lossypg_extra_for_8lanes

-- Accumulate sizes for private headrooms
local accumulative_private_headroom = 0
local force_enable_shp = false
if accumulative_xoff > 0 and shp_enabled ~= true then
    force_enable_shp = true
    shp_size = 655360
    shp_enabled = true
end
if shp_enabled then
    accumulative_private_headroom = lossless_port_count * private_headroom
    accumulative_occupied_buffer = accumulative_occupied_buffer + accumulative_private_headroom
    accumulative_xoff = accumulative_xoff - accumulative_private_headroom
    if accumulative_xoff < 0 then
        accumulative_xoff = 0
    end
end

-- Accumulate sizes for management PGs
local accumulative_management_pg = (admin_up_port - admin_up_8lanes_port) * lossypg_reserved + admin_up_8lanes_port * lossypg_reserved_8lanes
accumulative_occupied_buffer = accumulative_occupied_buffer + accumulative_management_pg

-- Accumulate sizes for egress mirror and management pool
local accumulative_egress_mirror_overhead = admin_up_port * egress_mirror_headroom
accumulative_occupied_buffer = accumulative_occupied_buffer + accumulative_egress_mirror_overhead + mgmt_pool_size + modification_descriptors_pool_size

-- Switch to CONFIG_DB
redis.call('SELECT', config_db)

-- Fetch all the pools that need update
local pools_need_update = {}
local ingress_pool_count = 0
local ingress_lossless_pool_size = nil
for i = 1, #ipools, 1 do
    local size = tonumber(redis.call('HGET', ipools[i], 'size'))
    if not size then
        table.insert(pools_need_update, ipools[i])
        ingress_pool_count = ingress_pool_count + 1
    else
        if ipools[i] == 'BUFFER_POOL|ingress_lossless_pool' and shp_enabled and shp_size == 0 then
            ingress_lossless_pool_size = size
        end
    end
end

for i = 1, #epools, 1 do
    local size = redis.call('HGET', epools[i], 'size')
    if not size then
        table.insert(pools_need_update, epools[i])
    end
end

if shp_enabled and shp_size == 0 then
    shp_size = math.ceil(accumulative_xoff / over_subscribe_ratio)
    if shp_size == 0 then
        shp_size = 655360
    end
end

local pool_size
if shp_size then
    accumulative_occupied_buffer = accumulative_occupied_buffer + shp_size
end

local available_buffer = mmu_size - accumulative_occupied_buffer
if ingress_pool_count == 1 then
    pool_size = available_buffer
else
    pool_size = available_buffer / 2
end

if pool_size > ceiling_mmu_size then
    pool_size = ceiling_mmu_size
end

local shp_deployed = false
for i = 1, #pools_need_update, 1 do
    local percentage = tonumber(redis.call('HGET', pools_need_update[i], 'percentage'))
    local effective_pool_size
    if percentage ~= nil and percentage >= 0 then
        effective_pool_size = available_buffer * percentage / 100
    else
        effective_pool_size = pool_size
    end
    local pool_name = string.match(pools_need_update[i], "BUFFER_POOL|([^%s]+)$")
    if shp_size ~= 0 and pool_name == "ingress_lossless_pool" then
        table.insert(result, pool_name .. ":" .. math.ceil(effective_pool_size) .. ":" .. math.ceil(shp_size))
        shp_deployed = true
    else
        table.insert(result, pool_name .. ":" .. math.ceil(effective_pool_size))
    end
end

if not shp_deployed and shp_size ~= 0 and ingress_lossless_pool_size ~= nil then
    table.insert(result, "ingress_lossless_pool:" .. math.ceil(ingress_lossless_pool_size) .. ":" .. math.ceil(shp_size))
end

table.insert(result, "debug:mmu_size:" .. mmu_size)
table.insert(result, "debug:accumulative size:" .. accumulative_occupied_buffer)
for i = 1, #statistics do
    table.insert(result, "debug:" .. statistics[i][1] .. ":" .. statistics[i][2] .. ":" .. statistics[i][3])
end
table.insert(result, "debug:extra_8lanes:" .. (lossypg_reserved_8lanes - lossypg_reserved) .. ":" .. lossypg_8lanes .. ":" .. port_count_8lanes)
table.insert(result, "debug:mgmt_pool:" .. mgmt_pool_size)
if shp_enabled then
    table.insert(result, "debug:accumulative_private_headroom:" .. accumulative_private_headroom)
    table.insert(result, "debug:accumulative xoff:" .. accumulative_xoff)
    table.insert(result, "debug:force enabled shp:" .. tostring(force_enable_shp))
end
table.insert(result, "debug:accumulative_mgmt_pg:" .. accumulative_management_pg)
table.insert(result, "debug:egress_mirror:" .. accumulative_egress_mirror_overhead)
table.insert(result, "debug:shp_enabled:" .. tostring(shp_enabled))
table.insert(result, "debug:shp_size:" .. shp_size)
table.insert(result, "debug:total port:" .. total_port .. " ports with 8 lanes:" .. port_count_8lanes)
table.insert(result, "debug:admin up port:" .. admin_up_port .. " admin up ports with 8 lanes:" .. admin_up_8lanes_port)
table.insert(result, "debug:modification_descriptors_pool_size:" .. modification_descriptors_pool_size)

return result
