#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

typedef struct {
    GstElement *pipeline;
    GstElement *webrtc;
    SoupWebsocketConnection *ws;
} AppState;

static AppState app_state = {NULL, NULL, NULL};

static void send_signaling_json(JsonObject *root) {
    if (!app_state.ws) return;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, node);
    gchar *data = json_generator_to_data(gen, NULL);

    soup_websocket_connection_send_text(app_state.ws, data);

    g_free(data);
    g_object_unref(gen);
    json_node_free(node);
}

static void on_ice_candidate_cb(GstElement *webrtc, guint mline_index, gchar *candidate, gpointer user_data) {
    JsonObject *root = json_object_new();
    JsonObject *ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mline_index);
    json_object_set_object_member(root, "ice", ice);

    g_print("[ICE] Sending candidate to browser (mline %u): %s\n", mline_index, candidate);
    send_signaling_json(root);
}

static void on_answer_created(GstPromise *promise, gpointer user_data) {
    GstWebRTCSessionDescription *answer = NULL;
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "answer", GST_WEBRTC_SESSION_DESCRIPTION_TYPE, &answer, NULL);
    gst_promise_unref(promise);

    promise = gst_promise_new();
    g_signal_emit_by_name(app_state.webrtc, "set-local-description", answer, promise);
    gst_promise_unref(promise);

    gchar *sdp_text = gst_sdp_message_as_text(answer->sdp);
    
    JsonObject *root = json_object_new();
    JsonObject *sdp = json_object_new();
    json_object_set_string_member(sdp, "type", "answer");
    json_object_set_string_member(sdp, "sdp", sdp_text);
    json_object_set_object_member(root, "sdp", sdp);

    g_print("[Signaling] SDP Answer generated and sent\n");
    send_signaling_json(root);

    g_free(sdp_text);
    gst_webrtc_session_description_free(answer);
}

static void on_offer_received(const gchar *sdp_text) {
    GstSDPMessage *sdp = NULL;
    gst_sdp_message_new(&sdp);
    gst_sdp_message_parse_buffer((guint8 *)sdp_text, strlen(sdp_text), sdp);

    GstWebRTCSessionDescription *offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    GstPromise *promise = gst_promise_new();
    g_signal_emit_by_name(app_state.webrtc, "set-remote-description", offer, promise);
    gst_promise_unref(promise);

    promise = gst_promise_new_with_change_func((GstPromiseChangeFunc)on_answer_created, NULL, NULL);
    g_signal_emit_by_name(app_state.webrtc, "create-answer", NULL, promise);
    gst_webrtc_session_description_free(offer);
}

static void on_ws_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type, GBytes *message, gpointer user_data) {
    if (!app_state.webrtc) return;

    gsize size;
    const gchar *data = g_bytes_get_data(message, &size);

    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, data, size, NULL)) {
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        
        if (json_object_has_member(root, "sdp")) {
            JsonObject *sdp = json_object_get_object_member(root, "sdp");
            const gchar *sdp_str = json_object_get_string_member(sdp, "sdp");
            g_print("[Signaling] Received SDP Offer from browser\n");
            on_offer_received(sdp_str);
        } else if (json_object_has_member(root, "ice")) {
            JsonObject *ice = json_object_get_object_member(root, "ice");
            const gchar *candidate = json_object_get_string_member(ice, "candidate");
            guint mline_index = json_object_get_int_member(ice, "sdpMLineIndex");
            g_signal_emit_by_name(app_state.webrtc, "add-ice-candidate", mline_index, candidate);
        }
    }
    g_object_unref(parser);
}

static void on_ws_opened(SoupServer *server, SoupWebsocketConnection *conn, const char *path, SoupClientContext *client, gpointer user_data) {
    g_print("[Signaling] Browser connected via WebSocket\n");
    app_state.ws = conn;
    g_signal_connect(conn, "message", G_CALLBACK(on_ws_message), NULL);

    gchar *pipeline_str = g_strdup_printf(
        "webrtcbin name=sendrecv stun-server=stun://stun.l.google.com:19302 "
        "rtspsrc name=rtspsrc location=\"rtsp://10.0.0.210:554/Streaming/Channels/102\" "
        "user-id=\"bigEye\" user-pw=\"traHiLook1\" protocols=tcp latency=200 ! "
        "rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! video/x-raw,format=I420 ! "
        "x264enc speed-preset=ultrafast tune=zerolatency key-int-max=15 ! "
        "video/x-h264,profile=constrained-baseline,stream-format=byte-stream,alignment=au ! "
        "rtph264pay config-interval=1 pt=96 aggregate-mode=zero-latency ! "
        "application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96 ! "
        "queue name=videoqueue");

    GError *error = NULL;
    app_state.pipeline = gst_parse_launch(pipeline_str, &error);
    g_free(pipeline_str);

    if (error) {
        g_printerr("Pipeline launch failed: %s\n", error->message);
        g_clear_error(&error);
        return;
    }

    app_state.webrtc = gst_bin_get_by_name(GST_BIN(app_state.pipeline), "sendrecv");
    GstElement *queue = gst_bin_get_by_name(GST_BIN(app_state.pipeline), "videoqueue");

    g_signal_connect(app_state.webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate_cb), NULL);

    GstPad *srcpad = gst_element_get_static_pad(queue, "src");
    GstPad *sinkpad = gst_element_request_pad_simple(app_state.webrtc, "sink_%u");
    
    if (gst_pad_link(srcpad, sinkpad) == GST_PAD_LINK_OK) {
        g_print("[GStreamer] Successfully linked videoqueue to webrtcbin sink pad\n");
    }

    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);
    gst_object_unref(queue);

    gst_element_set_state(app_state.pipeline, GST_STATE_PLAYING);
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    SoupServer *server = soup_server_new("server-header", "webrtc-server", NULL);
    soup_server_add_websocket_handler(server, "/ws", NULL, NULL, on_ws_opened, NULL, NULL);
    
    soup_server_listen_all(server, 8080, 0, NULL);
    g_print("WebRTC Server running on http://0.0.0.0:8080/\n");

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    return 0;
}