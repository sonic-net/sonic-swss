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

-- Get configuration
redis.call('SELECT', counters_db)
local smooth_interval = redis.call('HGET', rates_table_name .. ':' .. 'PORT', 'PORT_SMOOTH_INTERVAL')
local alpha = redis.call('HGET', rates_table_name .. ':' .. 'PORT', 'PORT_ALPHA')
if not alpha then
  logit("Alpha is not defined")
  return logtable
end
local one_minus_alpha = 1.0 - alpha
local delta = tonumber(ARGV[3])

logit(alpha)
logit(one_minus_alpha)
logit(delta)

local port_interface_oid_map = redis.call('HGETALL', "COUNTERS_PORT_NAME_MAP")
local port_interface_oid_key_count = redis.call('HLEN', "COUNTERS_PORT_NAME_MAP")

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

local function compute_rate(port)

    local state_table = rates_table_name .. ':' .. port .. ':' .. 'PORT'
    local initialized = redis.call('HGET', state_table, 'INIT_DONE')
    logit(initialized)

    -- FEC BER
    local fec_corr_bits, fec_uncorr_frames
    local fec_corr_bits_ber_new, fec_uncorr_bits_ber_new = -1, -1
    -- HLD review suggest to use the statistical average when calculate the post fec ber
    local rs_average_frame_ber = 1e-8
    local lanes_speed, serdes_speed, lanes_count = 0, 0, 0

    -- lookup interface name from oid
    local interface_name = find_interface_name_from_oid(port)
    if interface_name then
        lanes_count, lanes_speed, serdes_speed = find_lanes_and_serdes(interface_name)
 
        if lanes_count and serdes_speed then
            fec_corr_bits = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CORRECTED_BITS')
            fec_uncorr_frames = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES')
        end
    end


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

        -- only do the calculation when all info present
        
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

    -- do not update FEC related stat if we dont have it
    
    if not fec_corr_bits or not fec_uncorr_frames or not fec_corr_bits_ber_new or
       not fec_uncorr_bits_ber_new then
        logit("FEC counters not found on " .. port)
        return
    end
    -- Set BER values
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_FEC_CORRECTED_BITS_last', fec_corr_bits)
    redis.call('HSET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_FEC_NOT_CORRECTABLE_FARMES_last', fec_uncorr_frames)
    redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_PRE_BER', fec_corr_bits_ber_new)
    redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_POST_BER', fec_uncorr_bits_ber_new)
end

local n = table.getn(KEYS)
for i = 1, n do
    compute_rate(KEYS[i])
end

return logtable
