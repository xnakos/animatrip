#include <arpa/inet.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <vector>

// TODO Check whether this should be higher
const int BUFFER_SIZE = 4096;
const int SERVER_PORT = 62000;

struct sockaddr_in_cmp
{
    bool operator()(const sockaddr_in &lhs, const sockaddr_in &rhs) const
    {
        if (lhs.sin_addr.s_addr != rhs.sin_addr.s_addr)
            return lhs.sin_addr.s_addr < rhs.sin_addr.s_addr;
        return lhs.sin_port < rhs.sin_port;
    }
};

std::set<sockaddr_in, sockaddr_in_cmp> client_sockaddrs;

// Maps client address to GStreamer pipeline udpsrc address
std::map<sockaddr_in, sockaddr_in, sockaddr_in_cmp> client_routes;

// Maps client address to last activity time
std::map<sockaddr_in, gint64, sockaddr_in_cmp> client_activity;

// GStreamer pipeline unused udpsrc socket addresses
// Use as a stack? Initialize in reverse?
// TODO Initialize with specific size for efficiency, with reserve?
std::vector<sockaddr_in> udpsrc_sockaddrs_available;

std::vector<int> udpsrc_socks;

std::vector<GSocket *> udpsrc_gsocks;

// Maps udpsrc address to udpsrc index in pipeline
std::map<sockaddr_in, size_t, sockaddr_in_cmp> udpsrc_ixs;

// GStreamer pipeline compositor sink pads in order of creation
std::vector<GstPad *> compositor_pads;

void init_compositor_pads(GstElement *compositor)
{
    GstPad *sink;
    sink = gst_element_get_static_pad(compositor, "sink_0");
    compositor_pads.push_back(sink);
    sink = gst_element_get_static_pad(compositor, "sink_1");
    compositor_pads.push_back(sink);
    sink = gst_element_get_static_pad(compositor, "sink_2");
    compositor_pads.push_back(sink);
    sink = gst_element_get_static_pad(compositor, "sink_3");
    compositor_pads.push_back(sink);
}

// Sequence of {xpos, ypos} compositor coordinates in order of usage
std::vector<std::pair<uint16_t, uint16_t>> position_points;

void init_position_points()
{
    position_points.push_back({0, 0});
    position_points.push_back({320, 0});
    position_points.push_back({0, 240});
    position_points.push_back({320, 240});
}

// Available positions for placing clients, as a stack. The next available position to use is taken from the back.
std::vector<size_t> positions_available;

void init_positions_available()
{
    positions_available.push_back(3);
    positions_available.push_back(2);
    positions_available.push_back(1);
    positions_available.push_back(0);
}

// Maps udpsrc address to position
std::map<sockaddr_in, size_t, sockaddr_in_cmp> udpsrc_positions;

void compact_positions()
{
    // Ensure that at least one position is available
    if (positions_available.size())
    {
        // Sort available positions in descending order
        sort(positions_available.begin(), positions_available.end(), std::greater<size_t>());

        auto lowest_position_available = positions_available.back();
        // If the lowest available position is less than the client count, then there is at least one client with a higher position
        if (lowest_position_available < client_sockaddrs.size())
        {
            for (const auto &client_sockaddr : client_sockaddrs)
            {
                auto udpsrc_sockaddr = client_routes[client_sockaddr];
                auto udpsrc_position = udpsrc_positions[udpsrc_sockaddr];

                // If the current client position is greater than the lowest available position, then the latter should be used for the client and the former should become available for later use
                if (udpsrc_position > lowest_position_available)
                {
                    auto udpsrc_ix = udpsrc_ixs[udpsrc_sockaddr];
                    auto pad = compositor_pads[udpsrc_ix];

                    udpsrc_positions[udpsrc_sockaddr] = lowest_position_available;
                    auto position_point = position_points[lowest_position_available];
                    g_object_set(pad, "xpos", position_point.first, "ypos", position_point.second, nullptr);

                    positions_available.pop_back();
                    positions_available.push_back(udpsrc_position);

                    sort(positions_available.begin(), positions_available.end(), std::greater<size_t>());
                    lowest_position_available = positions_available.back();
                }
            }
        }
    }
}

/**
 * Initialize udpsrc elements. FIXME Do this or change name and description.
 */
void init_udpsrcs()
{
    for (int i = 0; i < 4; i++)
    {
        int udpsrc_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udpsrc_sock == -1)
        {
            std::cerr << "Failed to create udpsrc_sock." << std::endl;
            return;
        }

        sockaddr_in udpsrc_sockaddr{};
        udpsrc_sockaddr.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &(udpsrc_sockaddr.sin_addr));
        udpsrc_sockaddr.sin_port = htons(0); // Assign any port

        if (bind(udpsrc_sock, (struct sockaddr *)&udpsrc_sockaddr, sizeof(udpsrc_sockaddr)) == -1)
        {
            std::cerr << "Failed to bind udpsrc_sock." << std::endl;
            return;
        }

        socklen_t udpsrc_sockaddr_len = sizeof(udpsrc_sockaddr);
        if (getsockname(udpsrc_sock, (struct sockaddr *)&udpsrc_sockaddr, &udpsrc_sockaddr_len) == -1)
        {
            std::cerr << "Failed to get udpsrc_sock name." << std::endl;
            return;
        }

        GSocket *udpsrc_gsock = g_socket_new_from_fd(udpsrc_sock, nullptr);

        if (udpsrc_gsock == nullptr)
        {
            std::cerr << "Failed to create udpsrc_gsock." << std::endl;
            return;
        }

        udpsrc_ixs[udpsrc_sockaddr] = i;
        udpsrc_socks.push_back(udpsrc_sock);
        udpsrc_sockaddrs_available.push_back(udpsrc_sockaddr);
        udpsrc_gsocks.push_back(udpsrc_gsock);
    }
}

int main()
{
    // Initialize GStreamer
    gst_init(nullptr, nullptr);

    // Processing pipeline description (currently only flips horizontally)
    // const char *processing_pipeline_desc = "appsrc name=appsrc caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! decodebin ! videoscale ! videoconvert ! videoflip method=horizontal-flip ! x264enc tune=zerolatency bitrate=500 speed-preset=superfast ! rtph264pay ! appsink name=appsink";
    // Based on `gst-launch-1.0 compositor name=comp sink_0::xpos=0 sink_0::ypos=0 sink_1::xpos=320 sink_1::ypos=0 sink_2::xpos=640 sink_2::ypos=0 sink_3::xpos=0 sink_3::ypos=240 ! autovideosink videotestsrc pattern=white ! video/x-raw, framerate=30/1, width=320, height=240 ! comp. videotestsrc pattern=red ! videobox ! video/x-raw, framerate=60/1, width=320, height=240 ! comp. videotestsrc pattern=green ! videobox ! video/x-raw, framerate=30/1, width=320, height=240 ! comp. videotestsrc pattern=blue ! videobox ! video/x-raw, framerate=30/1, width=320, height=240 ! comp.`
    // const char *processing_pipeline_desc = "compositor name=compositor background=black sink_0::xpos=0 sink_0::ypos=0 sink_1::xpos=320 sink_1::ypos=0 sink_2::xpos=640 sink_2::ypos=0 sink_3::xpos=0 sink_3::ypos=240 ! x264enc tune=zerolatency bitrate=500 speed-preset=superfast ! rtph264pay ! udpsink name=udpsink host=127.0.0.1 udpsrc name=udpsrc_0 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! avdec_h264 ! videoscale ! videoconvert ! video/x-raw, framerate=30/1, width=320, height=240 ! compositor. videotestsrc pattern=red ! videobox ! video/x-raw, framerate=60/1, width=320, height=240 ! compositor. videotestsrc pattern=green ! videobox ! video/x-raw, framerate=30/1, width=320, height=240 ! compositor. udpsrc name=udpsrc_1 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! avdec_h264 ! videoscale ! videoconvert ! video/x-raw, framerate=30/1, width=320, height=240 ! compositor.";
    const char *processing_pipeline_desc = "compositor name=compositor background=black ! x264enc tune=zerolatency bitrate=500 speed-preset=superfast ! rtph264pay ! udpsink name=udpsink host=127.0.0.1 udpsrc name=udpsrc_0 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! avdec_h264 ! videoscale ! videoconvert ! video/x-raw, framerate=30/1, width=320, height=240 ! compositor. udpsrc name=udpsrc_1 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! avdec_h264 ! videoscale ! videoconvert ! video/x-raw, framerate=30/1, width=320, height=240 ! compositor. udpsrc name=udpsrc_2 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! avdec_h264 ! videoscale ! videoconvert ! video/x-raw, framerate=30/1, width=320, height=240 ! compositor. udpsrc name=udpsrc_3 caps=\"application/x-rtp, media=(string)video, clock-rate=(int)90000, encoding-name=(string)H264, payload=(int)96\" ! rtph264depay ! avdec_h264 ! videoscale ! videoconvert ! video/x-raw, framerate=30/1, width=320, height=240 ! compositor.";

    // Create processing pipeline
    GstElement *processing_pipeline = gst_parse_launch(processing_pipeline_desc, nullptr);

    // Socket that external clients send to
    int socket_ext = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ext < 0)
    {
        std::cerr << "Failed to create socket_ext." << std::endl;
        return 1;
    }

    sockaddr_in sockaddr_ext{};
    sockaddr_ext.sin_family = AF_INET;
    inet_pton(AF_INET, "0.0.0.0", &(sockaddr_ext.sin_addr));
    sockaddr_ext.sin_port = htons(SERVER_PORT);

    if (bind(socket_ext, (struct sockaddr *)&sockaddr_ext, sizeof(sockaddr_ext)) < 0)
    {
        std::cerr << "Failed to bind socket_ext." << std::endl;
        return 1;
    }

    // Socket that internal GStreamer sends to
    int socket_int = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_int < 0)
    {
        std::cerr << "Failed to create socket_int." << std::endl;
        return 1;
    }

    sockaddr_in sockaddr_int{};
    sockaddr_int.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &(sockaddr_int.sin_addr));
    sockaddr_int.sin_port = htons(0);

    if (bind(socket_int, (struct sockaddr *)&sockaddr_int, sizeof(sockaddr_int)) < 0)
    {
        std::cerr << "Failed to bind socket_int." << std::endl;
        return 1;
    }

    socklen_t sockaddr_int_len = sizeof(sockaddr_int);
    if (getsockname(socket_int, (struct sockaddr *)&sockaddr_int, &sockaddr_int_len) == -1)
    {
        std::cerr << "Failed to get socket name." << std::endl;
        return 1;
    }

    // Extract the port number from sockaddr_int
    unsigned short port_assigned = ntohs(sockaddr_int.sin_port);

    GstElement *udpsink = gst_bin_get_by_name(GST_BIN(processing_pipeline), "udpsink");
    g_object_set(udpsink, "port", port_assigned, nullptr);

    struct pollfd fds[2];

    // TODO Check whether using the same buffer for both sockets is OK and whether it should be outside of the loop
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    // TODO Same check for these
    sockaddr_in client_sockaddr{};
    socklen_t client_sockaddr_len = sizeof(client_sockaddr);

    fds[0].fd = socket_ext;
    fds[0].events = POLLIN;
    fds[1].fd = socket_int;
    fds[1].events = POLLIN;

    init_udpsrcs();

    GstElement *udpsrc_0 = gst_bin_get_by_name(GST_BIN(processing_pipeline), "udpsrc_0");
    g_object_set(udpsrc_0, "socket", udpsrc_gsocks[0], nullptr);

    GstElement *udpsrc_1 = gst_bin_get_by_name(GST_BIN(processing_pipeline), "udpsrc_1");
    g_object_set(udpsrc_1, "socket", udpsrc_gsocks[1], nullptr);

    GstElement *udpsrc_2 = gst_bin_get_by_name(GST_BIN(processing_pipeline), "udpsrc_2");
    g_object_set(udpsrc_2, "socket", udpsrc_gsocks[2], nullptr);

    GstElement *udpsrc_3 = gst_bin_get_by_name(GST_BIN(processing_pipeline), "udpsrc_3");
    g_object_set(udpsrc_3, "socket", udpsrc_gsocks[3], nullptr);

    gst_element_set_state(processing_pipeline, GST_STATE_PLAYING);

    init_position_points();

    init_positions_available();

    GstElement *compositor = gst_bin_get_by_name(GST_BIN(processing_pipeline), "compositor");

    init_compositor_pads(compositor);

    gint64 current_time = g_get_monotonic_time();
    gint64 client_activity_check = current_time;

    std::vector<sockaddr_in> client_sockaddrs_inactive;

    bool has_addition_occurred;
    bool has_removal_occurred;

    while (true)
    {
        // Block until a socket event occurs
        int poll_res = poll(fds, 2, -1);
        if (poll_res == -1)
        {
            std::cerr << "Poll error." << std::endl;
            return 1;
        }

        current_time = g_get_monotonic_time();
        has_addition_occurred = false;
        has_removal_occurred = false;

        // Check whether socket_ext has data
        if (fds[0].revents & POLLIN)
        {
            // Receive from client
            bytes_read = recvfrom(socket_ext, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_sockaddr, &client_sockaddr_len);
            if (bytes_read < 0)
            {
                std::cerr << "Failed to receive from client." << std::endl;
                continue;
            }

            // Add client to active clients, if client is not already there
            if (client_sockaddrs.count(client_sockaddr) == 0)
            {
                // TODO Handle the opposite case gracefully and do not fail silently
                if (udpsrc_sockaddrs_available.size() > 0)
                {
                    client_sockaddrs.insert(client_sockaddr);

                    auto udpsrc_sockaddr = udpsrc_sockaddrs_available.back();
                    client_routes[client_sockaddr] = udpsrc_sockaddr;

                    auto udpsrc_ix = udpsrc_ixs[udpsrc_sockaddr];
                    auto pad = compositor_pads[udpsrc_ix];

                    auto position = positions_available.back();
                    udpsrc_positions[udpsrc_sockaddr] = position;
                    auto position_point = position_points[position];

                    g_object_set(pad, "xpos", position_point.first, "ypos", position_point.second, nullptr);
                    g_object_set(pad, "alpha", 1.0, nullptr);

                    positions_available.pop_back();
                    udpsrc_sockaddrs_available.pop_back();

                    has_addition_occurred = true;
                }
            }

            // If a client route exists, store the client activity time and route to the associated udpsrc_sockaddr
            if (auto client_route = client_routes.find(client_sockaddr); client_route != client_routes.end())
            {
                client_activity[client_sockaddr] = current_time;

                // TODO Check whether it is OK to use socket_ext to send
                if (sendto(socket_ext, buffer, bytes_read, 0, (struct sockaddr *)&(client_route->second), sizeof(client_route->second)) < 0)
                {
                    std::cerr << "Failed to send to GStreamer." << std::endl;
                    continue;
                }
            }
        }

        // Check whether socket_int has data
        if (fds[1].revents & POLLIN)
        {
            // Receive from GStreamer
            // TODO Check whether the same buffer and struct should be used for GStreamer
            bytes_read = recvfrom(socket_int, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&client_sockaddr, &client_sockaddr_len);
            if (bytes_read < 0)
            {
                std::cerr << "Failed to receive from GStreamer." << std::endl;
                continue;
            }

            // Send received data from GStreamer to all active clients
            for (const auto &client_sockaddr : client_sockaddrs)
            {
                if (sendto(socket_ext, buffer, bytes_read, 0, (struct sockaddr *)&client_sockaddr, sizeof(client_sockaddr)) < 0)
                {
                    std::cerr << "Failed to send to client." << std::endl;
                    continue;
                }
            }
        }

        if (current_time - client_activity_check > 8000000)
        {
            client_activity_check = current_time;
            client_sockaddrs_inactive.clear();

            for (const auto &client_sockaddr : client_sockaddrs)
            {
                if (current_time - client_activity[client_sockaddr] > 2000000)
                {
                    client_sockaddrs_inactive.push_back(client_sockaddr);
                }
            }

            for (const auto &client_sockaddr : client_sockaddrs_inactive)
            {
                auto udpsrc_sockaddr = client_routes[client_sockaddr];
                auto udpsrc_ix = udpsrc_ixs[udpsrc_sockaddr];
                auto pad = compositor_pads[udpsrc_ix];

                g_object_set(pad, "alpha", 0.0, nullptr);
                g_object_set(pad, "xpos", 0, "ypos", 0, nullptr);

                auto udpsrc_position = udpsrc_positions[udpsrc_sockaddr];

                positions_available.push_back(udpsrc_position);
                udpsrc_sockaddrs_available.push_back(udpsrc_sockaddr);

                udpsrc_positions.erase(udpsrc_sockaddr);
                client_sockaddrs.erase(client_sockaddr);
                client_routes.erase(client_sockaddr);
                client_activity.erase(client_sockaddr);

                has_removal_occurred = true;
            }

            if (has_removal_occurred)
            {
                compact_positions();
            }
        }

        if (has_addition_occurred || has_removal_occurred)
        {
            std::cout << "---- Active clients ----" << std::endl;
            for (const auto &client_sockaddr : client_sockaddrs)
            {
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(client_sockaddr.sin_addr), client_ip, INET_ADDRSTRLEN);
                std::cout << client_ip << ":" << ntohs(client_sockaddr.sin_port) << std::endl;
            }
            std::cout << "------------------------" << std::endl;
        }
    }

    close(socket_ext);
    close(socket_int);

    return 0;
}
