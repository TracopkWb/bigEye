const express = require('express');
const WebSocket = require('ws');
const http = require('http');

const app = express();
app.use(express.static(__dirname));

const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

wss.on('connection', (ws) => {
    console.log('[Signaling] Client connected via WebSocket');

    ws.on('message', (message) => {
        const data = JSON.parse(message);
        // Standard JS object parsing instead of json-glib structures
        console.log('[Signaling] Received:', data.type);
    });
});

server.listen(8080, () => {
    console.log('Node.js WebRTC server running on http://localhost:8080');
});