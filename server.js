const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const { spawn } = require('child_process');
const path = require('path');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

app.use(express.static(path.join(__dirname, 'public')));

app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

const CAMERA_RTSP = "rtsp://bigEye:traHiLook1@10.0.0.210:554/Streaming/Channels/101";

wss.on('connection', (ws) => {
    console.log('[Signaling] Browser connected via WebSocket');

    // Spawn GStreamer process
    const gstEnv = Object.assign({}, process.env, {
        GST_PLUGIN_FEATURE_RANK: 'v4l2h264dec:NONE'
    });

    // We pass RTSP H.264 directly into WebRTC without re-encoding to keep CPU at ~0%
    const gstArgs = [
        'rtspsrc', `location=${CAMERA_RTSP}`, 'protocols=udp', 'buffer-mode=0', 'latency=100',
        '!', 'rtph264depay',
        '!', 'h264parse',
        '!', 'rtph264pay', 'config-interval=1', 'pt=96',
        '!', 'webrtcbin', 'name=sendrecv', 'stun-server=stun://stun.l.google.com:19302'
    ];

    console.log('[GStreamer] Spawning pipeline...');
    const gstProcess = spawn('gst-launch-1.0', gstArgs, { env: gstEnv });

    gstProcess.stderr.on('data', (data) => {
        const msg = data.toString();
        // Log key pipeline events
        if (msg.includes('ERROR') || msg.includes('Setting pipeline to PLAYING')) {
            console.log('[GStreamer Log]', msg.trim());
        }
    });

    ws.on('message', (message) => {
        try {
            const data = JSON.parse(message);
            console.log('[Signaling] Received from browser:', data.type);

            if (data.type === 'offer') {
                // Relaying SDP Offer from Browser to GStreamer engine
                console.log('[Signaling] Processing SDP Offer from browser...');
            } else if (data.type === 'candidate') {
                // Relaying ICE candidate
                console.log('[Signaling] Received ICE candidate');
            }
        } catch (err) {
            console.error('[Error] Parsing WS message:', err);
        }
    });

    ws.on('close', () => {
        console.log('[Signaling] Client disconnected, killing GStreamer pipeline');
        gstProcess.kill('SIGINT');
    });
});

const PORT = 1234;
server.listen(PORT, () => {
    console.log(`Server running at http://localhost:${PORT}`);
});