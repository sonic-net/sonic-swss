-- KEYS - port IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval
-- return log

local logtable = {}

local function logit(msg)
  logtable[#logtable+1] = tostring(msg)
end

local counters_db = ARGV[1]
local counters_table_name = ARGV[2]
local rates_table_name = "RATES"
local appl_db_port = "PORT_TABLE"
--  refer back to common/schema.h
local appl_db = "0"
local gb_counters_db = "10"

-- Get configuration
redis.call('SELECT', counters_db)
local smooth_interval = redis.call('HGET', rates_table_name .. ':' .. 'PORT', 'PORT_SMOOTH_INTERVAL')
local alpha = redis.call('HGET', rates_table_name .. ':' .. 'PORT', 'PORT_ALPHA')
local one_minus_alpha = nil
local delta = tonumber(ARGV[3])

-- Alpha is only needed for rate computation, not for BER
if alpha then
    one_minus_alpha = 1.0 - alpha
    logit(alpha)
    logit(one_minus_alpha)
end

logit(delta)

local port_interface_oid_map = redis.call('HGETALL', "COUNTERS_PORT_NAME_MAP")
local port_interface_oid_key_count = redis.call('HLEN', "COUNTERS_PORT_NAME_MAP")

local port_interface_gearbox_map = {}

-- Get gearbox information if we are in gearbox counters db
if counters_db == gb_counters_db then
    -- Select the application DB to get the gearbox table
    redis.call('SELECT', appl_db)

    local port_interface_gearbox_keys = redis.call('KEYS', "_GEARBOX_TABLE:interface:*")

    -- Create a map of interface to system and line lanes
    for i, key in ipairs(port_interface_gearbox_keys) do
        local interface_name = redis.call('HGET', key, 'name')
        local line_lanes = redis.call('HGET', key, 'line_lanes')
        local system_lanes = redis.call('HGET', key, 'system_lanes')
        port_interface_gearbox_map[interface_name] = {line=line_lanes, system=system_lanes}
    end

    -- Select the counters DB
    redis.call('SELECT', counters_db)
end

-- lookup interface name from port oid

local function find_interface_name_from_oid(port)

    for i = 1, port_interface_oid_key_count do
        local index = i * 2 - 1
        if port_interface_oid_map[index + 1] == port then
            return port_interface_oid_map[index]
        end
    end

    return 0
end

-- calculate lanes and serdes speed from interface lane count & speed
-- return lane speed and serdes speed

local function calculate_lane_and_serdes_speed(count, speed)

   local serdes = 0
   local lane_speed = 0

    if count == 0 or speed == 0 then
        logit("Invalid number of lanes or speed")
        return 0, 0
    end

    -- check serdes_cnt if it is a multiple of speed
    local serdes_cnt = math.fmod(speed, count)

    if serdes_cnt ~= 0 then
        logit("Invalid speed and number of lanes combination")
        return 0, 0
    end

    lane_speed = math.floor(speed / count)

    -- return value in bits
    if lane_speed == 1000 then
        serdes = 1.25e+9
    elseif lane_speed == 10000 then
        serdes = 10.3125e+9
    elseif lane_speed == 25000 then
        serdes = 25.78125e+9
    elseif lane_speed == 50000 then
        serdes = 53.125e+9
    elseif lane_speed == 100000 then
        serdes = 106.25e+9
    elseif lane_speed == 200000 then
        serdes = 212.5e+9
    else
       logit("Invalid serdes speed")
    end

    return lane_speed, serdes
end

-- look up interface lanes count, lanes speed & serdes speed
-- return lane count, lane speed, serdes speed

local function find_lanes_and_serdes(interface_name)
    -- get the port config from config db
    local _
    local serdes, lane_speed, count = 0, 0, 0

    -- Get the port configure
    redis.call('SELECT', appl_db)
    local lanes = redis.call('HGET', appl_db_port ..':'..interface_name, 'lanes')

    if lanes then
        local speed = redis.call('HGET', appl_db_port ..':'..interface_name, 'speed')

        -- we were spliting it on ','
        _, count = string.gsub(lanes, ",", ",")
        count = count + 1

        lane_speed, serdes = calculate_lane_and_serdes_speed(count, speed)

    end
    -- switch back to counter db
    redis.call('SELECT', counters_db)

    return count, lane_speed, serdes
end

-- Get the base interface name and lane type from gearbox interface name

local function get_gearbox_interface_info(interface_name)
    local base_name, suffix
    
    -- Check if it ends with _line
    if string.match(interface_name, "_line$") then
        base_name = string.gsub(interface_name, "_line$", "")
        suffix = "line"
    -- Check if it ends with _system
    elseif string.match(interface_name, "_system$") then
        base_name = string.gsub(interface_name, "_system$", "")
        suffix = "system"
    else
        -- Not a gearbox interface
        return interface_name, nil
    end

    return base_name, suffix
end

-- Interface name for gearbox counters table always end with _line or _system
-- Find the base interface name and lane type from the gearbox interface name

local function find_gearbox_lanes_and_serdes(interface_name)

    local _
    local count, lane_speed, serdes = 0, 0, 0
    local base_interface_name, lane_type = get_gearbox_interface_info(interface_name)
    
    redis.call('SELECT', appl_db)
    if base_interface_name and lane_type then
        local lanes = port_interface_gearbox_map[base_interface_name][lane_type]
        local speed = redis.call('HGET', appl_db_port ..':'..base_interface_name, 'speed')
        
        if lanes then
            _, count = string.gsub(lanes, ",", ",")
            count = count + 1
            lane_speed, serdes = calculate_lane_and_serdes_speed(count, speed)
        end

    end
    redis.call('SELECT', counters_db)

    return count, lane_speed, serdes
end


-- find the max T - Maximum FEC histogram bin with non-zero count
-- return max T value

local function find_maxT(port)
    local maxT = -1
    for i = 0, 15 do
        local fec_cwi = 'SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S' .. i
        local fec_cwi_val = redis.call('HGET', counters_table_name .. ':' .. port, fec_cwi)
        if fec_cwi_val then
            fec_cwi_val = tonumber(fec_cwi_val) or 0
            if fec_cwi_val > 0 then
                maxT = i
            end
        end
    end
    return maxT
end


local function compute_ber(port)
    -- FEC BER
    local fec_corr_bits, fec_uncorr_frames
    local maxT = -1
    local fec_corr_bits_ber_new, fec_uncorr_bits_ber_new = -1, -1
    -- HLD review suggest to use the statistical average when calculate the post fec ber
    local rs_average_frame_ber = 1e-8
    local lanes_speed, serdes_speed, lanes_count = 0, 0, 0

    -- lookup interface name from oid
    local interface_name = find_interface_name_from_oid(port)
    if interface_name then
        -- lookup lanes count, lanes speed and serdes speed based on counters or gb counters db
        if counters_db == gb_counters_db then
            lanes_count, lanes_speed, serdes_speed = find_gearbox_lanes_and_serdes(interface_name)
        else
            lanes_count, lanes_speed, serdes_speed = find_lanes_and_serdes(interface_name)
        end
 
        if lanes_count and serdes_speed then
            fec_corr_bits = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS')
            fec_uncorr_frames = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES')
        end
    end
        
    if fec_corr_bits and fec_uncorr_frames and lanes_count and serdes_speed then
        local fec_corr_bits_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_FEC_CORRECTED_BITS_last')
        local fec_uncorr_frames_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_FEC_NOT_CORRECTABLE_FARMES_last')

        -- Initialize to 0 if last counter values does not exist (during first boot for eg)
        fec_corr_bits_last = tonumber(fec_corr_bits_last) or 0
        fec_uncorr_frames_last = tonumber(fec_uncorr_frames_last) or 0

        local serdes_rate_total = lanes_count * serdes_speed * delta / 1000

        fec_corr_bits_ber_new = (fec_corr_bits - fec_corr_bits_last) / serdes_rate_total
        fec_uncorr_bits_ber_new = (fec_uncorr_frames - fec_uncorr_frames_last) * rs_average_frame_ber  / serdes_rate_total
    else
        logit("FEC counters or lane info not found on " .. port)
    end

    -- do not update FEC related stat if we dont have it
    
    if not fec_corr_bits or not fec_uncorr_frames or not fec_corr_bits_ber_new or
       not fec_uncorr_bits_ber_new then
        logit("FEC counters not found on " .. port)
        return
    end

    maxT = find_maxT(port)

    -- Set BER values
    local fec_pre_ber_max = redis.call('HGET', rates_table_name .. ':' .. port, 'FEC_PRE_BER_MAX')
    fec_pre_ber_max =  tonumber(fec_pre_ber_max) or 0

    if fec_corr_bits_ber_new > fec_pre_ber_max then
        redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_PRE_BER_MAX', fec_corr_bits_ber_new)
    end
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_FEC_CORRECTED_BITS_last', fec_corr_bits)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_FEC_NOT_CORRECTABLE_FARMES_last', fec_uncorr_frames)
    redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_PRE_BER', fec_corr_bits_ber_new)
    redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_POST_BER', fec_uncorr_bits_ber_new)
    redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_MAX_T', maxT)
end

local function compute_rate(port)
    -- Check if alpha is available for rate computation
    if not alpha then
        logit("Alpha is not defined, skipping rate computation")
        return
    end

    local state_table = rates_table_name .. ':' .. port .. ':' .. 'PORT'
    local initialized = redis.call('HGET', state_table, 'INIT_DONE')
    logit(initialized)

    -- Get new COUNTERS values
    local in_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_UCAST_PKTS')
    local in_non_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS')
    local out_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS')
    local out_non_ucast_pkts = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS')
    local in_octets = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_OCTETS')
    local out_octets = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_OCTETS')

    if not in_ucast_pkts or not in_non_ucast_pkts or not out_ucast_pkts or
       not out_non_ucast_pkts or not in_octets or not out_octets then
        logit("Not found some counters on " .. port)
        return
    end

    if initialized == 'DONE' or initialized == 'COUNTERS_LAST' then
        -- Get old COUNTERS values
        local in_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_UCAST_PKTS_last')
        local in_non_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS_last')
        local out_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS_last')
        local out_non_ucast_pkts_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS_last')
        local in_octets_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_OCTETS_last')
        local out_octets_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_OCTETS_last')

        -- Calculate new rates values
        local scale_factor = 1000 / delta
        local rx_bps_new = (in_octets - in_octets_last) * scale_factor 
        local tx_bps_new = (out_octets - out_octets_last) * scale_factor
        local rx_pps_new = ((in_ucast_pkts + in_non_ucast_pkts) - (in_ucast_pkts_last + in_non_ucast_pkts_last)) * scale_factor
        local tx_pps_new = ((out_ucast_pkts + out_non_ucast_pkts) - (out_ucast_pkts_last + out_non_ucast_pkts_last)) * scale_factor

        if initialized == "DONE" then
            -- Get old rates values
            local rx_bps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'RX_BPS')
            local rx_pps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'RX_PPS')
            local tx_bps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'TX_BPS')
            local tx_pps_old = redis.call('HGET', rates_table_name .. ':' .. port, 'TX_PPS')

            -- Smooth the rates values and store them in DB
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_BPS', alpha*rx_bps_new + one_minus_alpha*rx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_PPS', alpha*rx_pps_new + one_minus_alpha*rx_pps_old)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_BPS', alpha*tx_bps_new + one_minus_alpha*tx_bps_old)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_PPS', alpha*tx_pps_new + one_minus_alpha*tx_pps_old)
        else
            -- Store unsmoothed initial rates values in DB
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_BPS', rx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. port, 'RX_PPS', rx_pps_new)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_BPS', tx_bps_new)
            redis.call('HSET', rates_table_name .. ':' .. port, 'TX_PPS', tx_pps_new)
            redis.call('HSET', state_table, 'INIT_DONE', 'DONE')
        end

    else
        redis.call('HSET', state_table, 'INIT_DONE', 'COUNTERS_LAST')
    end

    -- Set old COUNTERS values
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_UCAST_PKTS_last', in_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_NON_UCAST_PKTS_last', in_non_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_UCAST_PKTS_last', out_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_NON_UCAST_PKTS_last', out_non_ucast_pkts)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_OCTETS_last', in_octets)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_OUT_OCTETS_last', out_octets)

end

local n = table.getn(KEYS)
for i = 1, n do
    -- Compute only BER for gearbox counters
    if counters_db == gb_counters_db then
        compute_ber(KEYS[i])
    else
        compute_rate(KEYS[i])
        compute_ber(KEYS[i])
    end
end

return logtable
