#include <iostream>
#include <string>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>

#include <cstring>
#include "deepstream_app_exports.h"
// JSON-GLib library
#include <json-glib/json-glib.h>

// RabbitMQ C++ library (SimpleAmqpClient)
#include <SimpleAmqpClient/SimpleAmqpClient.h>

// URL parsing
#include <regex>

using namespace std;
using namespace chrono;

// Configuration constants
const string RABBITMQ_HOST = "rabitmq.phoenixtech.vn";
const int RABBITMQ_PORT = 5672;
const string RABBITMQ_USER = "guest";
const string RABBITMQ_PASS = "guest";
const string QUEUE_NAME = "stream_command";
const string RESPONSE_QUEUE = "stream_response";

// Global variables
string JETSON_ID;
atomic<bool> should_shutdown(false);

// Forward declarations
class StreamManager;
class RabbitMQController;

// Global controller for signal handling
unique_ptr<RabbitMQController> global_controller;

// Stream information structure
struct StreamInfo {
    pid_t pid;
    int cam_id;
    string rtmp_url;
    time_t start_time;
    bool running;
    
    StreamInfo() : pid(0), cam_id(0), start_time(0), running(false) {}
    StreamInfo(pid_t p, int c, const string& url) 
        : pid(p), cam_id(c), rtmp_url(url), start_time(time(nullptr)), running(true) {}
};

// JSON helper functions
class JsonHelper {
public:
    static JsonObject* create_object() {
        return json_object_new();
    }
    
    static void object_set_string(JsonObject* obj, const char* key, const char* value) {
        json_object_set_string_member(obj, key, value);
    }
    
    static void object_set_int(JsonObject* obj, const char* key, gint64 value) {
        json_object_set_int_member(obj, key, value);
    }
    
    static void object_set_boolean(JsonObject* obj, const char* key, gboolean value) {
        json_object_set_boolean_member(obj, key, value);
    }
    
    static void object_set_double(JsonObject* obj, const char* key, gdouble value) {
        json_object_set_double_member(obj, key, value);
    }
    
    static void object_set_object(JsonObject* parent, const char* key, JsonObject* child) {
        JsonNode* node = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(node, child);
        json_object_set_member(parent, key, node);
    }
    
    static string object_to_string(JsonObject* obj) {
        JsonNode* root = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(root, obj);
        
        JsonGenerator* generator = json_generator_new();
        json_generator_set_root(generator, root);
        
        gchar* json_string = json_generator_to_data(generator, nullptr);
        string result(json_string);
        
        g_free(json_string);
        g_object_unref(generator);
        json_node_free(root);
        
        return result;
    }
    
    static JsonObject* parse_string(const string& json_str) {
        GError* error = nullptr;
        JsonParser* parser = json_parser_new();
        
        if (!json_parser_load_from_data(parser, json_str.c_str(), json_str.length(), &error)) {
            g_object_unref(parser);
            if (error) {
                g_error_free(error);
            }
            return nullptr;
        }
        
        JsonNode* root = json_parser_get_root(parser);
        if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
            g_object_unref(parser);
            return nullptr;
        }
        
        JsonObject* obj = json_node_dup_object(root);
        g_object_unref(parser);
        
        return obj;
    }
    
    static string get_string(JsonObject* obj, const char* key, const string& default_value = "") {
        if (!json_object_has_member(obj, key)) {
            return default_value;
        }
        
        const gchar* value = json_object_get_string_member_with_default(obj, key, default_value.c_str());
        return string(value);
    }
    
    static gint64 get_int(JsonObject* obj, const char* key, gint64 default_value = 0) {
        if (!json_object_has_member(obj, key)) {
            return default_value;
        }
        
        return json_object_get_int_member_with_default(obj, key, default_value);
    }
    
    static gboolean get_boolean(JsonObject* obj, const char* key, gboolean default_value = FALSE) {
        if (!json_object_has_member(obj, key)) {
            return default_value;
        }
        
        return json_object_get_boolean_member_with_default(obj, key, default_value);
    }
};


class StreamManager {
private:
    map<string, StreamInfo> active_streams;
    mutex streams_mutex;
    atomic<bool> shutdown_event;
    
    void log_info(const string& message) {
        auto now = system_clock::now();
        auto time_t = system_clock::to_time_t(now);
        printf("[INFO] %s - %s\n", 
               ctime(&time_t), message.c_str());
        fflush(stdout);
    }
    
    void log_error(const string& message) {
        auto now = system_clock::now();
        auto time_t = system_clock::to_time_t(now);
        fprintf(stderr, "[ERROR] %s - %s\n", 
                ctime(&time_t), message.c_str());
        fflush(stderr);
    }
    
    void log_warning(const string& message) {
        auto now = system_clock::now();
        auto time_t = system_clock::to_time_t(now);
        printf("[WARNING] %s - %s\n", 
               ctime(&time_t), message.c_str());
        fflush(stdout);
    }
    
public:
    StreamManager() : shutdown_event(false) {
        // Get JETSON_ID from environment
        const char* jetson_env = getenv("JETSON_ID");
        JETSON_ID = jetson_env ? string(jetson_env) : "jetson01";
    }
    
    ~StreamManager() {
        shutdown_event = true;
        stop_all_streams();
    }
    
    bool enable_rtsp_sink1_source_wrapper(int cam_id) {
        try {
            enable_rtsp_sink1_source(cam_id);
            log_info("Enabled RTSP sink1 for camera " + to_string(cam_id));
            return true;
        } catch (const exception& e) {
            log_error("Failed to enable RTSP sink1: " + string(e.what()));
            return false;
        }
    }
    
    bool disable_rtsp_sink1_wrapper() {
        try {
            disable_rtsp_sink1();
            log_info("Disabled RTSP sink1");
            return true;
        } catch (const exception& e) {
            log_error("Failed to disable RTSP sink1: " + string(e.what()));
            return false;
        }
    }
    
    bool validate_rtmp_url(const string& url) {
        regex rtmp_regex(R"(^rtmps?://[^/]+/.*)");
        return regex_match(url, rtmp_regex);
    }
    
    bool check_rtsp_available(const string& rtsp_uri, int timeout = 10) {
        try {
            string cmd = "timeout " + to_string(timeout) + 
                        " gst-launch-1.0 rtspsrc location=\"" + rtsp_uri + 
                        "\" num-buffers=1 ! fakesink >/dev/null 2>&1";
            
            int result = system(cmd.c_str());
            return WEXITSTATUS(result) == 0;
        } catch (const exception& e) {
            log_error("RTSP check failed: " + string(e.what()));
            return false;
        }
    }
    
    pair<bool, string> start_rtmp_stream(const string& stream_id, int cam_id, const string& rtmp_server_url) {
        try {
            // Validate inputs
            if (!validate_rtmp_url(rtmp_server_url)) {
                return make_pair(false, "Invalid RTMP URL: " + rtmp_server_url);
            }
            
            // Stop existing stream if any
            if (active_streams.find(stream_id) != active_streams.end()) {
                stop_stream(stream_id);
            }
            
            // Enable RTSP sink
            if (!enable_rtsp_sink1_source_wrapper(cam_id)) {
                return make_pair(false, "Failed to enable RTSP sink");
            }
            
            // Wait for RTSP to be ready
            log_info("Waiting for RTSP sink to be ready...");
            this_thread::sleep_for(seconds(3));
            
            // Check RTSP availability
            string rtsp_uri = "rtsp://localhost:554/ds-stream";
            if (!check_rtsp_available(rtsp_uri)) {
                return make_pair(false, "RTSP stream not available");
            }
            
            // Build GStreamer command
            string gst_cmd = "gst-launch-1.0 rtspsrc location=" + rtsp_uri +
                           " ! decodebin ! nvvidconv ! nvv4l2h264enc ! h264parse ! flvmux streamable=true" +
                           " ! rtmpsink location=" + rtmp_server_url + " >/dev/null 2>&1 &";
            
            log_info("Starting RTMP stream: " + gst_cmd);
            
            // Start process
            pid_t pid = fork();
            if (pid == 0) {
                // Child process
                execl("/bin/sh", "sh", "-c", gst_cmd.c_str(), nullptr);
                exit(1);
            } else if (pid > 0) {
                // Parent process
                lock_guard<mutex> lock(streams_mutex);
                active_streams[stream_id] = StreamInfo(pid, cam_id, rtmp_server_url);
                
                // Start monitoring thread
                thread monitor_thread(&StreamManager::monitor_stream, this, stream_id);
                monitor_thread.detach();
                
                log_info("RTMP stream " + stream_id + " started successfully (PID: " + to_string(pid) + ")");
                return make_pair(true, "Stream " + stream_id + " started successfully");
            } else {
                return make_pair(false, "Failed to fork process");
            }
            
        } catch (const exception& e) {
            log_error("Failed to start RTMP stream " + stream_id + ": " + string(e.what()));
            return make_pair(false, string(e.what()));
        }
    }
    
    void monitor_stream(const string& stream_id) {
        while (!shutdown_event) {
            {
                lock_guard<mutex> lock(streams_mutex);
                auto it = active_streams.find(stream_id);
                if (it == active_streams.end()) {
                    break;
                }
                
                StreamInfo& info = it->second;
                int status;
                pid_t result = waitpid(info.pid, &status, WNOHANG);
                
                if (result > 0) {
                    // Process has terminated
                    log_warning("Stream " + stream_id + " process terminated with code " + to_string(WEXITSTATUS(status)));
                    active_streams.erase(it);
                    break;
                } else if (result < 0) {
                    // Error occurred
                    log_error("Error monitoring stream " + stream_id);
                    active_streams.erase(it);
                    break;
                }
            }
            
            this_thread::sleep_for(seconds(5)); // Check every 5 seconds
        }
    }
    
    pair<bool, string> stop_stream(const string& stream_id) {
        lock_guard<mutex> lock(streams_mutex);
        
        auto it = active_streams.find(stream_id);
        if (it == active_streams.end()) {
            return make_pair(false, "Stream " + stream_id + " not found");
        }
        
        try {
            StreamInfo& info = it->second;
            
            // Terminate process gracefully
            if (kill(info.pid, SIGTERM) == 0) {
                // Wait for termination
                int status;
                pid_t result = waitpid(info.pid, &status, 0);
                
                if (result < 0) {
                    // Force kill if not terminated
                    kill(info.pid, SIGKILL);
                    waitpid(info.pid, &status, 0);
                }
            }
            
            active_streams.erase(it);
            log_info("Stream " + stream_id + " stopped successfully");
            return make_pair(true, "Stream " + stream_id + " stopped");
            
        } catch (const exception& e) {
            log_error("Failed to stop stream " + stream_id + ": " + string(e.what()));
            return make_pair(false, string(e.what()));
        }
    }
    
    void stop_all_streams() {
        lock_guard<mutex> lock(streams_mutex);
        
        for (auto& pair : active_streams) {
            try {
                kill(pair.second.pid, SIGTERM);
                int status;
                waitpid(pair.second.pid, &status, 0);
            } catch (...) {
                // Ignore errors during shutdown
            }
        }
        active_streams.clear();
        log_info("All streams stopped");
    }
    
    string get_stream_status() {
        lock_guard<mutex> lock(streams_mutex);
        JsonObject* status = JsonHelper::create_object();
        
        for (const auto& pair : active_streams) {
            const string& stream_id = pair.first;
            const StreamInfo& info = pair.second;
            
            // Check if process is still running
            int process_status;
            bool running = (waitpid(info.pid, &process_status, WNOHANG) == 0);
            
            JsonObject* stream_obj = JsonHelper::create_object();
            JsonHelper::object_set_int(stream_obj, "cam_id", info.cam_id);
            JsonHelper::object_set_string(stream_obj, "rtmp_url", info.rtmp_url.c_str());
            JsonHelper::object_set_int(stream_obj, "start_time", info.start_time);
            JsonHelper::object_set_int(stream_obj, "pid", info.pid);
            JsonHelper::object_set_boolean(stream_obj, "running", running ? TRUE : FALSE);
            JsonHelper::object_set_double(stream_obj, "uptime", difftime(time(nullptr), info.start_time));
            
            JsonHelper::object_set_object(status, stream_id.c_str(), stream_obj);
        }
        
        string result = JsonHelper::object_to_string(status);
        return result;
    }
};

class RabbitMQController {
private:
    unique_ptr<StreamManager> stream_manager;
    AmqpClient::Channel::ptr_t channel;
    atomic<bool> should_run;
    
    void log_info(const string& message) {
        auto now = system_clock::now();
        auto time_t = system_clock::to_time_t(now);
        printf("[INFO] %s - %s\n", 
               ctime(&time_t), message.c_str());
        fflush(stdout);
    }
    
    void log_error(const string& message) {
        auto now = system_clock::now();
        auto time_t = system_clock::to_time_t(now);
        fprintf(stderr, "[ERROR] %s - %s\n", 
                ctime(&time_t), message.c_str());
        fflush(stderr);
    }
    
public:
    RabbitMQController() : should_run(true) {
        stream_manager = make_unique<StreamManager>();
    }
    
    void send_response(bool success, const string& message, const string& command_id = "") {
        try {
            if (!channel) {
                return;
            }
            
            JsonObject* response = JsonHelper::create_object();
            JsonHelper::object_set_string(response, "jetson_id", JETSON_ID.c_str());
            JsonHelper::object_set_boolean(response, "success", success ? TRUE : FALSE);
            JsonHelper::object_set_string(response, "message", message.c_str());
            JsonHelper::object_set_int(response, "timestamp", time(nullptr));
            
            if (!command_id.empty()) {
                JsonHelper::object_set_string(response, "command_id", command_id.c_str());
            }
            
            string json_str = JsonHelper::object_to_string(response);
            
            channel->DeclareQueue(RESPONSE_QUEUE, true, false, false, false);
            AmqpClient::BasicMessage::ptr_t msg = AmqpClient::BasicMessage::Create(json_str);
            msg->DeliveryMode(AmqpClient::BasicMessage::dm_persistent);
            
            channel->BasicPublish("", RESPONSE_QUEUE, msg);
            log_info("Response sent: " + json_str);
            
        } catch (const exception& e) {
            log_error("Failed to send response: " + string(e.what()));
        }
    }
    
    void handle_command(const string& command_data) {
        try {
            JsonObject* msg = JsonHelper::parse_string(command_data);
            if (!msg) {
                log_error("Invalid JSON command received");
                send_response(false, "Invalid JSON format", "");
                return;
            }
            
            string jetson_id = JsonHelper::get_string(msg, "jetson_id", "");
            string cmd = JsonHelper::get_string(msg, "command", "");
            int cam_id = JsonHelper::get_int(msg, "cam_id", -1);
            string rtmp_server_url = JsonHelper::get_string(msg, "rtmp_server_url", "");
            string command_id = JsonHelper::get_string(msg, "command_id", "");
            string stream_id = JsonHelper::get_string(msg, "stream_id", "stream_" + to_string(cam_id));
            
            // Check if command is for this Jetson
            if (!jetson_id.empty() && jetson_id != JETSON_ID) {
                log_info("Command for " + jetson_id + ", ignoring (this is " + JETSON_ID + ")");
                json_object_unref(msg);
                return;
            }
            
            log_info("Processing command: " + cmd);
            
            if (cmd == "start_rtmp_deepstream") {
                if (cam_id < 0 || rtmp_server_url.empty()) {
                    send_response(false, "Missing cam_id or rtmp_server_url", command_id);
                    json_object_unref(msg);
                    return;
                }
                
                auto result = stream_manager->start_rtmp_stream(stream_id, cam_id, rtmp_server_url);
                send_response(result.first, result.second, command_id);
                
            } else if (cmd == "stop_stream") {
                auto result = stream_manager->stop_stream(stream_id);
                send_response(result.first, result.second, command_id);
                
            } else if (cmd == "stop_all_streams") {
                stream_manager->stop_all_streams();
                send_response(true, "All streams stopped", command_id);
                
            } else if (cmd == "get_status") {
                string status = stream_manager->get_stream_status();
                send_response(true, status, command_id);
                
            } else if (cmd == "disable_rtsp_sink1") {
                bool success = stream_manager->disable_rtsp_sink1_wrapper();
                string message = success ? "RTSP sink1 disabled" : "Failed to disable RTSP sink1";
                send_response(success, message, command_id);
                
            } else if (cmd.find("set_stream_source") == 0) {
                if (cam_id < 0) {
                    send_response(false, "Missing cam_id", command_id);
                    json_object_unref(msg);
                    return;
                }
                
                bool success = stream_manager->enable_rtsp_sink1_source_wrapper(cam_id);
                string message = success ? "RTSP sink1 enabled for camera " + to_string(cam_id) : 
                                         "Failed to enable RTSP sink1";
                send_response(success, message, command_id);
                
            } else {
                send_response(false, "Unknown command: " + cmd, command_id);
            }
            
            json_object_unref(msg);
            
        } catch (const exception& e) {
            log_error("Error handling command: " + string(e.what()));
            send_response(false, "Command processing error: " + string(e.what()), "");
        }
    }
    
    bool connect_rabbitmq() {
        while (should_run) {
            try {
                channel = AmqpClient::Channel::Create(RABBITMQ_HOST, RABBITMQ_PORT, RABBITMQ_USER, RABBITMQ_PASS);
                channel->DeclareQueue(QUEUE_NAME, true, false, false, false);
                
                log_info("RabbitMQ listener (" + JETSON_ID + ") connected and waiting for commands...");
                return true;
                
            } catch (const exception& e) {
                log_error("RabbitMQ connection failed: " + string(e.what()));
                this_thread::sleep_for(seconds(5));
            }
        }
        return false;
    }
    
    void start_consuming() {
        while (should_run) {
            try {
                if (!channel || !connect_rabbitmq()) {
                    continue;
                }
                
                string consumer_tag = channel->BasicConsume(QUEUE_NAME);
                
                while (should_run) {
                    AmqpClient::Envelope::ptr_t envelope;
                    if (channel->BasicConsumeMessage(consumer_tag, envelope, 1000)) {
                        handle_command(envelope->Message()->Body());
                        channel->BasicAck(envelope);
                    }
                }
                
            } catch (const exception& e) {
                log_error("Consuming error: " + string(e.what()));
                this_thread::sleep_for(seconds(5));
            }
        }
    }
    
    void shutdown() {
        log_info("Shutting down...");
        should_run = false;
        
        if (stream_manager) {
            stream_manager->stop_all_streams();
        }
        
        if (channel) {
            try {
                channel.reset();
            } catch (const exception& e) {
                log_error("Error closing RabbitMQ channel: " + string(e.what()));
            }
        }
        
        log_info("Shutdown completed");
    }
};

// Signal handler
void signal_handler(int signum) {
    printf("\nReceived signal %d\n", signum);
    should_shutdown = true;
    if (global_controller) {
        global_controller->shutdown();
    }
    exit(0);
}

int main() {
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    try {
        global_controller = make_unique<RabbitMQController>();
        global_controller->start_consuming();
    } catch (const exception& e) {
        fprintf(stderr, "Application error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}