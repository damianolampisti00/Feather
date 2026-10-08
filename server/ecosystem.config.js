// pm2 config:  pm2 start ecosystem.config.js && pm2 save
module.exports = {
  apps: [
    {
      name: 'feather',
      script: 'server.js',
      cwd: __dirname,
      // Stopping the server closes its browser cleanly; give it a moment.
      kill_timeout: 8000,
      env: {
        PORT: '8080',
        HOST: '127.0.0.1',        // local only: cloudflared connects to it from this machine
        // BROWSER_PATH: '/usr/bin/chromium', // a specific Chromium/Chrome executable
        // BROWSER_CHANNEL: 'chrome',          // or an installed browser: chrome | msedge
        //                                     // (unset: msedge on Windows, else Playwright's Chromium)
        // FEATHER_TOKEN: '...',               // else one is generated into token.txt at first start
        // More settings: README.md, "Settings".
      },
    },
  ],
};
