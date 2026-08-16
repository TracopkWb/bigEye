const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');

const app = express();
const server = http.createServer(app);

// Initialize the WebSocket Server instance correctly
const wss = new WebSocket.Server({ server });

app.use(express.static(path.join(__dirname, 'public')));

app.get('/', (req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

wss.on('connection', (ws) => {
    console.log('[Signaling] Browser connected via WebSocket');

    ws.on('message', (message) => {
        try {
            const data = JSON.parse(message);
            console.log('[Signaling] Received from browser:', data.type);
        } catch (err) {
            console.error('[Error] Parsing WS message:', err);
        }
    });

    ws.on('close', () => {
        console.log('[Signaling] Client disconnected');
    });
});

const PORT = 1234;
server.listen(PORT, () => {
    console.log(`Server running at http://localhost:${PORT}`);
});