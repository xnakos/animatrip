#include <arpa/inet.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <iostream>
#include <thread>

int main(int argc, char *argv[])
{
    bool is_test = false;
    std::string device = "/dev/video0";
    std::string host = "127.0.0.1";
    std::string port = "27884";

    int opt;

    while ((opt = getopt(argc, argv, "td:h:p:")) != -1)
    {
        switch (opt)
        {
        case 't':
            is_test = true;
            break;
        case 'd':
            device = optarg;
            break;
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        default:
            fprintf(stderr, "Usage: %s [-t] [-d device] [-h host] [-p port]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    // Initialize GStreamer
    gst_init(nullptr, nullptr);

    std::string camera_to_udp_pipeline_desc_str;

    if (!is_test)
        camera_to_udp_pipeline_desc_str = "v4l2src device=" + device + " ! videoconvert ! videoscale ! video/x-raw,framerate=30/1,width=320,height=240 ! videoscale ! videoconvert ! x264enc tune=zerolatency bitrate=500 speed-preset=superfast ! rtph264pay ! udpsink name=udpsink host=" + host + " port=" + port;
    else
        camera_to_udp_pipeline_desc_str = "videotestsrc pattern=ball ! videoconvert ! videoscale ! video/x-raw,framerate=30/1,width=320,height=240 ! videoscale ! videoconvert ! x264enc tune=zerolatency bitrate=500 speed-preset=superfast ! rtph264pay ! udpsink name=udpsink host=" + host + " port=" + port;

    // Playback pipeline description
    const char *udp_to_screen_pipeline_desc = "udpsrc name=udpsrc caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! avdec_h264 ! videoconvert ! autovideosink";

    // Capture pipeline description
    const char *camera_to_udp_pipeline_desc = camera_to_udp_pipeline_desc_str.c_str();

    // Server UDP communication socket
    int server_comm_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_comm_fd < 0)
    {
        std::cerr << "Failed to create socket: server_comm_fd" << std::endl;
        return 1;
    }

    sockaddr_in server_comm_sockaddr{};
    server_comm_sockaddr.sin_family = AF_INET;
    inet_pton(AF_INET, "0.0.0.0", &(server_comm_sockaddr.sin_addr));
    server_comm_sockaddr.sin_port = htons(0); // Assign any available port
    if (bind(server_comm_fd, (struct sockaddr *)&server_comm_sockaddr, sizeof(server_comm_sockaddr)) < 0)
    {
        std::cerr << "Failed to bind socket: server_comm_fd" << std::endl;
        close(server_comm_fd);
        return 1;
    }

    // Create GSocket object for use with udpsrc and udpsink
    GSocket *gsock = g_socket_new_from_fd(server_comm_fd, nullptr);

    // Create playback pipeline
    GstElement *playback_pipeline = gst_parse_launch(udp_to_screen_pipeline_desc, nullptr);

    // Set up the UDP source element of the playback pipeline
    GstElement *udpsrc = gst_bin_get_by_name(GST_BIN(playback_pipeline), "udpsrc");
    g_object_set(udpsrc, "socket", gsock, nullptr);

    // Create capture pipeline
    GstElement *capture_pipeline = gst_parse_launch(camera_to_udp_pipeline_desc, nullptr);

    // Set up the UDP sink element of the capture pipeline
    GstElement *udpsink = gst_bin_get_by_name(GST_BIN(capture_pipeline), "udpsink");
    g_object_set(udpsink, "socket", gsock, nullptr);

    // Start playing the pipelines
    gst_element_set_state(playback_pipeline, GST_STATE_PLAYING);
    gst_element_set_state(capture_pipeline, GST_STATE_PLAYING);

    // Create a GLib Main Loop and set it to run
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);

    // Free resources
    // TODO Verify cleanup
    gst_element_set_state(capture_pipeline, GST_STATE_NULL);
    gst_element_set_state(playback_pipeline, GST_STATE_NULL);
    gst_object_unref(capture_pipeline);
    gst_object_unref(playback_pipeline);
    g_main_loop_unref(loop);

    // Free GSocket object
    g_object_unref(gsock);

    return 0;
}
