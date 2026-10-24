const { createRequire } = require('module');
const fs = require('fs');
const os = require('os');
const path = require('path');

function resolvePatchright() {
  const roots = [];
  const home = os.homedir();
  for (const base of [
    path.join(home, '.vscode-server', 'extensions'),
    path.join(home, '.vscode', 'extensions')
  ]) {
    if (!fs.existsSync(base)) continue;
    const dirs = fs.readdirSync(base)
      .filter(d => d.startsWith('danielsanmedium.dscodegpt-'))
      .sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' }));
    const newest = dirs[dirs.length - 1];
    if (newest) roots.push(path.join(base, newest, 'standalone') + path.sep);
  }
  roots.push(process.cwd() + path.sep);

  for (const root of roots) {
    try {
      const mod = createRequire(root)('patchright');
      const chromium = mod?.chromium ?? mod?.default?.chromium;
      if (chromium) return chromium;
    } catch (e) { }
  }
  // Try standard playwright
  try {
    const pw = require('playwright');
    if (pw?.chromium) return pw.chromium;
  } catch (e) { }

  return null;
}

async function extractVideo(targetUrl) {
  const chromium = resolvePatchright();
  if (!chromium) {
    return { ok: false, error: 'Neither patchright nor playwright chromium could be resolved.' };
  }

  let browser;
  try {
    browser = await chromium.launch({
      headless: true,
      channel: 'chromium',
      args: ['--no-sandbox', '--disable-dev-shm-usage', '--enable-unsafe-swiftshader']
    });
  } catch (err) {
    try {
      browser = await chromium.launch({
        headless: true,
        args: ['--no-sandbox', '--disable-dev-shm-usage', '--enable-unsafe-swiftshader']
      });
    } catch (err2) {
      return { ok: false, error: 'Failed to launch browser: ' + err2.message };
    }
  }

  try {
    const ctx = await browser.newContext({
      locale: 'en-US',
      userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36'
    });
    const page = await ctx.newPage();

    let capturedM3u8 = null;
    let capturedReferer = null;
    let capturedOrigin = null;

    page.on('request', req => {
      const u = req.url();
      if ((u.includes('.m3u8') || u.includes('/master') || u.includes('/playlist')) && !capturedM3u8) {
        // Ignore analytics / tracking
        if (!u.includes('ping') && !u.includes('analytics') && !u.includes('beacon')) {
          capturedM3u8 = u;
          const headers = req.headers();
          capturedReferer = headers['referer'] || '';
          capturedOrigin = headers['origin'] || '';
        }
      }
    });

    await page.goto(targetUrl, { waitUntil: 'domcontentloaded', timeout: 30000 });

    let pageTitle = '';
    try {
      pageTitle = await page.title();
    } catch (e) { }

    let animeTitle = '';
    try {
      animeTitle = await page.evaluate(() => {
        const og = document.querySelector('meta[property="og:title"]');
        if (og && og.content) return og.content;
        const breadcrumbLinks = Array.from(document.querySelectorAll('a[href*="/watch/"], nav a, ol li a'));
        for (const a of breadcrumbLinks) {
          const t = a.innerText.trim();
          if (t && t !== 'Home' && t !== 'TV' && t !== 'Movie' && t !== 'Enma' && !t.includes('Episode')) {
            return t;
          }
        }
        return '';
      });
    } catch (e) { }

    // Deduce clean title from targetUrl if needed
    let cleanTitle = animeTitle;
    try {
      const parsed = new URL(targetUrl);
      const ep = parsed.searchParams.get('ep');
      const segments = parsed.pathname.split('/').filter(Boolean);
      let slug = segments[segments.length - 1] || 'video';
      slug = slug.replace(/-\d+$/, '').replace(/-/g, ' ');

      if (!cleanTitle || cleanTitle.toLowerCase().includes('enma') || cleanTitle.toLowerCase().includes('best hianime')) {
        cleanTitle = slug;
      } else if (cleanTitle.toLowerCase().includes('watch ') && cleanTitle.includes(' Episode')) {
        const m = cleanTitle.match(/Watch\s+(.*?)\s+Episode/i);
        if (m && m[1]) cleanTitle = m[1];
      }
      if (ep) {
        cleanTitle = cleanTitle + ' - Ep ' + ep;
      }
    } catch (e) { }

    // Wait up to 15 seconds for m3u8
    for (let i = 0; i < 30; i++) {
      if (capturedM3u8) break;
      await page.waitForTimeout(500);
    }

    // If still not captured, check <video> elements
    if (!capturedM3u8) {
      try {
        const vSrc = await page.evaluate(() => {
          const v = document.querySelector('video');
          if (v) return v.src || (v.querySelector('source') ? v.querySelector('source').src : '');
          return '';
        });
        if (vSrc) {
          capturedM3u8 = vSrc;
        }
      } catch (e) { }
    }

    await browser.close();

    if (!capturedM3u8) {
      return { ok: false, error: 'No video stream captured within timeout.' };
    }

    return {
      ok: true,
      url: capturedM3u8,
      title: cleanTitle || animeTitle || pageTitle || 'video',
      referer: capturedReferer || targetUrl,
      origin: capturedOrigin || ''
    };
  } catch (err) {
    if (browser) await browser.close().catch(() => { });
    return { ok: false, error: err.message };
  }
}

const targetUrl = process.argv[2];
if (!targetUrl) {
  console.log(JSON.stringify({ ok: false, error: 'No URL provided' }));
  process.exit(1);
}

extractVideo(targetUrl).then(res => {
  console.log(JSON.stringify(res));
  process.exit(res.ok ? 0 : 1);
}).catch(err => {
  console.log(JSON.stringify({ ok: false, error: err.message }));
  process.exit(1);
});
