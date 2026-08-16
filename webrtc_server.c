#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define RTSP_URL "rtsp://bigEye:traHiLook1@10.0.0.210:554/Streaming/Channels/101"

typedef struct {
    GstElement *pipeline;
    GstElement *webrtc;
    SoupWebsocketConnection *ws;
} AppState;

static AppState app_state;

// Helper to send JSON messages over WebSocket
static void send_ws_json(SoupWebsocketConnection *ws, JsonNode *root_node) {
    if (!ws) return;
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root_node);
    gchar *data = json_generator_to_data(gen, NULL);
    soup_websocket_connection_send_text(ws, data);
    g_free(data);
    g_object_unref(gen);
}

// Monitor pipeline errors on the GStreamer Bus
static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &err, &debug);
            g_printerr("[GStreamer Pipeline Error] From %s: %s\n", GST_OBJECT_NAME(msg->src), err->message);
            g_printerr("[GStreamer Debug Info] %s\n", debug ? debug : "None");
            g_clear_error(&err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_WARNING: {
            GError *warn = NULL;
            gchar *debug = NULL;
            gst_message_parse_warning(msg, &warn, &debug);
            g_print("[GStreamer Warning] From %s: %s\n", GST_OBJECT_NAME(msg->src), warn->message);
            g_clear_error(&warn);
            g_free(debug);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

// Dynamically link RTSP video stream to depayloader
static void on_rtspsrc_pad_added(GstElement *src, GstPad *new_pad, gpointer user_data) {
    GstElement *depay = GST_ELEMENT(user_data);
    GstPad *sink_pad = gst_element_get_static_pad(depay, "sink");

    if (!sink_pad) return;

    if (gst_pad_is_linked(sink_pad)) {
        gst_object_unref(sink_pad);
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(new_pad);
    if (!caps) {
        caps = gst_pad_query_caps(new_pad, NULL);
    }

    if (caps) {
        GstStructure *str = gst_caps_get_structure(caps, 0);
        const gchar *media_type = gst_structure_get_string(str, "media");

        if (g_strcmp0(media_type, "video") == 0) {
            if (gst_pad_link(new_pad, sink_pad) == GST_PAD_LINK_OK) {
                g_print("[GStreamer] RTSP video pad linked successfully\n");
            } else {
                g_printerr("[GStreamer Error] Failed to link RTSP video pad\n");
            }
        }
        gst_caps_unref(caps);
    }
    gst_object_unref(sink_pad);
}

// Callback when GStreamer creates an Answer
static void on_answer_created(GstPromise *promise, gpointer user_data) {
    if (!app_state.webrtc || !app_state.ws) return;

    GstWebRTCSessionDescription *answer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);

    if (reply) {
        gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    }

    if (!answer) {
        g_printerr("[Error] Failed to generate Answer SDP from webrtcbin\n");
        return;
    }

    g_print("[Signaling] SDP Answer generated successfully\n");

    // Set local description in webrtcbin
    GstPromise *local_promise = gst_promise_new();
    g_signal_emit_by_name(app_state.webrtc, "set-local-description", answer, local_promise);
    gst_promise_wait(local_promise);
    gst_promise_unref(local_promise);

    // Send Answer SDP back to browser
    gchar *sdp_text = gst_sdp_message_as_text(answer->sdp);
    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "type", "answer");
    json_object_set_string_member(root_obj, "sdp", sdp_text);
    
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root_obj);
    
    send_ws_json(app_state.ws, node);
    g_print("[Signaling] Sent SDP Answer to browser\n");
    
    json_node_free(node);
    json_object_unref(root_obj);
    g_free(sdp_text);
    gst_webrtc_session_description_free(answer);
}

// Callback when GStreamer discovers an ICE Candidate
static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data) {
    if (!app_state.ws) return;

    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "type", "candidate");
    
    JsonObject *cand_obj = json_object_new();
    json_object_set_string_member(cand_obj, "candidate", candidate);
    json_object_set_int_member(cand_obj, "sdpMLineIndex", mlineindex);
    
    json_object_set_object_member(root_obj, "candidate", cand_obj);
    
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root_obj);
    
    send_ws_json(app_state.ws, node);
    json_node_free(node);
    json_object_unref(root_obj);
}

// Process WebSocket messages
static void on_ws_message(SoupWebsocketConnection *ws, gint type, GBytes *message, gpointer user_data) {
    if (!app_state.webrtc) {
        g_printerr("[Error] WebRTC pipeline element is NULL\n");
        return;
    }

    // Verify element state before setting SDP
    GstState current_state, pending_state;
    gst_element_get_state(app_state.webrtc, &current_state, &pending_state, 0);
    if (current_state < GST_STATE_READY) {
        g_printerr("[Error] Cannot process SDP. webrtcbin is not ready (State: %s)\n",
                   gst_element_state_get_name(current_state));
        return;
    }

    gsize size;
    const gchar *data = g_bytes_get_data(message, &size);
    
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, size, NULL)) {
        g_object_unref(parser);
        return;
    }

    JsonObject *root = json_node_get_object(json_parser_get_root(parser));
    const gchar *msg_type = json_object_get_string_member(root, "type");

    if (g_strcmp0(msg_type, "offer") == 0) {
        g_print("[Signaling] Received SDP Offer from browser\n");
        const gchar *sdp_str = json_object_get_string_member(root, "sdp");

        GstSDPMessage *sdp = NULL;
        if (gst_sdp_message_new(&sdp) != GST_SDP_OK || 
            gst_sdp_message_parse_buffer((guint8 *)sdp_str, strlen(sdp_str), sdp) != GST_SDP_OK) {
            g_printerr("[Error] Failed to parse SDP Offer\n");
            g_object_unref(parser);
            return;
        }

        GstWebRTCSessionDescription *offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
        
        GstPromise *promise = gst_promise_new();
        g_signal_emit_by_name(app_state.webrtc, "set-remote-description", offer, promise);
        gst_promise_wait(promise);
        
        const GstStructure *reply = gst_promise_get_reply(promise);
        if (reply && gst_structure_has_field(reply, "error")) {
            const GError *err = NULL;
            gst_structure_get(reply, "error", G_TYPE_ERROR, &err, NULL);
            g_printerr("[Error] set-remote-description failed: %s\n", err ? err->message : "Unknown SDP error");
            gst_promise_unref(promise);
            gst_webrtc_session_description_free(offer);
            g_object_unref(parser);
            return;
        }
        gst_promise_unref(promise);
        g_print("[Signaling] Remote description set successfully\n");

        // Request Answer generation
        GstPromise *ans_promise = gst_promise_new_with_change_func(on_answer_created, NULL, NULL);
        g_signal_emit_by_name(app_state.webrtc, "create-answer", NULL, ans_promise);

        gst_webrtc_session_description_free(offer);
    } 
    else if (g_strcmp0(msg_type, "candidate") == 0) {
        JsonObject *cand_obj = json_object_get_object_member(root, "candidate");
        const gchar *candidate = json_object_get_string_member(cand_obj, "candidate");
        gint mlineindex = json_object_get_int_member(cand_obj, "sdpMLineIndex");

        g_signal_emit_by_name(app_state.webrtc, "add-ice-candidate", mlineindex, candidate);
    }

    g_object_unref(parser);
}

// Handle WebSocket initialization
static void on_ws_opened(SoupServer *server, SoupServerMessage *msg, const char *path, SoupWebsocketConnection *connection, gpointer user_data) {
    g_print("[Signaling] Browser connected via WebSocket\n");
    app_state.ws = connection;
    g_object_ref(connection);

    g_signal_connect(connection, "message", G_CALLBACK(on_ws_message), NULL);

    if (app_state.pipeline) {
        gst_element_set_state(app_state.pipeline, GST_STATE_NULL);
        gst_object_unref(app_state.pipeline);
        app_state.pipeline = NULL;
        app_state.webrtc = NULL;
    }

    GError *error = NULL;
    gchar *pipeline_str = g_strdup_printf(
        "webrtcbin name=sendrecv stun-server=stun://stun.l.google.com:19302 "
        "rtspsrc name=rtspsrc location=" RTSP_URL " protocols=tcp latency=100 "
        "rtph264depay name=depay ! h264parse ! "
        "rtph264pay config-interval=1 ! "
        "capsfilter caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=H264\" ! "
        "queue name=videoqueue"
    );

    app_state.pipeline = gst_parse_launch(pipeline_str, &error);
    g_free(pipeline_str);

    if (error) {
        g_printerr("[GStreamer Error] %s\n", error->message);
        g_error_free(error);
        return;
    }

    // Attach Bus Watch to log pipeline/RTSP errors
    GstBus *bus = gst_element_get_bus(app_state.pipeline);
    gst_bus_add_watch(bus, on_bus_message, NULL);
    gst_object_unref(bus);

    app_state.webrtc = gst_bin_get_by_name(GST_BIN(app_state.pipeline), "sendrecv");
    GstElement *rtspsrc = gst_bin_get_by_name(GST_BIN(app_state.pipeline), "rtspsrc");
    GstElement *depay = gst_bin_get_by_name(GST_BIN(app_state.pipeline), "depay");
    GstElement *videoqueue = gst_bin_get_by_name(GST_BIN(app_state.pipeline), "videoqueue");

    if (!app_state.webrtc || !videoqueue) {
        g_printerr("[GStreamer Error] Missing pipeline elements\n");
        return;
    }

    if (rtspsrc && depay) {
        g_signal_connect(rtspsrc, "pad-added", G_CALLBACK(on_rtspsrc_pad_added), depay);
        gst_object_unref(rtspsrc);
        gst_object_unref(depay);
    }

    // Explicitly add video transceiver to webrtcbin
    GstCaps *caps = gst_caps_new_simple("application/x-rtp",
        "media", G_TYPE_STRING, "video",
        "clock-rate", G_TYPE_INT, 90000,
        "encoding-name", G_TYPE_STRING, "H264",
        NULL);
    GstWebRTCRTPTransceiver *trans = NULL;
    g_signal_emit_by_name(app_state.webrtc, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, caps, &trans);
    if (trans) gst_object_unref(trans);
    gst_caps_unref(caps);

    // Link video queue to webrtcbin
    GstPad *srcpad = gst_element_get_static_pad(videoqueue, "src");
    GstPad *sinkpad = gst_element_request_pad_simple(app_state.webrtc, "sink_%u");
    if (srcpad && sinkpad) {
        gst_pad_link(srcpad, sinkpad);
        gst_object_unref(srcpad);
        gst_object_unref(sinkpad);
    }
    gst_object_unref(videoqueue);

    g_signal_connect(app_state.webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), NULL);

    // Transition to PLAYING and wait for state lock
    gst_element_set_state(app_state.pipeline, GST_STATE_PLAYING);
    gst_element_get_state(app_state.pipeline, NULL, NULL, 5 * GST_SECOND);
}

static void http_handler(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data) {
    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (!g_file_get_contents("index.html", &contents, &length, &error)) {
        if (error) { g_error_free(error); error = NULL; }
        if (!g_file_get_contents("public/index.html", &contents, &length, &error)) {
            soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
            if (error) g_error_free(error);
            return;
        }
    }

    soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_TAKE, contents, length);
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    SoupServer *server = soup_server_new(NULL, NULL);
    soup_server_add_handler(server, "/", http_handler, NULL, NULL);
    soup_server_add_websocket_handler(server, "/ws", NULL, NULL, on_ws_opened, NULL, NULL);

    GError *error = NULL;
    soup_server_listen_all(server, 8080, 0, &error);
    if (error) {
        g_printerr("Failed to start server: %s\n", error->message);
        return 1;
    }

    g_print("C WebRTC Server running on http://0.0.0.0:8080/\n");

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    return 0;
}