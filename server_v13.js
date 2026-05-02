/**
 * FlyteOS v1.3 - 安全加固版服务器
 * 修复: 路径遍历(CWE-22)、安全头(CWE-693)、信息泄露(CWE-209)
 * 审计: 云中鹤 2026-05-02
 */

const http = require('http');
const fs   = require('fs');
const path = require('path');

const PORT = 7792;
const ROOT = path.resolve(path.join(__dirname, 'simulator'));

// 最大URL长度
const MAX_URL_LENGTH = 2048;

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js':   'application/javascript',
  '.css':  'text/css',
  '.json': 'application/json',
  '.png':  'image/png',
  '.jpg':  'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.gif':  'image/gif',
  '.svg':  'image/svg+xml',
  '.ico':  'image/x-icon',
};

// 允许访问的文件扩展名白名单
const ALLOWED_EXTENSIONS = new Set(Object.keys(MIME));

/**
 * 安全路径解析 - 防止路径遍历攻击
 * 确保解析后的路径在ROOT目录内
 */
function safePathResolve(root, requestPath) {
  // 解码URL编码
  let decoded = decodeURIComponent(requestPath);
  // 去掉前导斜杠，防止被当作绝对路径
  if (decoded.startsWith('/')) decoded = decoded.slice(1);
  // 规范化路径，消除 ../ 和 ./ 
  const resolved = path.resolve(root, decoded);
  // 确保解析后的路径以ROOT开头
  if (!resolved.startsWith(root + path.sep) && resolved !== root) {
    return null;
  }
  return resolved;
}

/**
 * 生成安全响应头
 */
function securityHeaders(res, contentType) {
  // 防止点击劫持
  res.setHeader('X-Frame-Options', 'DENY');
  // 防止MIME嗅探
  res.setHeader('X-Content-Type-Options', 'nosniff');
  // XSS保护
  res.setHeader('X-XSS-Protection', '1; mode=block');
  // 内容安全策略
  res.setHeader('Content-Security-Policy',
    "default-src 'self'; " +
    "script-src 'self' https://cdn.jsdelivr.net; " +
    "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; " +
    "font-src 'self' https://fonts.googleapis.com https://fonts.gstatic.com; " +
    "img-src 'self' data: blob: https://*.arcgisonline.com https://s3.amazonaws.com; " +
    "connect-src 'self' https://*.arcgisonline.com https://s3.amazonaws.com https://api.open-meteo.com"
  );
  // 引用策略
  res.setHeader('Referrer-Policy', 'strict-origin-when-cross-origin');
  // 权限策略
  res.setHeader('Permissions-Policy', 'camera=(), microphone=(), geolocation=()');
}

http.createServer((req, res) => {
  // URL长度限制
  if (req.url.length > MAX_URL_LENGTH) {
    res.writeHead(414, { 'Content-Type': 'text/plain' });
    res.end('URI Too Long');
    return;
  }

  let urlPath = req.url.split('?')[0];
  if (urlPath === '/') urlPath = '/flight_sim_v13.html';

  // 安全路径解析
  const filePath = safePathResolve(ROOT, urlPath);
  if (!filePath) {
    // 路径遍历攻击 - 不泄露路径信息
    res.writeHead(403, { 'Content-Type': 'text/plain' });
    res.end('Forbidden');
    return;
  }

  // 扩展名白名单检查
  const ext = path.extname(filePath);
  if (!ALLOWED_EXTENSIONS.has(ext)) {
    res.writeHead(403, { 'Content-Type': 'text/plain' });
    res.end('Forbidden');
    return;
  }

  const contentType = MIME[ext] || 'application/octet-stream';

  fs.readFile(filePath, (err, data) => {
    if (err) {
      // 不泄露具体路径信息
      res.writeHead(404, { 'Content-Type': 'text/plain' });
      res.end('Not Found');
      return;
    }
    securityHeaders(res, contentType);
    res.writeHead(200, { 'Content-Type': contentType });
    res.end(data);
  });
}).listen(PORT, () => {
  console.log(`FlyteOS v1.3 server running at http://localhost:${PORT}`);
  console.log('  [Security] Path traversal protection: ON');
  console.log('  [Security] Security headers: ON');
  console.log('  [Security] Extension whitelist: ON');
  console.log('  [Security] URL length limit: ' + MAX_URL_LENGTH);
});
