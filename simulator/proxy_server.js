/**
 * FlyteOS v1.1 - Real-world terrain proxy server (安全加固版)
 * Serves satellite imagery + terrain tiles with CORS proxy
 * 
 * Data sources (all free):
 * - Satellite: ESRI World Imagery
 * - Terrain: Mapzen Terrain (AWS)
 *
 * 安全加固: 云中鹤 2026-05-02
 * 修复: 路径遍历(CWE-22), CORS(CWE-942), SSRF(CWE-918),
 *       信息泄露(CWE-209), 弱哈希(CWE-328), 输入验证(CWE-20)
 */

const http = require('http');
const https = require('https');
const path = require('path');
const fs = require('fs');
const url = require('url');
const crypto = require('crypto');

// Config
const PORT = 7890;
const CACHE_DIR = path.join(__dirname, 'cache');
const MAX_URL_LENGTH = 2048;
const MAX_TILE_Z = 18;        // 最大缩放级别
const RATE_LIMIT_WINDOW = 60000; // 60秒窗口
const RATE_LIMIT_MAX = 120;    // 每分钟最多120个请求

// 允许代理的目标域名白名单
const ALLOWED_HOSTS = new Set([
  'server.arcgisonline.com',
  's3.amazonaws.com',
]);

// 简单的速率限制存储
const rateLimiter = new Map();

// Ensure cache directory exists
if (!fs.existsSync(CACHE_DIR)) {
    fs.mkdirSync(CACHE_DIR, { recursive: true });
}

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

/**
 * 速率限制检查
 */
function checkRateLimit(clientIp) {
    const now = Date.now();
    const record = rateLimiter.get(clientIp);
    
    if (!record || now - record.windowStart > RATE_LIMIT_WINDOW) {
        rateLimiter.set(clientIp, { windowStart: now, count: 1 });
        return true;
    }
    
    if (record.count >= RATE_LIMIT_MAX) {
        return false;
    }
    
    record.count++;
    return true;
}

/**
 * 安全缓存路径 - 使用SHA256替代MD5
 */
function getCachePath(tileUrl) {
    const hash = crypto.createHash('sha256').update(tileUrl).digest('hex').substring(0, 32);
    return path.join(CACHE_DIR, hash + '.png');
}

/**
 * 安全路径解析 - 防止路径遍历
 */
function safePathResolve(base, requestPath) {
    let decoded = decodeURIComponent(requestPath);
    if (decoded.startsWith('/')) decoded = decoded.slice(1);
    const resolved = path.resolve(base, decoded);
    if (!resolved.startsWith(base + path.sep) && resolved !== base) {
        return null;
    }
    return resolved;
}

/**
 * 验证瓦片参数范围
 */
function validateTileParams(z, x, y) {
    const zn = parseInt(z, 10);
    const xn = parseInt(x, 10);
    const yn = parseInt(y, 10);
    
    if (isNaN(zn) || isNaN(xn) || isNaN(yn)) return false;
    if (zn < 0 || zn > MAX_TILE_Z) return false;
    const maxTile = Math.pow(2, zn);
    if (xn < 0 || xn >= maxTile) return false;
    if (yn < 0 || yn >= maxTile) return false;
    
    return true;
}

/**
 * 安全响应头
 */
function setSecurityHeaders(res) {
    res.setHeader('X-Frame-Options', 'DENY');
    res.setHeader('X-Content-Type-Options', 'nosniff');
    res.setHeader('X-XSS-Protection', '1; mode=block');
    res.setHeader('Referrer-Policy', 'strict-origin-when-cross-origin');
}

// Proxy a request with caching
function proxyRequest(req, res, tileUrl) {
    // SSRF防护: 验证目标URL在白名单内
    try {
        const parsedTarget = new URL(tileUrl);
        if (!ALLOWED_HOSTS.has(parsedTarget.hostname)) {
            res.statusCode = 403;
            res.setHeader('Content-Type', 'text/plain');
            res.end('Forbidden: target host not allowed');
            console.error('[BLOCKED] SSRF attempt to:', parsedTarget.hostname);
            return;
        }
    } catch (e) {
        res.statusCode = 400;
        res.setHeader('Content-Type', 'text/plain');
        res.end('Bad Request');
        return;
    }
    
    const cachePath = getCachePath(tileUrl);
    
    // Check cache first
    if (fs.existsSync(cachePath)) {
        const data = fs.readFileSync(cachePath);
        res.setHeader('Content-Type', 'image/png');
        res.setHeader('X-Cache', 'HIT');
        setSecurityHeaders(res);
        res.end(data);
        console.log('[CACHE HIT]', tileUrl);
        return;
    }
    
    console.log('[FETCH]', tileUrl);
    
    const protocol = tileUrl.startsWith('https') ? https : http;
    const proxyReq = protocol.get(tileUrl, { 
        headers: {
            'User-Agent': 'FlyteOS/1.1 (Open Source Flight Simulator)',
            'Referer': 'https://github.com/fulaite3-afk/flyteos-sim',
        },
        timeout: 10000,
    }, (proxyRes) => {
        // Handle redirects - 只允许同域名重定向
        if (proxyRes.statusCode === 301 || proxyRes.statusCode === 302) {
            try {
                const redirectUrl = new URL(proxyRes.headers.location, tileUrl);
                if (ALLOWED_HOSTS.has(redirectUrl.hostname)) {
                    proxyRequest(req, res, redirectUrl.href);
                } else {
                    res.statusCode = 403;
                    res.end('Forbidden: redirect to disallowed host');
                }
            } catch (e) {
                res.statusCode = 400;
                res.end('Bad redirect');
            }
            return;
        }
        
        const chunks = [];
        proxyRes.on('data', chunk => chunks.push(chunk));
        proxyRes.on('end', () => {
            const data = Buffer.concat(chunks);
            
            // Cache the tile (限制缓存文件大小 5MB)
            if (data.length <= 5 * 1024 * 1024) {
                fs.writeFileSync(cachePath, data);
            }
            
            res.setHeader('Content-Type', 'image/png');
            res.setHeader('X-Cache', 'MISS');
            setSecurityHeaders(res);
            res.end(data);
        });
    });
    
    proxyReq.on('error', (err) => {
        console.error('[ERROR] Proxy error (details hidden from client)');
        res.statusCode = 502;
        res.setHeader('Content-Type', 'text/plain');
        res.end('Bad Gateway');  // 不泄露内部错误信息
    });
    
    proxyReq.on('timeout', () => {
        proxyReq.destroy();
        res.statusCode = 504;
        res.setHeader('Content-Type', 'text/plain');
        res.end('Gateway Timeout');
    });
}

// Parse tile URL from query params
function getTileUrl(pathname) {
    // Pattern: /satellite/{z}/{x}/{y}.png
    const satMatch = pathname.match(/^\/satellite\/(\d+)\/(\d+)\/(\d+)\.png$/);
    if (satMatch) {
        const [, z, x, y] = satMatch;
        if (!validateTileParams(z, x, y)) return null;
        return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/${z}/${y}/${x}`;
    }
    
    // Pattern: /terrain/{z}/{x}/{y}.png
    const terrMatch = pathname.match(/^\/terrain\/(\d+)\/(\d+)\/(\d+)\.png$/);
    if (terrMatch) {
        const [, z, x, y] = terrMatch;
        if (!validateTileParams(z, x, y)) return null;
        return `https://s3.amazonaws.com/elevation-tiles-prod/terrarium/${z}/${x}/${y}.png`;
    }
    
    return null;
}

// Create HTTP server
const server = http.createServer((req, res) => {
    // URL长度限制
    if (req.url.length > MAX_URL_LENGTH) {
        res.statusCode = 414;
        res.end('URI Too Long');
        return;
    }
    
    // 速率限制
    const clientIp = req.socket.remoteAddress;
    if (!checkRateLimit(clientIp)) {
        res.statusCode = 429;
        res.setHeader('Content-Type', 'text/plain');
        res.setHeader('Retry-After', '60');
        res.end('Too Many Requests');
        return;
    }
    
    const parsedUrl = url.parse(req.url);
    const pathname = parsedUrl.pathname;
    
    // CORS preflight - 仅允许同源和GitHub Pages
    if (req.method === 'OPTIONS') {
        const origin = req.headers.origin || '';
        // 仅允许同源和已知域名
        const allowedOrigins = [
            'http://localhost:7792',
            'http://localhost:7890',
            'https://fulaite3-afk.github.io',
        ];
        if (allowedOrigins.includes(origin)) {
            res.setHeader('Access-Control-Allow-Origin', origin);
        }
        res.setHeader('Access-Control-Allow-Methods', 'GET, OPTIONS');
        res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
        res.setHeader('Access-Control-Max-Age', '86400');
        res.statusCode = 204;
        res.end();
        return;
    }
    
    // Proxy tile requests
    if (pathname.startsWith('/satellite/') || pathname.startsWith('/terrain/')) {
        const tileUrl = getTileUrl(pathname);
        if (tileUrl) {
            proxyRequest(req, res, tileUrl);
            return;
        } else {
            res.statusCode = 400;
            res.setHeader('Content-Type', 'text/plain');
            res.end('Bad Request: invalid tile parameters');
            return;
        }
    }
    
    // Serve static files - 安全加固: 路径遍历防护
    if (pathname.startsWith('/static/')) {
        const staticRoot = path.resolve(path.join(__dirname, '..'));
        const relativePath = pathname.replace('/static/', '');
        const filePath = safePathResolve(staticRoot, relativePath);
        
        if (!filePath) {
            res.statusCode = 403;
            res.setHeader('Content-Type', 'text/plain');
            res.end('Forbidden');
            return;
        }
        
        // 扩展名白名单 - 不允许暴露源码
        const ext = path.extname(filePath);
        const allowedStaticExts = new Set(['.html', '.css', '.js', '.png', '.jpg', '.svg', '.json']);
        if (!allowedStaticExts.has(ext)) {
            res.statusCode = 403;
            res.setHeader('Content-Type', 'text/plain');
            res.end('Forbidden');
            return;
        }
        
        if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
            res.setHeader('Content-Type', MIME_TYPES[ext] || 'application/octet-stream');
            setSecurityHeaders(res);
            res.end(fs.readFileSync(filePath));
            return;
        }
    }
    
    // Root: serve flight_sim_3d.html
    if (pathname === '/' || pathname === '/index.html') {
        const htmlPath = path.join(__dirname, 'flight_sim_3d.html');
        if (fs.existsSync(htmlPath)) {
            res.setHeader('Content-Type', 'text/html; charset=utf-8');
            setSecurityHeaders(res);
            res.end(fs.readFileSync(htmlPath));
            return;
        }
    }
    
    // 404 - 不泄露路径信息
    res.statusCode = 404;
    res.setHeader('Content-Type', 'text/plain');
    res.end('Not Found');
});

server.listen(PORT, () => {
    console.log(`
╔═══════════════════════════════════════════════════╗
║     FlyteOS v1.1 Tile Proxy Server (Hardened)    ║
╠═══════════════════════════════════════════════════╣
║  Server running at: http://localhost:${PORT}           
║  Satellite tiles: ESRI World Imagery (free)      ║
║  Terrain tiles:    Mapzen Terrain AWS (free)     ║
║  Cache directory:  ${CACHE_DIR.replace(/\\/g, '/').padEnd(29)}║
║  [Security] Path traversal protection: ON        ║
║  [Security] Rate limiting: ${RATE_LIMIT_MAX}/min              ║
║  [Security] SSRF whitelist: ON                   ║
║  [Security] Input validation: ON                 ║
║  [Security] CORS: restricted                     ║
║  [Security] SHA-256 cache hashing                ║
╚═══════════════════════════════════════════════════╝
    `);
});

server.on('error', (err) => {
    if (err.code === 'EADDRINUSE') {
        console.error('Port ' + PORT + ' is already in use. Please stop other servers.');
    } else {
        console.error('Server error:', err.message);
    }
    process.exit(1);
});
