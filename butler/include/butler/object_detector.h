#ifndef _OBJECT_DETECTOR_H_
#define _OBJECT_DETECTOR_H_

/*
 * ROS
 */
#include <ros/ros.h>
// image related
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
// message related
#include <darknet_ros_msgs/BoundingBoxes.h>
#include <darknet_ros_msgs/CheckForObjectsAction.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/CameraInfo.h>
#include <geometry_msgs/Point.h>
#include <sensor_msgs/image_encodings.h>
// action related
#include <actionlib/client/simple_action_client.h>
#include <actionlib/client/terminal_state.h>

/*
 * C++
 */
#include <cmath>


extern const std::string Depth_Topic_;
extern const std::string RGB_Topic_;
extern const std::string Action_Server_; 
extern const std::string Cup_Position_;
extern const std::string Camera_Info_;


typedef actionlib::SimpleActionClient<darknet_ros_msgs::CheckForObjectsAction> Client_T_;

namespace ObjectID {
	const int Cup = 0;
}

/** @brief Main class for Object Detection
 *
 * 		Used to send images to darknet_ros and publish XYZ coordinates for detected objects
 * 		Cup position is published on the "/object_detector/cup_position" topic
 */
class ObjectDetector 
{
	public:
		/** @brief Default constructor for ObjectDetector class
		 *
		 * 		Initializes Action client, RGB, Depth, and CameraInfo subscribers, and Cup position publisher
		 */
		ObjectDetector(const std::string& clientname) 
			: client_(clientname, true) //action client
		{
			/*
			 * subscribers and publisher
			 */
			depthS_ = n_.subscribe(Depth_Topic_, 0, &ObjectDetector::depthCallback, this);
			rgbS_ = n_.subscribe(RGB_Topic_, 0, &ObjectDetector::rgbCallback, this);
			cInfoS_ = n_.subscribe(Camera_Info_, 0, &ObjectDetector::cInfoCallback, this);
			cupPublisher_ = n_.advertise<geometry_msgs::Point>(Cup_Position_, 1);

			//connection to action server
			ROS_INFO("waiting for action server to start");
			client_.waitForServer();
			ROS_INFO("connected to action server");
		}

		~ObjectDetector() 
		{
		}

		/** @brief Callback function for depth image subscriber
		 *
		 *		stores the most recent depth image
		 */
		void depthCallback(const sensor_msgs::Image& img) 
		{
			depthImg_ = img;
			return;
		}

		/** @brief Callback function for rgb image subscriber
		 *
		 * 		stores the most recent RGB image
		 */
		void rgbCallback(const sensor_msgs::Image& img) 
		{
			rgbImg_ = img;
			return;
		}

		/** @brief Callback function for camera info subscriber
		 *
		 * 		retrieves the camera info
		 * 		specifically used to retrieve the camera matrices
		 */
		void cInfoCallback(const sensor_msgs::CameraInfo& info)
		{
			cameraInfo_ = info;
			return;
		}

		/** @brief Main function to call the object detector
		 *
		 * 		latest rgb image is attached to goal message, as is the object id
		 */
		void sendGoal(const int id) 
		{
			goal_.id = id;
			goal_.image = rgbImg_;

			/*
			 * convert sensor_msgs::Image to opencv image
			 */
			try
			{
				oldDepth_ = cv_bridge::toCvCopy(depthImg_, sensor_msgs::image_encodings::TYPE_32FC1);
				std::cout << "depth type: " << oldDepth_->image.type() << std::endl;
			}
			catch (cv_bridge::Exception& e)
			{
				ROS_ERROR("cv_bridge error: %s", e.what());
				return;
			}

			/*
			 * send goal function
			 * binds callback function
			 */
			client_.sendGoal(goal_,
					boost::bind(&ObjectDetector::detectorCallback, this, _1, _2),
					Client_T_::SimpleActiveCallback(),
					Client_T_::SimpleFeedbackCallback());
			ROS_INFO("sent goal");

			return;
		}

		/** @brief Detector callback function
		 *
		 * 		Used as a callback for sendGoal() and publishes the cup position
		 * 		Does K-Means clustering for each Depth image region of interest in order to extract a more precise depth measurement
		 */
		void detectorCallback(const actionlib::SimpleClientGoalState& state,
				const darknet_ros_msgs::CheckForObjectsResultConstPtr& result) 
		{
			short m = 0;
			for(const auto& bb : result->boundingBoxes.boundingBoxes) 
			{
				/*
				 * reshape bounding box pixels into a 1D array for k-means
				 */
				cv::Mat obj, labels, centers;
				reshape(obj, oldDepth_, bb);

				/*
				 * K-Means clustering to find a decent depth estimate to the detected object
				 * probably not necessary with image_geometry
				 */
				cv::kmeans(obj, 3, labels, 
						cv::TermCriteria(CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 10000, 0.001), 
						3, cv::KMEANS_PP_CENTERS, centers);

				float dist = 0.0;	//distance/depth to cup

				/*
				 * depth image is imprecise
				 * 3 clusters increases assurance that the cup map is extracted correctly
				 * background/foreground/misreadings generally give darker/brigher values than the cup
				 */
				dist = mean(centers.at<float>(0), centers.at<float>(1), centers.at<float>(2));

				/*
				 * publisher and info output
				 */
				publishXYZ(bb, (double) dist);
				ROS_INFO("#%d: %s; Prb: %3f, K: %3f", ++m, bb.Class.c_str(), bb.probability, dist);

				/*
				 * draw bounding boxes and info on depth image
				 * for debugging purposes, will be removed in the end i guess
				 */
				draw(oldDepth_, bb, m);
			}
			/*
			 * displays depth image with bounding boxes
			 * for debugging purposes, will be removed in the end i guess
			 */
			cv::imshow("depthwin", oldDepth_->image);
			char wKey = cv::waitKey(1) & 0xFF;

			return;
		}

	private:
		/*
		 * Node
		 * Subscribers
		 * Publisher
		 */
		ros::NodeHandle n_;
		ros::Subscriber depthS_, rgbS_, cInfoS_;
		ros::Publisher cupPublisher_; 


		/*
		 * Action client for darknet_ros object detector
		 */
		Client_T_ client_;

		/*
		 * message variables
		 * cupPos_: x, y, z position published on Cup_Position_ topic
		 * rgbImg_, depthImg_: image messages from orbbec astra camera
		 * goal_: rgbImg_ and id for object detector
		 * result_: resulting bounding boxes and probabilities from detector
		 */  
		geometry_msgs::Point cupPos_;
		sensor_msgs::Image rgbImg_, depthImg_;
		sensor_msgs::CameraInfo cameraInfo_;
		darknet_ros_msgs::CheckForObjectsGoal goal_;
		darknet_ros_msgs::CheckForObjectsResult result_;

		/*
		 * retention of depth image, stored every time a goal is sent to darknet_ros for detection
		 * in order to match the depth to the correlated rgb image
		 */
		cv_bridge::CvImagePtr oldDepth_;

		/** @brief Mean function, returns the mean of three same-type values and is mean to look at
		 */
		template<typename T>
			inline T mean(const T x, const T y, const T z)
			{
				if(((x < y) && (x > z)) || ((x < z) && (x > y))) return x;
				else if(((y < x) && (y > z)) || ((y < z) && (y > x))) return y;
				else if(((z < x) && (z > y)) || ((z < y) && (z > x))) return z;
				//what are the odds of two exactly equal inputs right
				else return (x + y + z) / (T) 3;
			}

		/** @brief Draw function, draws bounding boxes and information labels on each detected object
		 *
		 * might as well have a separate function for it to avoid a mess
		 */
		void draw(cv_bridge::CvImagePtr& img_, const darknet_ros_msgs::BoundingBox& box_, int counter_)
		{
			std::ostringstream title;
			title << std::setprecision(3) << "#" << counter_ << "; P: " << box_.probability;

			cv::rectangle(img_->image, cv::Point(box_.xmin, box_.ymin), cv::Point(box_.xmax, box_.ymin - 12), 1.0, -1);
			cv::rectangle(img_->image, cv::Point(box_.xmin, box_.ymin), cv::Point(box_.xmax, box_.ymax), 1.0);
			cv::putText(img_->image, title.str(), cv::Point(box_.xmin, box_.ymin), cv::FONT_HERSHEY_PLAIN, 1.0, 0.0, 2);

			return;
		}

		/** @brief Reshapes the boundingbox subset of img_ into a 1D array in obj_
		 */
		void reshape(cv::Mat& obj_, const cv_bridge::CvImagePtr& img_, const darknet_ros_msgs::BoundingBox& box_)
		{
			obj_ = cv::Mat((box_.xmax - box_.xmin) * (box_.ymax - box_.ymin), 1, CV_32F);
			for(int k = 0; k < (box_.xmax - box_.xmin); ++k)
			{
				for(int l = 0; l < (box_.ymax - box_.ymin); ++l)
				{
					float* cc = img_->image.ptr<float>(l + box_.ymin, k + box_.xmin);
					obj_.at<float>(l + (k * (box_.ymax - box_.ymin)), 0) = (std::isnan(*cc)) ? 0.0 : *cc;
				}
			}

			return;
		}

		/** @brief Maps depth image coordinates into meters
		 */
		void publishXYZ(const darknet_ros_msgs::BoundingBox& box_, double zd)
		{
			double xd = ((double) box_.xmin) + (((double) (box_.xmax - box_.xmin)) / 2);
			double yd = ((double) box_.ymin) + (((double) (box_.ymax - box_.ymin)) / 2);

			cupPos_.x = (xd - cameraInfo_.K[2] - cameraInfo_.P[3]) * zd / cameraInfo_.K[0];
			cupPos_.y = (yd - cameraInfo_.K[5] - cameraInfo_.P[7]) * zd / cameraInfo_.K[4];
			cupPos_.z = zd; //depth

			cupPublisher_.publish(cupPos_);

			return;
		}
};

#endif
