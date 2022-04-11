-- KEYS - gearbox port IDs
-- ARGV[1] - counters db index
-- ARGV[2] - counters table name
-- ARGV[3] - poll time interval
-- return KEY number

local counters_db = '2'
local gb_counters_db = ARGV[1]
local counters_table = ARGV[2]

local function sum(a)
    local r = 0
    for i,v in ipairs(a) do
        if v then
            r = r + v
        end
    end
    return r
end

local in_errors = {}
local in_discards = {}
local out_errors = {}
local out_discards = {}
local rx_oversizes = {}
local tx_oversizes = {}
local undersizes = {}
local jabbers = {}
local fragments = {}
local fec_not_correctable = {}
local fec_symbol_error = {}

local n = table.getn(KEYS)
for i = 1, n, 3 do
    -- Tuple KEYS(i, i+1, i+2) is (portName, portID_systemSide, portID_lineSide)

    redis.call('SELECT', gb_counters_db)
    in_errors[1] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+1], 'SAI_PORT_STAT_IF_OUT_ERRORS')
    in_errors[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_IF_IN_ERRORS')

    in_discards[1] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+1], 'SAI_PORT_STAT_IF_OUT_DISCARDS')
    in_discards[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_IF_IN_DISCARDS')

    out_errors[1] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+1], 'SAI_PORT_STAT_IF_IN_ERRORS')
    out_errors[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_IF_OUT_ERRORS')

    out_discards[1] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+1], 'SAI_PORT_STAT_IF_IN_DISCARDS')
    out_discards[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_IF_OUT_DISCARDS')

    rx_oversizes[1] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+1], 'SAI_PORT_STAT_ETHER_TX_OVERSIZE_PKTS')
    rx_oversizes[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS')

    tx_oversizes[1] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+1], 'SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS')
    tx_oversizes[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_ETHER_TX_OVERSIZE_PKTS')

    undersizes[1] = 0
    undersizes[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS')

    jabbers[1] = 0
    jabbers[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_ETHER_STATS_JABBERS')

    fragments[1] = 0
    fragments[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_ETHER_STATS_FRAGMENTS')

    fec_not_correctable[1] = 0
    fec_not_correctable[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES')

    fec_symbol_error[1] = 0
    fec_symbol_error[2] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i+2], 'SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS')


    redis.call('SELECT', counters_db)
    KEYS[i] = redis.call('HGET', 'COUNTERS_PORT_NAME_MAP', KEYS[i])

    in_errors[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_ERRORS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_ERRORS', sum (in_errors))

    in_discards[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_DISCARDS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_DISCARDS', sum (in_discards))

    out_errors[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_ERRORS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_ERRORS', sum (out_errors))

    out_discards[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_IF_OUT_DISCARDS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_DISCARDS', sum (out_discards))

    rx_oversizes[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS', sum (rx_oversizes))

    tx_oversizes[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_ETHER_TX_OVERSIZE_PKTS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_ETHER_RX_OVERSIZE_PKTS', sum (tx_oversizes))

    undersizes[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_ETHER_STATS_UNDERSIZE_PKTS', sum (undersizes))

    jabbers[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_ETHER_STATS_JABBERS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_ETHER_STATS_JABBERS', sum (jabbers))

    fragments[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_ETHER_STATS_FRAGMENTS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_ETHER_STATS_FRAGMENTS', sum (fragments))

    fec_not_correctable[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_FEC_NOT_CORRECTABLE_FRAMES', sum (fec_not_correctable))

    fec_symbol_error[3] = redis.call('HGET', counters_table .. ':PHY:' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS')
    redis.call('HSET', counters_table .. ':' .. KEYS[i], 'SAI_PORT_STAT_IF_IN_FEC_SYMBOL_ERRORS', sum (fec_symbol_error))
end

return table.getn(KEYS)
