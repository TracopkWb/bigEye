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
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root_node);
    gchar *data = json_generator_to_data(gen, NULL);
    soup_websocket_connection_send_text(ws, data);
    g_free(data);
    g_object_unref(gen);
}

// Callback when GStreamer creates an Answer
static void on_answer_created(GstPromise *promise, gpointer user_data) {
    GstWebRTCSessionDescription *answer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
    gst_promise_unref(promise);

    // Set local description in webrtcbin
    GstPromise *local_promise = gst_promise_new();
    g_signal_emit_by_name(app_state.webrtc, "set-local-description", answer, local_promise);
    gst_promise_unref(local_promise);

    // Send Answer SDP back to browser via WebSocket
    gchar *sdp_text = gst_sdp_message_as_text(answer->sdp);
    
    JsonObject *root_obj = json_object_new();
    json_object_set_string_member(root_obj, "type", "answer");
    json_object_set_string_member(root_obj, "sdp", sdp_text);
    
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root_obj);
    
    send_ws_json(app_state.ws, node);
    
    json_node_free(node);
    json_object_unref(root_obj);
    g_free(sdp_text);
    gst_webrtc_session_description_free(answer);
}

// Callback when GStreamer generates an ICE Candidate
static void on_ice_candidate(GstElement *webrtc, guint mlineindex, gchar *candidate, gpointer user_data) {
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

// Process incoming WebSocket messages from browser
static void on_ws_message(SoupWebsocketConnection *ws, gint type, GBytes *message, gpointer user_data) {
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

        GstSDPMessage *sdp;
        gst_sdp_message_new(&sdp);
        gst_sdp_message_parse_buffer((guint8 *)sdp_str, strlen(sdp_str), sdp);

        GstWebRTCSessionDescription *offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
        
        GstPromise *promise = gst_promise_new();
        g_signal_emit_by_name(app_state.webrtc, "set-remote-description", offer, promise);
        gst_promise_unref(promise);

        // Ask webrtcbin to generate Answer
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

// Handle WebSocket connection initialization
static void on_ws_opened(SoupServer *server, SoupServerMessage *msg, const char *path, SoupWebsocketConnection *connection, gpointer user_data) {
    g_print("[Signaling] Browser connected via WebSocket\n");
    app_state.ws = connection;
    g_object_ref(connection);

    g_signal_connect(connection, "message", G_CALLBACK(on_ws_message), NULL);

    // Build GStreamer pipeline
    GError *error = NULL;
    gchar *pipeline_str = g_strdup_printf(
        "rtspsrc location=" RTSP_URL " protocols=udp buffer-mode=0 latency=100 ! "
        "rtph264depay ! h264parse ! rtph264pay config-interval=1 pt=96 ! "
        "webrtcbin name=sendrecv stun-server=stun://stun.l.google.com:19302"
    );

    app_state.pipeline = gst_parse_launch(pipeline_str, &error);
    g_free(pipeline_str);

    if (error) {
        g_printerr("[GStreamer Error] %s\n", error->message);
        g_error_free(error);
        return;
    }

    app_state.webrtc = gst_bin_get_by_name(GST_BIN(app_state.pipeline), "sendrecv");
    g_signal_connect(app_state.webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), NULL);

    // Start streaming pipeline
    gst_element_set_state(app_state.pipeline, GST_STATE_PLAYING);
}

// HTTP callback to serve index.html directly from C
static void http_handler(SoupServer *server, SoupServerMessage *msg, const char *path, GHashTable *query, gpointer user_data) {
    if (g_strcmp0(path, "/") != 0) {
        soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }

    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (g_file_get_contents("index.html", &contents, &length, &error)) {
        soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_TAKE, contents, length);
        soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    } else {
        g_printerr("Could not load index.html: %s\n", error ? error->message : "File not found");
        soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
        if (error) g_error_free(error);
    }
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    // Force software decoding rank rule programmatically
    GstPluginFeature *feature = gst_registry_find_feature(gst_registry_get(), "v4l2h264dec", GST_TYPE_ELEMENT_FACTORY);
    if (feature) {
        gst_plugin_feature_set_rank(feature, GST_RANK_NONE);
        gst_object_unref(feature);
    }

    // SoupServer *server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "webrtc-c-server", NULL);
    SoupServer *server = soup_server_new(NULL, NULL);
    
    // Serve HTML static file on /
    soup_server_add_handler(server, "/", http_handler, NULL, NULL);

    // Serve WebSockets on /ws
    soup_server_add_websocket_handler(server, "/ws", NULL, NULL, on_ws_opened, NULL, NULL);

    GError *error = NULL;
    soup_server_listen_all(server, 8080, 0, &error);
    if (error) {
        g_printerr("Failed to start server: %s\n", error->message);
        return 1;
    }

    g_print("C WebRTC Server running on ws://0.0.0.0:8080/ws\n");

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    return 0;
}