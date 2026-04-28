/**
 * FlyteOS v1.1 - Real-world terrain proxy server
 * Serves satellite imagery + terrain tiles with CORS proxy
 * 
 * Data sources (all free):
 * - Satellite: ESRI World Imagery
 * - Terrain: Mapzen Terrain (AWS)
 */

const http = require('http');
const https = require('https');
const path = require('path');
const fs = require('fs');
const url = require('url');

// Config
const PORT = 7890;
const CACHE_DIR = path.join(__dirname, 'cache');

// Ensure cache directory exists
if (!fs.existsSync(CACHE_DIR)) {
    fs.mkdirSync(CACHE_DIR, { recursive: true });
}

// Tile source configurations
const TILE_SOURCES = {
    // ESRI World Imagery - Free satellite tiles
    'server.arcgisonline.com': {
        protocol: 'https',
        pathPrefix: '/ArcGIS/rest/services/World_Imagery/MapServer',
    },
    // Mapzen Terrain RGB tiles
    's3.amazonaws.com': {
        protocol: 'https',
        pathPrefix: '/elevation-tiles-prod/terrarium',
    },
    // Mapzen Terrain PNG tiles (alternative)
    'heightmap tiles.cloud': {
        protocol: 'https',
        pathPrefix: '/',
    },
};

// MIME types
const MIME_TYPES = {
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.jpeg': 'image/jpeg',
    '.json': 'application/json',
    '.html': 'text/html; charset=utf-8',
    '.js': 'application/javascript',
    '.css': 'text/css',
};

// Get cache path for a tile
function getCachePath(tileUrl) {
    const hash = require('crypto').createHash('md5').update(tileUrl).digest('hex');
    return path.join(CACHE_DIR, hash + '.png');
}

// Proxy a request with caching
function proxyRequest(req, res, tileUrl) {
    const cachePath = getCachePath(tileUrl);
    
    // Check cache first
    if (fs.existsSync(cachePath)) {
        const data = fs.readFileSync(cachePath);
        res.setHeader('Content-Type', 'image/png');
        res.setHeader('Access-Control-Allow-Origin', '*');
        res.setHeader('X-Cache', 'HIT');
        res.end(data);
        console.log(`[CACHE HIT] ${tileUrl}`);
        return;
    }
    
    console.log(`[FETCH] ${tileUrl}`);
    
    const protocol = tileUrl.startsWith('https') ? https : http;
    const proxyReq = protocol.get(tileUrl, { 
        headers: {
            'User-Agent': 'FlyteOS/1.1 (Open Source Flight Simulator)',
            'Referer': 'https://github.com/fulaite3-afk/flyteos-sim',
        },
        timeout: 10000,
    }, (proxyRes) => {
        // Handle redirects
        if (proxyRes.statusCode === 301 || proxyRes.statusCode === 302) {
            proxyRequest(req, res, proxyRes.headers.location);
            return;
        }
        
        const chunks = [];
        proxyRes.on('data', chunk => chunks.push(chunk));
        proxyRes.on('end', () => {
            const data = Buffer.concat(chunks);
            
            // Cache the tile
            fs.writeFileSync(cachePath, data);
            
            res.setHeader('Content-Type', 'image/png');
            res.setHeader('Access-Control-Allow-Origin', '*');
            res.setHeader('X-Cache', 'MISS');
            res.end(data);
        });
    });
    
    proxyReq.on('error', (err) => {
        console.error(`[ERROR] ${tileUrl}: ${err.message}`);
        res.statusCode = 500;
        res.end('Proxy error: ' + err.message);
    });
    
    proxyReq.on('timeout', () => {
        proxyReq.destroy();
        res.statusCode = 504;
        res.end('Request timeout');
    });
}

// Parse tile URL from query params
function getTileUrl(pathname, query) {
    const parsedUrl = url.parse(pathname, true);
    
    // Pattern: /satellite/{z}/{x}/{y}.png
    const satMatch = pathname.match(/^\/satellite\/(\d+)\/(\d+)\/(\d+)\.png$/);
    if (satMatch) {
        const [,, z, x, y] = satMatch;
        // ESRI World Imagery
        return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/${z}/${y}/${x}`;
    }
    
    // Pattern: /terrain/{z}/{x}/{y}.png
    const terrMatch = pathname.match(/^\/terrain\/(\d+)\/(\d+)\/(\d+)\.png$/);
    if (terrMatch) {
        const [,, z, x, y] = terrMatch;
        // Mapzen Terrain RGB
        return `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/${z}/${x}/${y}.png`;
    }
    
    return null;
}

// Create HTTP server
const server = http.createServer((req, res) => {
    const parsedUrl = url.parse(req.url);
    const pathname = parsedUrl.pathname;
    
    // CORS preflight
    if (req.method === 'OPTIONS') {
        res.setHeader('Access-Control-Allow-Origin', '*');
        res.setHeader('Access-Control-Allow-Methods', 'GET, OPTIONS');
        res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
        res.statusCode = 204;
        res.end();
        return;
    }
    
    // Proxy tile requests
    if (pathname.startsWith('/satellite/') || pathname.startsWith('/terrain/')) {
        const tileUrl = getTileUrl(pathname, parsedUrl.query);
        if (tileUrl) {
            proxyRequest(req, res, tileUrl);
            return;
        }
    }
    
    // Serve static files from parent directory
    if (pathname.startsWith('/static/')) {
        const filePath = path.join(__dirname, '..', pathname.replace('/static/', ''));
        if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
            const ext = path.extname(filePath);
            res.setHeader('Content-Type', MIME_TYPES[ext] || 'application/octet-stream');
            res.setHeader('Access-Control-Allow-Origin', '*');
            res.end(fs.readFileSync(filePath));
            return;
        }
    }
    
    // Root: serve flight_sim_3d.html
    if (pathname === '/' || pathname === '/index.html') {
        const htmlPath = path.join(__dirname, 'flight_sim_3d.html');
        if (fs.existsSync(htmlPath)) {
            res.setHeader('Content-Type', 'text/html; charset=utf-8');
            res.end(fs.readFileSync(htmlPath));
            return;
        }
    }
    
    // 404
    res.statusCode = 404;
    res.setHeader('Content-Type', 'text/plain');
    res.end('Not found');
});

server.listen(PORT, () => {
    console.log(`
╔═══════════════════════════════════════════════════╗
║         FlyteOS v1.1 Tile Proxy Server           ║
╠═══════════════════════════════════════════════════╣
║  Server running at: http://localhost:${PORT}           
║  Satellite tiles: ESRI World Imagery (free)      ║
║  Terrain tiles:    Mapzen Terrain AWS (free)     ║
║  Cache directory:  ${CACHE_DIR.replace(/\\/g, '/').padEnd(29)}║
╚═══════════════════════════════════════════════════╝
    `);
});

server.on('error', (err) => {
    if (err.code === 'EADDRINUSE') {
        console.error(`Port ${PORT} is already in use. Please stop other servers.`);
    } else {
        console.error('Server error:', err);
    }
    process.exit(1);
});
