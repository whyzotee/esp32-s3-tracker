#pragma once

namespace DebugWeb {
// No styles, external assets, frameworks or device-control endpoints.
constexpr char page[] = R"HTML(<!doctype html>
<html lang="en">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Tracker debug</title></head>
<body>
<h1>Tracker debug</h1>
<p>Read only. Debug mode simulates report sleep; Wi-Fi stays connected.
Power-off and End Trip still shut the device down.</p>
<p>Events mirror firmware Serial output (J1/J4). Latest 64 line chunks; ROM/library logs are not captured.
Location appears after telemetry is prepared.</p>
<button id="refresh" type="button">Refresh</button>
<label><input id="auto" type="checkbox" checked>Auto refresh (1 second)</label>
<p id="connection">Connecting...</p>
<h2>Status</h2><pre id="status"></pre>
<h2>Events (oldest first)</h2><pre id="events"></pre>
<script>
let busy = false;
async function refresh() {
  if (busy) return;
  busy = true;
  const abort = new AbortController();
  const timeout = setTimeout(() => abort.abort(), 4000);
  try {
    const response = await fetch('/api/status', {cache: 'no-store', signal: abort.signal});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const data = await response.json();
    document.getElementById('status').textContent = JSON.stringify(data.status, null, 2);
    document.getElementById('events').textContent = data.events.map(
      event => '[' + event.ms + ' ms] ' + event.message).join('\n');
    document.getElementById('connection').textContent = 'Updated ' + new Date().toLocaleTimeString();
  } catch (error) {
    document.getElementById('connection').textContent =
      'Disconnected or request timed out. Check tracker Wi-Fi. ' + error.message;
  } finally {
    clearTimeout(timeout);
    busy = false;
  }
}
document.getElementById('refresh').addEventListener('click', refresh);
setInterval(() => { if (document.getElementById('auto').checked) refresh(); }, 1000);
refresh();
</script>
</body></html>)HTML";
}
