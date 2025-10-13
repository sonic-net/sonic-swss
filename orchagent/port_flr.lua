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
local default_poll_interval = tonumber(ARGV[3])

local APPL_DB         = 0      -- Application database
local COUNTERS_DB     = 2      -- Counters and statistics
local STATE_DB        = 6      -- State database

local KEY_SPEED = 'speed'
local KEY_LANES = 'lanes'
local KEY_OPER_STATUS = 'oper_status'

local STATE_DB_PORT_TABLE_PREFIX = 'PORT_TABLE|'
local APPL_DB_PORT_TABLE_PREFIX  = 'PORT_TABLE:'

local rates_table_name = "RATES"
local bookmark_table_name = "RATES:GLOBAL"
local BIN_FILTER_VALUE = 10
local MIN_SIGNIFICANT_BINS = 2
local FEC_FLR_POLL_INTERVAL = 120
local MFC = 8

local function get_port_name_from_oid(port)
    redis.call('SELECT', COUNTERS_DB)
    local port_name_hash = redis.call('HGETALL', 'COUNTERS_PORT_NAME_MAP')
    local num_port_keys = redis.call('HLEN', 'COUNTERS_PORT_NAME_MAP')
    -- flip port name hash
    for i = 1, num_port_keys do
        local k_index = i*2 -1
        local v_index = i*2
        if (port_name_hash[v_index] == port) then
            return port_name_hash[k_index]
        end
    end
    return 0
end

local function get_port_speed(port_name)
    redis.call('SELECT', STATE_DB)
    local raw_speed = redis.call('HGET', STATE_DB_PORT_TABLE_PREFIX .. port_name, KEY_SPEED)

    redis.call('SELECT', APPL_DB)
    local oper_status = redis.call('HGET', APPL_DB_PORT_TABLE_PREFIX .. port_name, KEY_OPER_STATUS)

    if (raw_speed == false) or (oper_status ~= 'up') then
        redis.call('SELECT', APPL_DB)
        raw_speed = redis.call('HGET', APPL_DB_PORT_TABLE_PREFIX .. port_name, KEY_SPEED)
    end
    return raw_speed
end

local function get_port_numlanes(port_name)
    redis.call('SELECT', APPL_DB)
    local port_lanes = redis.call('HGET', APPL_DB_PORT_TABLE_PREFIX .. port_name, KEY_LANES)
    local count = select(2, string.gsub(port_lanes, ',', ''))
    return (count+1)
end


local function get_interleaving_factor_for_port(port_oid)
    -- Correlation between port-speeds, number of lanes and
    -- Interleaving factor
    -- This lookup table is a direct implementation of the table present in the HLD.
    -- The key is a string in the format: 'speed_lanes'
    local interleaving_map = {
        ['1600000_8'] = 4,
        ['800000_8']  = 4,
        ['400000_8']  = 2,
        ['400000_4']  = 2,
        ['200000_4']  = 2,
        ['200000_2']  = 2,
        ['100000_2']  = 2,
    }

    local port_name = get_port_name_from_oid(port_oid)
    local port_speed = get_port_speed(port_name)
    local port_numlanes = get_port_numlanes(port_name)

    -- Create the key from the port's properties to search the map.
    local key = tostring(port_speed) .. '_' .. tostring(port_numlanes)

    -- reset redis object to COUNTERS_DB
    redis.call('SELECT', COUNTERS_DB)

    -- Look up the factor.
    return interleaving_map[key] or 1
end

-- Get configuration
redis.call('SELECT', counters_db)

-- Get numeric value from Redis table:port, returns 0 if not found
local function get_kv_from_redis_db(table_name, port, key)
    local value = redis.call('HGET', table_name .. ':' .. port, key)
    value = tonumber(value) or 0
    return value
end

-- Store value in Redis table:port
local function set_kv_in_redis_db(table_name, port, key, value)
    redis.call('HSET', table_name .. ':' .. port, key, tostring(value))
end


local fec_cwerr_keys = {
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S1",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S2",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S3",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S4",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S5",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S6",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S7",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S8",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S9",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S10",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S11",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S12",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S13",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S14",
    "SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S15",
}

-- Calculate delta values for FEC codeword error bins (S0-S15) for predicted FLR computation
-- This function computes the difference between current and previous counter values for each
-- symbol error bin, which represents codeword errors that occurred in the current interval.
-- Parameters:
--   port: Port identifier
-- Returns:
--   delta_bins: Array of delta values for each symbol error bin (S0-S15)
local function get_fec_cws_delta(port)
    local delta_bins = {}

    local binval = 0
    local binval_last = 0
    local delta = 0
    for _, key in ipairs(fec_cwerr_keys) do
        -- Get current counter value from COUNTERS table
        binval = tonumber(get_kv_from_redis_db(counters_table_name, port, key))
        -- Get previous counter value from RATES table (where "_last" values are stored)
        binval_last = tonumber(get_kv_from_redis_db(rates_table_name, port, key .. "_last"))
        -- Calculate delta for this interval
        delta = binval - binval_last
        table.insert(delta_bins, delta)
        -- Store current value as "_last" for next interval calculation
        set_kv_in_redis_db(rates_table_name, port, key .. "_last", binval)
    end

    return delta_bins
end

-- Sum all codeword counts across symbol error bins
local function get_total_cws(codewords)
    local total_cw = 0

    for _, value in ipairs(codewords) do
        total_cw = total_cw + value
    end

    return total_cw
end

-- Count the number of symbol error bins with significant codeword error counts
-- Only bins with counts greater than BIN_FILTER_VALUE (=10) are considered significant
-- for linear regression analysis. This filters out noise and ensures statistical reliability.
-- Parameters:
--   bins: Array of codeword error counts for each symbol error bin
-- Returns:
--   significant_count: Number of bins with values greater than BIN_FILTER_VALUE
local function count_significant_bins(bins)
    local significant_count = 0
    for _, value in ipairs(bins) do
        if value > BIN_FILTER_VALUE then
            significant_count = significant_count + 1
        end
    end

    return significant_count
end

-- Compute slope and intercept for linear regression on logarithmic codeword error ratios
-- The codeword error ratio typically follows an exponential decay curve, which becomes
-- linear when transformed to logarithmic scale, enabling linear regression analysis.
-- Parameters:
--   bins: Array of codeword error counts for each symbol error bin (S1-S15, S0 excluded)
--   total_cws: Total number of codewords across all bins
-- Returns:
--   slope, intercept, r_squared: Linear regression parameters and accuracy measure
local function compute_slope_intercept(bins, total_cws)
    -- Step1: Normalize to probability of cw_i errors where cw_i is the probability of a
    -- CW with i symbol errors (only consider bins with significant error counts)
    local normalised_cw = {}
    for _, value in ipairs(bins) do
        if value > BIN_FILTER_VALUE then
            table.insert(normalised_cw, value/total_cws)
        else
            table.insert(normalised_cw, 0)
        end
    end

    -- Step2 :Convert the exponential data to logarithmic data
    local log_values_cw = {}
    local nan = 0/0
    for _, normalised_cw_i in ipairs(normalised_cw) do
        if normalised_cw_i > 0 then
            table.insert(log_values_cw, math.log10(normalised_cw_i))
        else
            table.insert(log_values_cw, nan)
        end
    end

    -- Step3 : Prepare mask vector
    local mask = {}
    for _, log_value_cw_i in ipairs(log_values_cw) do
        if log_value_cw_i ~= log_value_cw_i then
            table.insert(mask, 0)
        else
            table.insert(mask, 1)
        end
    end

    --Step4 : Linear Regression
    local data_length = #bins
    logit("Data Length :" .. data_length)

    local B = 0     -- ## n
    local C = 0     -- ## sigma(x)
    local D = 0     -- ## sigma(y)
    local E = 0     -- ## sigma(x^2)
    local F = 0     -- ## sigma(xy)
    local G = 0     -- ## sigma(y^2)

    for i = 1, data_length do
        if mask[i] == 1 then
            B = B + mask[i]
            C = C + (i)
            D = D + (log_values_cw[i])
            E = E + ((i) * (i))
            F = F + ((i) * (log_values_cw[i]))
            G = G + ((log_values_cw[i]) * (log_values_cw[i]))
        end
    end

    -- Slope and Intercept
    local slope = (B*F - C*D)/(B*E - C*C)
    local intercept = (D - slope*C) / B

    -- R^2 (measure of accuracy)
    local numerator = (B * F - C * D)
    local denominator = math.sqrt((B * E - C*C) * (B * G - D*D))
    local r_squared = (numerator / denominator) * (numerator / denominator)

    return slope, intercept, r_squared
end

-- Compute the predicted Frame Loss Ratio (FLR) from linear regression parameters
-- Uses the fitted slope and intercept to extrapolate CER for uncorrectable symbol errors
-- (window 16-20) and converts to FLR using IEEE FEC formula with interleaving factor.
-- Parameters:
--   slope: Fitted slope from log-linear regression
--   intercept: Fitted intercept from log-linear regression
--   sum_window: Array of [start_index, end_index] for uncorrectable error window (16-20)
--   x_interleaving: FEC interleaving factor (1=no interleaving, 2=400G, 4=800G+)
--   mfc: MAC frames per codeword (8 for RS-544 FEC)
-- Returns:
--   cer: Predicted Codeword Error Ratio for the window
--   flr: Predicted Frame Loss Ratio using IEEE formula
local function extrapolate_flr_from_regression(slope, intercept, sum_window, x_interleaving, mfc)
    -- Transform logarithmic regression line back to linear scale to get predicted CER
    local function line_function(x)
        return 10 ^ (intercept + (slope*x))
    end

    -- # Sum predicted corrected errors in the given window
    local cer = 0
    local flr = 0
    for x = sum_window[1], sum_window[2]+1 do
        cer = cer + line_function(x)
    end

    -- # IEEE FLR formula
    flr = cer * (1 + x_interleaving * mfc) / mfc

    return cer, flr
end

-- Main function to calculate predicted FLR using linear regression on codeword error distribution
-- Steps: Get error deltas -> Remove S0 -> Check sufficient data -> Perform regression -> Extrapolate FLR
-- Parameters:
--   port: Port identifier
-- Returns:
--   flr: Predicted Frame Loss Ratio (0 if insufficient data for prediction)
local function compute_predicted_flr(port)
    local bins = get_fec_cws_delta(port)

    local total_cws = get_total_cws(bins)
    logit("SUM : " .. total_cws)
    if total_cws == 0 then
        logit("Total corrected codewords is zero, cannot compute slope and intercept.")
        return 0
    end

    -- Trim out _S0 from the data
    table.remove(bins, 1)

    local significant_bins = count_significant_bins(bins)
    logit("Significant Bins : " .. significant_bins)
    if significant_bins < MIN_SIGNIFICANT_BINS then
        logit("Not enough significant bins to compute slope and intercept.")
        return 0
    end

    local slope = 0
    local intercept = 0
    local r_squared = 0

    slope, intercept, r_squared = compute_slope_intercept(bins, total_cws)
    logit("Slope : " .. slope)
    logit("Intercept : " .. intercept)
    logit("R^2 : " .. r_squared)

    local cer = 0
    local flr = 0

    local sum_window = {16,20}
    local x_interleaving = get_interleaving_factor_for_port(port)
    cer, flr = extrapolate_flr_from_regression(slope, intercept, {16, 20}, x_interleaving, MFC)
    logit("CER : " .. cer)
    logit("FLR : " .. flr)
    return flr, r_squared
end

-- Calculate observed FEC FLR based on uncorrectable codeword ratio
-- Formula: CER = Uncorrectable_CWs / Total_CWs, FLR = 1.125 * CER (X=1 interleaving)
-- Parameters:
--   port: Port identifier
-- Returns:
--   fec_flr: Observed Frame Loss Ratio (0 if no data change or counters unavailable)
local function compute_observed_flr(port)

    local fec_uncorr_codewords = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES')
    local fec_corr_codewords = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES')
    local fec_codewords_with_zero_errors = redis.call('HGET', counters_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0')

    -- check if these values are defined
    if fec_uncorr_codewords and fec_corr_codewords and fec_codewords_with_zero_errors then
        local fec_uncorr_codewords_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES_last')
        local fec_corr_codewords_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES_last')
        local fec_codewords_with_zero_errors_last = redis.call('HGET', rates_table_name .. ':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0_last')

        fec_uncorr_codewords_last = tonumber(fec_uncorr_codewords_last) or 0
        fec_corr_codewords_last = tonumber(fec_corr_codewords_last) or 0
        fec_codewords_with_zero_errors_last = tonumber(fec_codewords_with_zero_errors_last) or 0

        local fec_uncorr_codewords_delta = fec_uncorr_codewords - fec_uncorr_codewords_last
        local fec_corr_codewords_delta = fec_corr_codewords - fec_corr_codewords_last
        local fec_codewords_with_zero_errors_delta = fec_codewords_with_zero_errors - fec_codewords_with_zero_errors_last

        local total_codewords_delta = fec_uncorr_codewords_delta +
                                      fec_corr_codewords_delta +
                                      fec_codewords_with_zero_errors_delta

        -- if total_delta == 0, nothing has changed so don't compute flr
        if (total_codewords_delta == 0) then
            return 0
        end

        local codeword_error_ratio = fec_uncorr_codewords_delta / total_codewords_delta
        -- assuming interleaving factor is X = 1
        local x_interleaving = get_interleaving_factor_for_port(port)
        local fec_flr = x_interleaving * codeword_error_ratio

        -- update old counter values
        redis.call('HSET', rates_table_name ..':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES_last', fec_uncorr_codewords)
        redis.call('HSET', rates_table_name ..':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES_last', fec_corr_codewords)
        redis.call('HSET', rates_table_name ..':' .. port, 'SAI_PORT_STAT_IF_IN_FEC_CODEWORD_ERRORS_S0_last', fec_codewords_with_zero_errors)

        return tonumber(fec_flr)
    end
    return 0
end

-- Update FLR timestamp in bookmark table
local function update_flr_timestamp()
    local time = redis.call('TIME')
    local timestamp_current = time[1]
    redis.call('HSET', bookmark_table_name, 'FEC_FLR_TIMESTAMP_last', timestamp_current)
end

-- Check if FLR calculation interval has elapsed (default 120s)
local function time_to_calculate_flr()
    local time = redis.call('TIME')
    local timestamp_current = time[1]

    -- Check if FEC_FLR_TIMESTAMP_last exists in the bookmark table
    local timestamp_last = redis.call('HGET', bookmark_table_name, 'FEC_FLR_TIMESTAMP_last')

    -- If the key doesn't exist, return true
    if timestamp_last == false then
        return true  -- First time calculation
    end

    timestamp_last = tonumber(timestamp_last) or 0

    if (timestamp_last == 0) or ((timestamp_current - timestamp_last) >= FEC_FLR_POLL_INTERVAL) then
        return true
    end

    return false
end

-- Check if FEC data exists for a port by verifying the presence of correctable frames counter
-- Parameters:
--   port: Port identifier
-- Returns:
--   true if FEC data exists, false otherwise
local function fec_data_exists(port)
    local hash_key = counters_table_name .. ':' .. port
    local exists = redis.call('HEXISTS', hash_key, 'SAI_PORT_STAT_IF_IN_FEC_CORRECTABLE_FRAMES')
    return exists == 1
end

-- Main FLR computation function that orchestrates both observed and predicted FLR calculation
-- Called for each port at the configured interval. Computes and stores both FEC_FLR (observed)
-- and FEC_FLR_PREDICTED in the RATES table for telemetry collection.
-- Parameters:
--   port: Port identifier
local function compute_flr_for_port(port)
    if (fec_data_exists(port)) then
        -- Calculate observed FLR from uncorrectable codeword ratio
        local fec_flr = compute_observed_flr(port)
        redis.call('HSET', rates_table_name ..':' .. port, 'FEC_FLR', fec_flr)

        -- Calculate predicted FLR using linear regression on codeword error distribution
        local predicted_flr
        local r_squared
        predicted_flr, r_squared = compute_predicted_flr(port)
        predicted_flr = predicted_flr or 0
        r_squared = r_squared or 0
        redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_FLR_PREDICTED', tostring(predicted_flr))
        redis.call('HSET', rates_table_name .. ':' .. port, 'FEC_FLR_R_SQUARED', tostring(r_squared))
    end
end

local n = table.getn(KEYS)

if (time_to_calculate_flr()) then
    for i = 1, n do
        compute_flr_for_port(KEYS[i])
    end
    update_flr_timestamp()
end

return logtable
