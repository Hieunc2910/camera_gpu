import pika
import time
import ctypes
import subprocess
import os
import json
import signal
import sys
import threading
import logging
from urllib.parse import urlparse

# Setup logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')
logger = logging.getLogger(__name__)

RABBITMQ_HOST = 'rabitmq.phoenixtech.vn'
RABBITMQ_PORT = 5672
RABBITMQ_USER = 'guest'
RABBITMQ_PASS = 'guest'
QUEUE_NAME = 'stream_command'
RESPONSE_QUEUE = 'stream_response'
JETSON_ID = os.environ.get("JETSON_ID", "jetson01")

class StreamManager:
    def __init__(self):
        self.active_streams = {}
        self.deepstream_lib = None
        self.load_deepstream_lib()
        self.shutdown_event = threading.Event()
        
    def load_deepstream_lib(self):
        """Load DeepStream library with error handling"""
        try:
            self.deepstream_lib = ctypes.CDLL("d:/Computervision/camera_gpu/build/deepstream_app.dll")
            logger.info("DeepStream library loaded successfully")
        except Exception as e:
            logger.error(f"Failed to load DeepStream library: {e}")
            raise
    
    def enable_rtsp_sink1_source(self, cam_id):
        """Enable RTSP sink with error handling"""
        try:
            if self.deepstream_lib:
                self.deepstream_lib.enable_rtsp_sink1_source(ctypes.c_int(cam_id))
                logger.info(f"Enabled RTSP sink1 for camera {cam_id}")
                return True
        except Exception as e:
            logger.error(f"Failed to enable RTSP sink1: {e}")
            return False
        return False
    
    def disable_rtsp_sink1(self):
        """Disable RTSP sink with error handling"""
        try:
            if self.deepstream_lib:
                self.deepstream_lib.disable_rtsp_sink1()
                logger.info("Disabled RTSP sink1")
                return True
        except Exception as e:
            logger.error(f"Failed to disable RTSP sink1: {e}")
            return False
        return False
    
    def validate_rtmp_url(self, url):
        """Validate RTMP URL format"""
        try:
            parsed = urlparse(url)
            return parsed.scheme in ['rtmp', 'rtmps'] and parsed.netloc
        except:
            return False
    
    def check_rtsp_available(self, rtsp_uri, timeout=10):
        """Check if RTSP stream is available"""
        try:
            cmd = f'timeout {timeout} gst-launch-1.0 rtspsrc location="{rtsp_uri}" num-buffers=1 ! fakesink'
            result = subprocess.run(cmd, shell=True, capture_output=True, timeout=timeout+2)
            return result.returncode == 0
        except subprocess.TimeoutExpired:
            logger.warning(f"RTSP check timeout for {rtsp_uri}")
            return False
        except Exception as e:
            logger.error(f"RTSP check failed: {e}")
            return False
    
    def start_rtmp_stream(self, stream_id, cam_id, rtmp_server_url):
        """Start RTMP stream with comprehensive error handling"""
        try:
            # Validate inputs
            if not self.validate_rtmp_url(rtmp_server_url):
                raise ValueError(f"Invalid RTMP URL: {rtmp_server_url}")
            
            # Stop existing stream if any
            if stream_id in self.active_streams:
                self.stop_stream(stream_id)
            
            # Enable RTSP sink
            if not self.enable_rtsp_sink1_source(cam_id):
                raise Exception("Failed to enable RTSP sink")
            
            # Wait for RTSP to be ready
            logger.info("Waiting for RTSP sink to be ready...")
            time.sleep(3)
            
            # Check RTSP availability
            rtsp_uri = f"rtsp://localhost:554/ds-stream"
            if not self.check_rtsp_available(rtsp_uri):
                raise Exception("RTSP stream not available")
            
            # Build GStreamer command (safer without shell=True)
            gst_cmd = [
                'gst-launch-1.0',
                'rtspsrc', f'location={rtsp_uri}',
                '!', 'decodebin',
                '!', 'nvvidconv',
                '!', 'nvv4l2h264enc',
                '!', 'h264parse',
                '!', 'flvmux', 'streamable=true',
                '!', 'rtmpsink', f'location={rtmp_server_url}'
            ]
            
            logger.info(f"Starting RTMP stream: {' '.join(gst_cmd)}")
            
            # Start process
            process = subprocess.Popen(
                gst_cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                preexec_fn=os.setsid if os.name != 'nt' else None
            )
            
            # Store stream info
            self.active_streams[stream_id] = {
                'process': process,
                'cam_id': cam_id,
                'rtmp_url': rtmp_server_url,
                'start_time': time.time(),
                'pid': process.pid
            }
            
            # Start monitoring thread
            monitor_thread = threading.Thread(
                target=self._monitor_stream,
                args=(stream_id,),
                daemon=True
            )
            monitor_thread.start()
            
            logger.info(f"RTMP stream {stream_id} started successfully (PID: {process.pid})")
            return True, f"Stream {stream_id} started successfully"
            
        except Exception as e:
            logger.error(f"Failed to start RTMP stream {stream_id}: {e}")
            return False, str(e)
    
    def _monitor_stream(self, stream_id):
        """Monitor stream health"""
        if stream_id not in self.active_streams:
            return
            
        stream_info = self.active_streams[stream_id]
        process = stream_info['process']
        
        while not self.shutdown_event.is_set():
            if process.poll() is not None:
                # Process has terminated
                logger.warning(f"Stream {stream_id} process terminated with code {process.returncode}")
                
                # Read error output
                try:
                    stderr = process.stderr.read().decode() if process.stderr else "No error output"
                    logger.error(f"Stream {stream_id} error output: {stderr}")
                except:
                    pass
                
                # Clean up
                if stream_id in self.active_streams:
                    del self.active_streams[stream_id]
                break
            
            time.sleep(5)  # Check every 5 seconds
    
    def stop_stream(self, stream_id):
        """Stop specific stream"""
        if stream_id not in self.active_streams:
            return False, f"Stream {stream_id} not found"
        
        try:
            stream_info = self.active_streams[stream_id]
            process = stream_info['process']
            
            # Terminate process gracefully
            if os.name == 'nt':  # Windows
                process.terminate()
            else:  # Linux
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)
            
            # Wait for termination
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                # Force kill if not terminated
                if os.name == 'nt':
                    process.kill()
                else:
                    os.killpg(os.getpgid(process.pid), signal.SIGKILL)
                process.wait()
            
            del self.active_streams[stream_id]
            logger.info(f"Stream {stream_id} stopped successfully")
            return True, f"Stream {stream_id} stopped"
            
        except Exception as e:
            logger.error(f"Failed to stop stream {stream_id}: {e}")
            return False, str(e)
    
    def stop_all_streams(self):
        """Stop all active streams"""
        stream_ids = list(self.active_streams.keys())
        for stream_id in stream_ids:
            self.stop_stream(stream_id)
        logger.info("All streams stopped")
    
    def get_stream_status(self):
        """Get status of all streams"""
        status = {}
        for stream_id, info in self.active_streams.items():
            process = info['process']
            status[stream_id] = {
                'cam_id': info['cam_id'],
                'rtmp_url': info['rtmp_url'],
                'start_time': info['start_time'],
                'pid': info['pid'],
                'running': process.poll() is None,
                'uptime': time.time() - info['start_time']
            }
        return status

class RabbitMQController:
    def __init__(self):
        self.stream_manager = StreamManager()
        self.connection = None
        self.channel = None
        self.should_run = True
        
    def send_response(self, success, message, command_id=None):
        """Send response back to server"""
        try:
            if not self.channel:
                return
                
            response = {
                "jetson_id": JETSON_ID,
                "success": success,
                "message": message,
                "timestamp": time.time(),
                "command_id": command_id
            }
            
            self.channel.queue_declare(queue=RESPONSE_QUEUE, durable=True)
            self.channel.basic_publish(
                exchange='',
                routing_key=RESPONSE_QUEUE,
                body=json.dumps(response),
                properties=pika.BasicProperties(delivery_mode=2)
            )
            logger.info(f"Response sent: {response}")
        except Exception as e:
            logger.error(f"Failed to send response: {e}")
    
    def handle_command(self, command_data):
        """Handle received command"""
        try:
            msg = json.loads(command_data)
            jetson_id = msg.get("jetson_id")
            cmd = msg.get("command")
            cam_id = msg.get("cam_id")
            rtmp_server_url = msg.get("rtmp_server_url")
            command_id = msg.get("command_id")
            stream_id = msg.get("stream_id", f"stream_{cam_id}")
            
            # Check if command is for this Jetson
            if jetson_id and jetson_id != JETSON_ID:
                logger.info(f"Command for {jetson_id}, ignoring (this is {JETSON_ID})")
                return
            
            logger.info(f"Processing command: {cmd}")
            
            if cmd == "start_rtmp_deepstream":
                if cam_id is None or not rtmp_server_url:
                    self.send_response(False, "Missing cam_id or rtmp_server_url", command_id)
                    return
                
                success, message = self.stream_manager.start_rtmp_stream(
                    stream_id, cam_id, rtmp_server_url
                )
                self.send_response(success, message, command_id)
                
            elif cmd == "stop_stream":
                stream_id = msg.get("stream_id", f"stream_{cam_id}")
                success, message = self.stream_manager.stop_stream(stream_id)
                self.send_response(success, message, command_id)
                
            elif cmd == "stop_all_streams":
                self.stream_manager.stop_all_streams()
                self.send_response(True, "All streams stopped", command_id)
                
            elif cmd == "get_status":
                status = self.stream_manager.get_stream_status()
                self.send_response(True, json.dumps(status), command_id)
                
            elif cmd == "disable_rtsp_sink1":
                success = self.stream_manager.disable_rtsp_sink1()
                message = "RTSP sink1 disabled" if success else "Failed to disable RTSP sink1"
                self.send_response(success, message, command_id)
                
            elif cmd.startswith("set_stream_source"):
                if cam_id is None:
                    self.send_response(False, "Missing cam_id", command_id)
                    return
                
                success = self.stream_manager.enable_rtsp_sink1_source(cam_id)
                message = f"RTSP sink1 enabled for camera {cam_id}" if success else "Failed to enable RTSP sink1"
                self.send_response(success, message, command_id)
                
            else:
                self.send_response(False, f"Unknown command: {cmd}", command_id)
                
        except json.JSONDecodeError:
            logger.error("Invalid JSON command received")
            self.send_response(False, "Invalid JSON format", None)
        except Exception as e:
            logger.error(f"Error handling command: {e}")
            self.send_response(False, f"Command processing error: {str(e)}", None)
    
    def on_message(self, ch, method, properties, body):
        """RabbitMQ message callback"""
        try:
            command = body.decode()
            self.handle_command(command)
        except Exception as e:
            logger.error(f"Error processing message: {e}")
    
    def connect_rabbitmq(self):
        """Connect to RabbitMQ with retry logic"""
        while self.should_run:
            try:
                self.connection = pika.BlockingConnection(
                    pika.ConnectionParameters(
                        host=RABBITMQ_HOST,
                        port=RABBITMQ_PORT,
                        credentials=pika.PlainCredentials(RABBITMQ_USER, RABBITMQ_PASS),
                        heartbeat=600,
                        blocked_connection_timeout=300
                    )
                )
                self.channel = self.connection.channel()
                self.channel.queue_declare(queue=QUEUE_NAME, durable=True)
                self.channel.basic_qos(prefetch_count=1)
                self.channel.basic_consume(
                    queue=QUEUE_NAME,
                    on_message_callback=self.on_message,
                    auto_ack=True
                )
                
                logger.info(f"RabbitMQ listener ({JETSON_ID}) connected and waiting for commands...")
                return True
                
            except Exception as e:
                logger.error(f"RabbitMQ connection failed: {e}")
                time.sleep(5)
                
        return False
    
    def start_consuming(self):
        """Start consuming messages"""
        while self.should_run:
            try:
                if not self.connection or self.connection.is_closed:
                    if not self.connect_rabbitmq():
                        continue
                
                self.channel.start_consuming()
                
            except KeyboardInterrupt:
                logger.info("Keyboard interrupt received")
                break
            except Exception as e:
                logger.error(f"Consuming error: {e}")
                time.sleep(5)
    
    def shutdown(self):
        """Graceful shutdown"""
        logger.info("Shutting down...")
        self.should_run = False
        
        # Stop all streams
        self.stream_manager.stop_all_streams()
        self.stream_manager.shutdown_event.set()
        
        # Close RabbitMQ connection
        try:
            if self.channel and not self.channel.is_closed:
                self.channel.stop_consuming()
            if self.connection and not self.connection.is_closed:
                self.connection.close()
        except Exception as e:
            logger.error(f"Error closing RabbitMQ connection: {e}")
        
        logger.info("Shutdown completed")

# Global controller instance
controller = None

def signal_handler(signum, frame):
    """Handle shutdown signals"""
    logger.info(f"Received signal {signum}")
    if controller:
        controller.shutdown()
    sys.exit(0)

def main():
    global controller
    
    # Setup signal handlers
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    
    try:
        controller = RabbitMQController()
        controller.start_consuming()
    except Exception as e:
        logger.error(f"Application error: {e}")
    finally:
        if controller:
            controller.shutdown()

if __name__ == "__main__":
    main()