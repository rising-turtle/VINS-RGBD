#include <ros/ros.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/Bool.h>
#include <cv_bridge/cv_bridge.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>

#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>

#include "feature_tracker.h"

#define SHOW_UNDISTORTION 0

vector<uchar> r_status;
vector<float> r_err;
queue<sensor_msgs::ImageConstPtr> img_buf;

ros::Publisher pub_img,pub_match;
ros::Publisher pub_restart;

FeatureTracker trackerData[NUM_OF_CAM];
double first_image_time;
int pub_count = 1;
bool first_image_flag = true;
double last_image_time = 0;
bool init_pub = 0;

void img_callback(const sensor_msgs::ImageConstPtr &color_msg, const sensor_msgs::ImageConstPtr &depth_msg)
{
    if(first_image_flag)
    {
        first_image_flag = false;
        first_image_time = color_msg->header.stamp.toSec();
        last_image_time = color_msg->header.stamp.toSec();
        return;
    }
    // detect unstable camera stream
    if (color_msg->header.stamp.toSec() - last_image_time > 1.0 || color_msg->header.stamp.toSec() < last_image_time)
    {
        ROS_WARN("image discontinue! reset the feature tracker!");
        first_image_flag = true;
        last_image_time = 0;
        pub_count = 1;
        std_msgs::Bool restart_flag;
        restart_flag.data = true;
        pub_restart.publish(restart_flag);
        return;
    }
    last_image_time = color_msg->header.stamp.toSec();
    // frequency control
    if (round(1.0 * pub_count / (color_msg->header.stamp.toSec() - first_image_time)) <= FREQ)
    {
        PUB_THIS_FRAME = true;
        // reset the frequency control
        if (abs(1.0 * pub_count / (color_msg->header.stamp.toSec() - first_image_time) - FREQ) < 0.01 * FREQ)
        {
            first_image_time = color_msg->header.stamp.toSec();
            pub_count = 0;
        }
    }
    else
        PUB_THIS_FRAME = false;
    // encodings in ros: http://docs.ros.org/diamondback/api/sensor_msgs/html/image__encodings_8cpp_source.html
    //color has encoding RGB8
    cv_bridge::CvImageConstPtr ptr;
    cv::Mat ret_img; 
    if (color_msg->encoding == "8UC1")//shan:why 8UC1 need this operation? Find answer:https://github.com/ros-perception/vision_opencv/issues/175
    {
        sensor_msgs::Image img;
        img.header = color_msg->header;
        img.height = color_msg->height;
        img.width = color_msg->width;
        img.is_bigendian = color_msg->is_bigendian;
        img.step = color_msg->step;
        img.data = color_msg->data;
        img.encoding = "mono8";
        ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::MONO8);
        ret_img = ptr->image; 
    } else if(color_msg->encoding == "8UC3"){
        sensor_msgs::Image img;
        img.header = color_msg->header;
        img.height = color_msg->height;
        img.width = color_msg->width;
        img.is_bigendian = color_msg->is_bigendian;
        img.step = color_msg->step;
        img.data = color_msg->data;
        img.encoding = "bgr8";
        ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::BGR8);
        ret_img = ptr->image.clone(); 
        cv::cvtColor(ret_img, ret_img, cv::COLOR_BGR2GRAY);
    }
    else{
        ptr = cv_bridge::toCvCopy(color_msg, sensor_msgs::image_encodings::MONO8);
        ret_img = ptr->image.clone(); 
    }

    //depth has encoding TYPE_16UC1
    cv_bridge::CvImageConstPtr depth_ptr;
    // debug use     std::cout<<depth_msg->encoding<<std::endl;
    if (depth_msg->encoding == "16UC1")
    {
        sensor_msgs::Image img;
        img.header = depth_msg->header;
        img.height = depth_msg->height;
        img.width = depth_msg->width;
        img.is_bigendian = depth_msg->is_bigendian;
        img.step = depth_msg->step;
        img.data = depth_msg->data;
        img.encoding = "mono16"; // sensor_msgs::image_encodings::MONO16;
        depth_ptr = cv_bridge::toCvCopy(img, sensor_msgs::image_encodings::MONO16);
    }else
        depth_ptr = cv_bridge::toCvCopy(depth_msg, sensor_msgs::image_encodings::MONO16);


    cv::Mat show_img = ret_img; //ptr->image;
    TicToc t_r;
    cv::Mat dep_img = depth_ptr->image.clone(); 

    // init pts here, using readImage()
    ROS_INFO("depth_image size: width: %d height: %d", dep_img.cols, dep_img.rows);
    // cv::imshow("dep", dep_img);
    // cv::waitKey(5);
    float x_scale = ret_img.cols / dep_img.cols; 
    float y_scale = ret_img.rows / dep_img.rows; 

    for (int i = 0; i < NUM_OF_CAM; i++)
    {
        ROS_DEBUG("processing camera %d", i);
        if (i != 1 || !STEREO_TRACK)
        {

            // trackerData[i].readImage(ptr->image.rowRange(ROW * i, ROW * (i + 1)), depth_ptr->image.rowRange(ROW * i, ROW * (i + 1)), color_msg->header.stamp.toSec());
            // trackerData[i].readImage(ret_img, depth_ptr->image.rowRange(ROW * i, ROW * (i + 1)), color_msg->header.stamp.toSec()); 
            trackerData[i].readImage(ret_img, dep_img, color_msg->header.stamp.toSec()); 
        }
        else
        {
            if (EQUALIZE)
            {
                cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
                // clahe->apply(ptr->image.rowRange(ROW * i, ROW * (i + 1)), trackerData[i].cur_img);
                clahe->apply(ret_img, trackerData[i].cur_img);
            }
            else
            {
                trackerData[i].cur_img = ret_img; //ptr->image.rowRange(ROW * i, ROW * (i + 1));
                trackerData[i].cur_depth = dep_img; // depth_ptr->image.rowRange(ROW * i, ROW * (i + 1));
            }

        }
//always 0
#if SHOW_UNDISTORTION
        trackerData[i].showUndistortion("undistrotion_" + std::to_string(i));
#endif
    }
    // update all id in ids[]
    //If has ids[i] == -1 (newly added pts by cv::goodFeaturesToTrack), substitute by gloabl id counter (n_id)
    for (unsigned int i = 0;; i++)
    {
        bool completed = false;
        for (int j = 0; j < NUM_OF_CAM; j++)
            if (j != 1 || !STEREO_TRACK)
                completed |= trackerData[j].updateID(i);
        if (!completed)
            break;
    }
    if (PUB_THIS_FRAME)
    {
        // vector<int> test;
        cv::Mat show_depth = dep_img; // depth_ptr->image;
        pub_count++;
        //http://docs.ros.org/api/sensor_msgs/html/msg/PointCloud.html
        sensor_msgs::PointCloudPtr feature_points(new sensor_msgs::PointCloud);
        sensor_msgs::ChannelFloat32 id_of_point;
        sensor_msgs::ChannelFloat32 u_of_point;
        sensor_msgs::ChannelFloat32 v_of_point;
        sensor_msgs::ChannelFloat32 velocity_x_of_point;
        sensor_msgs::ChannelFloat32 velocity_y_of_point;
        //Use round to get depth value of corresponding points
        sensor_msgs::ChannelFloat32 depth_of_point;

        feature_points->header = color_msg->header;
        feature_points->header.frame_id = "world";

        vector<set<int>> hash_ids(NUM_OF_CAM);
        for (int i = 0; i < NUM_OF_CAM; i++)
        {
            auto &un_pts = trackerData[i].cur_un_pts;
            auto &cur_pts = trackerData[i].cur_pts;
            auto &ids = trackerData[i].ids;
            auto &pts_velocity = trackerData[i].pts_velocity;
            for (unsigned int j = 0; j < ids.size(); j++)
            {
                if (trackerData[i].track_cnt[j] > 1)
                {
                    int p_id = ids[j];
                    //not used
                    hash_ids[i].insert(p_id);
                    geometry_msgs::Point32 p;
                    p.x = un_pts[j].x;
                    p.y = un_pts[j].y;
                    p.z = 1;
                    // push normalized point to pointcloud
                    feature_points->points.push_back(p);
                    // push other info
                    id_of_point.values.push_back(p_id * NUM_OF_CAM + i);
                    u_of_point.values.push_back(cur_pts[j].x);
                    v_of_point.values.push_back(cur_pts[j].y);
                    velocity_x_of_point.values.push_back(pts_velocity[j].x);
                    velocity_y_of_point.values.push_back(pts_velocity[j].y);

                    //nearest neighbor....fastest  may be changed
                    // show_depth: 480*640   y:[0,480]   x:[0,640]

                    
                    depth_of_point.values.push_back((int)show_depth.at<unsigned short>(round(cur_pts[j].y/y_scale), round(cur_pts[j].x/x_scale)));
                    // depth_of_point.values.push_back(0.001*show_depth.at<unsigned short>(round(cur_pts[j].y), round(cur_pts[j].x)));
                    //debug use: print depth pixels
                   //  test.push_back((int)show_depth.at<unsigned short>(round(cur_pts[j].y),round(cur_pts[j].x)));
                }
            }
        }
        //debug use: print depth pixels
        // for (int iii = test.size() - 1; iii >= 0; iii--)
        // {
        //     std::cout << test[iii] << " ";
        // }
        //std::cout << std::endl;
        feature_points->channels.push_back(id_of_point);
        feature_points->channels.push_back(u_of_point);
        feature_points->channels.push_back(v_of_point);
        feature_points->channels.push_back(velocity_x_of_point);
        feature_points->channels.push_back(velocity_y_of_point);
        feature_points->channels.push_back(depth_of_point);
        ROS_DEBUG("publish %f, at %f", feature_points->header.stamp.toSec(), ros::Time::now().toSec());
        // skip the first image; since no optical speed on frist image
        if (!init_pub)
        {
            init_pub = 1;
        }
        else
        {
          pub_img.publish(feature_points);//"feature"
        }
        // Show image with tracked points in rviz (by topic pub_match)
        if (SHOW_TRACK)
        {
            ptr = cv_bridge::cvtColor(ptr, sensor_msgs::image_encodings::BGR8);
            //cv::Mat stereo_img(ROW * NUM_OF_CAM, COL, CV_8UC3);
            cv::Mat stereo_img = ptr->image;

            for (int i = 0; i < NUM_OF_CAM; i++)
            {
                cv::Mat tmp_img = stereo_img; // stereo_img.rowRange(i * ROW, (i + 1) * ROW);
                cv::cvtColor(show_img, tmp_img, CV_GRAY2RGB);//??seems useless?

                for (unsigned int j = 0; j < trackerData[i].cur_pts.size(); j++)
                {
                    double len = std::min(1.0, 1.0 * trackerData[i].track_cnt[j] / WINDOW_SIZE);
                    cv::circle(tmp_img, trackerData[i].cur_pts[j], 2, cv::Scalar(255 * (1 - len), 0, 255 * len), 2);
                    //draw speed line
                    /*
                    Vector2d tmp_cur_un_pts (trackerData[i].cur_un_pts[j].x, trackerData[i].cur_un_pts[j].y);
                    Vector2d tmp_pts_velocity (trackerData[i].pts_velocity[j].x, trackerData[i].pts_velocity[j].y);
                    Vector3d tmp_prev_un_pts;
                    tmp_prev_un_pts.head(2) = tmp_cur_un_pts - 0.10 * tmp_pts_velocity;
                    tmp_prev_un_pts.z() = 1;
                    Vector2d tmp_prev_uv;
                    trackerData[i].m_camera->spaceToPlane(tmp_prev_un_pts, tmp_prev_uv);
                    cv::line(tmp_img, trackerData[i].cur_pts[j], cv::Point2f(tmp_prev_uv.x(), tmp_prev_uv.y()), cv::Scalar(255 , 0, 0), 1 , 8, 0);
                    */
                    //char name[10];
                    //sprintf(name, "%d", trackerData[i].ids[j]);
                    //cv::putText(tmp_img, name, trackerData[i].cur_pts[j], cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 0));
                }
            }
            // cv::imshow("vis", stereo_img);
            // cv::waitKey(5);
            pub_match.publish(ptr->toImageMsg());
        }
    }
    //ROS_INFO("whole feature tracker processing costs: %f", t_r.toc());
}

int main(int argc, char **argv)
{
    ros::init(argc, argv, "feature_tracker");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    //paremeters.cpp
    readParameters(n);

    //trackerData defined as global parameter   type: FeatureTracker list   size: 1
    for (int i = 0; i < NUM_OF_CAM; i++)
        trackerData[i].readIntrinsicParameter(CAM_NAMES[i]);

    if(FISHEYE)
    {
        for (int i = 0; i < NUM_OF_CAM; i++)
        {
            trackerData[i].fisheye_mask = cv::imread(FISHEYE_MASK, 0);
            if(!trackerData[i].fisheye_mask.data)
            {
                ROS_INFO("load mask fail");
                ROS_BREAK();
            }
            else
                ROS_INFO("load mask success");
        }
    }
    //ref: http://docs.ros.org/api/message_filters/html/c++/classmessage__filters_1_1TimeSynchronizer.html#a9e58750270e40a2314dd91632a9570a6
    //     https://blog.csdn.net/zyh821351004/article/details/47758433
    message_filters::Subscriber<sensor_msgs::Image> sub_image(n, IMAGE_TOPIC, 1);
    message_filters::Subscriber<sensor_msgs::Image> sub_depth(n, DEPTH_TOPIC, 1);
//    message_filters::TimeSynchronizer<sensor_msgs::Image, sensor_msgs::Image> sync(sub_image, sub_depth, 100);
    // use ApproximateTime to fit fisheye camera
    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image,sensor_msgs::Image> syncPolicy;
    message_filters::Synchronizer<syncPolicy> sync(syncPolicy(10), sub_image, sub_depth);
    sync.registerCallback(boost::bind(&img_callback, _1, _2));

    //有图像发布到IMAGE_TOPIC，执行img_callback     100: queue size
    //ros::Subscriber sub_img = n.subscribe(IMAGE_TOPIC, 100, img_callback);
    //返回一个ros::Publisher对象  std_msgs::xxx的publisher,     1000: queue size
    pub_img = n.advertise<sensor_msgs::PointCloud>("feature", 1000);
    pub_match = n.advertise<sensor_msgs::Image>("feature_img",1000);
    pub_restart = n.advertise<std_msgs::Bool>("restart",1000);
    /*
    if (SHOW_TRACK)
        cv::namedWindow("vis", cv::WINDOW_NORMAL);
    */
    ros::spin();
    return 0;
}


// new points velocity is 0, pub or not?
// track cnt > 1 pub?
