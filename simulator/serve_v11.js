// FlyteOS static server - serves v1.2 by default
const http = require('http');
const fs   = require('fs');
const path = require('path');

const PORT = 7792;
const DIR  = __dirname;

const MIME = {
    '.html':'text/html; charset=utf-8',
    '.js'  :'application/javascript',
    '.css' :'text/css',
    '.png' :'image/png',
    '.jpg' :'image/jpeg',
};

// Route map
const ROUTES = {
    '/'         : 'flight_sim_v12.html',   // v1.2 实景版（默认）
    '/v12'      : 'flight_sim_v12.html',
    '/v11'      : 'flight_sim_3d.html',
    '/v2'       : 'flight_sim_2.html',
    '/v2cn'     : 'flight_sim_2_cn.html',
};

const server = http.createServer((req, res) => {
    const url = req.url.split('?')[0];

    // 瓦片代理转发
    if(url.startsWith('/satellite/') || url.startsWith('/terrain/')){
        const pr = http.request({hostname:'localhost',port:7890,path:url,method:'GET'},(pr2)=>{
            res.writeHead(pr2.statusCode, pr2.headers);
            pr2.pipe(res);
        });
        pr.on('error', () => { res.writeHead(502); res.end('Proxy error'); });
        req.pipe(pr);
        return;
    }

    const file = ROUTES[url] ? path.join(DIR, ROUTES[url]) : path.join(DIR, url);
    const ext  = path.extname(file);

    if(fs.existsSync(file)){
        res.setHeader('Content-Type', MIME[ext] || 'application/octet-stream');
        res.setHeader('Access-Control-Allow-Origin','*');
        res.end(fs.readFileSync(file));
    } else {
        res.writeHead(404);
        res.end(`Not found: ${url}`);
    }
});

server.listen(PORT, () => {
    console.log(`FlyteOS Server  http://localhost:${PORT}`);
    console.log(`  / (v1.2)      http://localhost:${PORT}/`);
    console.log(`  /v11          http://localhost:${PORT}/v11`);
    console.log(`  /v2cn         http://localhost:${PORT}/v2cn`);
});
