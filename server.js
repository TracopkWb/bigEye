const express = require("express");
const http = require("http");
const WebSocket = require("ws");
const { spawn } = require("child_process");
const path = require("path");

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

app.use(express.static(path.join(__dirname, "public")));

// Serve main page
app.get("/", (req, res) => {
  res.sendFile(path.join(__dirname, "public", "index.html"));
});

// Camera RTSP Config
const CAMERA_RTSP =
  "rtsp://bigEye:traHiLook1@10.0.0.210:554/Streaming/Channels/101";

wss.on("connection", (ws) => {
  console.log("[Signaling] Browser connected via WebSocket");

  // Spawn GStreamer pipeline for WebRTC
  // Disables hardware decoder (GST_PLUGIN_FEATURE_RANK) and streams raw RTSP H.264
  const gstEnv = Object.assign({}, process.env, {
    GST_PLUGIN_FEATURE_RANK: "v4l2h264dec:NONE",
  });

  const gstArgs = [
    "rtspsrc",
    `location=${CAMERA_RTSP}`,
    "protocols=udp",
    "buffer-mode=0",
    "latency=100",
    "!",
    "rtph264depay",
    "!",
    "h264parse",
    "!",
    "rtph264pay",
    "config-interval=1",
    "pt=96",
    "!",
    "webrtcbin",
    "name=webstream",
  ];

  console.log("[GStreamer] Starting WebRTC pipeline...");

  ws.on("message", (message) => {
    try {
      const data = JSON.parse(message);
      console.log("[Signaling] Received from browser:", data.type);

      // Handle WebRTC signaling exchange here (SDP / ICE)
    } catch (err) {
      console.error("[Error] Parsing WS message:", err);
    }
  });

  ws.on("close", () => {
    console.log("[Signaling] Client disconnected");
  });
});

const PORT = 3000;
server.listen(PORT, () => {
  console.log(`Server running at http://localhost:${PORT}`);
});
