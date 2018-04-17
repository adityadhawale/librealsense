// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2017 Intel Corporation. All Rights Reserved

#include <iostream>
#include <map>
#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>
#include <image_transport/image_transport.h>
#include <ros/ros.h>
#include <ros/package.h>
#include <librealsense2/rs.hpp>
#include <librealsense2/rsutil.h>
#include <librealsense2/hpp/rs_processing.hpp>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/CameraInfo.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/point_cloud2_iterator.h>
#include <constants.h>
#include <realsense_ros_camera/Extrinsics.h>
#include <tf/transform_broadcaster.h>
#include <tf2_ros/static_transform_broadcaster.h>
#include <sensor_msgs/Imu.h>
#include <realsense_ros_camera/IMUInfo.h>
#include <csignal>
#include <eigen3/Eigen/Geometry>


#define REALSENSE_ROS_EMBEDDED_VERSION_STR (VAR_ARG_STRING(VERSION: REALSENSE_ROS_MAJOR_VERSION.REALSENSE_ROS_MINOR_VERSION.REALSENSE_ROS_PATCH_VERSION))
constexpr auto realsense_ros_camera_version = REALSENSE_ROS_EMBEDDED_VERSION_STR;

namespace realsense_ros_camera
{
    using stream_index_pair = std::pair<rs2_stream, int>;

    const stream_index_pair COLOR{RS2_STREAM_COLOR, 0};
    const stream_index_pair DEPTH{RS2_STREAM_DEPTH, 0};
    const stream_index_pair INFRA1{RS2_STREAM_INFRARED, 1};
    const stream_index_pair INFRA2{RS2_STREAM_INFRARED, 2};
    const stream_index_pair FISHEYE{RS2_STREAM_FISHEYE, 0};
    const stream_index_pair GYRO{RS2_STREAM_GYRO, 0};
    const stream_index_pair ACCEL{RS2_STREAM_ACCEL, 0};

    const std::vector<std::vector<stream_index_pair>> IMAGE_STREAMS = {{{DEPTH, INFRA1, INFRA2}, {COLOR}, {FISHEYE}}};

    const std::vector<std::vector<stream_index_pair>> HID_STREAMS = {{GYRO, ACCEL}};

    inline void signalHandler(int signum)
    {
        ROS_INFO_STREAM(strsignal(signum) << " Signal is received! Terminate RealSense Node...");
        ros::shutdown();
        exit(signum);
    }

    class RealSenseCameraNodelet: public nodelet::Nodelet
    {
    public:
        RealSenseCameraNodelet() :
            _serial_no(""),
            _base_frame_id(""),
            _intialize_time_base(false)
        {
            ROS_INFO("RealSense ROS v%s", REALSENSE_ROS_VERSION_STR);
            ROS_INFO("Running with LibRealSense v%s", RS2_API_VERSION_STR);

            signal(SIGINT, signalHandler);
            auto severity = rs2_log_severity::RS2_LOG_SEVERITY_ERROR;
            tryGetLogSeverity(severity);
            if (rs2_log_severity::RS2_LOG_SEVERITY_DEBUG == severity)
                ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Debug);

            rs2::log_to_console(severity);

            // Types for depth stream
            _format[DEPTH] = RS2_FORMAT_Z16;   // libRS type
            _image_format[DEPTH] = CV_16UC1;    // CVBridge type
            _encoding[DEPTH] = sensor_msgs::image_encodings::TYPE_16UC1; // ROS message type
            _unit_step_size[DEPTH] = sizeof(uint16_t); // sensor_msgs::ImagePtr row step size
            _stream_name[DEPTH] = "depth";

            // Infrared stream - Left
            _format[INFRA1] = RS2_FORMAT_Y8;   // libRS type
            _image_format[INFRA1] = CV_8UC1;    // CVBridge type
            _encoding[INFRA1] = sensor_msgs::image_encodings::TYPE_8UC1; // ROS message type
            _unit_step_size[INFRA1] = sizeof(uint8_t); // sensor_msgs::ImagePtr row step size
            _stream_name[INFRA1] = "infra1";

            // Infrared stream - Right
            _format[INFRA2] = RS2_FORMAT_Y8;   // libRS type
            _image_format[INFRA2] = CV_8UC1;    // CVBridge type
            _encoding[INFRA2] = sensor_msgs::image_encodings::TYPE_8UC1; // ROS message type
            _unit_step_size[INFRA2] = sizeof(uint8_t); // sensor_msgs::ImagePtr row step size
            _stream_name[INFRA2] = "infra2";

            // Types for color stream
            _format[COLOR] = RS2_FORMAT_RGB8;   // libRS type
            _image_format[COLOR] = CV_8UC3;    // CVBridge type
            _encoding[COLOR] = sensor_msgs::image_encodings::RGB8; // ROS message type
            _unit_step_size[COLOR] = 3; // sensor_msgs::ImagePtr row step size
            _stream_name[COLOR] = "color";

            // Types for fisheye stream
            _format[FISHEYE] = RS2_FORMAT_RAW8;   // libRS type
            _image_format[FISHEYE] = CV_8UC1;    // CVBridge type
            _encoding[FISHEYE] = sensor_msgs::image_encodings::TYPE_8UC1; // ROS message type
            _unit_step_size[FISHEYE] = sizeof(uint8_t); // sensor_msgs::ImagePtr row step size
            _stream_name[FISHEYE] = "fisheye";

            // Types for Motion-Module streams
            _format[GYRO] = RS2_FORMAT_MOTION_XYZ32F;   // libRS type
            _image_format[GYRO] = CV_8UC1;    // CVBridge type
            _encoding[GYRO] = sensor_msgs::image_encodings::TYPE_8UC1; // ROS message type
            _unit_step_size[GYRO] = sizeof(uint8_t); // sensor_msgs::ImagePtr row step size
            _stream_name[GYRO] = "gyro";

            _format[ACCEL] = RS2_FORMAT_MOTION_XYZ32F;   // libRS type
            _image_format[ACCEL] = CV_8UC1;    // CVBridge type
            _encoding[ACCEL] = sensor_msgs::image_encodings::TYPE_8UC1; // ROS message type
            _unit_step_size[ACCEL] = sizeof(uint8_t); // sensor_msgs::ImagePtr row step size
            _stream_name[ACCEL] = "accel";
        }

        virtual ~RealSenseCameraNodelet()
        {}

    private:
        virtual void onInit()
        {
            getParameters();
            setupDevice();
            setupPublishers();
            setupStreams();
            publishStaticTransforms();
            ROS_INFO_STREAM("RealSense Node Is Up!");
        }

        void getParameters()
        {
            ROS_INFO("getParameters...");

            _pnh = getPrivateNodeHandle();

            _pnh.param("serial_no", _serial_no);
            _pnh.param("float_depth_image", _float_depth_image);

            if(_pnh.hasParam("serial_no"))
            {
              _pnh.getParam("serial_no", _serial_no);
            }
            if(_pnh.hasParam("float_depth_image"))
            {
              _pnh.getParam("float_depth_image", _float_depth_image);
            }

            _pnh.param("enable_pointcloud", _pointcloud, POINTCLOUD);
            _pnh.param("enable_sync", _sync_frames, SYNC_FRAMES);

            if (_pointcloud || _float_depth_image)
                _sync_frames = true;

            _pnh.param("depth_width", _width[DEPTH], DEPTH_WIDTH);
            _pnh.param("depth_height", _height[DEPTH], DEPTH_HEIGHT);
            _pnh.param("depth_fps", _fps[DEPTH], DEPTH_FPS);
            _pnh.param("enable_depth", _enable[DEPTH], ENABLE_DEPTH);

            _pnh.param("infra1_width", _width[INFRA1], INFRA1_WIDTH);
            _pnh.param("infra1_height", _height[INFRA1], INFRA1_HEIGHT);
            _pnh.param("infra1_fps", _fps[INFRA1], INFRA1_FPS);
            _pnh.param("enable_infra1", _enable[INFRA1], ENABLE_INFRA1);

            _pnh.param("infra2_width", _width[INFRA2], INFRA2_WIDTH);
            _pnh.param("infra2_height", _height[INFRA2], INFRA2_HEIGHT);
            _pnh.param("infra2_fps", _fps[INFRA2], INFRA2_FPS);
            _pnh.param("enable_infra2", _enable[INFRA2], ENABLE_INFRA2);

            _pnh.param("color_width", _width[COLOR], COLOR_WIDTH);
            _pnh.param("color_height", _height[COLOR], COLOR_HEIGHT);
            _pnh.param("color_fps", _fps[COLOR], COLOR_FPS);
            _pnh.param("enable_color", _enable[COLOR], ENABLE_COLOR);

            _pnh.param("fisheye_width", _width[FISHEYE], FISHEYE_WIDTH);
            _pnh.param("fisheye_height", _height[FISHEYE], FISHEYE_HEIGHT);
            _pnh.param("fisheye_fps", _fps[FISHEYE], FISHEYE_FPS);
            _pnh.param("enable_fisheye", _enable[FISHEYE], ENABLE_FISHEYE);

            _pnh.param("gyro_fps", _fps[GYRO], GYRO_FPS);
            _pnh.param("accel_fps", _fps[ACCEL], ACCEL_FPS);
            _pnh.param("enable_imu", _enable[GYRO], ENABLE_IMU);
            _pnh.param("enable_imu", _enable[ACCEL], ENABLE_IMU);

            _pnh.param("base_frame_id", _base_frame_id, DEFAULT_BASE_FRAME_ID);
            _pnh.param("depth_frame_id", _frame_id[DEPTH], DEFAULT_DEPTH_FRAME_ID);
            _pnh.param("infra1_frame_id", _frame_id[INFRA1], DEFAULT_INFRA1_FRAME_ID);
            _pnh.param("infra2_frame_id", _frame_id[INFRA2], DEFAULT_INFRA2_FRAME_ID);
            _pnh.param("color_frame_id", _frame_id[COLOR], DEFAULT_COLOR_FRAME_ID);
            _pnh.param("fisheye_frame_id", _frame_id[FISHEYE], DEFAULT_FISHEYE_FRAME_ID);
            _pnh.param("imu_gyro_frame_id", _frame_id[GYRO], DEFAULT_IMU_FRAME_ID);
            _pnh.param("imu_accel_frame_id", _frame_id[ACCEL], DEFAULT_IMU_FRAME_ID);

            _pnh.param("depth_optical_frame_id", _optical_frame_id[DEPTH], DEFAULT_DEPTH_OPTICAL_FRAME_ID);
            _pnh.param("infra1_optical_frame_id", _optical_frame_id[INFRA1], DEFAULT_INFRA1_OPTICAL_FRAME_ID);
            _pnh.param("infra2_optical_frame_id", _optical_frame_id[INFRA2], DEFAULT_INFRA2_OPTICAL_FRAME_ID);
            _pnh.param("color_optical_frame_id", _optical_frame_id[COLOR], DEFAULT_COLOR_OPTICAL_FRAME_ID);
            _pnh.param("fisheye_optical_frame_id", _optical_frame_id[FISHEYE], DEFAULT_FISHEYE_OPTICAL_FRAME_ID);
            _pnh.param("gyro_optical_frame_id", _optical_frame_id[GYRO], DEFAULT_GYRO_OPTICAL_FRAME_ID);
            _pnh.param("accel_optical_frame_id", _optical_frame_id[ACCEL], DEFAULT_ACCEL_OPTICAL_FRAME_ID);
        }

        void setupDevice()
        {
            ROS_INFO("setupDevice...");
            try{
                _ctx.reset(new rs2::context());

                auto list = _ctx->query_devices();

                if (0 == list.size())
                {
                    _ctx.reset();
                    ROS_ERROR("No RealSense devices were found! Terminate RealSense Node...");
                    ros::shutdown();
                    exit(1);
                }


                if (_serial_no.size() != 0)
                {
                  std::stringstream ss(_serial_no);
                  int x = 0;
                  ss >> x;
                  _dev = list[x];
                    //for (uint i = 0; i < list.size(); i++)
                    //{
                        rs2::device temp_dev;
                                               // temp_dev = list[i];
                       // std::string new_serial_no = temp_dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
                       // if (new_serial_no == _serial_no)
                       // {
                       //     _dev = temp_dev;
                       //     break;
                       // }
                    //}

                }
                else
                {
                    _dev = list.front();
                }
                                
                // Take the first device in the list.
                // TODO: Add an ability to get the specific device to work with from outside
               
                _ctx->set_devices_changed_callback([this](rs2::event_information& info)
                {
                    if (info.was_removed(_dev))
                    {
                        ROS_FATAL("The device has been disconnected! Terminate RealSense Node...");
                        ros::shutdown();
                        exit(1);
                    }
                });

                auto camera_name = _dev.get_info(RS2_CAMERA_INFO_NAME);
                ROS_INFO_STREAM("Device Name: " << camera_name);

                _serial_no = _dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
                ROS_INFO_STREAM("Device Serial No: " << _serial_no);

                auto fw_ver = _dev.get_info(RS2_CAMERA_INFO_FIRMWARE_VERSION);
                ROS_INFO_STREAM("Device FW version: " << fw_ver);

                auto pid = _dev.get_info(RS2_CAMERA_INFO_PRODUCT_ID);
                ROS_INFO_STREAM("Device Product ID: " << pid);

                ROS_INFO_STREAM("Sync Mode: " << ((_sync_frames)?"On":"Off"));

                auto dev_sensors = _dev.query_sensors();

                ROS_INFO_STREAM("Device Sensors: ");
                for(auto&& elem : dev_sensors)
                {
                    std::string module_name = elem.get_info(RS2_CAMERA_INFO_NAME);
                    if ("Stereo Module" == module_name || "Coded-Light Depth Sensor" == module_name)
                    {
                        auto sen = new rs2::sensor(elem);
                        _sensors[DEPTH] = std::unique_ptr<rs2::sensor>(sen);
                        _sensors[INFRA1] = std::unique_ptr<rs2::sensor>(sen);
                        _sensors[INFRA2] = std::unique_ptr<rs2::sensor>(sen);
                    }
                    else if ("RGB Camera" == module_name)
                    {
                        _sensors[COLOR] = std::unique_ptr<rs2::sensor>(new rs2::sensor(elem));
                    }
                    else if ("Wide FOV Camera" == module_name)
                    {
                        _sensors[FISHEYE] = std::unique_ptr<rs2::sensor>(new rs2::sensor(elem));
                    }
                    else if ("Motion Module" == module_name)
                    {
                        auto hid_sensor = new rs2::sensor(elem);
                        _sensors[GYRO] = std::unique_ptr<rs2::sensor>(hid_sensor);
                        _sensors[ACCEL] = std::unique_ptr<rs2::sensor>(hid_sensor);
                    }
                    else
                    {
                        ROS_ERROR_STREAM("Module Name \"" << module_name << "\" isn't supported by LibRealSense! Terminate RealSense Node...");
                        ros::shutdown();
                        exit(1);
                    }
                    ROS_INFO_STREAM(std::string(elem.get_info(RS2_CAMERA_INFO_NAME)) << " was found.");
                }

                // Update "enable" map
                std::vector<std::vector<stream_index_pair>> streams(IMAGE_STREAMS);
                streams.insert(streams.end(), HID_STREAMS.begin(), HID_STREAMS.end());
                for (auto& elem : streams)
                {
                    for (auto& stream_index : elem)
                    {
                        if (true == _enable[stream_index] && _sensors.find(stream_index) == _sensors.end()) // check if device supports the enabled stream
                        {
                            ROS_INFO_STREAM(rs2_stream_to_string(stream_index.first) << " sensor isn't supported by current device! -- Skipping...");
                            _enable[stream_index] = false;
                        }
                    }
                }
            }
            catch(const std::exception& ex)
            {
                ROS_ERROR_STREAM("An exception has been thrown: " << ex.what());
                throw;
            }
            catch(...)
            {
                ROS_ERROR_STREAM("Unknown exception has occured!");
                throw;
            }
        }

        void setupPublishers()
        {
            ROS_INFO("setupPublishers...");
            image_transport::ImageTransport image_transport(getNodeHandle());

            if (true == _enable[DEPTH])
            {
                _image_publishers[DEPTH] = image_transport.advertise("camera/depth/image_raw", 1);

                if(_float_depth_image)
                {
                  _float_image_publisher = image_transport.advertise("camera/depth/image_rect", 1);  
                }                
                _info_publisher[DEPTH] = getNodeHandle().advertise<sensor_msgs::CameraInfo>("camera/depth/camera_info", 1);

                if (_pointcloud)
                    _pointcloud_publisher = getNodeHandle().advertise<sensor_msgs::PointCloud2>("camera/points", 1);
            }

            if (true == _enable[INFRA1])
            {
                _image_publishers[INFRA1] = image_transport.advertise("camera/infra1/image_raw", 1);
                _info_publisher[INFRA1] = getNodeHandle().advertise<sensor_msgs::CameraInfo>("camera/infra1/camera_info", 1);
            }

            if (true == _enable[INFRA2])
            {
                _image_publishers[INFRA2] = image_transport.advertise("camera/infra2/image_raw", 1);
                _info_publisher[INFRA2] = getNodeHandle().advertise<sensor_msgs::CameraInfo>("camera/infra2/camera_info", 1);
            }

            if (true == _enable[COLOR])
            {
                _image_publishers[COLOR] = image_transport.advertise("camera/color/image_raw", 1);
                _info_publisher[COLOR] = getNodeHandle().advertise<sensor_msgs::CameraInfo>("camera/color/camera_info", 1);
            }

            if (true == _enable[FISHEYE] &&
                true == _enable[DEPTH])
            {
                _image_publishers[FISHEYE] = image_transport.advertise("camera/fisheye/image_raw", 1);
                _info_publisher[FISHEYE] = _node_handle.advertise<sensor_msgs::CameraInfo>("camera/fisheye/camera_info", 1);
                _fe_to_depth_publisher = _node_handle.advertise<Extrinsics>("camera/extrinsics/fisheye2depth", 1, true);
            }

            if (true == _enable[GYRO])
            {
                _imu_publishers[GYRO] = _node_handle.advertise<sensor_msgs::Imu>("camera/gyro/sample", 100);
                _info_publisher[GYRO] = _node_handle.advertise<IMUInfo>("camera/gyro/imu_info", 1, true);
            }

            if (true == _enable[ACCEL])
            {
                _imu_publishers[ACCEL] = _node_handle.advertise<sensor_msgs::Imu>("camera/accel/sample", 100);
                _info_publisher[ACCEL] = _node_handle.advertise<IMUInfo>("camera/accel/imu_info", 1, true);
            }

            if (true == _enable[FISHEYE] &&
                (true == _enable[GYRO] ||
                 true == _enable[ACCEL]))
            {
                _fe_to_imu_publisher = _node_handle.advertise<Extrinsics>("camera/extrinsics/fisheye2imu", 1, true);
            }
        }

        void setupStreams()
        {
            ROS_INFO("setupStreams...");
            try{
                for (auto& streams : IMAGE_STREAMS)
                {
                    for (auto& elem : streams)
                    {
                        if (true == _enable[elem])
                        {
                            auto& sens = _sensors[elem];
                            auto profiles = sens->get_stream_profiles();
                            for (auto& profile : profiles)
                            {
                                auto video_profile = profile.as<rs2::video_stream_profile>();
                                if (video_profile.format() == _format[elem] &&
                                    video_profile.width()  == _width[elem] &&
                                    video_profile.height() == _height[elem] &&
                                    video_profile.fps()    == _fps[elem] &&
                                    video_profile.stream_index() == elem.second)
                                {
                                    _enabled_profiles[elem].push_back(profile);

                                    _image[elem] = cv::Mat(_width[elem], _height[elem], _image_format[elem], cv::Scalar(0, 0, 0));
                                    ROS_INFO_STREAM(_stream_name[elem] << " stream is enabled - width: " << _width[elem] << ", height: " << _height[elem] << ", fps: " << _fps[elem]);
                                    break;
                                }
                            }
                            if (_enabled_profiles.find(elem) == _enabled_profiles.end())
                            {
                                ROS_WARN_STREAM("Given stream configuration is not supported by the device! " <<
                                                " Stream: " << rs2_stream_to_string(elem.first) <<
                                                ", Format: " << _format[elem] <<
                                                ", Width: " << _width[elem] <<
                                                ", Height: " << _height[elem] <<
                                                ", FPS: " << _fps[elem]);
                                _enable[elem] = false;
                            }
                        }
                    }
                }

                // Publish image stream info
                for (auto& profiles : _enabled_profiles)
                {
                    for (auto& profile : profiles.second)
                    {
                        auto video_profile = profile.as<rs2::video_stream_profile>();
                        updateStreamCalibData(video_profile);
                    }
                }

                auto frame_callback = [this](rs2::frame frame)
                {
                    // We compute a ROS timestamp which is based on an initial ROS time at point of first frame,
                    // and the incremental timestamp from the camera.
                    // In sync mode the timestamp is based on ROS time
                    if (false == _intialize_time_base)
                    {
                        _intialize_time_base = true;
                        _ros_time_base = ros::Time::now();
                        _camera_time_base = frame.get_timestamp();
                    }

                    ros::Time t;
                    if (_sync_frames)
                        t = ros::Time::now();
                    else
                        t = ros::Time(_ros_time_base.toSec()+ (/*ms*/ frame.get_timestamp() - /*ms*/ _camera_time_base) / /*ms to seconds*/ 1000);

                    auto is_color_frame_arrived = false;
                    auto is_depth_frame_arrived = false;
                    if (frame.is<rs2::frameset>())
                    {
                        ROS_DEBUG("Frameset arrived");
                        auto frameset = frame.as<rs2::frameset>();
                        for (auto it = frameset.begin(); it != frameset.end(); ++it)
                        {
                            auto f = (*it);
                            auto stream_type = f.get_profile().stream_type();
                            if (RS2_STREAM_COLOR == stream_type)
                                is_color_frame_arrived = true;
                            else if (RS2_STREAM_DEPTH == stream_type)
                                is_depth_frame_arrived = true;

                            ROS_DEBUG("Frameset contain %s frame. frame_number: %llu ; frame_TS: %f ; ros_TS(NSec): %lu",
                                      rs2_stream_to_string(stream_type), frame.get_frame_number(), frame.get_timestamp(), t.toNSec());
                            publishFrame(f, t);
                        }
                    }
                    else
                    {
                        auto stream_type = frame.get_profile().stream_type();
                        ROS_DEBUG("%s video frame arrived. frame_number: %llu ; frame_TS: %f ; ros_TS(NSec): %lu",
                                  rs2_stream_to_string(stream_type), frame.get_frame_number(), frame.get_timestamp(), t.toNSec());
                        publishFrame(frame, t);
                    }

                    if(_pointcloud && is_depth_frame_arrived && is_color_frame_arrived &&
                       (0 != _pointcloud_publisher.getNumSubscribers()))
                    {
                        ROS_DEBUG("publishPCTopic(...)");
                        publishPCTopic(t);
                    }
                    if(_float_depth_image && is_depth_frame_arrived && (0 != _float_image_publisher.getNumSubscribers()))
                    {
                      ROS_DEBUG("publishFloatDepthImage(...)");
                      publishFloatDepthImage(t);
                    }
                };

                // Streaming IMAGES
                for (auto& streams : IMAGE_STREAMS)
                {
                    std::vector<rs2::stream_profile> profiles;
                    for (auto& elem : streams)
                    {
                        if (!_enabled_profiles[elem].empty())
                        {
                            profiles.insert(profiles.begin(),
                                            _enabled_profiles[elem].begin(),
                                            _enabled_profiles[elem].end());
                        }
                    }

                    if (!profiles.empty())
                    {
                        auto stream = streams.front();
                        auto& sens = _sensors[stream];
                        sens->open(profiles);

                        if (DEPTH == stream)
                        {
                            auto depth_sensor = sens->as<rs2::depth_sensor>();
                            _depth_scale_meters = depth_sensor.get_depth_scale();
                        }

                        if (_sync_frames)
                        {
                            sens->start(_syncer);
                        }
                        else
                        {
                            sens->start(frame_callback);
                        }
                    }
                }//end for

                if (_sync_frames)
                {
                    _syncer.start(frame_callback);
                }

                // Streaming HID
                for (const auto streams : HID_STREAMS)
                {
                    for (auto& elem : streams)
                    {
                        if (true == _enable[elem])
                        {
                            auto& sens = _sensors[elem];
                            auto profiles = sens->get_stream_profiles();
                            for (rs2::stream_profile& profile : profiles)
                            {
                                if (profile.fps() == _fps[elem] &&
                                    profile.format() == _format[elem])
                                {
                                    _enabled_profiles[elem].push_back(profile);
                                    break;
                                }
                            }
                        }
                    }
                }

                auto gyro_profile = _enabled_profiles.find(GYRO);
                auto accel_profile = _enabled_profiles.find(ACCEL);

                if (gyro_profile != _enabled_profiles.end() &&
                    accel_profile != _enabled_profiles.end())
                {
                    std::vector<rs2::stream_profile> profiles;
                    profiles.insert(profiles.begin(), gyro_profile->second.begin(), gyro_profile->second.end());
                    profiles.insert(profiles.begin(), accel_profile->second.begin(), accel_profile->second.end());
                    auto& sens = _sensors[GYRO];
                    sens->open(profiles);

                    sens->start([this](rs2::frame frame){
                        auto stream = frame.get_profile().stream_type();
                        if (false == _intialize_time_base)
                            return;

                        ROS_DEBUG("Frame arrived: stream: %s ; index: %d ; Timestamp Domain: %s",
                                  rs2_stream_to_string(frame.get_profile().stream_type()),
                                  frame.get_profile().stream_index(),
                                  rs2_timestamp_domain_to_string(frame.get_frame_timestamp_domain()));

                        auto stream_index = (stream == GYRO.first)?GYRO:ACCEL;
                        if (0 != _info_publisher[stream_index].getNumSubscribers() ||
                            0 != _imu_publishers[stream_index].getNumSubscribers())
                        {
                            double elapsed_camera_ms = (/*ms*/ frame.get_timestamp() - /*ms*/ _camera_time_base) / /*ms to seconds*/ 1000;
                            ros::Time t(_ros_time_base.toSec() + elapsed_camera_ms);

                            auto imu_msg = sensor_msgs::Imu();
                            imu_msg.header.frame_id = _optical_frame_id[stream_index];
                            imu_msg.orientation.x = 0.0;
                            imu_msg.orientation.y = 0.0;
                            imu_msg.orientation.z = 0.0;
                            imu_msg.orientation.w = 0.0;
                            imu_msg.orientation_covariance = { -1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

                            auto axes = *(reinterpret_cast<const float3*>(frame.get_data()));
                            if (GYRO == stream_index)
                            {
                                imu_msg.angular_velocity.x = axes.x;
                                imu_msg.angular_velocity.y = axes.y;
                                imu_msg.angular_velocity.z = axes.z;
                            }
                            else if (ACCEL == stream_index)
                            {
                                imu_msg.linear_acceleration.x = axes.x;
                                imu_msg.linear_acceleration.y = axes.y;
                                imu_msg.linear_acceleration.z = axes.z;
                            }
                            _seq[stream_index] += 1;
                            imu_msg.header.seq = _seq[stream_index];
                            imu_msg.header.stamp = t;
                            _imu_publishers[stream_index].publish(imu_msg);
                            ROS_DEBUG("Publish %s stream", rs2_stream_to_string(frame.get_profile().stream_type()));
                        }
                    });

                    if (true == _enable[GYRO])
                    {
                        ROS_INFO_STREAM(_stream_name[GYRO] << " stream is enabled - " << "fps: " << _fps[GYRO]);
                        auto gyroInfo = getImuInfo(GYRO);
                        _info_publisher[GYRO].publish(gyroInfo);
                    }

                    if (true == _enable[ACCEL])
                    {
                        ROS_INFO_STREAM(_stream_name[ACCEL] << " stream is enabled - " << "fps: " << _fps[ACCEL]);
                        auto accelInfo = getImuInfo(ACCEL);
                        _info_publisher[ACCEL].publish(accelInfo);
                    }
                }


                if (true == _enable[DEPTH] &&
                    true == _enable[FISHEYE])
                {
                    auto ex = getFisheye2DepthExtrinsicsMsg();
                    _fe_to_depth_publisher.publish(ex);
                }

                if (true == _enable[FISHEYE] &&
                    (_enable[GYRO] || _enable[ACCEL]))
                {
                    auto ex = getFisheye2ImuExtrinsicsMsg();
                    _fe_to_imu_publisher.publish(ex);
                }
            }
            catch(const std::exception& ex)
            {
                ROS_ERROR_STREAM("An exception has been thrown: " << ex.what());
                throw;
            }
            catch(...)
            {
                ROS_ERROR_STREAM("Unknown exception has occured!");
                throw;
            }
        }

        void updateStreamCalibData(const rs2::video_stream_profile& video_profile)
        {
            stream_index_pair stream_index{video_profile.stream_type(), video_profile.stream_index()};
            auto intrinsic = video_profile.get_intrinsics();
            _stream_intrinsics[stream_index] = intrinsic;

            _camera_info[stream_index].width = intrinsic.width;
            _camera_info[stream_index].height = intrinsic.height;
            _camera_info[stream_index].header.frame_id = _optical_frame_id[stream_index];

            _camera_info[stream_index].K.at(0) = intrinsic.fx;
            _camera_info[stream_index].K.at(2) = intrinsic.ppx;
            _camera_info[stream_index].K.at(4) = intrinsic.fy;
            _camera_info[stream_index].K.at(5) = intrinsic.ppy;
            _camera_info[stream_index].K.at(8) = 1;

            _camera_info[stream_index].P.at(0) = _camera_info[stream_index].K.at(0);
            _camera_info[stream_index].P.at(1) = 0;
            _camera_info[stream_index].P.at(2) = _camera_info[stream_index].K.at(2);
            _camera_info[stream_index].P.at(3) = 0;
            _camera_info[stream_index].P.at(4) = 0;
            _camera_info[stream_index].P.at(5) = _camera_info[stream_index].K.at(4);
            _camera_info[stream_index].P.at(6) = _camera_info[stream_index].K.at(5);
            _camera_info[stream_index].P.at(7) = 0;
            _camera_info[stream_index].P.at(8) = 0;
            _camera_info[stream_index].P.at(9) = 0;
            _camera_info[stream_index].P.at(10) = 1;
            _camera_info[stream_index].P.at(11) = 0;

            rs2::stream_profile depth_profile;
            if (!getEnabledProfile(DEPTH, depth_profile))
            {
                ROS_ERROR_STREAM("Depth profile not found!");
                ros::shutdown();
                exit(1);
            }

            // TODO: Why Depth to Color?
            if (stream_index == DEPTH && _enable[DEPTH] && _enable[COLOR])
            {
                _depth2color_extrinsics = depth_profile.get_extrinsics_to(_enabled_profiles[COLOR].front());
                // set depth to color translation values in Projection matrix (P)
                _camera_info[stream_index].P.at(3) = _depth2color_extrinsics.translation[0];     // Tx
                _camera_info[stream_index].P.at(7) = _depth2color_extrinsics.translation[1];     // Ty
                _camera_info[stream_index].P.at(11) = _depth2color_extrinsics.translation[2];    // Tz
            }

            _camera_info[stream_index].distortion_model = "plumb_bob";

            // set R (rotation matrix) values to identity matrix
            _camera_info[stream_index].R.at(0) = 1.0;
            _camera_info[stream_index].R.at(1) = 0.0;
            _camera_info[stream_index].R.at(2) = 0.0;
            _camera_info[stream_index].R.at(3) = 0.0;
            _camera_info[stream_index].R.at(4) = 1.0;
            _camera_info[stream_index].R.at(5) = 0.0;
            _camera_info[stream_index].R.at(6) = 0.0;
            _camera_info[stream_index].R.at(7) = 0.0;
            _camera_info[stream_index].R.at(8) = 1.0;

            for (int i = 0; i < 5; i++)
            {
                _camera_info[stream_index].D.push_back(intrinsic.coeffs[i]);
            }
        }

        Eigen::Quaternionf rotationMatrixToQuaternion(float rotation[3]) const
        {
            Eigen::Matrix3f m;
            m << rotation[0], rotation[1], rotation[2],
                 rotation[3], rotation[4], rotation[5],
                 rotation[6], rotation[7], rotation[8];
            Eigen::Quaternionf q(m);
            return q;
        }

        void publishStaticTransforms()
        {
            ROS_INFO("publishStaticTransforms...");
            // Publish transforms for the cameras
            tf::Quaternion q_c2co;
            geometry_msgs::TransformStamped b2c_msg; // Base to Color
            geometry_msgs::TransformStamped c2co_msg; // Color to Color_Optical

            tf::Quaternion q_d2do;
            geometry_msgs::TransformStamped b2d_msg; // Base to Depth
            geometry_msgs::TransformStamped d2do_msg; // Depth to Depth_Optical

            tf::Quaternion ir1_2_ir1o;
            geometry_msgs::TransformStamped b2ir1_msg; // Base to IR1
            geometry_msgs::TransformStamped ir1_2_ir1o_msg; // IR1 to IR1_Optical

            tf::Quaternion ir2_2_ir2o;
            geometry_msgs::TransformStamped b2ir2_msg; // Base to IR2
            geometry_msgs::TransformStamped ir2_2_ir2o_msg; // IR2 to IR2_Optical

            // Get the current timestamp for all static transforms
            ros::Time transform_ts_ = ros::Time::now();

            // The depth frame is used as the base frame.
            // Hence no additional transformation is done from base frame to depth frame.
            // Transform base frame to depth frame
            b2d_msg.header.stamp = transform_ts_;
            b2d_msg.header.frame_id = _base_frame_id;
            b2d_msg.child_frame_id = _frame_id[DEPTH];
            b2d_msg.transform.translation.x = 0;
            b2d_msg.transform.translation.y = 0;
            b2d_msg.transform.translation.z = 0;
            b2d_msg.transform.rotation.x = 0;
            b2d_msg.transform.rotation.y = 0;
            b2d_msg.transform.rotation.z = 0;
            b2d_msg.transform.rotation.w = 1;
            _static_tf_broadcaster.sendTransform(b2d_msg);

            // Transform depth frame to depth optical frame
            q_d2do.setRPY(-M_PI / 2, 0.0, -M_PI / 2);
            d2do_msg.header.stamp = transform_ts_;
            d2do_msg.header.frame_id = _frame_id[DEPTH];
            d2do_msg.child_frame_id = _optical_frame_id[DEPTH];
            d2do_msg.transform.translation.x = 0;
            d2do_msg.transform.translation.y = 0;
            d2do_msg.transform.translation.z = 0;
            d2do_msg.transform.rotation.x = q_d2do.getX();
            d2do_msg.transform.rotation.y = q_d2do.getY();
            d2do_msg.transform.rotation.z = q_d2do.getZ();
            d2do_msg.transform.rotation.w = q_d2do.getW();
            _static_tf_broadcaster.sendTransform(d2do_msg);

            rs2::stream_profile depth_profile;
            if (!getEnabledProfile(DEPTH, depth_profile))
            {
                ROS_ERROR_STREAM("Depth profile not found!");
                ros::shutdown();
                exit(1);
            }

            if (true == _enable[COLOR])
            {
                // Transform base frame to color frame
                auto q = rotationMatrixToQuaternion(_depth2color_extrinsics.rotation);

                b2c_msg.header.stamp = transform_ts_;
                b2c_msg.header.frame_id = _base_frame_id;
                b2c_msg.child_frame_id = _frame_id[COLOR];
                b2c_msg.transform.translation.x = _depth2color_extrinsics.translation[2];
                b2c_msg.transform.translation.y = -_depth2color_extrinsics.translation[0];
                b2c_msg.transform.translation.z = -_depth2color_extrinsics.translation[1];
                b2c_msg.transform.rotation.x = q.x();
                b2c_msg.transform.rotation.y = q.y();
                b2c_msg.transform.rotation.z = q.z();
                b2c_msg.transform.rotation.w = q.w();
                _static_tf_broadcaster.sendTransform(b2c_msg);

                // Transform color frame to color optical frame
                q_c2co.setRPY(-M_PI / 2, 0.0, -M_PI / 2);
                c2co_msg.header.stamp = transform_ts_;
                c2co_msg.header.frame_id = _frame_id[COLOR];
                c2co_msg.child_frame_id = _optical_frame_id[COLOR];
                c2co_msg.transform.translation.x = 0;
                c2co_msg.transform.translation.y = 0;
                c2co_msg.transform.translation.z = 0;
                c2co_msg.transform.rotation.x = q_c2co.getX();
                c2co_msg.transform.rotation.y = q_c2co.getY();
                c2co_msg.transform.rotation.z = q_c2co.getZ();
                c2co_msg.transform.rotation.w = q_c2co.getW();
                _static_tf_broadcaster.sendTransform(c2co_msg);
            }

            if (true == _enable[INFRA1])
            {
                auto d2ir1_extrinsics = depth_profile.get_extrinsics_to(_enabled_profiles[INFRA1].front());
                auto q = rotationMatrixToQuaternion(d2ir1_extrinsics.rotation);

                // Transform base frame to infra1
                b2ir1_msg.header.stamp = transform_ts_;
                b2ir1_msg.header.frame_id = _base_frame_id;
                b2ir1_msg.child_frame_id = _frame_id[INFRA1];
                b2ir1_msg.transform.translation.x = d2ir1_extrinsics.translation[2];
                b2ir1_msg.transform.translation.y = -d2ir1_extrinsics.translation[0];
                b2ir1_msg.transform.translation.z = -d2ir1_extrinsics.translation[1];

                b2ir1_msg.transform.rotation.x = q.x();
                b2ir1_msg.transform.rotation.y = q.y();
                b2ir1_msg.transform.rotation.z = q.z();
                b2ir1_msg.transform.rotation.w = q.w();
                _static_tf_broadcaster.sendTransform(b2ir1_msg);

                // Transform infra1 frame to infra1 optical frame
                ir1_2_ir1o.setRPY(-M_PI / 2, 0.0, -M_PI / 2);
                ir1_2_ir1o_msg.header.stamp = transform_ts_;
                ir1_2_ir1o_msg.header.frame_id = _frame_id[INFRA1];
                ir1_2_ir1o_msg.child_frame_id = _optical_frame_id[INFRA1];
                ir1_2_ir1o_msg.transform.translation.x = 0;
                ir1_2_ir1o_msg.transform.translation.y = 0;
                ir1_2_ir1o_msg.transform.translation.z = 0;
                ir1_2_ir1o_msg.transform.rotation.x = ir1_2_ir1o.getX();
                ir1_2_ir1o_msg.transform.rotation.y = ir1_2_ir1o.getY();
                ir1_2_ir1o_msg.transform.rotation.z = ir1_2_ir1o.getZ();
                ir1_2_ir1o_msg.transform.rotation.w = ir1_2_ir1o.getW();
                _static_tf_broadcaster.sendTransform(ir1_2_ir1o_msg);
            }

            if (true == _enable[INFRA2])
            {
                auto d2ir2_extrinsics = depth_profile.get_extrinsics_to(_enabled_profiles[INFRA2].front());
                auto q = rotationMatrixToQuaternion(d2ir2_extrinsics.rotation);

                // Transform base frame to infra2
                b2ir2_msg.header.stamp = transform_ts_;
                b2ir2_msg.header.frame_id = _base_frame_id;
                b2ir2_msg.child_frame_id = _frame_id[INFRA2];
                b2ir2_msg.transform.translation.x = d2ir2_extrinsics.translation[2];
                b2ir2_msg.transform.translation.y = -d2ir2_extrinsics.translation[0];
                b2ir2_msg.transform.translation.z = -d2ir2_extrinsics.translation[1];
                b2ir2_msg.transform.rotation.x = q.x();
                b2ir2_msg.transform.rotation.y = q.y();
                b2ir2_msg.transform.rotation.z = q.z();
                b2ir2_msg.transform.rotation.w = q.w();
                _static_tf_broadcaster.sendTransform(b2ir2_msg);

                // Transform infra2 frame to infra1 optical frame
                ir2_2_ir2o.setRPY(-M_PI / 2, 0.0, -M_PI / 2);
                ir2_2_ir2o_msg.header.stamp = transform_ts_;
                ir2_2_ir2o_msg.header.frame_id = _frame_id[INFRA2];
                ir2_2_ir2o_msg.child_frame_id = _optical_frame_id[INFRA2];
                ir2_2_ir2o_msg.transform.translation.x = 0;
                ir2_2_ir2o_msg.transform.translation.y = 0;
                ir2_2_ir2o_msg.transform.translation.z = 0;
                ir2_2_ir2o_msg.transform.rotation.x = ir2_2_ir2o.getX();
                ir2_2_ir2o_msg.transform.rotation.y = ir2_2_ir2o.getY();
                ir2_2_ir2o_msg.transform.rotation.z = ir2_2_ir2o.getZ();
                ir2_2_ir2o_msg.transform.rotation.w = ir2_2_ir2o.getW();
                _static_tf_broadcaster.sendTransform(ir2_2_ir2o_msg);
            }
            // TODO: Publish Fisheye TF
        }

        void publishPCTopic(const ros::Time& t)
        {
            auto color_intrinsics = _stream_intrinsics[COLOR];
            auto image_depth16 = reinterpret_cast<const uint16_t*>(_image[DEPTH].data);
            auto depth_intrinsics = _stream_intrinsics[DEPTH];
            sensor_msgs::PointCloud2 msg_pointcloud;
            msg_pointcloud.header.stamp = t;
            msg_pointcloud.header.frame_id = _optical_frame_id[DEPTH];
            msg_pointcloud.width = depth_intrinsics.width;
            msg_pointcloud.height = depth_intrinsics.height;
            msg_pointcloud.is_dense = true;

            sensor_msgs::PointCloud2Modifier modifier(msg_pointcloud);

            modifier.setPointCloud2Fields(4,
                                          "x", 1, sensor_msgs::PointField::FLOAT32,
                                          "y", 1, sensor_msgs::PointField::FLOAT32,
                                          "z", 1, sensor_msgs::PointField::FLOAT32,
                                          "rgb", 1, sensor_msgs::PointField::FLOAT32);
            modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");

            sensor_msgs::PointCloud2Iterator<float>iter_x(msg_pointcloud, "x");
            sensor_msgs::PointCloud2Iterator<float>iter_y(msg_pointcloud, "y");
            sensor_msgs::PointCloud2Iterator<float>iter_z(msg_pointcloud, "z");

            sensor_msgs::PointCloud2Iterator<uint8_t>iter_r(msg_pointcloud, "r");
            sensor_msgs::PointCloud2Iterator<uint8_t>iter_g(msg_pointcloud, "g");
            sensor_msgs::PointCloud2Iterator<uint8_t>iter_b(msg_pointcloud, "b");

            float depth_point[3], color_point[3], color_pixel[2], scaled_depth;
            unsigned char* color_data = _image[COLOR].data;

            // Fill the PointCloud2 fields
            for (int y = 0; y < depth_intrinsics.height; ++y)
            {
                for (int x = 0; x < depth_intrinsics.width; ++x)
                {
                    scaled_depth = static_cast<float>(*image_depth16) * _depth_scale_meters;
                    float depth_pixel[2] = {static_cast<float>(x), static_cast<float>(y)};
                    rs2_deproject_pixel_to_point(depth_point, &depth_intrinsics, depth_pixel, scaled_depth);

                    if (depth_point[2] <= 0.f || depth_point[2] > 5.f)
                    {
                        depth_point[0] = 0.f;
                        depth_point[1] = 0.f;
                        depth_point[2] = 0.f;
                    }

                    *iter_x = depth_point[0];
                    *iter_y = depth_point[1];
                    *iter_z = depth_point[2];

                    rs2_transform_point_to_point(color_point, &_depth2color_extrinsics, depth_point);
                    rs2_project_point_to_pixel(color_pixel, &color_intrinsics, color_point);

                    if (color_pixel[1] < 0.f || color_pixel[1] > color_intrinsics.height
                        || color_pixel[0] < 0.f || color_pixel[0] > color_intrinsics.width)
                    {
                        // For out of bounds color data, default to a shade of blue in order to visually distinguish holes.
                        // This color value is same as the librealsense out of bounds color value.
                        *iter_r = static_cast<uint8_t>(96);
                        *iter_g = static_cast<uint8_t>(157);
                        *iter_b = static_cast<uint8_t>(198);
                    }
                    else
                    {
                        auto i = static_cast<int>(color_pixel[0]);
                        auto j = static_cast<int>(color_pixel[1]);

                        auto offset = i * 3 + j * color_intrinsics.width * 3;
                        *iter_r = static_cast<uint8_t>(color_data[offset]);
                        *iter_g = static_cast<uint8_t>(color_data[offset + 1]);
                        *iter_b = static_cast<uint8_t>(color_data[offset + 2]);
                    }

                    ++image_depth16;
                    ++iter_x; ++iter_y; ++iter_z;
                    ++iter_r; ++iter_g; ++iter_b;
                }
            }
            _pointcloud_publisher.publish(msg_pointcloud);
        }

        void publishFloatDepthImage(const ros::Time& t)
        {
            auto color_intrinsics = _stream_intrinsics[COLOR];
            auto image_depth16 = reinterpret_cast<const uint16_t*>(_image[DEPTH].data);
            auto depth_intrinsics = _stream_intrinsics[DEPTH];

            cv::Mat float_image = cv::Mat(depth_intrinsics.height, depth_intrinsics.width, CV_32FC1, cvScalar(0.));

            float depth_point[3], color_point[3], color_pixel[2], scaled_depth;

            // Fill the PointCloud2 fields
            for (int y = 0; y < depth_intrinsics.height; ++y)
            {
                for (int x = 0; x < depth_intrinsics.width; ++x)
                {
                    scaled_depth = static_cast<float>(*image_depth16) * _depth_scale_meters;
                    float depth_pixel[2] = {static_cast<float>(x), static_cast<float>(y)};
                    rs2_deproject_pixel_to_point(depth_point, &depth_intrinsics, depth_pixel, scaled_depth);

                    if (depth_point[2] <= 0.f || depth_point[2] > 5.f)
                    {
                        depth_point[0] = 0.f;
                        depth_point[1] = 0.f;
                        depth_point[2] = 0.f;
                    }

                    rs2_transform_point_to_point(color_point, &_depth2color_extrinsics, depth_point);
                    rs2_project_point_to_pixel(color_pixel, &color_intrinsics, color_point);

                    if (color_pixel[1] >= 0.f && color_pixel[1] < color_intrinsics.height
                        && color_pixel[0] >= 0.f && color_pixel[0] < color_intrinsics.width)
                    {
                        auto i = static_cast<int>(color_pixel[0]);
                        auto j = static_cast<int>(color_pixel[1]);
        
                        float_image.at<float>(j, i) = color_point[2];
                    }
                    ++image_depth16;
                }
            }
            sensor_msgs::ImagePtr img;
            img = cv_bridge::CvImage(std_msgs::Header(), sensor_msgs::image_encodings::TYPE_32FC1, float_image).toImageMsg();
            img->header.stamp = t;
            img->header.frame_id = _optical_frame_id[COLOR];
            _float_image_publisher.publish(img);

        }

        Extrinsics rsExtrinsicsToMsg(const rs2_extrinsics& extrinsics) const
        {
            Extrinsics extrinsicsMsg;
            for (int i = 0; i < 9; ++i)
            {
                extrinsicsMsg.rotation[i] = extrinsics.rotation[i];
                if (i < 3)
                    extrinsicsMsg.translation[i] = extrinsics.translation[i];
            }

            return extrinsicsMsg;
        }

        Extrinsics getFisheye2ImuExtrinsicsMsg()
        {
            auto& fisheye = _enabled_profiles[FISHEYE].front();
            auto& hid = _enabled_profiles[GYRO].front();
            Extrinsics extrinsicsMsg = rsExtrinsicsToMsg(fisheye.get_extrinsics_to(hid));
            extrinsicsMsg.header.frame_id = "fisheye2imu_extrinsics";
            return extrinsicsMsg;
        }

        Extrinsics getFisheye2DepthExtrinsicsMsg()
        {
            auto& fisheye = _enabled_profiles[FISHEYE].front();
            auto& depth = _enabled_profiles[DEPTH].front();
            Extrinsics extrinsicsMsg = rsExtrinsicsToMsg(fisheye.get_extrinsics_to(depth));
            extrinsicsMsg.header.frame_id = "fisheye2depth_extrinsics";
            return extrinsicsMsg;
        }

        struct float3
        {
            float x, y, z;
        };

        IMUInfo getImuInfo(const stream_index_pair& stream_index)
        {
            IMUInfo info{};
           // auto imuIntrinsics = _sensors[stream_index]->get_motion_intrinsics(stream_index.first);
           // if (GYRO == stream_index)
           // {
           //     info.header.frame_id = "imu_gyro";
           // }
           // else if (ACCEL == stream_index)
           // {
           //     info.header.frame_id = "imu_accel";
           // }

           // auto index = 0;
           // for (int i = 0; i < 3; ++i)
           // {
           //     for (int j = 0; j < 4; ++j)
           //     {
           //         info.data[index] = imuIntrinsics.data[i][j];
           //         ++index;
           //     }
           //     info.noise_variances[i] =  imuIntrinsics.noise_variances[i];
           //     info.bias_variances[i] = imuIntrinsics.bias_variances[i];
           // }
           // return info;
        }

        void tryGetLogSeverity(rs2_log_severity& severity) const
        {
            static const char* severity_var_name = "LRS_LOG_LEVEL";
            auto content = getenv(severity_var_name);

            if (content)
            {
                std::string content_str(content);
                std::transform(content_str.begin(), content_str.end(), content_str.begin(), ::toupper);

                for (uint32_t i = 0; i < RS2_LOG_SEVERITY_COUNT; i++)
                {
                    auto current = std::string(rs2_log_severity_to_string((rs2_log_severity)i));
                    std::transform(current.begin(), current.end(), current.begin(), ::toupper);
                    if (content_str == current)
                    {
                        severity = (rs2_log_severity)i;
                        break;
                    }
                }
            }
        }

        void publishFrame(rs2::frame f, const ros::Time& t)
        {
            ROS_DEBUG("publishFrame(...)");
            stream_index_pair stream{f.get_profile().stream_type(), f.get_profile().stream_index()};
            auto& image = _image[stream];
            image.data = (uint8_t*)f.get_data();
            ++(_seq[stream]);
            auto& info_publisher = _info_publisher[stream];
            auto& image_publisher = _image_publishers[stream];
            if(0 != info_publisher.getNumSubscribers() ||
               0 != image_publisher.getNumSubscribers())
            {
                auto width = 0;
                auto height = 0;
                auto bpp = 1;
                if (f.is<rs2::video_frame>())
                {
                    auto image = f.as<rs2::video_frame>();
                    width = image.get_width();
                    height = image.get_height();
                    bpp = image.get_bytes_per_pixel();
                }

                sensor_msgs::ImagePtr img;
                img = cv_bridge::CvImage(std_msgs::Header(), _encoding[stream], image).toImageMsg();
                img->width = width;
                img->height = height;
                img->is_bigendian = false;
                img->step = width * bpp;
                img->header.frame_id = _optical_frame_id[stream];
                img->header.stamp = t;
                img->header.seq = _seq[stream];

                auto& cam_info = _camera_info[stream];
                cam_info.header.stamp = t;
                cam_info.header.seq = _seq[stream];
                info_publisher.publish(cam_info);

                image_publisher.publish(img);
                ROS_DEBUG("%s stream published", rs2_stream_to_string(f.get_profile().stream_type()));
            }
        }

        bool getEnabledProfile(const stream_index_pair& stream_index, rs2::stream_profile& profile)
        {
            // Assuming that all D400 SKUs have depth sensor
            auto profiles = _enabled_profiles[stream_index];
            auto it = std::find_if(profiles.begin(), profiles.end(),
                                   [&](const rs2::stream_profile& profile)
                                   { return (profile.stream_type() == stream_index.first); });
            if (it == profiles.end())
                return false;

            profile =  *it;
            return true;
        }

        ros::NodeHandle _node_handle, _pnh;
        std::unique_ptr<rs2::context> _ctx;
        rs2::device _dev;

        std::map<stream_index_pair, std::unique_ptr<rs2::sensor>> _sensors;

        std::string _serial_no;
        bool _float_depth_image;
        float _depth_scale_meters;

        std::map<stream_index_pair, rs2_intrinsics> _stream_intrinsics;
        std::map<stream_index_pair, int> _width;
        std::map<stream_index_pair, int> _height;
        std::map<stream_index_pair, int> _fps;
        std::map<stream_index_pair, bool> _enable;
        std::map<stream_index_pair, std::string> _stream_name;
        tf2_ros::StaticTransformBroadcaster _static_tf_broadcaster;

        std::map<stream_index_pair, image_transport::Publisher> _image_publishers;
        image_transport::Publisher _float_image_publisher;
        std::map<stream_index_pair, ros::Publisher> _imu_publishers;
        std::map<stream_index_pair, int> _image_format;
        std::map<stream_index_pair, rs2_format> _format;
        std::map<stream_index_pair, ros::Publisher> _info_publisher;
        std::map<stream_index_pair, cv::Mat> _image;
        std::map<stream_index_pair, std::string> _encoding;

        std::string _base_frame_id;
        std::map<stream_index_pair, std::string> _frame_id;
        std::map<stream_index_pair, std::string> _optical_frame_id;
        std::map<stream_index_pair, int> _seq;
        std::map<stream_index_pair, int> _unit_step_size;
        std::map<stream_index_pair, sensor_msgs::CameraInfo> _camera_info;
        ros::Publisher _fe_to_depth_publisher, _fe_to_imu_publisher;
        bool _intialize_time_base;
        double _camera_time_base;
        std::map<stream_index_pair, std::vector<rs2::stream_profile>> _enabled_profiles;

        ros::Publisher _pointcloud_publisher;
        ros::Time _ros_time_base;
        bool _sync_frames;
        bool _pointcloud;
        rs2::asynchronous_syncer _syncer;
        rs2_extrinsics _depth2color_extrinsics;
    };//end class

    PLUGINLIB_EXPORT_CLASS(realsense_ros_camera::RealSenseCameraNodelet, nodelet::Nodelet)

}//end namespace