#define GST_USE_UNSTABLE_API

#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include <string.h>

typedef struct
{
    GstElement *pipeline;
    GstElement *webrtc;
    SoupWebsocketConnection *ws;

} AppState;

static AppState app_state = {
    NULL,
    NULL,
    NULL};

/* ============================================================
 * Signaling
 * ============================================================ */

static void send_signaling_json(JsonObject *root)
{
    if (app_state.ws == NULL)
    {
        g_printerr("[Signaling] No WebSocket connection\n");
        return;
    }

    if (soup_websocket_connection_get_state(app_state.ws) !=
        SOUP_WEBSOCKET_STATE_OPEN)
    {
        g_printerr("[Signaling] WebSocket is not open\n");
        return;
    }

    JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, root);

    JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, node);

    gchar *data = json_generator_to_data(generator, NULL);

    soup_websocket_connection_send_text(
        app_state.ws,
        data);

    g_print("[Signaling] Sent: %s\n", data);

    g_free(data);
    g_object_unref(generator);
    json_node_free(node);
}

/* ============================================================
 * ICE
 * ============================================================ */

static void on_ice_candidate_cb(
    GstElement *webrtc,
    guint mline_index,
    gchar *candidate,
    gpointer user_data)
{
    g_print(
        "[ICE] Server generated candidate "
        "(mline %u): %s\n",
        mline_index,
        candidate);

    JsonObject *root = json_object_new();

    JsonObject *ice = json_object_new();

    json_object_set_string_member(
        ice,
        "candidate",
        candidate);

    json_object_set_int_member(
        ice,
        "sdpMLineIndex",
        mline_index);

    json_object_set_object_member(
        root,
        "ice",
        ice);

    send_signaling_json(root);

    json_object_unref(root);
}

/* ============================================================
 * SDP ANSWER
 * ============================================================ */

static void on_answer_created(
    GstPromise *promise,
    gpointer user_data)
{
    const GstStructure *reply =
        gst_promise_get_reply(promise);

    if (reply == NULL)
    {
        g_printerr(
            "[WebRTC] create-answer returned no reply\n");

        gst_promise_unref(promise);
        return;
    }

    GstWebRTCSessionDescription *answer = NULL;

    gboolean success = gst_structure_get(
        reply,
        "answer",
        GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
        &answer,
        NULL);

    gst_promise_unref(promise);

    if (!success || answer == NULL)
    {
        g_printerr(
            "[WebRTC] Failed to retrieve SDP answer\n");

        return;
    }

    g_print(
        "[WebRTC] SDP answer created successfully\n");

    /* --------------------------------------------------------
     * Set local description
     * -------------------------------------------------------- */

    GstPromise *local_promise =
        gst_promise_new();

    g_signal_emit_by_name(
        app_state.webrtc,
        "set-local-description",
        answer,
        local_promise);

    gst_promise_unref(local_promise);

    /* --------------------------------------------------------
     * Convert SDP to text
     * -------------------------------------------------------- */

    gchar *sdp_text =
        gst_sdp_message_as_text(answer->sdp);

    if (sdp_text == NULL)
    {
        g_printerr(
            "[WebRTC] Could not convert answer SDP to text\n");

        gst_webrtc_session_description_free(answer);
        return;
    }

    /* --------------------------------------------------------
     * Send SDP answer to browser
     * -------------------------------------------------------- */

    JsonObject *root =
        json_object_new();

    JsonObject *sdp =
        json_object_new();

    json_object_set_string_member(
        sdp,
        "type",
        "answer");

    json_object_set_string_member(
        sdp,
        "sdp",
        sdp_text);

    json_object_set_object_member(
        root,
        "sdp",
        sdp);

    g_print(
        "[Signaling] Sending SDP Answer to browser\n");

    send_signaling_json(root);

    json_object_unref(root);

    g_free(sdp_text);

    gst_webrtc_session_description_free(answer);
}

/* ============================================================
 * REMOTE SDP SET CALLBACK
 *
 * IMPORTANT:
 * create-answer must happen AFTER the remote SDP has
 * actually been set.
 * ============================================================ */

static void on_remote_description_set(
    GstPromise *promise,
    gpointer user_data)
{
    const GstStructure *reply =
        gst_promise_get_reply(promise);

    if (reply == NULL)
    {
        g_printerr(
            "[WebRTC] Failed to set remote description\n");

        gst_promise_unref(promise);
        return;
    }

    g_print(
        "[WebRTC] Remote SDP successfully set\n");

    gst_promise_unref(promise);

    /* --------------------------------------------------------
     * Now create the answer
     * -------------------------------------------------------- */

    GstPromise *answer_promise =
        gst_promise_new_with_change_func(
            (GstPromiseChangeFunc)on_answer_created,
            NULL,
            NULL);

    g_print(
        "[WebRTC] Creating SDP Answer...\n");

    g_signal_emit_by_name(
        app_state.webrtc,
        "create-answer",
        NULL,
        answer_promise);
}

/* ============================================================
 * SDP OFFER RECEIVED
 * ============================================================ */

static void on_offer_received(
    const gchar *sdp_text)
{
    if (sdp_text == NULL)
    {
        g_printerr(
            "[SDP] Received NULL SDP\n");

        return;
    }

    g_print(
        "[SDP] Parsing browser SDP offer...\n");

    /* --------------------------------------------------------
     * Parse SDP
     * -------------------------------------------------------- */

    GstSDPMessage *sdp = NULL;

    GstSDPResult result =
        gst_sdp_message_new_from_text(
            sdp_text,
            &sdp);

    if (result != GST_SDP_OK)
    {
        g_printerr(
            "[SDP] Failed to parse SDP offer\n");

        return;
    }

    /* --------------------------------------------------------
     * Create WebRTC offer object
     * -------------------------------------------------------- */

    GstWebRTCSessionDescription *offer =
        gst_webrtc_session_description_new(
            GST_WEBRTC_SDP_TYPE_OFFER,
            sdp);

    if (offer == NULL)
    {
        g_printerr(
            "[WebRTC] Failed to create offer object\n");

        gst_sdp_message_free(sdp);

        return;
    }

    /* --------------------------------------------------------
     * Set remote description
     *
     * DO NOT create the answer yet.
     * The callback will do that once this completes.
     * -------------------------------------------------------- */

    GstPromise *promise =
        gst_promise_new_with_change_func(
            (GstPromiseChangeFunc)
                on_remote_description_set,
            NULL,
            NULL);

    g_print(
        "[WebRTC] Setting remote SDP...\n");

    g_signal_emit_by_name(
        app_state.webrtc,
        "set-remote-description",
        offer,
        promise);

    gst_webrtc_session_description_free(offer);
}

/* ============================================================
 * WEBSOCKET MESSAGE
 * ============================================================ */

static void on_ws_message(
    SoupWebsocketConnection *conn,
    SoupWebsocketDataType type,
    GBytes *message,
    gpointer user_data)
{
    if (app_state.webrtc == NULL)
    {
        g_printerr(
            "[WebSocket] WebRTC element does not exist\n");

        return;
    }

    if (type != SOUP_WEBSOCKET_DATA_TEXT)
    {
        g_printerr(
            "[WebSocket] Received non-text message\n");

        return;
    }

    gsize size = 0;

    const gchar *data =
        g_bytes_get_data(
            message,
            &size);

    if (data == NULL || size == 0)
    {
        g_printerr(
            "[WebSocket] Empty message\n");

        return;
    }

    g_print(
        "[WebSocket] Received: %.*s\n",
        (int)size,
        data);

    /* --------------------------------------------------------
     * Parse JSON
     * -------------------------------------------------------- */

    JsonParser *parser =
        json_parser_new();

    GError *error = NULL;

    gboolean parsed =
        json_parser_load_from_data(
            parser,
            data,
            size,
            &error);

    if (!parsed)
    {
        g_printerr(
            "[JSON] Failed to parse message: %s\n",
            error ? error->message : "unknown error");

        g_clear_error(&error);
        g_object_unref(parser);

        return;
    }

    JsonNode *root_node =
        json_parser_get_root(parser);

    if (!JSON_NODE_HOLDS_OBJECT(root_node))
    {
        g_printerr(
            "[JSON] Root is not an object\n");

        g_object_unref(parser);

        return;
    }

    JsonObject *root =
        json_node_get_object(root_node);

    /* ========================================================
     * SDP
     * ======================================================== */

    if (json_object_has_member(root, "sdp"))
    {
        JsonObject *sdp =
            json_object_get_object_member(
                root,
                "sdp");

        if (sdp == NULL)
        {
            g_printerr(
                "[SDP] Invalid SDP object\n");

            g_object_unref(parser);
            return;
        }

        const gchar *type =
            json_object_get_string_member(
                sdp,
                "type");

        const gchar *sdp_str =
            json_object_get_string_member(
                sdp,
                "sdp");

        g_print(
            "[Signaling] Received SDP %s from browser\n",
            type ? type : "unknown");

        if (sdp_str != NULL)
        {
            on_offer_received(sdp_str);
        }
    }

    /* ========================================================
     * ICE
     * ======================================================== */

    else if (json_object_has_member(root, "ice"))
    {
        JsonObject *ice =
            json_object_get_object_member(
                root,
                "ice");

        if (ice == NULL)
        {
            g_printerr(
                "[ICE] Invalid ICE object\n");

            g_object_unref(parser);
            return;
        }

        const gchar *candidate =
            json_object_get_string_member(
                ice,
                "candidate");

        gint mline_index =
            json_object_get_int_member(
                ice,
                "sdpMLineIndex");

        if (candidate == NULL)
        {
            g_printerr(
                "[ICE] Candidate is NULL\n");

            g_object_unref(parser);
            return;
        }

        g_print(
            "[ICE] Browser candidate "
            "(mline %d): %s\n",
            mline_index,
            candidate);

        g_signal_emit_by_name(
            app_state.webrtc,
            "add-ice-candidate",
            mline_index,
            candidate);
    }

    else
    {
        g_printerr(
            "[WebSocket] Unknown signaling message\n");
    }

    g_object_unref(parser);
}

/* ============================================================
 * GSTREAMER BUS
 * ============================================================ */

static gboolean on_bus_message(
    GstBus *bus,
    GstMessage *message,
    gpointer user_data)
{
    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
    {
        GError *error = NULL;
        gchar *debug = NULL;

        gst_message_parse_error(
            message,
            &error,
            &debug);

        g_printerr(
            "\n[GStreamer ERROR]\n%s\n",
            error
                ? error->message
                : "unknown error");

        if (debug)
        {
            g_printerr(
                "[GStreamer DEBUG]\n%s\n",
                debug);
        }

        g_clear_error(&error);
        g_free(debug);

        break;
    }

    case GST_MESSAGE_WARNING:
    {
        GError *error = NULL;
        gchar *debug = NULL;

        gst_message_parse_warning(
            message,
            &error,
            &debug);

        g_printerr(
            "\n[GStreamer WARNING]\n%s\n",
            error
                ? error->message
                : "unknown warning");

        if (debug)
        {
            g_printerr(
                "[GStreamer DEBUG]\n%s\n",
                debug);
        }

        g_clear_error(&error);
        g_free(debug);

        break;
    }

    case GST_MESSAGE_EOS:
    {
        g_print(
            "[GStreamer] End of stream\n");

        break;
    }

    case GST_MESSAGE_STATE_CHANGED:
    {
        if (GST_MESSAGE_SRC(message) ==
            GST_OBJECT(app_state.pipeline))
        {
            GstState old_state;
            GstState new_state;
            GstState pending_state;

            gst_message_parse_state_changed(
                message,
                &old_state,
                &new_state,
                &pending_state);

            g_print(
                "[GStreamer] Pipeline state: %s -> %s\n",
                gst_element_state_get_name(old_state),
                gst_element_state_get_name(new_state));
        }

        break;
    }

    default:
        break;
    }

    return G_SOURCE_CONTINUE;
}

/* ============================================================
 * STOP PIPELINE
 * ============================================================ */

static void stop_pipeline(void)
{
    if (app_state.pipeline == NULL)
        return;

    g_print(
        "[GStreamer] Stopping existing pipeline...\n");

    gst_element_set_state(
        app_state.pipeline,
        GST_STATE_NULL);

    gst_object_unref(
        app_state.pipeline);

    app_state.pipeline = NULL;
    app_state.webrtc = NULL;

    g_print(
        "[GStreamer] Pipeline stopped\n");
}

/* ============================================================
 * WEBSOCKET OPENED
 * ============================================================ */

static void on_ws_opened(
    SoupServer *server,
    SoupServerMessage *msg,
    const char *path,
    SoupWebsocketConnection *conn,
    gpointer user_data)
{
    g_print(
        "\n==================================================\n");

    g_print(
        "[Signaling] Browser connected via WebSocket\n");

    g_print(
        "==================================================\n");

    /* --------------------------------------------------------
     * Stop previous session
     * -------------------------------------------------------- */

    stop_pipeline();

    /* --------------------------------------------------------
     * Store WebSocket
     * -------------------------------------------------------- */

    app_state.ws = conn;

    g_signal_connect(
        conn,
        "message",
        G_CALLBACK(on_ws_message),
        NULL);

    /* ========================================================
     * CREATE GSTREAMER PIPELINE
     * ======================================================== */

    const gchar *pipeline_description =
        "webrtcbin name=sendrecv "
        "stun-server=stun://stun.l.google.com:19302 "

        "rtspsrc "
        "name=rtspsrc "
        "location=\"rtsp://10.0.0.210:554/Streaming/Channels/102\" "
        "user-id=\"bigEye\" "
        "user-pw=\"traHiLook1\" "
        "protocols=tcp "
        "latency=200 "

        "! rtph264depay "
        "! h264parse "
        "! avdec_h264 "
        "! videoconvert "
        "! video/x-raw,format=I420 "
        "! x264enc "
        "speed-preset=ultrafast "
        "tune=zerolatency "
        "key-int-max=15 "
        "! video/x-h264,profile=constrained-baseline,stream-format=byte-stream,alignment=au "
        "! rtph264pay "
        "config-interval=1 "
        "pt=96 "
        "aggregate-mode=zero-latency "
        "! application/x-rtp,media=video,clock-rate=90000,encoding-name=H264,payload=96 "
        "! queue name=videoqueue";

    g_print(
        "[GStreamer] Creating pipeline...\n");

    GError *error = NULL;

    app_state.pipeline =
        gst_parse_launch(
            pipeline_description,
            &error);

    if (app_state.pipeline == NULL)
    {
        g_printerr(
            "[GStreamer] Pipeline creation failed\n");

        if (error)
        {
            g_printerr(
                "[GStreamer] %s\n",
                error->message);

            g_clear_error(&error);
        }

        return;
    }

    if (error)
    {
        g_printerr(
            "[GStreamer] Pipeline warning: %s\n",
            error->message);

        g_clear_error(&error);
    }

    /* --------------------------------------------------------
     * Add GStreamer bus watch
     * -------------------------------------------------------- */

    GstBus *bus =
        gst_element_get_bus(
            app_state.pipeline);

    gst_bus_add_watch(
        bus,
        on_bus_message,
        NULL);

    gst_object_unref(bus);

    /* --------------------------------------------------------
     * Get webrtcbin
     * -------------------------------------------------------- */

    app_state.webrtc =
        gst_bin_get_by_name(
            GST_BIN(app_state.pipeline),
            "sendrecv");

    if (app_state.webrtc == NULL)
    {
        g_printerr(
            "[WebRTC] Could not find webrtcbin\n");

        stop_pipeline();
        return;
    }

    /* --------------------------------------------------------
     * Get video queue
     * -------------------------------------------------------- */

    GstElement *queue =
        gst_bin_get_by_name(
            GST_BIN(app_state.pipeline),
            "videoqueue");

    if (queue == NULL)
    {
        g_printerr(
            "[GStreamer] Could not find videoqueue\n");

        stop_pipeline();
        return;
    }

    /* --------------------------------------------------------
     * ICE callback
     * -------------------------------------------------------- */

    g_signal_connect(
        app_state.webrtc,
        "on-ice-candidate",
        G_CALLBACK(on_ice_candidate_cb),
        NULL);

    /* ========================================================
     * LINK QUEUE -> WEBRTCBIN
     * ======================================================== */

    GstPad *srcpad =
        gst_element_get_static_pad(
            queue,
            "src");

    if (srcpad == NULL)
    {
        g_printerr(
            "[GStreamer] Could not get queue src pad\n");

        gst_object_unref(queue);
        stop_pipeline();
        return;
    }

    GstPad *sinkpad =
        gst_element_request_pad_simple(
            app_state.webrtc,
            "sink_%u");

    if (sinkpad == NULL)
    {
        g_printerr(
            "[WebRTC] Could not request webrtcbin sink pad\n");

        gst_object_unref(srcpad);
        gst_object_unref(queue);
        stop_pipeline();
        return;
    }

    GstPadLinkReturn link_result =
        gst_pad_link(
            srcpad,
            sinkpad);

    if (link_result == GST_PAD_LINK_OK)
    {
        g_print(
            "[GStreamer] Successfully linked "
            "videoqueue -> webrtcbin\n");
    }
    else
    {
        g_printerr(
            "[GStreamer] Failed to link "
            "videoqueue -> webrtcbin: %s\n",
            gst_pad_link_get_name(link_result));
    }

    gst_object_unref(srcpad);
    gst_object_unref(sinkpad);
    gst_object_unref(queue);

    /* --------------------------------------------------------
     * Start pipeline
     * -------------------------------------------------------- */

    g_print(
        "[GStreamer] Starting pipeline...\n");

    GstStateChangeReturn state_result =
        gst_element_set_state(
            app_state.pipeline,
            GST_STATE_PLAYING);

    if (state_result == GST_STATE_CHANGE_FAILURE)
    {
        g_printerr(
            "[GStreamer] Failed to start pipeline\n");

        stop_pipeline();
        return;
    }

    g_print(
        "[GStreamer] Pipeline is PLAYING\n");
}

/* ============================================================
 * WEBSOCKET CLOSED
 * ============================================================ */

static void on_ws_closed(
    SoupWebsocketConnection *conn,
    gpointer user_data)
{
    g_print(
        "[Signaling] Browser WebSocket closed\n");

    if (app_state.ws == conn)
    {
        app_state.ws = NULL;

        stop_pipeline();
    }
}

/* ============================================================
 * HTTP REQUEST
 * ============================================================ */

static void on_http_request(
    SoupServer *server,
    SoupServerMessage *msg,
    const char *path,
    GHashTable *query,
    gpointer user_data)
{
    if (g_strcmp0(path, "/") == 0 ||
        g_strcmp0(path, "/index.html") == 0)
    {
        gchar *contents = NULL;
        gsize length = 0;

        if (g_file_get_contents(
                "public/index.html",
                &contents,
                &length,
                NULL))
        {
            SoupMessageHeaders *headers =
                soup_server_message_get_response_headers(
                    msg);

            soup_message_headers_set_content_type(
                headers,
                "text/html",
                NULL);

            soup_server_message_set_response(
                msg,
                "text/html",
                SOUP_MEMORY_TAKE,
                contents,
                length);

            soup_server_message_set_status(
                msg,
                SOUP_STATUS_OK,
                NULL);
        }
        else
        {
            g_printerr(
                "[HTTP Error] Could not find "
                "'public/index.html'\n");

            soup_server_message_set_status(
                msg,
                SOUP_STATUS_NOT_FOUND,
                NULL);
        }

        return;
    }

    soup_server_message_set_status(
        msg,
        SOUP_STATUS_NOT_FOUND,
        NULL);
}

/* ============================================================
 * MAIN
 * ============================================================ */

int main(
    int argc,
    char *argv[])
{
    gst_init(
        &argc,
        &argv);

    g_print(
        "\n"
        "==================================================\n"
        " BigEye WebRTC Server\n"
        "==================================================\n");

    /* --------------------------------------------------------
     * Create HTTP/WebSocket server
     * -------------------------------------------------------- */

    SoupServer *server =
        soup_server_new(
            "server-header",
            "webrtc-server",
            NULL);

    if (server == NULL)
    {
        g_printerr(
            "[HTTP] Failed to create SoupServer\n");

        return 1;
    }

    /* --------------------------------------------------------
     * HTTP
     * -------------------------------------------------------- */

    soup_server_add_handler(
        server,
        "/",
        on_http_request,
        NULL,
        NULL);

    /* --------------------------------------------------------
     * WebSocket
     * -------------------------------------------------------- */

    soup_server_add_websocket_handler(
        server,
        "/ws",
        NULL,
        NULL,
        on_ws_opened,
        NULL,
        NULL);

    /* --------------------------------------------------------
     * Listen
     * -------------------------------------------------------- */

    GError *error = NULL;

    gboolean listening =
        soup_server_listen_all(
            server,
            8080,
            0,
            &error);

    if (!listening)
    {
        g_printerr(
            "[HTTP] Failed to listen on port 8080\n");

        if (error)
        {
            g_printerr(
                "[HTTP] %s\n",
                error->message);

            g_clear_error(&error);
        }

        g_object_unref(server);

        return 1;
    }

    g_print(
        "[HTTP] Server running on:\n"
        "       http://0.0.0.0:8080/\n");

    g_print(
        "[WebSocket] Signaling endpoint:\n"
        "       ws://0.0.0.0:8080/ws\n");

    /* --------------------------------------------------------
     * Main loop
     * -------------------------------------------------------- */

    GMainLoop *loop =
        g_main_loop_new(
            NULL,
            FALSE);

    g_main_loop_run(loop);

    /* --------------------------------------------------------
     * Cleanup
     * -------------------------------------------------------- */

    stop_pipeline();

    g_main_loop_unref(loop);

    g_object_unref(server);

    return 0;
}