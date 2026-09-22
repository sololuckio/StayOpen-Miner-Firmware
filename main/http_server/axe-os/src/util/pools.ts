export interface PoolConfig {
    id: string; // Unique identifier
    label: string; // Display name
    logo: string; // Filename in /public/pools/ or public URL
    value: string; // Stratum URL with port
    type: 'BTC' | 'LTC'; // Pool type for filtering
}

export const POOL_MODELS: PoolConfig[] = [
    // BTC Pools
    {
        id: 'atlaspool_btc',
        label: 'AtlasPool.io',
        logo: '/pools/atlaspool.svg',
        value: 'solo.atlaspool.io:3333',
        type: 'BTC'
    },
    {
        id: 'bitcoinmerch_btc',
        label: 'Bitcoin Merch',
        logo: '/pools/BitcoinMerch.png',
        value: 'pool.bitcoinmerch.com:3333',
        type: 'BTC'
    },
    {
        id: 'ckpool',
        label: 'Solo CKPool',
        logo: '/pools/ckpool.svg',
        value: 'solo.ckpool.org:3333',
        type: 'BTC'
    },
    {
        id: 'miningdutch_btc',
        label: 'Mining-Dutch',
        logo: '/pools/miningdutch.png',
        value: 'americas.mining-dutch.nl:9996',
        type: 'BTC'
    },
    {
        id: 'sololuck_btc',
        label: 'SoloLuck',
        logo: '/pools/sololuck.svg',
        value: 'stratum.sololuck.io:3333',
        type: 'BTC'
    },

    // LTC Pools
    {
        id: 'litesolo',
        label: 'LiteSolo',
        logo: '/pools/litesolo.svg',
        value: 'us.litesolo.org:3333',
        type: 'LTC'
    },
    {
        id: 'bitcoinmerch_doge',
        label: 'Bitcoin Merch Doge',
        logo: '/pools/BitcoinMerch.png',
        value: 'doge.bitcoinmerch.com:3333',
        type: 'LTC'
    },
    {
        id: 'miningdutch_ltc',
        label: 'Mining-Dutch',
        logo: '/pools/miningdutch.png',
        value: 'scrypt.mining-dutch.nl:4083',
        type: 'LTC'
    },
    {
        id: 'viabtc_ltc',
        label: 'ViaBTC (Set Solo in Dashboard)',
        logo: '/pools/viabtc.svg',
        value: 'ltc.viabtc.top:3333',
        type: 'LTC'
    },
];
