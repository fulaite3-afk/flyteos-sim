// Simple static server for FlyteOS v1.1
const http = require('http');
const fs = require('path');
const path = require('path');

const PORT = 7792;
const DIR = __dirname;

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'application/javascript',
    '.css': 'text/css',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
};

const server = http.createServer((req, res) => {
    let filePath = path.join(DIR, req.url === '/' ? 'flight_sim_3d.html' : req.url);
    
    // Proxy tile requests to our tile proxy
    if (req.url.startsWith('/satellite/') || req.url.startsWith('/terrain/')) {
        const proxyPort = 7890;
        const proxyReq = http.request({
            hostname: 'localhost',
            port: proxyPort,
            path: req.url,
            method: 'GET',
        }, (proxyRes) => {
            res.writeHead(proxyRes.statusCode, proxyRes.headers);
            proxyRes.pipe(res);
        });
        req.pipe(proxyReq);
        return;
    }
    
    const ext = path.extname(filePath);
    if (fs.existsSync(filePath)) {
        res.setHeader('Content-Type', MIME[ext] || 'application/octet-stream');
        res.setHeader('Access-Control-Allow-Origin', '*');
        res.end(fs.readFileSync(filePath));
    } else {
        res.writeHead(404);
        res.end('Not found: ' + req.url);
    }
});

server.listen(PORT, () => {
    console.log(`FlyteOS v1.1 running at http://localhost:${PORT}`);
});
